#include "mini_oss/object_store.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <mutex>
#include <fcntl.h>
#include <openssl/evp.h>
#include <openssl/sha.h>
#include <optional>
#include <shared_mutex>
#include <sstream>
#include <unistd.h>
#include <utility>

namespace mini_oss {
namespace {

constexpr const char* kObjectsPrefix = "/objects/";

std::string headerOrDefault(const HttpRequest& request, const std::string& key,
                            const std::string& default_value)
{
    const auto it = request.headers.find(key);
    return it == request.headers.end() || it->second.empty() ? default_value : it->second;
}

std::string jsonEscape(const std::string& value)
{
    std::ostringstream oss;
    for (char ch : value) {
        switch (ch) {
        case '\\':
            oss << "\\\\";
            break;
        case '"':
            oss << "\\\"";
            break;
        case '\n':
            oss << "\\n";
            break;
        case '\r':
            oss << "\\r";
            break;
        case '\t':
            oss << "\\t";
            break;
        default:
            oss << ch;
            break;
        }
    }
    return oss.str();
}

std::string lowerCopy(std::string value)
{
    for (char& ch : value) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return value;
}

std::string objectInfoJson(const ObjectInfo& info, const std::string& extra_fields = {})
{
    std::ostringstream body;
    body << "{"
         << "\"id\":\"" << jsonEscape(info.id) << "\","
         << "\"filename\":\"" << jsonEscape(info.filename) << "\","
         << "\"size\":" << info.size << ","
         << "\"sha256\":\"" << info.sha256 << "\","
         << "\"created_at\":\"" << info.created_at << "\","
         << "\"owner_user_id\":" << info.owner_user_id << ','
         << "\"upload_count\":" << info.upload_count << ','
         << "\"download_count\":" << info.download_count;
    if (!extra_fields.empty()) {
        body << ',' << extra_fields;
    }
    body
         << "}";
    return body.str();
}

int openObjectFile(const std::filesystem::path& path)
{
    return ::open(path.string().c_str(), O_RDONLY | O_CLOEXEC);
}

std::string pathWithoutQuery(const std::string& path)
{
    const auto query_pos = path.find('?');
    return query_pos == std::string::npos ? path : path.substr(0, query_pos);
}

bool canAccessObject(const HttpRequest& request, const ObjectInfo& info)
{
    if (!request.authenticated || request.is_admin || request.legacy_token || request.user_id == 0) {
        return true;
    }
    return info.owner_user_id == request.user_id;
}

} // namespace

ObjectStore::ObjectStore(std::filesystem::path root_dir, RedisConfig redis_config, Metrics* metrics)
    : root_dir_(std::move(root_dir))
    , object_dir_(root_dir_ / "objects")
    , temp_upload_dir_(root_dir_ / "tmp_uploads")
    , metadata_store_(root_dir_ / "metadata.db")
    , metadata_cache_(std::move(redis_config), metrics)
{
    std::filesystem::create_directories(object_dir_);
    std::filesystem::create_directories(temp_upload_dir_);
}

HttpResponse ObjectStore::createObject(const HttpRequest& request)
{
    if (request.body_in_file) {
        return createObjectFromFileBody(request);
    }

    std::unique_lock<std::shared_mutex> lock(mutex_);

    if (request.body.empty()) {
        return HttpResponse::badRequest("empty object body");
    }

    const std::uint64_t object_size = request.body.size();
    const std::string object_sha256 = sha256Hex(request.body);
    std::string error;
    const auto existing = findObjectBySha256Cached(object_sha256, object_size, error);
    if (!error.empty()) {
        return HttpResponse::text(500, "Internal Server Error",
                                  "cannot query object metadata: " + error + "\n");
    }
    if (existing.has_value()) {
        if (!std::filesystem::exists(existing->path)) {
            return HttpResponse::text(500, "Internal Server Error",
                                      "deduplicated object file is missing\n");
        }

        const std::string filename = sanitizeFilename(
            headerOrDefault(request, "x-filename", existing->filename));
        return createMetadataAlias(existing.value(), filename, false, request.user_id);
    }

    const auto id = metadata_store_.nextObjectId(error);
    if (!id.has_value()) {
        return HttpResponse::text(500, "Internal Server Error", "cannot allocate object id: " + error + "\n");
    }

    const std::string filename = sanitizeFilename(headerOrDefault(request, "x-filename", "object_" + id.value()));
    const std::filesystem::path path = object_dir_ / id.value();

    std::ofstream out(path, std::ios::binary);
    if (!out) {
        return HttpResponse::text(500, "Internal Server Error", "cannot open object file\n");
    }
    out.write(request.body.data(), static_cast<std::streamsize>(request.body.size()));
    if (!out) {
        return HttpResponse::text(500, "Internal Server Error", "cannot write object file\n");
    }

    ObjectInfo info;
    info.id = id.value();
    info.filename = filename;
    info.path = path;
    info.size = object_size;
    info.sha256 = object_sha256;
    info.created_at = now();
    info.owner_user_id = request.user_id;
    info.upload_count = 1;
    info.download_count = 0;

    if (!insertObjectMetadata(info, error)) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return HttpResponse::text(500, "Internal Server Error", "cannot save object metadata: " + error + "\n");
    }

