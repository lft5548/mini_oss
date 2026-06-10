#include "mini_oss/http.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <stdexcept>
#include <utility>

namespace mini_oss {
namespace {

std::string trim(const std::string& value)
{
    auto begin = value.begin();
    while (begin != value.end() && std::isspace(static_cast<unsigned char>(*begin)) != 0) {
        ++begin;
    }

    auto end = value.end();
    while (end != begin && std::isspace(static_cast<unsigned char>(*(end - 1))) != 0) {
        --end;
    }

    return std::string(begin, end);
}

std::string lowerCopy(std::string value)
{
    std::transform(value.begin(), value.end(), value.begin(), [](unsigned char ch) {
        return static_cast<char>(std::tolower(ch));
    });
    return value;
}

bool parseContentLength(const std::unordered_map<std::string, std::string>& headers,
                        std::size_t& content_length, std::string& error)
{
    content_length = 0;
    const auto it = headers.find("content-length");
    if (it == headers.end()) {
        return true;
    }

    try {
        std::size_t consumed = 0;
        const auto length = std::stoull(it->second, &consumed);
        if (consumed != it->second.size()) {
            error = "invalid Content-Length";
            return false;
        }
        content_length = static_cast<std::size_t>(length);
        return true;
    } catch (const std::exception&) {
        error = "invalid Content-Length";
        return false;
    }
}

} // namespace

HttpResponse::HttpResponse(int status_code, std::string status_text, std::string content_type,
                           std::string body)
    : status_code_(status_code)
    , status_text_(std::move(status_text))
    , content_type_(std::move(content_type))
    , body_(std::move(body))
{
}

HttpResponse HttpResponse::json(int status_code, std::string status_text, std::string body)
{
    return HttpResponse(status_code, std::move(status_text), "application/json", std::move(body));
}

HttpResponse HttpResponse::text(int status_code, std::string status_text, std::string body)
{
    return HttpResponse(status_code, std::move(status_text), "text/plain", std::move(body));
}

HttpResponse HttpResponse::badRequest(const std::string& message)
{
    return text(400, "Bad Request", message + "\n");
}

HttpResponse HttpResponse::notFound()
{
    return text(404, "Not Found", "not found\n");
}

HttpResponse HttpResponse::methodNotAllowed()
{
    return text(405, "Method Not Allowed", "method not allowed\n");
}

std::string HttpResponse::serialize() const
{
    std::ostringstream oss;
    oss << "HTTP/1.1 " << status_code_ << ' ' << status_text_ << "\r\n"
        << "Content-Type: " << content_type_ << "\r\n"
        << "Content-Length: " << body_.size() << "\r\n"
        << "Connection: close\r\n"
        << "\r\n"
        << body_;
    return oss.str();
}

HttpMethod parseHttpMethod(const std::string& method)
{
    if (method == "GET") {
        return HttpMethod::Get;
    }
    if (method == "POST") {
        return HttpMethod::Post;
    }
    if (method == "PUT") {
        return HttpMethod::Put;
    }
    if (method == "DELETE") {
        return HttpMethod::Delete;
    }
    return HttpMethod::Unknown;
}

std::string httpMethodName(HttpMethod method)
{
    switch (method) {
    case HttpMethod::Get:
        return "GET";
    case HttpMethod::Post:
        return "POST";
    case HttpMethod::Put:
        return "PUT";
    case HttpMethod::Delete:
        return "DELETE";
    case HttpMethod::Unknown:
        return "UNKNOWN";
    }
    return "UNKNOWN";
}

HttpParseResult parseHttpRequest(const std::string& raw)
{
    HttpParseResult result;

    const auto header_end = raw.find("\r\n\r\n");
    if (header_end == std::string::npos) {
        return result;
    }

    const std::string header_block = raw.substr(0, header_end);
    std::istringstream stream(header_block);

    std::string request_line;
    if (!std::getline(stream, request_line)) {
        result.complete = true;
        result.error = "missing request line";
        return result;
    }
    if (!request_line.empty() && request_line.back() == '\r') {
        request_line.pop_back();
    }

    std::istringstream request_line_stream(request_line);
    std::string method;
    if (!(request_line_stream >> method >> result.request.path >> result.request.version)) {
        result.complete = true;
        result.error = "invalid request line";
        return result;
    }

    result.request.method = parseHttpMethod(method);
    if (result.request.method == HttpMethod::Unknown || result.request.path.empty()
        || result.request.version.rfind("HTTP/", 0) != 0) {
        result.complete = true;
        result.error = "invalid request line";
        return result;
    }

    std::string line;
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r') {
            line.pop_back();
        }
        if (line.empty()) {
            continue;
        }

        const auto colon = line.find(':');
        if (colon == std::string::npos) {
            result.complete = true;
            result.error = "invalid header line";
            return result;
        }

        const std::string key = lowerCopy(trim(line.substr(0, colon)));
        const std::string value = trim(line.substr(colon + 1));
        if (key.empty()) {
            result.complete = true;
            result.error = "empty header name";
            return result;
        }
        result.request.headers[key] = value;
    }

    std::size_t content_length = 0;
    if (!parseContentLength(result.request.headers, content_length, result.error)) {
        result.complete = true;
        return result;
    }

    const auto body_begin = header_end + 4;
    if (raw.size() < body_begin + content_length) {
        return result;
    }

    result.request.body = raw.substr(body_begin, content_length);
    result.complete = true;
    result.ok = true;
    return result;
}

} // namespace mini_oss
