#include "mini_oss/auth_service.h"
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

void testAuthAuditAndStats()
{
    const auto root = makeTempDir("auth_audit_stats");
    {
        mini_oss::MetadataStore metadata(root / "metadata.db");
        mini_oss::AuthService auth(metadata);
        std::string error;
        expect(auth.initializeDefaults(error), "default users and roles should initialize");

        HttpRequest bad_login;
        bad_login.method = HttpMethod::Post;
        bad_login.path = "/auth/login";
        bad_login.body = "{\"username\":\"admin\",\"password\":\"bad\"}";
        expect(auth.login(bad_login).statusCode() == 401, "bad password should be unauthorized");

        HttpRequest login;
        login.method = HttpMethod::Post;
        login.path = "/auth/login";
        login.body = "{\"username\":\"admin\",\"password\":\"admin123\"}";
        const auto login_response = auth.login(login);
        expect(login_response.statusCode() == 200, "admin login should succeed");
        const auto login_body = responseBody(login_response);
        const auto token_key = std::string("\"token\":\"");
        const auto token_begin = login_body.find(token_key);
        expect(token_begin != std::string::npos, "login response should contain token");
        const auto value_begin = token_begin + token_key.size();
        const auto value_end = login_body.find('"', value_begin);
        expect(value_end != std::string::npos, "token should be quoted");
        const auto token = login_body.substr(value_begin, value_end - value_begin);

        HttpRequest authed;
        authed.method = HttpMethod::Get;
        authed.path = "/admin/users";
        authed.headers["authorization"] = "Bearer " + token;
        expect(auth.authenticate(authed, error), "bearer token should authenticate");
        expect(authed.authenticated, "request should be marked authenticated");
        expect(authed.is_admin, "admin user should carry admin role");
        expect(authed.username == "admin", "authenticated username should be admin");

        mini_oss::ObjectInfo object;
        object.id = "1";
        object.filename = "owned.txt";
        object.path = root / "objects" / "1";
        object.size = 128;
        object.sha256 = "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa";
        object.created_at = "2026-06-27T00:00:00Z";
        object.owner_user_id = authed.user_id;
        expect(metadata.insertObject(object, error), "object metadata should insert with owner");
        expect(metadata.incrementObjectDownloadCount("1", error), "download count should increment");
        const auto stats = metadata.objectStorageStats(error);
        expect(error.empty(), "object stats should not set error");
        expect(stats.file_total == 1, "stats should count one object");
        expect(stats.storage_bytes == 128, "stats should sum unique storage bytes");
        expect(stats.upload_count == 1, "stats should count uploads");
        expect(stats.download_count == 1, "stats should count downloads");

        mini_oss::AuditLogRecord record;
        record.request_id = "unit-1";
        record.user_id = authed.user_id;
        record.username = authed.username;
        record.method = "GET";
        record.path = "/admin/stats/overview";
        record.action = "query_admin_stats";
        record.status_code = 200;
        record.result = "success";
        record.client_ip = "127.0.0.1:12345";
        record.created_at = "2026-06-27T00:00:01Z";
        expect(metadata.insertAuditLog(record, error), "audit log should insert");
        mini_oss::AuditLogQuery query;
        query.action = "query_admin_stats";
        const auto logs = metadata.listAuditLogs(query, error);
        expect(error.empty(), "audit log query should not set error");
        expect(logs.size() == 1, "audit query should return inserted record");
        expect(logs.front().username == "admin", "audit log should keep username");
    }
    std::filesystem::remove_all(root);
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
        expect(full.bodyInFile(), "full download should use file response");
        expect(full.bodySize() == 14, "full download size should match");
        expectContains(full.serialize(), "Accept-Ranges: bytes", "download should advertise ranges");
        expectContains(full.serialize(), "Content-Length: 14", "download content length should match");

        HttpRequest prefix_range = download;
        prefix_range.headers["range"] = "bytes=0-4";
        const auto prefix = store.getObject(prefix_range);
        expect(prefix.statusCode() == 206, "prefix range should return partial content");
        expect(prefix.bodyInFile(), "prefix range should use file response");
        expect(prefix.bodySize() == 5, "prefix range size should match");
        expectContains(prefix.serialize(), "Content-Range: bytes 0-4/14", "prefix Content-Range should match");

        HttpRequest open_range = download;
        open_range.headers["range"] = "bytes=6-";
        const auto open = store.getObject(open_range);
        expect(open.statusCode() == 206, "open-ended range should return partial content");
        expect(open.bodySize() == 8, "open-ended range size should match");

        HttpRequest suffix_range = download;
        suffix_range.headers["range"] = "bytes=-3";
        const auto suffix = store.getObject(suffix_range);
        expect(suffix.statusCode() == 206, "suffix range should return partial content");
        expect(suffix.bodySize() == 3, "suffix range size should match");

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
    testAuthAuditAndStats();
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