    return HttpResponse::json(201, "Created", objectInfoJson(info) + "\n");
}

HttpResponse ObjectStore::createInstantObject(const HttpRequest& request)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    const std::string sha256 = lowerCopy(headerOrDefault(request, "x-object-sha256", ""));
    if (!isValidSha256(sha256)) {
        return HttpResponse::badRequest("missing or invalid X-Object-Sha256");
    }

    const auto size = parseSize(headerOrDefault(request, "x-object-size", ""));
    if (!size.has_value()) {
        return HttpResponse::badRequest("missing or invalid X-Object-Size");
    }

    std::string error;
    const auto existing = findObjectBySha256Cached(sha256, size.value(), error);
    if (!error.empty()) {
        return HttpResponse::text(500, "Internal Server Error",
                                  "cannot query object metadata: " + error + "\n");
    }
    if (!existing.has_value()) {
        return HttpResponse::notFound();
    }
    if (!std::filesystem::exists(existing->path)) {
        return HttpResponse::text(500, "Internal Server Error",
                                  "deduplicated object file is missing\n");
    }

    const std::string filename = sanitizeFilename(
        headerOrDefault(request, "x-filename", existing->filename));
    return createMetadataAlias(existing.value(), filename, true, request.user_id);
}

HttpResponse ObjectStore::listObjects(const HttpRequest& request)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    std::string error;
    const auto objects = request.authenticated && !request.is_admin && !request.legacy_token
        ? metadata_store_.listObjectsForOwner(request.user_id, error)
        : metadata_store_.listObjects(error);
    if (!error.empty()) {
        return HttpResponse::text(500, "Internal Server Error", "cannot list object metadata: " + error + "\n");
    }

    std::ostringstream body;
    body << "{\"objects\":[";
    bool first = true;
    for (const auto& info : objects) {
        if (!first) {
            body << ',';
        }
        first = false;
        body << objectInfoJson(info);
    }
    body << "]}\n";
    return HttpResponse::json(200, "OK", body.str());
}

HttpResponse ObjectStore::getObject(const HttpRequest& request)
{
    std::shared_lock<std::shared_mutex> lock(mutex_);

    const std::string id = extractObjectId(request.path);
    if (id.empty()) {
        return HttpResponse::badRequest("missing object id");
    }

    std::string error;
    const auto info = getObjectMetadata(id, error);
    if (!error.empty()) {
        return error == "invalid object id" ? HttpResponse::badRequest(error)
                                            : HttpResponse::text(500, "Internal Server Error",
                                                                 "cannot get object metadata: " + error + "\n");
    }
    if (!info.has_value()) {
        return HttpResponse::notFound();
    }
    if (!canAccessObject(request, info.value())) {
        return HttpResponse::forbidden();
    }

    if (!metadata_store_.incrementObjectDownloadCount(info->id, error)) {
        return HttpResponse::text(500, "Internal Server Error",
                                  "cannot update download count: " + error + "\n");
    }

    const int fd = openObjectFile(info->path);
    if (fd < 0) {
        return HttpResponse::text(500, "Internal Server Error", "cannot open object file\n");
    }

    const auto range_header = request.headers.find("range");
    if (range_header != request.headers.end()) {
        const auto range = parseRangeHeader(range_header->second, info->size);
        if (!range.has_value()) {
            ::close(fd);
            return HttpResponse::rangeNotSatisfiable(info->size);
        }

        const auto length = range->end - range->start + 1;
        std::ostringstream content_range;
        content_range << "bytes " << range->start << '-' << range->end << '/' << info->size;
        return HttpResponse::file(206, "Partial Content", "application/octet-stream", fd,
                                  info->path, range->start, length,
                                  {{"Content-Range", content_range.str()}, {"Accept-Ranges", "bytes"}});
    }

    return HttpResponse::file(200, "OK", "application/octet-stream", fd, info->path, 0, info->size,
                              {{"Accept-Ranges", "bytes"}});
}

