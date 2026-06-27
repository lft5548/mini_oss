#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace mini_oss {

struct HttpFileBody {
    HttpFileBody(int fd, std::filesystem::path path, std::uint64_t offset, std::uint64_t length);
    ~HttpFileBody();

    HttpFileBody(const HttpFileBody&) = delete;
    HttpFileBody& operator=(const HttpFileBody&) = delete;

    int fd = -1;
    std::filesystem::path path;
    std::uint64_t offset = 0;
    std::uint64_t length = 0;
};

enum class HttpMethod {
    Get,
    Post,
    Put,
    Delete,
    Unknown,
};

struct HttpRequest {
    HttpMethod method = HttpMethod::Unknown;
    std::string path;
    std::string version;
    std::unordered_map<std::string, std::string> headers;
    std::string body;
    std::filesystem::path body_file_path;
    std::uint64_t body_size = 0;
    bool body_in_file = false;
    bool temporary_body_file = false;
    std::string request_id;
    bool authenticated = false;
    bool is_admin = false;
    bool legacy_token = false;
    int user_id = 0;
    std::string username;
    std::vector<std::string> roles;
};

class HttpResponse {
public:
    using Header = std::pair<std::string, std::string>;

    HttpResponse(int status_code, std::string status_text, std::string content_type, std::string body,
                 std::vector<Header> headers = {});

    static HttpResponse json(int status_code, std::string status_text, std::string body);
    static HttpResponse file(int status_code, std::string status_text, std::string content_type,
                             int fd, std::filesystem::path path, std::uint64_t offset,
                             std::uint64_t length, std::vector<Header> headers = {});
    static HttpResponse text(int status_code, std::string status_text, std::string body,
                             std::vector<Header> headers = {});
    static HttpResponse badRequest(const std::string& message);
    static HttpResponse notFound();
    static HttpResponse methodNotAllowed();
    static HttpResponse unauthorized();
    static HttpResponse forbidden();
    static HttpResponse rangeNotSatisfiable(std::uint64_t total_size);

    int statusCode() const;
    std::size_t bodySize() const;
    bool bodyInFile() const;
    std::shared_ptr<HttpFileBody> fileBody() const;
    std::string serializeHeaders() const;
    std::string serialize() const;

private:
    int status_code_ = 200;
    std::string status_text_;
    std::string content_type_;
    std::string body_;
    std::shared_ptr<HttpFileBody> file_body_;
    std::vector<Header> headers_;
};

struct HttpParseResult {
    bool complete = false;
    bool ok = false;
    std::string error;
    HttpRequest request;
};

HttpMethod parseHttpMethod(const std::string& method);
std::string httpMethodName(HttpMethod method);
HttpParseResult parseHttpRequestHead(const std::string& raw);
HttpParseResult parseHttpRequest(const std::string& raw);

} // namespace mini_oss
