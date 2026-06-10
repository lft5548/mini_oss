#pragma once

#include "mini_oss/http.h"
#include "mini_oss/metadata_store.h"

#include <filesystem>
#include <string>

namespace mini_oss {

class ObjectStore {
public:
    explicit ObjectStore(std::filesystem::path root_dir);

    HttpResponse createObject(const HttpRequest& request);
    HttpResponse listObjects(const HttpRequest& request);
    HttpResponse getObject(const HttpRequest& request);
    HttpResponse deleteObject(const HttpRequest& request);

private:
    static std::string extractObjectId(const std::string& path);
    static std::string sanitizeFilename(const std::string& filename);
    static std::string sha256Hex(const std::string& data);
    static std::string now();

    std::filesystem::path root_dir_;
    std::filesystem::path object_dir_;
    MetadataStore metadata_store_;
};

} // namespace mini_oss
