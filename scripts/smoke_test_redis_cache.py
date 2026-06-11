#!/usr/bin/env python3
import json
import os
import shutil
import socket
import subprocess
import time
from pathlib import Path

AUTH_HEADERS = {"Authorization": "Bearer redis-token"}
PORT = 18084
FALLBACK_PORT = 18085
PREFIX = "mini_oss_smoke"
MINI_OSS_BINARY = os.environ.get("MINI_OSS_BINARY", "./build/mini_oss")


def request(
    path: str,
    method: str = "GET",
    body: bytes = b"",
    headers: dict[str, str] | None = None,
    port: int = PORT,
) -> str:
    with socket.create_connection(("127.0.0.1", port), timeout=4) as sock:
        sock.settimeout(4)
        lines = [
            f"{method} {path} HTTP/1.1",
            "Host: 127.0.0.1",
            "Connection: close",
        ]
        for key, value in (headers or {}).items():
            lines.append(f"{key}: {value}")
        if body:
            lines.append(f"Content-Length: {len(body)}")
        sock.sendall(("\r\n".join(lines) + "\r\n\r\n").encode("ascii") + body)
        chunks: list[bytes] = []
        while True:
            data = sock.recv(4096)
            if not data:
                break
            chunks.append(data)
    return b"".join(chunks).decode("utf-8", "replace")


def response_body(response: str) -> str:
    return response.split("\r\n\r\n", 1)[1]


def redis(*args: str) -> str:
    return subprocess.check_output(["redis-cli", *args], text=True).strip()


