// Copyright 2022 PingCAP, Ltd.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <Common/Exception.h>
#include <Common/FmtUtils.h>
#include <Common/SyncPoint/SyncPoint.h>
#include <Common/TiFlashMetrics.h>
#include <IO/IOThreadPool.h>
#include <Storages/DeltaMerge/Remote/DataStore/DataStore.h>
#include <Storages/Page/V3/Blob/BlobConfig.h>
#include <Storages/Page/V3/BlobStore.h>
#include <Storages/Page/V3/CheckpointFile/CPFilesWriter.h>
#include <Storages/Page/V3/CheckpointFile/CPWriteDataSource.h>
#include <Storages/Page/V3/CheckpointFile/CheckpointFiles.h>
#include <Storages/Page/V3/PageDirectoryFactory.h>
#include <Storages/Page/V3/Universal/S3LockLocalManager.h>
#include <Storages/Page/V3/Universal/UniversalPageStorage.h>
#include <Storages/Page/V3/Universal/UniversalWriteBatchImpl.h>
#include <Storages/Page/V3/WAL/WALConfig.h>
#include <Storages/S3/S3Common.h>
#include <Storages/S3/S3Filename.h>
#include <common/logger_useful.h>
#include <fiu.h>

#include <future>
#include <mutex>
#include <unordered_map>
#include <unordered_set>


