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

#pragma once

#include <Common/Exception.h>
#include <Common/Logger.h>
#include <Encryption/RandomAccessFile.h>
#include <Storages/DeltaMerge/File/MergedFile.h>
#include <Storages/S3/S3Common.h>
#include <aws/s3/model/GetObjectResult.h>
#include <common/types.h>

#include <ext/scope_guard.h>

/// Remove the population of thread_local from Poco
#ifdef thread_local
#undef thread_local
#endif

namespace Aws::S3
{
class S3Client;
}

namespace DB::ErrorCodes
{
extern const int NOT_IMPLEMENTED;
}

namespace DB::S3
{
class S3RandomAccessFile final : public RandomAccessFile
{
public:
    static RandomAccessFilePtr create(const String & remote_fname);

    S3RandomAccessFile(
        std::shared_ptr<TiFlashS3Client> client_ptr_,
        const String & remote_fname_,
        std::optional<std::pair<UInt64, UInt64>> offset_and_size_ = std::nullopt);

    off_t seek(off_t offset, int whence) override;

    ssize_t read(char * buf, size_t size) override;

    std::string getFileName() const override
    {
        return fmt::format("{}/{}", client_ptr->bucket(), remote_fname);
    }

    ssize_t pread(char * /*buf*/, size_t /*size*/, off_t /*offset*/) const override
    {
        throw Exception("S3RandomAccessFile not support pread", ErrorCodes::NOT_IMPLEMENTED);
    }

    int getFd() const override
    {
        return -1;
    }

    bool isClosed() const override
    {
        return is_close;
    }

    void close() override
    {
        is_close = true;
    }

    struct ReadFileInfo
    {
        UInt64 size = 0; // File size of `remote_fname` or `merged_filename`, mainly used for FileCache.
        String merged_filename; // If `merged_filename` is not empty, data should read from `merged_filename`.
        UInt64 read_merged_offset = 0;
        UInt64 read_merged_size = 0;
    };

    [[nodiscard]] static auto setReadFileInfo(ReadFileInfo && read_file_info_)
    {
        read_file_info = std::move(read_file_info_);
        return ext::make_scope_guard([]() {
            read_file_info.reset();
        });
    }

private:
    void initialize();

    // When reading, it is necessary to pass the extra information of file, such file size, the merged file information to S3RandomAccessFile::create.
    // It is troublesome to pass parameters layer by layer. So currently, use thread_local global variable to pass parameters.
    // TODO: refine these codes later.
    inline static thread_local std::optional<ReadFileInfo> read_file_info;

    std::shared_ptr<TiFlashS3Client> client_ptr;
    String remote_fname;
    std::optional<std::pair<UInt64, UInt64>> offset_and_size;

    Aws::S3::Model::GetObjectResult read_result;

    DB::LoggerPtr log;
    bool is_close = false;
};

} // namespace DB::S3
