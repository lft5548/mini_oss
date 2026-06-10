#pragma once

#include <cstddef>
#include <cstdint>
#include <utility>
#include <string>
#include <unordered_map>
#include <vector>

namespace mini_oss {

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
};

class HttpResponse {
public:
    using Header = std::pair<std::string, std::string>;

    HttpResponse(int status_code, std::string status_text, std::string content_type, std::string body,
                 std::vector<Header> headers = {});

    static HttpResponse json(int status_code, std::string status_text, std::string body);
    static HttpResponse text(int status_code, std::string status_text, std::string body,
                             std::vector<Header> headers = {});
    static HttpResponse badRequest(const std::string& message);
    static HttpResponse notFound();
    static HttpResponse methodNotAllowed();
    static HttpResponse unauthorized();
    static HttpResponse rangeNotSatisfiable(std::uint64_t total_size);

    int statusCode() const;
    std::size_t bodySize() const;
    std::string serialize() const;

private:
    int status_code_ = 200;
    std::string status_text_;
    std::string content_type_;
    std::string body_;
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
HttpParseResult parseHttpRequest(const std::string& raw);

} // namespace mini_oss
