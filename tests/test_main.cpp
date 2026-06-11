#include "mini_oss/config.h"
#include "mini_oss/http.h"
#include "mini_oss/object_store.h"
#include "mini_oss/redis_metadata_cache.h"
#include "mini_oss/router.h"

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unistd.h>

namespace {

using mini_oss::HttpMethod;
using mini_oss::HttpRequest;
using mini_oss::HttpResponse;
using mini_oss::ObjectStore;
using mini_oss::Router;

void expect(bool condition, std::string_view message)
{
    if (!condition) {
        throw std::runtime_error(std::string(message));
    }
}

void expectContains(const std::string& text, std::string_view needle, std::string_view message)
{
    if (text.find(needle) == std::string::npos) {
        throw std::runtime_error(std::string(message));
    }
}

std::string responseBody(const HttpResponse& response)
{
    const std::string serialized = response.serialize();
    const auto body_pos = serialized.find("\r\n\r\n");
    if (body_pos == std::string::npos) {
        return {};
    }
    return serialized.substr(body_pos + 4);
}

std::filesystem::path makeTempDir(const std::string& name)
{
    auto path = std::filesystem::temp_directory_path()
        / ("mini_oss_" + name + "_" + std::to_string(::getpid()));
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

void testHttpParsing()
{
    const std::string raw =
        "POST /objects HTTP/1.1\r\n"
        "Host: 127.0.0.1\r\n"
        "X-Filename: demo.txt\r\n"
        "Content-Length: 5\r\n"
        "\r\n"
        "helloextra";

    const auto parsed = mini_oss::parseHttpRequest(raw);
    expect(parsed.complete, "HTTP request should be complete");
    expect(parsed.ok, "HTTP request should parse successfully");
    expect(parsed.request.method == HttpMethod::Post, "method should be POST");
    expect(parsed.request.path == "/objects", "path should match");
    expect(parsed.request.version == "HTTP/1.1", "version should match");
    expect(parsed.request.headers.at("host") == "127.0.0.1", "header keys should be normalized");
    expect(parsed.request.headers.at("x-filename") == "demo.txt", "custom header should parse");
    expect(parsed.request.body == "hello", "body should respect Content-Length");
    expect(parsed.request.body_size == 5, "body_size should match parsed body");

    const auto incomplete = mini_oss::parseHttpRequest(
        "POST /objects HTTP/1.1\r\nContent-Length: 8\r\n\r\nhello");
    expect(!incomplete.complete, "incomplete body should not be complete");
    expect(!incomplete.ok, "incomplete body should not be ok");

    const auto bad = mini_oss::parseHttpRequest("BAD_REQUEST\r\n\r\n");
    expect(bad.complete, "bad request line should still be a complete parse attempt");
    expect(!bad.ok, "bad request line should fail");
    expect(bad.error == "invalid request line", "bad request should report invalid request line");
}

void testHttpResponse()
{
    const auto response = HttpResponse::rangeNotSatisfiable(14).serialize();
    expectContains(response, "HTTP/1.1 416 Range Not Satisfiable", "416 status should serialize");
    expectContains(response, "Content-Range: bytes */14", "416 should include Content-Range");
    expectContains(response, "Accept-Ranges: bytes", "416 should include Accept-Ranges");
}

void testRouter()
{
    Router router;
    router.addRoute(HttpMethod::Get, "/health", [](const HttpRequest&) {
        return HttpResponse::json(200, "OK", "{\"status\":\"ok\"}\n");
    });
    router.addPrefixRoute(HttpMethod::Get, "/objects/", [](const HttpRequest& request) {
        return HttpResponse::text(200, "OK", "object:" + request.path + "\n");
    });

    HttpRequest health;
    health.method = HttpMethod::Get;
    health.path = "/health";
    expect(router.route(health).statusCode() == 200, "GET /health should route");

    HttpRequest method_not_allowed = health;
    method_not_allowed.method = HttpMethod::Post;
    expect(router.route(method_not_allowed).statusCode() == 405, "known path with wrong method should be 405");

    HttpRequest object;
    object.method = HttpMethod::Get;
    object.path = "/objects/123";
    expect(router.route(object).statusCode() == 200, "prefix object path should route");

    HttpRequest prefix_without_id;
    prefix_without_id.method = HttpMethod::Get;
    prefix_without_id.path = "/objects/";
    expect(router.route(prefix_without_id).statusCode() == 404, "prefix route should require suffix");

    HttpRequest missing;
    missing.method = HttpMethod::Get;
    missing.path = "/missing";
    expect(router.route(missing).statusCode() == 404, "unknown path should be 404");
}

void testConfig()
{
    const auto dir = makeTempDir("config");
    const auto path = dir / "config.ini";
    {
        std::ofstream out(path);
        out << "[server]\n"
            << "port = 18080\n"
            << "threads = 3\n"
            << "thread_queue_limit = 7\n"
            << "max_connections = 11\n"
            << "request_timeout_ms = 1200\n"
            << "upload_timeout_ms = 3400\n"
            << "\n[storage]\n"
            << "dir = tmp/unit_storage\n"
            << "\n[logging]\n"
            << "dir = tmp/unit_logs\n"
            << "queue_limit = 123\n"
            << "slow_request_ms = 9\n"
            << "\n[auth]\n"
            << "token = unit-token\n"
            << "\n[redis]\n"
            << "enabled = true\n"
            << "host = 127.0.0.1\n"
            << "port = 6379\n"
            << "db = 2\n"
            << "key_prefix = unit_oss\n"
            << "ttl_seconds = 60\n"
            << "connect_timeout_ms = 50\n"
            << "io_timeout_ms = 50\n";
    }

    mini_oss::AppConfig config;
    std::string error;
    expect(mini_oss::loadConfigFile(path, config, error), "config file should load");
    expect(config.port == 18080, "port should parse");
    expect(config.worker_threads == 3, "threads should parse");
    expect(config.thread_queue_limit == 7, "thread queue limit should parse");
    expect(config.max_connections == 11, "max connections should parse");
    expect(config.request_timeout_ms == 1200, "request timeout should parse");
    expect(config.upload_timeout_ms == 3400, "upload timeout should parse");
    expect(config.log_queue_limit == 123, "log queue limit should parse");
    expect(config.slow_request_ms == 9, "slow request threshold should parse from logging section");
    expect(config.auth_token == "unit-token", "auth token should parse");
    expect(config.redis.enabled, "redis enabled should parse");
    expect(config.redis.host == "127.0.0.1", "redis host should parse");
    expect(config.redis.port == 6379, "redis port should parse");
    expect(config.redis.db == 2, "redis db should parse");
    expect(config.redis.key_prefix == "unit_oss", "redis key prefix should parse");
    expect(config.redis.ttl_seconds == 60, "redis ttl should parse");
    expect(config.redis.connect_timeout_ms == 50, "redis connect timeout should parse");
    expect(config.redis.io_timeout_ms == 50, "redis io timeout should parse");

    const auto bad_path = dir / "bad.ini";
    {
        std::ofstream out(bad_path);
        out << "[server]\nunknown_key = 1\n";
    }
    mini_oss::AppConfig bad_config;
    error.clear();
    expect(!mini_oss::loadConfigFile(bad_path, bad_config, error), "unknown config key should fail");
    expectContains(error, "unknown config key", "unknown key error should be clear");

    std::filesystem::remove_all(dir);
}

void testRedisMetadataSerialization()
{
    mini_oss::ObjectInfo info;
    info.id = "42";
    info.filename = "demo:name.txt";
    info.path = "/tmp/mini oss/object:42";
    info.size = 12345;
    info.sha256 = "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
    info.created_at = "2026-06-11T00:00:00Z";

    const auto serialized = mini_oss::serializeObjectInfo(info);
    const auto parsed = mini_oss::deserializeObjectInfo(serialized);
    expect(parsed.has_value(), "redis metadata serialization should parse");
    expect(parsed->id == info.id, "redis metadata id should round-trip");
    expect(parsed->filename == info.filename, "redis metadata filename should round-trip");
    expect(parsed->path == info.path, "redis metadata path should round-trip");
    expect(parsed->size == info.size, "redis metadata size should round-trip");
    expect(parsed->sha256 == info.sha256, "redis metadata sha256 should round-trip");
    expect(parsed->created_at == info.created_at, "redis metadata timestamp should round-trip");
    expect(!mini_oss::deserializeObjectInfo("bad-data").has_value(),
           "invalid redis metadata should fail parsing");
}

void testObjectStore()
{
    const auto root = makeTempDir("object_store");
    {
        ObjectStore store(root);

        HttpRequest upload;
        upload.method = HttpMethod::Post;
        upload.path = "/objects";
        upload.headers["x-filename"] = "../hello.txt";
        upload.body = "hello mini oss";
        upload.body_size = upload.body.size();
        const auto created = store.createObject(upload);
        expect(created.statusCode() == 201, "object upload should create metadata");
        expectContains(responseBody(created), "\"id\":\"1\"", "first object id should be 1");
        expectContains(responseBody(created), "\"filename\":\"hello.txt\"", "filename should be sanitized");
        expectContains(responseBody(created), "\"size\":14", "object size should be stored");

        const auto duplicate = store.createObject(upload);
        expect(duplicate.statusCode() == 201, "duplicate upload should create metadata alias");
        expectContains(responseBody(duplicate), "\"id\":\"2\"", "duplicate object id should be 2");
        expectContains(responseBody(duplicate), "\"deduplicated\":true", "duplicate should be marked deduplicated");
        expectContains(responseBody(duplicate), "\"source_id\":\"1\"", "duplicate should reference original");

        HttpRequest list;
        list.method = HttpMethod::Get;
        list.path = "/objects";
        const auto listed = store.listObjects(list);
        expect(listed.statusCode() == 200, "list objects should succeed");
        expectContains(responseBody(listed), "\"id\":\"1\"", "list should include first object");
        expectContains(responseBody(listed), "\"id\":\"2\"", "list should include duplicate object");

        HttpRequest download;
        download.method = HttpMethod::Get;
        download.path = "/objects/1";
        const auto full = store.getObject(download);
        expect(full.statusCode() == 200, "full download should succeed");
        expect(responseBody(full) == "hello mini oss", "full download body should match");
        expectContains(full.serialize(), "Accept-Ranges: bytes", "download should advertise ranges");

        HttpRequest prefix_range = download;
        prefix_range.headers["range"] = "bytes=0-4";
        const auto prefix = store.getObject(prefix_range);
        expect(prefix.statusCode() == 206, "prefix range should return partial content");
        expect(responseBody(prefix) == "hello", "prefix range body should match");
        expectContains(prefix.serialize(), "Content-Range: bytes 0-4/14", "prefix Content-Range should match");

        HttpRequest open_range = download;
        open_range.headers["range"] = "bytes=6-";
        const auto open = store.getObject(open_range);
        expect(open.statusCode() == 206, "open-ended range should return partial content");
        expect(responseBody(open) == "mini oss", "open-ended range body should match");

        HttpRequest suffix_range = download;
        suffix_range.headers["range"] = "bytes=-3";
        const auto suffix = store.getObject(suffix_range);
        expect(suffix.statusCode() == 206, "suffix range should return partial content");
        expect(responseBody(suffix) == "oss", "suffix range body should match");

        HttpRequest invalid_range = download;
        invalid_range.headers["range"] = "bytes=999-1000";
        const auto invalid = store.getObject(invalid_range);
        expect(invalid.statusCode() == 416, "invalid range should be rejected");
        expectContains(invalid.serialize(), "Content-Range: bytes */14", "invalid range should include total size");

        HttpRequest delete_first;
        delete_first.method = HttpMethod::Delete;
        delete_first.path = "/objects/1";
        const auto deleted_first = store.deleteObject(delete_first);
        expect(deleted_first.statusCode() == 200, "delete first object should succeed");
        expectContains(responseBody(deleted_first), "\"removed_file\":false", "shared file should remain");

        HttpRequest delete_second;
        delete_second.method = HttpMethod::Delete;
        delete_second.path = "/objects/2";
        const auto deleted_second = store.deleteObject(delete_second);
        expect(deleted_second.statusCode() == 200, "delete last alias should succeed");
        expectContains(responseBody(deleted_second), "\"removed_file\":true", "last alias should remove file");
    }

    std::filesystem::remove_all(root);
}

void runAllTests()
{
    testHttpParsing();
    testHttpResponse();
    testRouter();
    testConfig();
    testRedisMetadataSerialization();
    testObjectStore();
}

} // namespace

int main()
{
    try {
        runAllTests();
        std::cout << "mini_oss unit tests passed\n";
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "mini_oss unit tests failed: " << ex.what() << '\n';
        return 1;
    }
}