HttpResponse ObjectStore::deleteObject(const HttpRequest& request)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    const std::string id = extractObjectId(request.path);
    if (id.empty()) {
        return HttpResponse::badRequest("missing object id");
    }

    std::string error;
    const auto info = getObjectMetadata(id, error);
    if (!error.empty()) {
        return error == "invalid object id" ? HttpResponse::badRequest(error)
                                            : HttpResponse::text(500, "Internal Server Error",
                                                                 "cannot get object metadata: " + error + "\n");
    }
    if (!info.has_value()) {
        return HttpResponse::notFound();
    }
    if (!canAccessObject(request, info.value())) {
        return HttpResponse::forbidden();
    }

    if (!deleteObjectMetadata(info.value(), error)) {
        return error == "invalid object id" ? HttpResponse::badRequest(error)
                                            : HttpResponse::text(500, "Internal Server Error",
                                                                 "cannot delete object metadata: " + error + "\n");
    }

    const auto remaining_refs = metadata_store_.countObjectsByPath(info->path, error);
    if (!error.empty()) {
        return HttpResponse::text(500, "Internal Server Error",
                                  "cannot count object references: " + error + "\n");
    }

    bool removed_file = false;
    if (remaining_refs == 0) {
        std::error_code ec;
        std::filesystem::remove(info->path, ec);
        if (ec) {
            return HttpResponse::text(500, "Internal Server Error", "cannot delete object file\n");
        }
        removed_file = true;
    }
    return HttpResponse::json(200, "OK",
                              std::string("{\"deleted\":true,\"removed_file\":")
                                  + (removed_file ? "true" : "false") + "}\n");
}

std::string ObjectStore::extractObjectId(const std::string& path)
{
    const std::string clean_path = pathWithoutQuery(path);
    if (clean_path.rfind(kObjectsPrefix, 0) != 0 || clean_path.size() <= std::string(kObjectsPrefix).size()) {
        return {};
    }
    const std::string id = clean_path.substr(std::string(kObjectsPrefix).size());
    return id.find('/') == std::string::npos ? id : std::string();
}

std::string ObjectStore::sanitizeFilename(const std::string& filename)
{
    std::filesystem::path path(filename);
    std::string clean = path.filename().string();
    if (clean.empty() || clean == "." || clean == "..") {
        clean = "object";
    }
    for (char& ch : clean) {
        if (ch == '/' || ch == '\\' || ch == '\0') {
            ch = '_';
        }
    }
    return clean;
}

bool ObjectStore::isValidSha256(const std::string& sha256)
{
    if (sha256.size() != 64) {
        return false;
    }
    for (unsigned char ch : sha256) {
        if (std::isxdigit(ch) == 0) {
            return false;
        }
    }
    return true;
}

std::optional<std::uint64_t> ObjectStore::parseSize(const std::string& value)
{
    if (value.empty()) {
        return std::nullopt;
    }

    char* end = nullptr;
    errno = 0;
    const auto parsed = std::strtoull(value.c_str(), &end, 10);
    if (errno != 0 || end == value.c_str() || *end != '\0') {
        return std::nullopt;
    }
    return static_cast<std::uint64_t>(parsed);
}

std::optional<ObjectStore::ByteRange> ObjectStore::parseRangeHeader(const std::string& value,
                                                                    std::uint64_t total_size)
{
    if (total_size == 0 || value.rfind("bytes=", 0) != 0) {
        return std::nullopt;
    }

    const std::string spec = value.substr(6);
    if (spec.empty() || spec.find(',') != std::string::npos) {
        return std::nullopt;
    }

    const auto dash = spec.find('-');
    if (dash == std::string::npos) {
        return std::nullopt;
    }

    const std::string start_text = spec.substr(0, dash);
    const std::string end_text = spec.substr(dash + 1);
    if (start_text.empty() && end_text.empty()) {
        return std::nullopt;
    }

    ByteRange range;
    if (start_text.empty()) {
        const auto suffix_length = parseSize(end_text);
        if (!suffix_length.has_value() || suffix_length.value() == 0) {
            return std::nullopt;
        }
        if (suffix_length.value() >= total_size) {
            range.start = 0;
        } else {
            range.start = total_size - suffix_length.value();
        }
        range.end = total_size - 1;
        return range;
    }

    const auto start = parseSize(start_text);
    if (!start.has_value() || start.value() >= total_size) {
        return std::nullopt;
    }

    range.start = start.value();
    if (end_text.empty()) {
        range.end = total_size - 1;
    } else {
        const auto end = parseSize(end_text);
        if (!end.has_value() || end.value() < range.start) {
            return std::nullopt;
        }
        range.end = std::min(end.value(), total_size - 1);
    }
    return range;
}

