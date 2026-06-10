#pragma once

#include <string>
#include <unordered_map>

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
    HttpResponse(int status_code, std::string status_text, std::string content_type, std::string body);

    static HttpResponse json(int status_code, std::string status_text, std::string body);
    static HttpResponse text(int status_code, std::string status_text, std::string body);
    static HttpResponse badRequest(const std::string& message);
    static HttpResponse notFound();
    static HttpResponse methodNotAllowed();

    std::string serialize() const;

private:
    int status_code_ = 200;
    std::string status_text_;
    std::string content_type_;
    std::string body_;
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