def start_server(config_path: Path) -> subprocess.Popen[str]:
    proc = subprocess.Popen(
        [MINI_OSS_BINARY, "--config", str(config_path)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.8)
    return proc


def stop_server(proc: subprocess.Popen[str]) -> tuple[str, str]:
    proc.terminate()
    try:
        return proc.communicate(timeout=4)
    except subprocess.TimeoutExpired:
        proc.kill()
        return proc.communicate(timeout=4)


def expect(condition: bool, message: str) -> None:
    if not condition:
        raise RuntimeError(message)


def run_redis_fallback_check() -> None:
    storage_dir = Path("tmp/redis_fallback_storage")
    log_dir = Path("tmp/redis_fallback_logs")
    config_path = Path("tmp/redis_fallback_config.ini")
    shutil.rmtree(storage_dir, ignore_errors=True)
    shutil.rmtree(log_dir, ignore_errors=True)
    config_path.parent.mkdir(parents=True, exist_ok=True)
    config_path.write_text(
        "[server]\n"
        f"port = {FALLBACK_PORT}\n"
        "threads = 1\n"
        "max_request_bytes = 4096\n"
        "max_upload_bytes = 2097152\n"
        "stream_upload_threshold_bytes = 1024\n"
        "max_connections = 16\n"
        "thread_queue_limit = 16\n"
        "request_timeout_ms = 2000\n"
        "upload_timeout_ms = 5000\n\n"
        "[storage]\ndir = tmp/redis_fallback_storage\n\n"
        "[logging]\ndir = tmp/redis_fallback_logs\nqueue_limit = 4096\n\n"
        "[auth]\ntoken = redis-token\n\n"
        "[redis]\n"
        "enabled = true\n"
        "host = 127.0.0.1\n"
        "port = 6390\n"
        "db = 0\n"
        "key_prefix = mini_oss_fallback\n"
        "ttl_seconds = 120\n"
        "connect_timeout_ms = 50\n"
        "io_timeout_ms = 50\n",
        encoding="utf-8",
    )

    proc = start_server(config_path)
    try:
        upload = request(
            "/objects",
            method="POST",
            body=b"fallback object",
            headers={**AUTH_HEADERS, "X-Filename": "fallback.txt"},
            port=FALLBACK_PORT,
        )
        print("=== fallback upload ===")
        print(upload)
        expect("HTTP/1.1 201 Created" in upload, "upload should succeed without redis")
        object_id = json.loads(response_body(upload))["id"]
        download = request(f"/objects/{object_id}", headers=AUTH_HEADERS, port=FALLBACK_PORT)
        expect("fallback object" in download, "download should succeed without redis")
        metrics = request("/metrics", port=FALLBACK_PORT)
        print("=== fallback metrics ===")
        print(metrics)
        data = json.loads(response_body(metrics))
        expect(data["metadata_cache_errors"] >= 1, "redis failures should be observable")
    finally:
        out, err = stop_server(proc)
        print("=== fallback server stdout ===")
        print(out)
        print("=== fallback server stderr ===")
        print(err)


def main() -> int:
    expect(redis("ping") == "PONG", "redis server should respond to PING")
    keys = redis("--raw", "keys", f"{PREFIX}:*").splitlines()
    for key in keys:
        if key:
            redis("del", key)

    storage_dir = Path("tmp/redis_smoke_storage")
    log_dir = Path("tmp/redis_smoke_logs")
    config_path = Path("tmp/redis_smoke_config.ini")
    shutil.rmtree(storage_dir, ignore_errors=True)
    shutil.rmtree(log_dir, ignore_errors=True)
    config_path.parent.mkdir(parents=True, exist_ok=True)
    config_path.write_text(
        "[server]\n"
        f"port = {PORT}\n"
        "threads = 2\n"
        "max_request_bytes = 4096\n"
        "max_upload_bytes = 2097152\n"
        "stream_upload_threshold_bytes = 1024\n"
        "max_connections = 32\n"
        "thread_queue_limit = 64\n"
        "request_timeout_ms = 2000\n"
        "upload_timeout_ms = 5000\n\n"
        "[storage]\ndir = tmp/redis_smoke_storage\n\n"
        "[logging]\ndir = tmp/redis_smoke_logs\nqueue_limit = 4096\n\n"
        "[auth]\ntoken = redis-token\n\n"
        "[redis]\n"
        "enabled = true\n"
        "host = 127.0.0.1\n"
        "port = 6379\n"
        "db = 0\n"
        f"key_prefix = {PREFIX}\n"
        "ttl_seconds = 120\n"
        "connect_timeout_ms = 100\n"
        "io_timeout_ms = 100\n",
        encoding="utf-8",
    )

    proc = start_server(config_path)
    try:
        upload = request(
            "/objects",
            method="POST",
            body=b"redis cache object",
            headers={**AUTH_HEADERS, "X-Filename": "redis.txt"},
        )
        print("=== upload ===")
        print(upload)
        expect("HTTP/1.1 201 Created" in upload, "upload should succeed")
        upload_body = json.loads(response_body(upload))
        object_id = upload_body["id"]

        first_download = request(f"/objects/{object_id}", headers=AUTH_HEADERS)
        second_download = request(f"/objects/{object_id}", headers=AUTH_HEADERS)
        expect("redis cache object" in first_download, "first download should return object")
        expect("redis cache object" in second_download, "second download should return object")

        instant = request(
            "/objects/instant",
            method="POST",
            headers={
                **AUTH_HEADERS,
                "X-Filename": "redis-copy.txt",
                "X-Object-Sha256": upload_body["sha256"],
                "X-Object-Size": str(upload_body["size"]),
            },
        )
        expect("HTTP/1.1 201 Created" in instant, "instant upload should use cached sha index")

        metrics = request("/metrics")
        print("=== metrics ===")
        print(metrics)
        data = json.loads(response_body(metrics))
        expect(data["metadata_cache_hits"] >= 1, "redis cache should record hits")
        expect(data["metadata_cache_misses"] >= 1, "redis cache should record misses")
        expect(data["metadata_cache_errors"] == 0, "redis cache should not record errors")

        object_key = redis("get", f"{PREFIX}:object:{object_id}")
        expect(object_key != "", "object metadata should be cached in Redis")
    finally:
        out, err = stop_server(proc)
        print("=== server stdout ===")
        print(out)
        print("=== server stderr ===")
        print(err)
    run_redis_fallback_check()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