std::string ObjectStore::sha256Hex(const std::string& data)
{
    unsigned char hash[SHA256_DIGEST_LENGTH];
    SHA256(reinterpret_cast<const unsigned char*>(data.data()), data.size(), hash);

    std::ostringstream oss;
    for (unsigned char byte : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

std::optional<std::string> ObjectStore::sha256File(const std::filesystem::path& path,
                                                   std::uint64_t& file_size,
                                                   std::string& error)
{
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        error = "cannot open uploaded object file";
        return std::nullopt;
    }

    EVP_MD_CTX* context = EVP_MD_CTX_new();
    if (context == nullptr) {
        error = "cannot allocate sha256 context";
        return std::nullopt;
    }

    if (EVP_DigestInit_ex(context, EVP_sha256(), nullptr) != 1) {
        EVP_MD_CTX_free(context);
        error = "cannot initialize sha256 context";
        return std::nullopt;
    }

    file_size = 0;
    std::array<char, 64 * 1024> buffer {};
    while (in) {
        in.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto bytes = in.gcount();
        if (bytes > 0) {
            if (EVP_DigestUpdate(context, buffer.data(), static_cast<std::size_t>(bytes)) != 1) {
                EVP_MD_CTX_free(context);
                error = "cannot update sha256 digest";
                return std::nullopt;
            }
            file_size += static_cast<std::uint64_t>(bytes);
        }
    }
    if (!in.eof()) {
        EVP_MD_CTX_free(context);
        error = "cannot read uploaded object file";
        return std::nullopt;
    }

    unsigned char hash[EVP_MAX_MD_SIZE];
    unsigned int hash_length = 0;
    if (EVP_DigestFinal_ex(context, hash, &hash_length) != 1) {
        EVP_MD_CTX_free(context);
        error = "cannot finalize sha256 digest";
        return std::nullopt;
    }
    EVP_MD_CTX_free(context);

    std::ostringstream oss;
    for (unsigned int i = 0; i < hash_length; ++i) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(hash[i]);
    }
    return oss.str();
}

HttpResponse ObjectStore::createObjectFromFileBody(const HttpRequest& request)
{
    std::unique_lock<std::shared_mutex> lock(mutex_);

    bool temp_file_moved = false;
    const auto cleanup_temp = [&] {
        if (request.temporary_body_file && !temp_file_moved && !request.body_file_path.empty()) {
            std::error_code ignored;
            std::filesystem::remove(request.body_file_path, ignored);
        }
    };

    if (request.body_file_path.empty()) {
        cleanup_temp();
        return HttpResponse::badRequest("missing upload body file");
    }

    std::error_code ec;
    if (!std::filesystem::exists(request.body_file_path, ec)) {
        cleanup_temp();
        return HttpResponse::text(500, "Internal Server Error", "uploaded body file is missing\n");
    }

    std::uint64_t object_size = 0;
    std::string hash_error;
    const auto object_sha256 = sha256File(request.body_file_path, object_size, hash_error);
    if (!object_sha256.has_value()) {
        cleanup_temp();
        return HttpResponse::text(500, "Internal Server Error", hash_error + "\n");
    }
    if (object_size == 0) {
        cleanup_temp();
        return HttpResponse::badRequest("empty object body");
    }
    if (request.body_size != 0 && object_size != request.body_size) {
        cleanup_temp();
        return HttpResponse::badRequest("uploaded body size mismatch");
    }

    std::string error;
    const auto existing = findObjectBySha256Cached(object_sha256.value(), object_size, error);
    if (!error.empty()) {
        cleanup_temp();
        return HttpResponse::text(500, "Internal Server Error",
                                  "cannot query object metadata: " + error + "\n");
    }
    if (existing.has_value()) {
        if (!std::filesystem::exists(existing->path)) {
            cleanup_temp();
            return HttpResponse::text(500, "Internal Server Error",
                                      "deduplicated object file is missing\n");
        }

        const std::string filename = sanitizeFilename(
            headerOrDefault(request, "x-filename", existing->filename));
        cleanup_temp();
        return createMetadataAlias(existing.value(), filename, false, request.user_id);
    }

    const auto id = metadata_store_.nextObjectId(error);
    if (!id.has_value()) {
        cleanup_temp();
        return HttpResponse::text(500, "Internal Server Error", "cannot allocate object id: " + error + "\n");
    }

    const std::string filename = sanitizeFilename(headerOrDefault(request, "x-filename", "object_" + id.value()));
    const std::filesystem::path path = object_dir_ / id.value();

    std::filesystem::rename(request.body_file_path, path, ec);
    if (ec) {
        ec.clear();
        std::filesystem::copy_file(request.body_file_path, path,
                                   std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            cleanup_temp();
            return HttpResponse::text(500, "Internal Server Error", "cannot move uploaded object file\n");
        }
        std::filesystem::remove(request.body_file_path, ec);
    }
    temp_file_moved = true;

    ObjectInfo info;
    info.id = id.value();
    info.filename = filename;
    info.path = path;
    info.size = object_size;
    info.sha256 = object_sha256.value();
    info.created_at = now();
    info.owner_user_id = request.user_id;
    info.upload_count = 1;
    info.download_count = 0;

    if (!insertObjectMetadata(info, error)) {
        std::error_code remove_error;
        std::filesystem::remove(path, remove_error);
        return HttpResponse::text(500, "Internal Server Error", "cannot save object metadata: " + error + "\n");
    }

    return HttpResponse::json(201, "Created", objectInfoJson(info) + "\n");
}