namespace DB
{
UniversalPageStoragePtr UniversalPageStorage::create(
    const String & name,
    PSDiskDelegatorPtr delegator,
    const PageStorageConfig & config,
    const FileProviderPtr & file_provider)
{
    UniversalPageStoragePtr storage = std::make_shared<UniversalPageStorage>(name, delegator, config, file_provider);
    storage->blob_store = std::make_unique<PS::V3::universal::BlobStoreType>(
        name,
        file_provider,
        delegator,
        PS::V3::BlobConfig::from(config));
    if (S3::ClientFactory::instance().isEnabled())
    {
        storage->remote_reader = std::make_unique<PS::V3::S3PageReader>();
        storage->remote_locks_local_mgr = std::make_unique<PS::V3::S3LockLocalManager>();
    }
    return storage;
}

void UniversalPageStorage::restore()
{
    blob_store->registerPaths();

    PS::V3::universal::PageDirectoryFactory factory;
    page_directory = factory
                         .setBlobStore(*blob_store)
                         .create(storage_name, file_provider, delegator, PS::V3::WALConfig::from(config));
}

UniversalPageStorage::~UniversalPageStorage() = default;

size_t UniversalPageStorage::getNumberOfPages(const String & prefix) const
{
    return page_directory->numPagesWithPrefix(prefix);
}

void UniversalPageStorage::write(UniversalWriteBatch && write_batch, const WriteLimiterPtr & write_limiter) const
{
    if (unlikely(write_batch.empty()))
        return;

    Stopwatch watch;
    SCOPE_EXIT({ GET_METRIC(tiflash_storage_page_write_duration_seconds, type_total).Observe(watch.elapsedSeconds()); });
    bool has_writes_from_remote = write_batch.hasWritesFromRemote();
    if (has_writes_from_remote)
    {
        assert(remote_locks_local_mgr != nullptr);
        // Before ingesting remote pages/remote external pages, we need to create "lock" on S3
        // to ensure the correctness between FAP and S3GC.
        // If any "lock" failed to be created, then it will throw exception.
        // Note that if `remote_locks_local_mgr`'s store_id is not inited, it will blocks until inited
        remote_locks_local_mgr->createS3LockForWriteBatch(write_batch);
    }
    auto edit = blob_store->write(std::move(write_batch), write_limiter);
    auto applied_lock_ids = page_directory->apply(std::move(edit), write_limiter);
    if (has_writes_from_remote)
    {
        assert(remote_locks_local_mgr != nullptr);
        // Remove the applied locks from checkpoint_manager.pre_lock_files
        remote_locks_local_mgr->cleanAppliedS3ExternalFiles(std::move(applied_lock_ids));
    }
}

Page UniversalPageStorage::read(const UniversalPageId & page_id, const ReadLimiterPtr & read_limiter, SnapshotPtr snapshot, bool throw_on_not_exist) const
{
    if (!snapshot)
    {
        snapshot = this->getSnapshot("");
    }

    auto page_entry = throw_on_not_exist ? page_directory->getByID(page_id, snapshot) : page_directory->getByIDOrNull(page_id, snapshot);
    auto & checkpoint_info = page_entry.second.checkpoint_info;
    if (checkpoint_info.has_value() && checkpoint_info.is_local_data_reclaimed)
    {
        auto page = remote_reader->read(page_entry);
        UniversalWriteBatch wb;
        auto buf = std::make_shared<ReadBufferFromMemory>(page.data.begin(), page.data.size());
        wb.updateRemotePage(page_id, buf, page.data.size());
        tryUpdateLocalCacheForRemotePages(wb, snapshot);
        return page;
    }
    else
    {
        return blob_store->read(page_entry, read_limiter);
    }
}

UniversalPageMap UniversalPageStorage::read(const UniversalPageIds & page_ids, const ReadLimiterPtr & read_limiter, SnapshotPtr snapshot, bool throw_on_not_exist) const
{
    if (!snapshot)
    {
        snapshot = this->getSnapshot("");
    }

    auto do_read = [&](const UniversalPageIdAndEntries & page_entries) {
        UniversalPageIdAndEntries local_entries, remote_entries;
        for (const auto & entry : page_entries)
        {
            const auto & checkpoint_info = entry.second.checkpoint_info;
            if (checkpoint_info.has_value() && checkpoint_info.is_local_data_reclaimed)
            {
                remote_entries.emplace_back(std::move(entry));
            }
            else
            {
                local_entries.emplace_back(std::move(entry));
            }
        }
        auto local_page_map = blob_store->read(local_entries, read_limiter);
        auto remote_page_map = remote_reader->read(remote_entries);
        UniversalWriteBatch wb;
        for (const auto & [page_id, page] : remote_page_map)
        {
            auto buf = std::make_shared<ReadBufferFromMemory>(page.data.begin(), page.data.size());
            wb.updateRemotePage(page_id, buf, page.data.size());
            local_page_map.emplace(page_id, page);
        }
        tryUpdateLocalCacheForRemotePages(wb, snapshot);
        return local_page_map;
    };

    if (throw_on_not_exist)
    {
        auto page_entries = page_directory->getByIDs(page_ids, snapshot);
        return do_read(page_entries);
    }
    else
    {
        auto [page_entries, page_ids_not_found] = page_directory->getByIDsOrNull(page_ids, snapshot);
        auto page_map = do_read(page_entries);
        for (const auto & page_id_not_found : page_ids_not_found)
        {
            page_map.emplace(page_id_not_found, Page::invalidPage());
        }
        return page_map;
    }
}

UniversalPageMap UniversalPageStorage::read(const std::vector<PageReadFields> & page_fields, const ReadLimiterPtr & read_limiter, SnapshotPtr snapshot, bool throw_on_not_exist) const
{
    if (!snapshot)
    {
        snapshot = this->getSnapshot("");
    }

    // get the entries from directory, keep track
    // for not found page_ids
    UniversalPageIds page_ids_not_found;
    PS::V3::universal::BlobStoreType::FieldReadInfos local_read_infos, remote_read_infos;
    for (const auto & [page_id, field_indices] : page_fields)
    {
        const auto & [id, entry] = throw_on_not_exist ? page_directory->getByID(page_id, snapshot) : page_directory->getByIDOrNull(page_id, snapshot);

        if (entry.isValid())
        {
            auto info = PS::V3::universal::BlobStoreType::FieldReadInfo(page_id, entry, field_indices);
            const auto & checkpoint_info = entry.checkpoint_info;
            if (checkpoint_info.has_value() && checkpoint_info.is_local_data_reclaimed)
            {
                remote_read_infos.emplace_back(info);
            }
            else
            {
                local_read_infos.emplace_back(info);
            }
        }
        else
        {
            page_ids_not_found.emplace_back(id);
        }
    }

    // read page data from blob_store
    auto local_page_map = blob_store->read(local_read_infos, read_limiter);
    auto [page_map_for_update_cache, remote_page_map] = remote_reader->read(remote_read_infos);
    UniversalWriteBatch wb;
    for (const auto & [page_id, page] : page_map_for_update_cache)
    {
        auto buf = std::make_shared<ReadBufferFromMemory>(page.data.begin(), page.data.size());
        wb.updateRemotePage(page_id, buf, page.data.size());
    }
    tryUpdateLocalCacheForRemotePages(wb, snapshot);
    for (const auto & [page_id, page] : remote_page_map)
    {
        local_page_map.emplace(page_id, page);
    }
    for (const auto & page_id_not_found : page_ids_not_found)
    {
        local_page_map.emplace(page_id_not_found, Page::invalidPage());
    }
    return local_page_map;
}

void UniversalPageStorage::traverse(const String & prefix, const std::function<void(const UniversalPageId & page_id, const DB::Page & page)> & acceptor, SnapshotPtr snapshot) const
{
    if (!snapshot)
    {
        snapshot = this->getSnapshot("");
    }

    // TODO: This could hold the read lock of `page_directory` for a long time
    const auto page_ids = page_directory->getAllPageIdsWithPrefix(prefix, snapshot);
    for (const auto & page_id : page_ids)
    {
        const auto page_id_and_entry = page_directory->getByID(page_id, snapshot);
        const auto & checkpoint_info = page_id_and_entry.second.checkpoint_info;
        if (checkpoint_info.has_value() && checkpoint_info.is_local_data_reclaimed)
        {
            auto page = remote_reader->read(page_id_and_entry);
            UniversalWriteBatch wb;
            auto buf = std::make_shared<ReadBufferFromMemory>(page.data.begin(), page.data.size());
            wb.updateRemotePage(page_id, buf, page.data.size());
            tryUpdateLocalCacheForRemotePages(wb, snapshot);
            acceptor(page_id_and_entry.first, page);
        }
        else
        {
            acceptor(page_id_and_entry.first, blob_store->read(page_id_and_entry));
        }
    }
}

void UniversalPageStorage::traverseEntries(const String & prefix, const std::function<void(UniversalPageId page_id, DB::PageEntry entry)> & acceptor, SnapshotPtr snapshot) const
{
    if (!snapshot)
    {
        snapshot = this->getSnapshot("");
    }

    // TODO: This could hold the read lock of `page_directory` for a long time
    const auto page_ids = page_directory->getAllPageIdsWithPrefix(prefix, snapshot);
    for (const auto & page_id : page_ids)
    {
        acceptor(page_id, getEntry(page_id, snapshot));
    }
}

UniversalPageId UniversalPageStorage::getNormalPageId(const UniversalPageId & page_id, SnapshotPtr snapshot, bool throw_on_not_exist) const
{
    if (!snapshot)
    {
        snapshot = this->getSnapshot("");
    }

    return page_directory->getNormalPageId(page_id, snapshot, throw_on_not_exist);
}

DB::PageEntry UniversalPageStorage::getEntry(const UniversalPageId & page_id, SnapshotPtr snapshot) const
{
    if (!snapshot)
    {
        snapshot = this->getSnapshot("");
    }

    try
    {
        const auto & [id, entry] = page_directory->getByIDOrNull(page_id, snapshot);
        (void)id;
        PageEntry entry_ret;
        entry_ret.file_id = entry.file_id;
        entry_ret.offset = entry.offset;
        entry_ret.tag = entry.tag;
        entry_ret.size = entry.size;
        entry_ret.field_offsets = entry.field_offsets;
        entry_ret.checksum = entry.checksum;

        return entry_ret;
    }
    catch (DB::Exception & e)
    {
        LOG_WARNING(log, "{}", e.message());
        return {.file_id = INVALID_BLOBFILE_ID}; // return invalid PageEntry
    }
}

std::optional<DB::PS::V3::CheckpointLocation> UniversalPageStorage::getCheckpointLocation(const UniversalPageId & page_id, SnapshotPtr snapshot) const
{
    if (!snapshot)
    {
        snapshot = this->getSnapshot("");
    }

    try
    {
        const auto & [id, entry] = page_directory->getByIDOrNull(page_id, snapshot);
        (void)id;
        if (entry.checkpoint_info.has_value())
        {
            return entry.checkpoint_info.data_location;
        }
        else
        {
            return std::nullopt;
        }
    }
    catch (DB::Exception & e)
    {
        LOG_WARNING(log, "{}", e.message());
        return std::nullopt;
    }
}

PageIdU64 UniversalPageStorage::getMaxIdAfterRestart() const
{
    return page_directory->getMaxIdAfterRestart();
}

bool UniversalPageStorage::gc(bool /*not_skip*/, const WriteLimiterPtr & write_limiter, const ReadLimiterPtr & read_limiter)
{
    PS::V3::RemoteFileValidSizes remote_valid_sizes;
    bool done_anything = manager.gc(
        *blob_store,
        *page_directory,
        write_limiter,
        read_limiter,
        &remote_valid_sizes,
        log);
    // update the valid size cache of remote file ids
    remote_data_files_stat_cache.updateValidSize(remote_valid_sizes);
    return done_anything;
}

void UniversalPageStorage::registerUniversalExternalPagesCallbacks(const UniversalExternalPageCallbacks & callbacks)
{
    manager.registerExternalPagesCallbacks(callbacks);
}

void UniversalPageStorage::unregisterUniversalExternalPagesCallbacks(const String & prefix)
{
    manager.unregisterExternalPagesCallbacks(prefix);
    // clean all external ids ptrs
    page_directory->unregisterNamespace(prefix);
}

void UniversalPageStorage::tryUpdateLocalCacheForRemotePages(UniversalWriteBatch & wb, SnapshotPtr snapshot) const
{
    auto edit = blob_store->write(std::move(wb));
    auto ignored_entries = page_directory->updateLocalCacheForRemotePages(std::move(edit), snapshot);
    if (!ignored_entries.empty())
    {
        blob_store->remove(ignored_entries);
    }
}

void UniversalPageStorage::initLocksLocalManager(StoreID store_id, S3::S3LockClientPtr lock_client)
{
    assert(remote_locks_local_mgr != nullptr);
    auto last_mf_prefix_opt = remote_locks_local_mgr->initStoreInfo(store_id, lock_client);
    if (last_mf_prefix_opt)
    {
        // First init, we need to restore the `last_checkpoint_sequence` from last checkpoint
        std::scoped_lock lock(checkpoint_mu);
        last_checkpoint_sequence = last_mf_prefix_opt->local_sequence();
    }
}

PS::V3::S3LockLocalManager::ExtraLockInfo UniversalPageStorage::allocateNewUploadLocksInfo() const
{
    assert(remote_locks_local_mgr != nullptr);
    return remote_locks_local_mgr->allocateNewUploadLocksInfo();
}

// a pre-checking to avoid unnecessary consumption of resources
bool UniversalPageStorage::canSkipCheckpoint() const
{
    std::scoped_lock lock(checkpoint_mu);
    auto snap = page_directory->createSnapshot(/*tracing_id*/ "canSkipCheckpoint");
    return snap->sequence == last_checkpoint_sequence;
}

PS::V3::CPDataWriteStats UniversalPageStorage::dumpIncrementalCheckpoint(const UniversalPageStorage::DumpCheckpointOptions & options)
{
    std::scoped_lock lock(checkpoint_mu);

    // Let's keep this snapshot until all finished, so that blob data will not be GCed.
    auto snap = page_directory->createSnapshot(/*tracing_id*/ "dumpIncrementalCheckpoint");

    if (snap->sequence == last_checkpoint_sequence)
        return {.has_new_data = false};

    auto edit_from_mem = page_directory->dumpSnapshotToEdit(snap);

    // As a checkpoint, we write both entries (in manifest) and its data.
    // Some entries' data may be already written by a previous checkpoint. These data will not be written again.
    UInt64 sequence = snap->sequence;
    if (options.override_sequence)
        sequence = options.override_sequence.value();

    auto data_file_id = fmt::format(
        fmt::runtime(options.data_file_id_pattern),
        fmt::arg("seq", sequence),
        fmt::arg("index", 0));
    auto data_file_path = fmt::format(
        fmt::runtime(options.data_file_path_pattern),
        fmt::arg("seq", sequence),
        fmt::arg("index", 0));

    auto manifest_file_id = fmt::format(
        fmt::runtime(options.manifest_file_id_pattern),
        fmt::arg("seq", sequence));
    auto manifest_file_path = fmt::format(
        fmt::runtime(options.manifest_file_path_pattern),
        fmt::arg("seq", sequence));

    RUNTIME_CHECK(
        data_file_path != manifest_file_path,
        data_file_path,
        manifest_file_path);

    auto writer = PS::V3::CPFilesWriter::create({
        .data_file_path = data_file_path,
        .data_file_id = data_file_id,
        .manifest_file_path = manifest_file_path,
        .manifest_file_id = manifest_file_id,
        .data_source = PS::V3::CPWriteDataSourceBlobStore::create(*blob_store),
        .must_locked_files = options.must_locked_files,
    });

    writer->writePrefix({
        .writer = options.writer_info,
        .sequence = snap->sequence,
        .last_sequence = last_checkpoint_sequence,
    });
    std::unordered_set<String> file_ids_to_compact;
    if (options.compact_getter != nullptr)
    {
        file_ids_to_compact = options.compact_getter();
    }
    // get the remote file ids that need to be compacted
    auto write_stats = writer->writeEditsAndApplyCheckpointInfo(edit_from_mem, file_ids_to_compact);
    writer->writeSuffix();
    writer.reset();

    // Persist the checkpoint to remote store.
    // If not persisted or exception throw, then we can not apply the checkpoint
    // info to directory. Neither update `last_checkpoint_sequence`.
    try
    {
        auto checkpoint = PS::V3::LocalCheckpointFiles{
            .data_files = {data_file_path},
            .manifest_file = {manifest_file_path},
        };
        bool persist_done = options.persist_checkpoint(checkpoint);
        if (!persist_done)
        {
            LOG_ERROR(log, "failed to persist checkpoint");
            return {.has_new_data = false}; // TODO: maybe return has_new_data=true but upload_success=false?
        }
    }
    catch (...)
    {
        tryLogCurrentException(log, "failed to persist checkpoint");
        return {.has_new_data = false}; // TODO: maybe return has_new_data=true but upload_success=false?
    }

    SYNC_FOR("before_PageStorage::dumpIncrementalCheckpoint_copyInfo");

    // TODO: Currently, even when has_new_data == false,
    //   something will be written to DataFile (i.e., the file prefix).
    //   This can be avoided, as its content is useless.
    if (write_stats.has_new_data)
    {
        // Copy back the checkpoint info to the current PageStorage.
        // New checkpoint infos are attached in `writeEditsAndApplyCheckpointInfo`.
        page_directory->copyCheckpointInfoFromEdit(edit_from_mem);
    }

    last_checkpoint_sequence = snap->sequence;
    return write_stats;
}

} // namespace DB
