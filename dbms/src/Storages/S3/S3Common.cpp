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
#include <Common/ProfileEvents.h>
#include <Common/Stopwatch.h>
#include <Common/StringUtils/StringUtils.h>
#include <Common/TiFlashMetrics.h>
#include <Storages/S3/MockS3Client.h>
#include <Storages/S3/S3Common.h>
#include <aws/core/auth/AWSCredentials.h>
#include <aws/core/http/Scheme.h>
#include <aws/core/utils/logging/LogMacros.h>
#include <aws/core/utils/logging/LogSystemInterface.h>
#include <aws/s3/S3Client.h>
#include <aws/s3/S3Errors.h>
#include <aws/s3/model/CopyObjectRequest.h>
#include <aws/s3/model/DeleteObjectRequest.h>
#include <aws/s3/model/ExpirationStatus.h>
#include <aws/s3/model/GetBucketLifecycleConfigurationRequest.h>
#include <aws/s3/model/GetObjectRequest.h>
#include <aws/s3/model/HeadObjectRequest.h>
#include <aws/s3/model/LifecycleConfiguration.h>
#include <aws/s3/model/LifecycleExpiration.h>
#include <aws/s3/model/LifecycleRule.h>
#include <aws/s3/model/LifecycleRuleAndOperator.h>
#include <aws/s3/model/ListObjectsRequest.h>
#include <aws/s3/model/ListObjectsV2Request.h>
#include <aws/s3/model/ListObjectsV2Result.h>
#include <aws/s3/model/PutBucketLifecycleConfigurationRequest.h>
#include <aws/s3/model/PutObjectRequest.h>
#include <aws/s3/model/TaggingDirective.h>
#include <common/logger_useful.h>

#include <boost/algorithm/string/predicate.hpp>
#include <filesystem>
#include <fstream>
#include <ios>
#include <memory>

namespace ProfileEvents
{
extern const Event S3HeadObject;
extern const Event S3GetObject;
extern const Event S3ReadBytes;
extern const Event S3PutObject;
extern const Event S3WriteBytes;
extern const Event S3ListObjects;
extern const Event S3DeleteObject;
extern const Event S3CopyObject;
} // namespace ProfileEvents

namespace
{
Poco::Message::Priority convertLogLevel(Aws::Utils::Logging::LogLevel log_level)
{
    switch (log_level)
    {
    case Aws::Utils::Logging::LogLevel::Off:
    case Aws::Utils::Logging::LogLevel::Fatal:
    case Aws::Utils::Logging::LogLevel::Error:
        return Poco::Message::PRIO_ERROR;
    case Aws::Utils::Logging::LogLevel::Warn:
        return Poco::Message::PRIO_WARNING;
    case Aws::Utils::Logging::LogLevel::Info:
        // treat aws info logging as trace level
        return Poco::Message::PRIO_TRACE;
    case Aws::Utils::Logging::LogLevel::Debug:
        // treat aws debug logging as trace level
        return Poco::Message::PRIO_TRACE;
    case Aws::Utils::Logging::LogLevel::Trace:
        return Poco::Message::PRIO_TRACE;
    default:
        return Poco::Message::PRIO_INFORMATION;
    }
}

class AWSLogger final : public Aws::Utils::Logging::LogSystemInterface
{
public:
    AWSLogger()
        : default_logger(&Poco::Logger::get("AWSClient"))
    {}

    ~AWSLogger() final = default;

    Aws::Utils::Logging::LogLevel GetLogLevel() const final
    {
        return Aws::Utils::Logging::LogLevel::Info;
    }

    void Log(Aws::Utils::Logging::LogLevel log_level, const char * tag, const char * format_str, ...) final // NOLINT
    {
        callLogImpl(log_level, tag, format_str);
    }

    void LogStream(Aws::Utils::Logging::LogLevel log_level, const char * tag, const Aws::OStringStream & message_stream) final
    {
        callLogImpl(log_level, tag, message_stream.str().c_str());
    }

    void callLogImpl(Aws::Utils::Logging::LogLevel log_level, const char * tag, const char * message)
    {
        auto prio = convertLogLevel(log_level);
        LOG_IMPL(default_logger, prio, "tag={} message={}", tag, message);
    }