HttpResponse ObjectStore::createMetadataAlias(const ObjectInfo& source, const std::string& filename,
                                              bool instant_upload, int owner_user_id)
{
    std::string error;
    const auto id = metadata_store_.nextObjectId(error);
    if (!id.has_value()) {
        return HttpResponse::text(500, "Internal Server Error", "cannot allocate object id: " + error + "\n");
    }

    ObjectInfo info;
    info.id = id.value();
    info.filename = filename;
    info.path = source.path;
    info.size = source.size;
    info.sha256 = source.sha256;
    info.created_at = now();
    info.owner_user_id = owner_user_id;
    info.upload_count = 1;
    info.download_count = 0;

    if (!insertObjectMetadata(info, error)) {
        return HttpResponse::text(500, "Internal Server Error",
                                  "cannot save object metadata: " + error + "\n");
    }

    std::ostringstream extra;
    extra << "\"deduplicated\":true,"
          << "\"instant_upload\":" << (instant_upload ? "true" : "false") << ','
          << "\"source_id\":\"" << jsonEscape(source.id) << "\"";
    return HttpResponse::json(201, "Created", objectInfoJson(info, extra.str()) + "\n");
}

MetadataStore& ObjectStore::metadataStore()
{
    return metadata_store_;
}

const MetadataStore& ObjectStore::metadataStore() const
{
    return metadata_store_;
}

std::optional<ObjectInfo> ObjectStore::getObjectMetadata(const std::string& id, std::string& error)
{
    const auto cached = metadata_cache_.getObject(id);
    if (cached.has_value()) {
        return cached;
    }

    auto info = metadata_store_.getObject(id, error);
    if (error.empty() && info.has_value()) {
        metadata_cache_.putObject(info.value());
        metadata_cache_.putShaIndex(info->sha256, info->size, info->id);
    }
    return info;
}

std::optional<ObjectInfo> ObjectStore::findObjectBySha256Cached(const std::string& sha256,
                                                               std::uint64_t size,
                                                               std::string& error)
{
    const auto cached_id = metadata_cache_.getShaIndex(sha256, size);
    if (cached_id.has_value()) {
        auto cached_object = metadata_cache_.getObject(cached_id.value());
        if (cached_object.has_value() && cached_object->sha256 == sha256
            && cached_object->size == size) {
            return cached_object;
        }

        auto db_object = metadata_store_.getObject(cached_id.value(), error);
        if (!error.empty()) {
            return std::nullopt;
        }
        if (db_object.has_value() && db_object->sha256 == sha256 && db_object->size == size) {
            metadata_cache_.putObject(db_object.value());
            return db_object;
        }
        metadata_cache_.deleteShaIndex(sha256, size);
    }

    auto info = metadata_store_.findObjectBySha256(sha256, size, error);
    if (error.empty() && info.has_value()) {
        metadata_cache_.putObject(info.value());
        metadata_cache_.putShaIndex(sha256, size, info->id);
    }
    return info;
}

bool ObjectStore::insertObjectMetadata(const ObjectInfo& info, std::string& error)
{
    if (!metadata_store_.insertObject(info, error)) {
        return false;
    }
    metadata_cache_.putObject(info);
    metadata_cache_.putShaIndex(info.sha256, info.size, info.id);
    return true;
}

bool ObjectStore::deleteObjectMetadata(const ObjectInfo& info, std::string& error)
{
    if (!metadata_store_.deleteObject(info.id, error)) {
        return false;
    }
    metadata_cache_.deleteObject(info);
    return true;
}

std::string ObjectStore::now()
{
    const auto current = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(current);
    std::tm tm {};
    gmtime_r(&time, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

} // namespace mini_oss
