#include "mini_oss/auth_service.h"

#include <openssl/evp.h>
#include <openssl/rand.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cctype>
#include <iomanip>
#include <sstream>

namespace mini_oss {
namespace {

constexpr int kPasswordIterations = 10000;
constexpr std::size_t kPasswordHashBytes = 32;
constexpr std::int64_t kSessionTtlSeconds = 24 * 60 * 60;

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

} // namespace

AuthService::AuthService(MetadataStore& metadata_store)
    : metadata_store_(metadata_store)
{
}

bool AuthService::initializeDefaults(std::string& error)
{
    if (!metadata_store_.ensureRole("admin", "system administrator", error)) {
        return false;
    }
    if (!metadata_store_.ensureRole("user", "regular object user", error)) {
        return false;
    }

    const std::string now = nowIso();
    if (!metadata_store_.ensureUser("admin", passwordHash("admin123", "mini_oss_admin_salt"),
                                    "mini_oss_admin_salt", now, error)) {
        return false;
    }
    if (!metadata_store_.ensureUser("user", passwordHash("user123", "mini_oss_user_salt"),
                                    "mini_oss_user_salt", now, error)) {
        return false;
    }
    if (!metadata_store_.assignRole("admin", "admin", error)) {
        return false;
    }
    return metadata_store_.assignRole("user", "user", error);
}

HttpResponse AuthService::login(const HttpRequest& request)
{
    std::string username = jsonStringValue(request.body, "username");
    std::string password = jsonStringValue(request.body, "password");
    if (username.empty()) {
        username = headerOrEmpty(request, "x-username");
    }
    if (password.empty()) {
        password = headerOrEmpty(request, "x-password");
    }
    if (username.empty() || password.empty()) {
        return HttpResponse::badRequest("missing username or password");
    }

    std::string error;
    auto user = metadata_store_.findUserByUsername(username, error);
    if (!error.empty()) {
        return HttpResponse::text(500, "Internal Server Error", "cannot load user: " + error + "\n");
    }
    if (!user.has_value() || user->status != 1 || !verifyPassword(password, user.value())) {
        return HttpResponse::unauthorized();
    }

    const std::string token = generateToken();
    const auto created_at = nowEpochSeconds();
    const auto expires_at = created_at + kSessionTtlSeconds;
    if (!metadata_store_.createSession(token, user->id, created_at, expires_at, error)) {
        return HttpResponse::text(500, "Internal Server Error", "cannot create session: " + error + "\n");
    }
    if (!metadata_store_.updateLastLogin(user->id, nowIso(), error)) {
        return HttpResponse::text(500, "Internal Server Error", "cannot update login time: " + error + "\n");
    }

    std::ostringstream body;
    body << "{"
         << "\"token\":\"" << token << "\","
         << "\"token_type\":\"Bearer\","
         << "\"expires_in\":" << kSessionTtlSeconds << ','
         << "\"user\":{"
         << "\"id\":" << user->id << ','
         << "\"username\":\"" << jsonEscape(user->username) << "\","
         << "\"roles\":" << rolesJson(user.value())
         << "}}\n";
    return HttpResponse::json(200, "OK", body.str());
}

bool AuthService::authenticate(HttpRequest& request, std::string& error)
{
    const auto token = bearerToken(request);
    if (!token.has_value()) {
        return false;
    }

    auto user = metadata_store_.findUserByToken(token.value(), nowEpochSeconds(), error);
    if (!error.empty() || !user.has_value() || user->status != 1) {
        return false;
    }

    request.authenticated = true;
    request.user_id = user->id;
    request.username = user->username;
    request.roles = user->roles;
    request.is_admin = hasRole(user.value(), "admin");
    return true;
}

bool AuthService::hasRole(const UserRecord& user, const std::string& role_name)
{
    return std::find(user.roles.begin(), user.roles.end(), role_name) != user.roles.end();
}

std::string AuthService::nowIso()
{
    const auto current = std::chrono::system_clock::now();
    const auto time = std::chrono::system_clock::to_time_t(current);
    std::tm tm {};
    gmtime_r(&time, &tm);

    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%dT%H:%M:%SZ");
    return oss.str();
}

std::int64_t AuthService::nowEpochSeconds()
{
    const auto now = std::chrono::system_clock::now().time_since_epoch();
    return std::chrono::duration_cast<std::chrono::seconds>(now).count();
}

std::string AuthService::passwordHash(const std::string& password, const std::string& salt)
{
    std::array<unsigned char, kPasswordHashBytes> hash {};
    PKCS5_PBKDF2_HMAC(password.c_str(), static_cast<int>(password.size()),
                      reinterpret_cast<const unsigned char*>(salt.data()),
                      static_cast<int>(salt.size()), kPasswordIterations, EVP_sha256(),
                      static_cast<int>(hash.size()), hash.data());

    std::ostringstream oss;
    for (unsigned char byte : hash) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

bool AuthService::verifyPassword(const std::string& password, const UserRecord& user)
{
    return passwordHash(password, user.password_salt) == user.password_hash;
}

std::string AuthService::generateToken()
{
    std::array<unsigned char, 32> bytes {};
    if (RAND_bytes(bytes.data(), static_cast<int>(bytes.size())) != 1) {
        const auto fallback = std::chrono::steady_clock::now().time_since_epoch().count();
        return passwordHash(std::to_string(fallback), "mini_oss_token_fallback");
    }

    std::ostringstream oss;
    for (unsigned char byte : bytes) {
        oss << std::hex << std::setw(2) << std::setfill('0') << static_cast<int>(byte);
    }
    return oss.str();
}

std::optional<std::string> AuthService::bearerToken(const HttpRequest& request)
{
    const auto it = request.headers.find("authorization");
    if (it == request.headers.end()) {
        return std::nullopt;
    }

    const std::string prefix = "Bearer ";
    if (it->second.rfind(prefix, 0) != 0 || it->second.size() <= prefix.size()) {
        return std::nullopt;
    }
    return it->second.substr(prefix.size());
}

std::string AuthService::jsonStringValue(const std::string& body, const std::string& key)
{
    const std::string quoted_key = "\"" + key + "\"";
    const auto key_pos = body.find(quoted_key);
    if (key_pos == std::string::npos) {
        return {};
    }
    const auto colon = body.find(':', key_pos + quoted_key.size());
    if (colon == std::string::npos) {
        return {};
    }
    const auto first_quote = body.find('"', colon + 1);
    if (first_quote == std::string::npos) {
        return {};
    }
    std::string value;
    bool escaping = false;
    for (std::size_t i = first_quote + 1; i < body.size(); ++i) {
        const char ch = body[i];
        if (escaping) {
            value.push_back(ch);
            escaping = false;
            continue;
        }
        if (ch == '\\') {
            escaping = true;
            continue;
        }
        if (ch == '"') {
            return value;
        }
        value.push_back(ch);
    }
    return {};
}

std::string AuthService::headerOrEmpty(const HttpRequest& request, const std::string& key)
{
    const auto it = request.headers.find(lowerCopy(key));
    return it == request.headers.end() ? std::string() : it->second;
}

std::string AuthService::jsonEscape(const std::string& value)
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

std::string AuthService::rolesJson(const UserRecord& user)
{
    std::ostringstream oss;
    oss << '[';
    bool first = true;
    for (const auto& role : user.roles) {
        if (!first) {
            oss << ',';
        }
        first = false;
        oss << "\"" << jsonEscape(role) << "\"";
    }
    oss << ']';
    return oss.str();
}

} // namespace mini_oss
