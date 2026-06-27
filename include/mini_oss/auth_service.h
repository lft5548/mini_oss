#pragma once

#include "mini_oss/http.h"
#include "mini_oss/metadata_store.h"

#include <optional>
#include <string>

namespace mini_oss {

class AuthService {
public:
    explicit AuthService(MetadataStore& metadata_store);

    bool initializeDefaults(std::string& error);
    HttpResponse login(const HttpRequest& request);
    bool authenticate(HttpRequest& request, std::string& error);

    static bool hasRole(const UserRecord& user, const std::string& role_name);

private:
    static std::string nowIso();
    static std::int64_t nowEpochSeconds();
    static std::string passwordHash(const std::string& password, const std::string& salt);
    static bool verifyPassword(const std::string& password, const UserRecord& user);
    static std::string generateToken();
    static std::optional<std::string> bearerToken(const HttpRequest& request);
    static std::string jsonStringValue(const std::string& body, const std::string& key);
    static std::string headerOrEmpty(const HttpRequest& request, const std::string& key);
    static std::string jsonEscape(const std::string& value);
    static std::string rolesJson(const UserRecord& user);

    MetadataStore& metadata_store_;
};

} // namespace mini_oss
