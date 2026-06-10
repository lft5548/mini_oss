#include "mini_oss/object_store.h"

#include <chrono>
#include <fstream>
#include <iomanip>
#include <openssl/sha.h>
#include <sstream>
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

std::string objectInfoJson(const ObjectInfo& info)
{
    std::ostringstream body;
    body << "{"
         << "\"id\":\"" << jsonEscape(info.id) << "\","
         << "\"filename\":\"" << jsonEscape(info.filename) << "\","
         << "\"size\":" << info.size << ","
         << "\"sha256\":\"" << info.sha256 << "\","
         << "\"created_at\":\"" << info.created_at << "\""
         << "}";
    return body.str();
}

} // namespace

ObjectStore::ObjectStore(std::filesystem::path root_dir)
    : root_dir_(std::move(root_dir))
    , object_dir_(root_dir_ / "objects")
    , metadata_store_(root_dir_ / "metadata.db")
{
    std::filesystem::create_directories(object_dir_);
}

HttpResponse ObjectStore::createObject(const HttpRequest& request)
{
    if (request.body.empty()) {
        return HttpResponse::badRequest("empty object body");
    }

    std::string error;
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
    info.size = request.body.size();
    info.sha256 = sha256Hex(request.body);
    info.created_at = now();

    if (!metadata_store_.insertObject(info, error)) {
        std::error_code ec;
        std::filesystem::remove(path, ec);
        return HttpResponse::text(500, "Internal Server Error", "cannot save object metadata: " + error + "\n");
    }

    return HttpResponse::json(201, "Created", objectInfoJson(info) + "\n");
}

HttpResponse ObjectStore::listObjects(const HttpRequest&)
{
    std::string error;
    const auto objects = metadata_store_.listObjects(error);
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
    const std::string id = extractObjectId(request.path);
    if (id.empty()) {
        return HttpResponse::badRequest("missing object id");
    }

    std::string error;
    const auto info = metadata_store_.getObject(id, error);
    if (!error.empty()) {
        return error == "invalid object id" ? HttpResponse::badRequest(error)
                                            : HttpResponse::text(500, "Internal Server Error",
                                                                 "cannot get object metadata: " + error + "\n");
    }
    if (!info.has_value()) {
        return HttpResponse::notFound();
    }

    std::ifstream in(info->path, std::ios::binary);
    if (!in) {
        return HttpResponse::text(500, "Internal Server Error", "cannot open object file\n");
    }
    std::ostringstream content;
    content << in.rdbuf();
    return HttpResponse(200, "OK", "application/octet-stream", content.str());
}

HttpResponse ObjectStore::deleteObject(const HttpRequest& request)
{
    const std::string id = extractObjectId(request.path);
    if (id.empty()) {
        return HttpResponse::badRequest("missing object id");
    }

    std::string error;
    const auto info = metadata_store_.getObject(id, error);
    if (!error.empty()) {
        return error == "invalid object id" ? HttpResponse::badRequest(error)
                                            : HttpResponse::text(500, "Internal Server Error",
                                                                 "cannot get object metadata: " + error + "\n");
    }
    if (!info.has_value()) {
        return HttpResponse::notFound();
    }

    if (!metadata_store_.deleteObject(id, error)) {
        return error == "invalid object id" ? HttpResponse::badRequest(error)
                                            : HttpResponse::text(500, "Internal Server Error",
                                                                 "cannot delete object metadata: " + error + "\n");
    }

    std::error_code ec;
    std::filesystem::remove(info->path, ec);
    if (ec) {
        return HttpResponse::text(500, "Internal Server Error", "cannot delete object file\n");
    }
    return HttpResponse::json(200, "OK", "{\"deleted\":true}\n");
}

std::string ObjectStore::extractObjectId(const std::string& path)
{
    if (path.rfind(kObjectsPrefix, 0) != 0 || path.size() <= std::string(kObjectsPrefix).size()) {
        return {};
    }
    const std::string id = path.substr(std::string(kObjectsPrefix).size());
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