    void Flush() final {}

private:
    Poco::Logger * default_logger;
};

} // namespace


namespace DB::S3
{

// ensure the `key_root` format like "user0/". No '/' at the beginning and '/' at the end
String normalizedRoot(String ori_root) // a copy for changing
{
    if (startsWith(ori_root, "/") && ori_root.size() != 1)
    {
        ori_root = ori_root.substr(1, ori_root.size());
    }
    if (!endsWith(ori_root, "/"))
    {
        ori_root += "/";
    }
    return ori_root;
}

TiFlashS3Client::TiFlashS3Client(const String & bucket_name_, const String & root_)
    : bucket_name(bucket_name_)
    , key_root(normalizedRoot(root_))
    , log(Logger::get(fmt::format("bucket={} root={}", bucket_name, key_root)))
{
}

TiFlashS3Client::TiFlashS3Client(
    const String & bucket_name_,
    const String & root_,
    const Aws::Auth::AWSCredentials & credentials,
    const Aws::Client::ClientConfiguration & clientConfiguration,
    Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy signPayloads,
    bool useVirtualAddressing)
    : Aws::S3::S3Client(credentials, clientConfiguration, signPayloads, useVirtualAddressing)
    , bucket_name(bucket_name_)
    , key_root(normalizedRoot(root_))
    , log(Logger::get(fmt::format("bucket={} root={}", bucket_name, key_root)))
{
}

TiFlashS3Client::TiFlashS3Client(
    const String & bucket_name_,
    const String & root_,
    std::unique_ptr<Aws::S3::S3Client> && raw_client)
    : Aws::S3::S3Client(std::move(*raw_client))
    , bucket_name(bucket_name_)
    , key_root(normalizedRoot(root_))
    , log(Logger::get(fmt::format("bucket={} root={}", bucket_name, key_root)))
{
}

bool ClientFactory::isEnabled() const
{
    return config.isS3Enabled();
}

void ClientFactory::init(const StorageS3Config & config_, bool mock_s3_)
{
    config = config_;
    config.root = normalizedRoot(config.root);
    Aws::InitAPI(aws_options);
    Aws::Utils::Logging::InitializeAWSLogging(std::make_shared<AWSLogger>());
    if (!mock_s3_)
    {
        shared_tiflash_client = std::make_shared<TiFlashS3Client>(config.bucket, config.root, create());
    }
    else
    {
        shared_tiflash_client = std::make_unique<tests::MockS3Client>(config.bucket, config.root);
    }
}

void ClientFactory::shutdown()
{
    // Reset S3Client before Aws::ShutdownAPI.
    shared_tiflash_client.reset();
    Aws::Utils::Logging::ShutdownAWSLogging();
    Aws::ShutdownAPI(aws_options);
}

ClientFactory::~ClientFactory() = default;

ClientFactory & ClientFactory::instance()
{
    static ClientFactory ret;
    return ret;
}

std::unique_ptr<Aws::S3::S3Client> ClientFactory::create() const
{
    return create(config);
}

std::shared_ptr<TiFlashS3Client> ClientFactory::sharedTiFlashClient() const
{
    // `shared_tiflash_client` is created during initialization and destroyed
    // when process exits which means it is read-only when processing requests.
    // So, it is safe to read `shared_tiflash_client` without acquiring lock.
    return shared_tiflash_client;
}

std::unique_ptr<Aws::S3::S3Client> ClientFactory::create(const StorageS3Config & config_)
{
    Aws::Client::ClientConfiguration cfg;
    cfg.maxConnections = config_.max_connections;
    cfg.requestTimeoutMs = config_.request_timeout_ms;
    cfg.connectTimeoutMs = config_.connection_timeout_ms;
    if (!config_.endpoint.empty())
    {
        cfg.endpointOverride = config_.endpoint;
        auto scheme = parseScheme(config_.endpoint);
        cfg.scheme = scheme;
        cfg.verifySSL = scheme == Aws::Http::Scheme::HTTPS;
    }
    if (config_.access_key_id.empty() && config_.secret_access_key.empty())
    {
        // Request that does not require authentication.
        // Such as the EC2 access permission to the S3 bucket is configured.
        // If the empty access_key_id and secret_access_key are passed to S3Client,
        // an authentication error will be reported.
        return std::make_unique<Aws::S3::S3Client>(cfg);
    }
    else
    {
        Aws::Auth::AWSCredentials cred(config_.access_key_id, config_.secret_access_key);
        return std::make_unique<Aws::S3::S3Client>(
            cred,
            cfg,
            Aws::Client::AWSAuthV4Signer::PayloadSigningPolicy::Never,
            /*useVirtualAddressing*/ true);
    }
}

Aws::Http::Scheme ClientFactory::parseScheme(std::string_view endpoint)
{
    return boost::algorithm::starts_with(endpoint, "https://") ? Aws::Http::Scheme::HTTPS : Aws::Http::Scheme::HTTP;
}

bool isNotFoundError(Aws::S3::S3Errors error)
{
    return error == Aws::S3::S3Errors::RESOURCE_NOT_FOUND || error == Aws::S3::S3Errors::NO_SUCH_KEY;
}

Aws::S3::Model::HeadObjectOutcome headObject(const TiFlashS3Client & client, const String & key)
{
    ProfileEvents::increment(ProfileEvents::S3HeadObject);
    Stopwatch sw;
    SCOPE_EXIT({ GET_METRIC(tiflash_storage_s3_request_seconds, type_head_object).Observe(sw.elapsedSeconds()); });
    Aws::S3::Model::HeadObjectRequest req;
    client.setBucketAndKeyWithRoot(req, key);
    return client.HeadObject(req);
}

bool objectExists(const TiFlashS3Client & client, const String & key)
{
    auto outcome = headObject(client, key);
    if (outcome.IsSuccess())
    {
        return true;
    }
    const auto & error = outcome.GetError();
    if (isNotFoundError(error.GetErrorType()))
    {
        return false;
    }
    throw fromS3Error(outcome.GetError(), "S3 HeadObject failed, bucket={} root={} key={}", client.bucket(), client.root(), key);
}

void uploadEmptyFile(const TiFlashS3Client & client, const String & key, const String & tagging)
{
    Stopwatch sw;
    Aws::S3::Model::PutObjectRequest req;
    client.setBucketAndKeyWithRoot(req, key);
    if (!tagging.empty())
        req.SetTagging(tagging);
    req.SetContentType("binary/octet-stream");
    auto istr = Aws::MakeShared<Aws::StringStream>("EmptyObjectInputStream", "", std::ios_base::in | std::ios_base::binary);
    req.SetBody(istr);
    ProfileEvents::increment(ProfileEvents::S3PutObject);
    auto result = client.PutObject(req);
    if (!result.IsSuccess())
    {
        throw fromS3Error(result.GetError(), "S3 PutEmptyObject failed, bucket={} root={} key={}", client.bucket(), client.root(), key);
    }
    auto elapsed_seconds = sw.elapsedSeconds();
    GET_METRIC(tiflash_storage_s3_request_seconds, type_put_object).Observe(elapsed_seconds);
    LOG_DEBUG(client.log, "uploadEmptyFile key={}, cost={:.2f}s", key, elapsed_seconds);
}

void uploadFile(const TiFlashS3Client & client, const String & local_fname, const String & remote_fname)
{
    Stopwatch sw;
    Aws::S3::Model::PutObjectRequest req;
    client.setBucketAndKeyWithRoot(req, remote_fname);
    req.SetContentType("binary/octet-stream");
    auto istr = Aws::MakeShared<Aws::FStream>("PutObjectInputStream", local_fname, std::ios_base::in | std::ios_base::binary);
    RUNTIME_CHECK_MSG(istr->is_open(), "Open {} fail: {}", local_fname, strerror(errno));
    auto write_bytes = std::filesystem::file_size(local_fname);
    req.SetBody(istr);
    ProfileEvents::increment(ProfileEvents::S3PutObject);
    auto result = client.PutObject(req);
    if (!result.IsSuccess())
    {
        throw fromS3Error(result.GetError(), "S3 PutObject failed, local_fname={} bucket={} root={} key={}", local_fname, client.bucket(), client.root(), remote_fname);
    }
    ProfileEvents::increment(ProfileEvents::S3WriteBytes, write_bytes);
    auto elapsed_seconds = sw.elapsedSeconds();
    GET_METRIC(tiflash_storage_s3_request_seconds, type_put_object).Observe(elapsed_seconds);
    LOG_DEBUG(client.log, "uploadFile local_fname={}, key={}, write_bytes={} cost={:.2f}s", local_fname, remote_fname, write_bytes, elapsed_seconds);
}

void downloadFile(const TiFlashS3Client & client, const String & local_fname, const String & remote_fname)
{
    Stopwatch sw;
    Aws::S3::Model::GetObjectRequest req;
    client.setBucketAndKeyWithRoot(req, remote_fname);
    ProfileEvents::increment(ProfileEvents::S3GetObject);
    auto outcome = client.GetObject(req);
    if (!outcome.IsSuccess())
    {
        throw fromS3Error(outcome.GetError(), "S3 GetObject failed, bucket={} root={} key={}", client.bucket(), client.root(), remote_fname);
    }
    ProfileEvents::increment(ProfileEvents::S3ReadBytes, outcome.GetResult().GetContentLength());
    GET_METRIC(tiflash_storage_s3_request_seconds, type_get_object).Observe(sw.elapsedSeconds());
    Aws::OFStream ostr(local_fname, std::ios_base::out | std::ios_base::binary);
    RUNTIME_CHECK_MSG(ostr.is_open(), "Open {} fail: {}", local_fname, strerror(errno));
    ostr << outcome.GetResult().GetBody().rdbuf();
    RUNTIME_CHECK_MSG(ostr.good(), "Write {} fail: {}", local_fname, strerror(errno));
}

void rewriteObjectWithTagging(const TiFlashS3Client & client, const String & key, const String & tagging)
{
    Stopwatch sw;
    Aws::S3::Model::CopyObjectRequest req;
    // rewrite the object with `key`, adding tagging to the new object
    // The copy_source format is "${source_bucket}/${source_key}"
    auto copy_source = client.bucket() + "/" + (client.root() == "/" ? "" : client.root()) + key;
    client.setBucketAndKeyWithRoot(req, key);
    req.WithCopySource(copy_source) //
        .WithTagging(tagging)
        .WithTaggingDirective(Aws::S3::Model::TaggingDirective::REPLACE);
    ProfileEvents::increment(ProfileEvents::S3CopyObject);
    auto outcome = client.CopyObject(req);
    if (!outcome.IsSuccess())
    {
        throw fromS3Error(outcome.GetError(), "S3 CopyObject failed, bucket={} root={} key={}", client.bucket(), client.root(), key);
    }
    auto elapsed_seconds = sw.elapsedSeconds();
    GET_METRIC(tiflash_storage_s3_request_seconds, type_copy_object).Observe(elapsed_seconds);
    LOG_DEBUG(client.log, "rewrite object key={} cost={:.2f}s", key, elapsed_seconds);
}

void ensureLifecycleRuleExist(const TiFlashS3Client & client, Int32 expire_days)
{
    bool lifecycle_rule_has_been_set = false;
    Aws::Vector<Aws::S3::Model::LifecycleRule> old_rules;
    {
        Aws::S3::Model::GetBucketLifecycleConfigurationRequest req;
        req.SetBucket(client.bucket());
        auto outcome = client.GetBucketLifecycleConfiguration(req);
        if (!outcome.IsSuccess())
        {
            throw fromS3Error(outcome.GetError(), "GetBucketLifecycle fail");
        }

        auto res = outcome.GetResultWithOwnership();
        old_rules = res.GetRules();
        static_assert(TaggingObjectIsDeleted == "tiflash_deleted=true");
        for (const auto & rule : old_rules)
        {
            const auto & filt = rule.GetFilter();
            if (!filt.AndHasBeenSet())
            {
                continue;
            }
            const auto & and_op = filt.GetAnd();
            const auto & tags = and_op.GetTags();
            if (tags.size() != 1 || !and_op.PrefixHasBeenSet() || !and_op.GetPrefix().empty())
            {
                continue;
            }

            const auto & tag = tags[0];
            if (rule.GetStatus() == Aws::S3::Model::ExpirationStatus::Enabled
                && tag.GetKey() == "tiflash_deleted" && tag.GetValue() == "true")
            {
                lifecycle_rule_has_been_set = true;
                break;
            }
        }
    }

    if (lifecycle_rule_has_been_set)
    {
        LOG_INFO(client.log, "The lifecycle rule has been set, n_rules={} filter={}", old_rules.size(), TaggingObjectIsDeleted);
        return;
    }
    else
    {
        UNUSED(expire_days);
        LOG_WARNING(client.log, "The lifecycle rule with filter \"{}\" has not been set, please check the bucket lifecycle configuration", TaggingObjectIsDeleted);
        return;
    }

#if 0
    // Adding rule by AWS SDK is failed, don't know why
    // Reference: https://docs.aws.amazon.com/AmazonS3/latest/userguide/S3OutpostsLifecycleCLIJava.html
    LOG_INFO(client.log, "The lifecycle rule with filter \"{}\" has not been added, n_rules={}", TaggingObjectIsDeleted, old_rules.size());
    static_assert(TaggingObjectIsDeleted == "tiflash_deleted=true");
    std::vector<Aws::S3::Model::Tag> filter_tags{Aws::S3::Model::Tag().WithKey("tiflash_deleted").WithValue("true")};
    Aws::S3::Model::LifecycleRuleFilter filter;
    filter.WithAnd(Aws::S3::Model::LifecycleRuleAndOperator()
                       .WithPrefix("")
                       .WithTags(filter_tags));

    Aws::S3::Model::LifecycleRule rule;
    rule.WithStatus(Aws::S3::Model::ExpirationStatus::Enabled)
        .WithFilter(filter)
        .WithExpiration(Aws::S3::Model::LifecycleExpiration()
                            .WithExpiredObjectDeleteMarker(false)
                            .WithDays(expire_days))
        .WithID("tiflashgc");

    old_rules.emplace_back(rule); // existing rules + new rule
    Aws::S3::Model::BucketLifecycleConfiguration lifecycle_config;
    lifecycle_config
        .WithRules(old_rules);

    Aws::S3::Model::PutBucketLifecycleConfigurationRequest request;
    request.WithBucket(bucket)
        .WithLifecycleConfiguration(lifecycle_config);

    auto outcome = client.PutBucketLifecycleConfiguration(request);
    if (!outcome.IsSuccess())
    {
        throw fromS3Error(outcome.GetError(), "PutBucketLifecycle fail");
    }
    LOG_INFO(client.log, "The lifecycle rule has been added, new_n_rules={} tag={}", old_rules.size(), TaggingObjectIsDeleted);
#endif
}

void listPrefix(
    const TiFlashS3Client & client,
    const String & prefix,
    std::function<PageResult(const Aws::S3::Model::Object & object)> pager)
{
    Stopwatch sw;
    Aws::S3::Model::ListObjectsV2Request req;
    req.WithBucket(client.bucket()).WithPrefix(client.root() + prefix);

    // If the `root == '/'`, then the return result will cut it off
    // else we need to cut the root in the following codes
    bool need_cut = client.root() != "/";
    size_t cut_size = client.root().size();

    bool done = false;
    size_t num_keys = 0;
    while (!done)
    {
        Stopwatch sw_list;
        ProfileEvents::increment(ProfileEvents::S3ListObjects);
        auto outcome = client.ListObjectsV2(req);
        if (!outcome.IsSuccess())
        {
            throw fromS3Error(outcome.GetError(), "S3 ListObjectV2s failed, bucket={} root={} prefix={}", client.bucket(), client.root(), prefix);
        }
        GET_METRIC(tiflash_storage_s3_request_seconds, type_list_objects).Observe(sw_list.elapsedSeconds());

        PageResult page_res{};
        const auto & result = outcome.GetResult();
        auto page_keys = result.GetContents().size();
        num_keys += page_keys;
        for (const auto & object : result.GetContents())
        {
            if (!need_cut)
            {
                page_res = pager(object);
            }
            else
            {
                // Copy the `Object` to cut off the `root` from key, the cost should be acceptable :(
                auto object_without_root = object;
                object_without_root.SetKey(object.GetKey().substr(cut_size, object.GetKey().size()));
                page_res = pager(object_without_root);
            }
            if (!page_res.more)
                break;
        }

        // handle the result size over max size
        done = !result.GetIsTruncated();
        if (!done && page_res.more)
        {
            const auto & next_token = result.GetNextContinuationToken();
            req.SetContinuationToken(next_token);
            LOG_DEBUG(client.log, "listPrefix prefix={}, keys={}, total_keys={}, next_token={}", prefix, page_keys, num_keys, next_token);
        }
    }
    LOG_DEBUG(client.log, "listPrefix prefix={}, total_keys={}, cost={:.2f}s", prefix, num_keys, sw.elapsedSeconds());
}

// Check the docs here for Delimiter && CommonPrefixes when you really need it.
// https://docs.aws.amazon.com/AmazonS3/latest/userguide/using-prefixes.html
void listPrefixWithDelimiter(
    const TiFlashS3Client & client,
    const String & prefix,
    std::string_view delimiter,
    std::function<PageResult(const Aws::S3::Model::CommonPrefix & common_prefix)> pager)
{
    Stopwatch sw;
    Aws::S3::Model::ListObjectsV2Request req;
    req.WithBucket(client.bucket()).WithPrefix(client.root() + prefix);
    if (!delimiter.empty())
    {
        req.SetDelimiter(String(delimiter));
    }

    // If the `root == '/'`, then the return result will cut it off
    // else we need to cut the root in the following codes
    bool need_cut = client.root() != "/";
    size_t cut_size = client.root().size();

    bool done = false;
    size_t num_keys = 0;
    while (!done)
    {
        Stopwatch sw_list;
        ProfileEvents::increment(ProfileEvents::S3ListObjects);
        auto outcome = client.ListObjectsV2(req);
        if (!outcome.IsSuccess())
        {
            throw fromS3Error(outcome.GetError(), "S3 ListObjectV2s failed, bucket={} root={} prefix={} delimiter={}", client.bucket(), client.root(), prefix, delimiter);
        }
        GET_METRIC(tiflash_storage_s3_request_seconds, type_list_objects).Observe(sw_list.elapsedSeconds());

        PageResult page_res{};
        const auto & result = outcome.GetResult();
        auto page_keys = result.GetCommonPrefixes().size();
        num_keys += page_keys;
        for (const auto & prefix : result.GetCommonPrefixes())
        {
            if (!need_cut)
            {
                page_res = pager(prefix);
            }
            else
            {
                // Copy the `CommonPrefix` to cut off the `root`, the cost should be acceptable :(
                auto prefix_without_root = prefix;
                prefix_without_root.SetPrefix(prefix.GetPrefix().substr(cut_size, prefix.GetPrefix().size()));
                page_res = pager(prefix_without_root);
            }
            if (!page_res.more)
                break;
        }

        // handle the result size over max size
        done = !result.GetIsTruncated();
        if (!done && page_res.more)
        {
            const auto & next_token = result.GetNextContinuationToken();
            req.SetContinuationToken(next_token);
            LOG_DEBUG(client.log, "listPrefixWithDelimiter prefix={}, delimiter={}, keys={}, total_keys={}, next_token={}", prefix, delimiter, page_keys, num_keys, next_token);
        }
    }
    LOG_DEBUG(client.log, "listPrefixWithDelimiter prefix={}, delimiter={}, total_keys={}, cost={:.2f}s", prefix, delimiter, num_keys, sw.elapsedSeconds());
}

std::optional<String> anyKeyExistWithPrefix(const TiFlashS3Client & client, const String & prefix)
{
    std::optional<String> key_opt;
    listPrefix(client, prefix, [&key_opt](const Aws::S3::Model::Object & object) {
        key_opt = object.GetKey();
        return PageResult{
            .num_keys = 1,
            .more = false, // do not need more result
        };
    });
    return key_opt;
}

std::unordered_map<String, size_t> listPrefixWithSize(const TiFlashS3Client & client, const String & prefix)
{
    std::unordered_map<String, size_t> keys_with_size;
    listPrefix(client, prefix, [&](const Aws::S3::Model::Object & object) {
        keys_with_size.emplace(object.GetKey().substr(prefix.size()), object.GetSize()); // Cut prefix
        return PageResult{.num_keys = 1, .more = true};
    });

    return keys_with_size;
}

ObjectInfo tryGetObjectInfo(
    const TiFlashS3Client & client,
    const String & key)
{
    auto o = headObject(client, key);
    if (!o.IsSuccess())
    {
        if (const auto & err = o.GetError(); isNotFoundError(err.GetErrorType()))
        {
            return ObjectInfo{.exist = false, .size = 0, .last_modification_time = {}};
        }
        throw fromS3Error(o.GetError(), "Failed to check existence of object, bucket={} key={}", client.bucket(), key);
    }
    // Else the object still exist
    const auto & res = o.GetResult();
    // "DeleteMark" of S3 service, don't know what will lead to this
    RUNTIME_CHECK(!res.GetDeleteMarker(), client.bucket(), key);
    return ObjectInfo{.exist = true, .size = res.GetContentLength(), .last_modification_time = res.GetLastModified()};
}

void deleteObject(const TiFlashS3Client & client, const String & key)
{
    Stopwatch sw;
    Aws::S3::Model::DeleteObjectRequest req;
    client.setBucketAndKeyWithRoot(req, key);
    ProfileEvents::increment(ProfileEvents::S3DeleteObject);
    auto o = client.DeleteObject(req);
    RUNTIME_CHECK(o.IsSuccess(), o.GetError().GetMessage());
    const auto & res = o.GetResult();
    UNUSED(res);
    GET_METRIC(tiflash_storage_s3_request_seconds, type_delete_object).Observe(sw.elapsedSeconds());
}

void rawListPrefix(
    const Aws::S3::S3Client & client,
    const String & bucket,
    const String & prefix,
    std::string_view delimiter,
    std::function<PageResult(const Aws::S3::Model::ListObjectsV2Result & result)> pager)
{
    Stopwatch sw;
    Aws::S3::Model::ListObjectsV2Request req;
    req.WithBucket(bucket).WithPrefix(prefix);
    if (!delimiter.empty())
    {
        req.SetDelimiter(String(delimiter));
    }

    static auto log = Logger::get("S3RawListPrefix");

    bool done = false;
    size_t num_keys = 0;
    while (!done)
    {
        Stopwatch sw_list;
        ProfileEvents::increment(ProfileEvents::S3ListObjects);
        auto outcome = client.ListObjectsV2(req);
        if (!outcome.IsSuccess())
        {
            throw fromS3Error(outcome.GetError(), "S3 ListObjectV2s failed, bucket={} prefix={} delimiter={}", bucket, prefix, delimiter);
        }
        GET_METRIC(tiflash_storage_s3_request_seconds, type_list_objects).Observe(sw_list.elapsedSeconds());

        const auto & result = outcome.GetResult();
        PageResult page_res = pager(result);
        num_keys += page_res.num_keys;

        // handle the result size over max size
        done = !result.GetIsTruncated();
        if (!done && page_res.more)
        {
            const auto & next_token = result.GetNextContinuationToken();
            req.SetContinuationToken(next_token);
            LOG_DEBUG(log, "rawListPrefix bucket={} prefix={} delimiter={} keys={} total_keys={} next_token={}", bucket, prefix, delimiter, page_res.num_keys, num_keys, next_token);
        }
    }
    LOG_DEBUG(log, "rawListPrefix bucket={} prefix={} delimiter={} total_keys={} cost={:.2f}s", bucket, prefix, delimiter, num_keys, sw.elapsedSeconds());
}

} // namespace DB::S3
