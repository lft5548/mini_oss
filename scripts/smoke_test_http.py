#!/usr/bin/env python3
import hashlib
import json
from concurrent.futures import ThreadPoolExecutor
import shutil
import socket
import subprocess
import sys
import time
from pathlib import Path


def request(
    path: str,
    method: str = "GET",
    body: bytes = b"",
    headers: dict[str, str] | None = None,
    raw_request: bytes | None = None,
) -> str:
    with socket.create_connection(("127.0.0.1", 18080), timeout=3) as sock:
        sock.settimeout(3)
        if raw_request is None:
            header_lines = [
                f"{method} {path} HTTP/1.1",
                "Host: 127.0.0.1",
                "Connection: close",
            ]
            for key, value in (headers or {}).items():
                header_lines.append(f"{key}: {value}")
            if body:
                header_lines.append(f"Content-Length: {len(body)}")
            raw = ("\r\n".join(header_lines) + "\r\n\r\n").encode("ascii") + body
        else:
            raw = (
                raw_request
            )
        sock.sendall(raw)
        chunks = []
        while True:
            data = sock.recv(4096)
            if not data:
                break
            chunks.append(data)
    return b"".join(chunks).decode("utf-8", "replace")


def response_body(response: str) -> str:
    return response.split("\r\n\r\n", 1)[1]


AUTH_HEADERS = {"Authorization": "Bearer smoke-token"}


def upload_parallel(index: int) -> str:
    response = request(
        "/objects",
        method="POST",
        body=f"parallel object {index}".encode("utf-8"),
        headers={**AUTH_HEADERS, "X-Filename": f"parallel-{index}.txt"},
    )
    if "HTTP/1.1 201 Created" not in response:
        raise RuntimeError(response)
    return json.loads(response_body(response))["id"]


def main() -> int:
    storage_dir = Path("tmp/smoke_storage")
    log_dir = Path("tmp/smoke_logs")
    config_path = Path("tmp/smoke_config.ini")
    shutil.rmtree(storage_dir, ignore_errors=True)
    shutil.rmtree(log_dir, ignore_errors=True)
    config_path.parent.mkdir(parents=True, exist_ok=True)
    config_path.write_text(
        "[server]\nport = 18080\nthreads = 2\nslow_request_ms = 0\n"
        "max_request_bytes = 4096\nmax_upload_bytes = 2097152\n"
        "stream_upload_threshold_bytes = 1024\n\n"
        "[storage]\ndir = tmp/smoke_storage\n\n"
        "[logging]\ndir = tmp/smoke_logs\n\n"
        "[auth]\ntoken = smoke-token\n",
        encoding="utf-8",
    )

    proc = start_server(config_path)

    try:
        health = request("/health")
        missing = request("/not-found")
        method_not_allowed = request("/health", method="POST")
        bad_request = request("/", raw_request=b"BAD_REQUEST\r\n\r\n")
        unauthorized_upload = request(
            "/objects",
            method="POST",
            body=b"hello mini oss",
            headers={"X-Filename": "hello.txt"},
        )
        upload = request(
            "/objects",
            method="POST",
            body=b"hello mini oss",
            headers={**AUTH_HEADERS, "X-Filename": "hello.txt"},
        )
        upload_body = json.loads(response_body(upload))
        object_id = upload_body["id"]
        duplicate_upload = request(
            "/objects",
            method="POST",
            body=b"hello mini oss",
            headers={**AUTH_HEADERS, "X-Filename": "hello-copy.txt"},
        )
        duplicate_body = json.loads(response_body(duplicate_upload))
        duplicate_id = duplicate_body["id"]
        instant_upload = request(
            "/objects/instant",
            method="POST",
            headers={
                **AUTH_HEADERS,
                "X-Filename": "hello-instant.txt",
                "X-Object-Sha256": upload_body["sha256"],
                "X-Object-Size": str(upload_body["size"]),
            },
        )
        instant_body = json.loads(response_body(instant_upload))
        instant_id = instant_body["id"]
        large_body = (b"large-stream-upload-" * 4096)[:65536]
        large_sha256 = hashlib.sha256(large_body).hexdigest()
        large_upload = request(
            "/objects",
            method="POST",
            body=large_body,
            headers={**AUTH_HEADERS, "X-Filename": "large-stream.bin"},
        )
        large_upload_body = json.loads(response_body(large_upload))
        large_object_id = large_upload_body["id"]
        large_duplicate_upload = request(
            "/objects",
            method="POST",
            body=large_body,
            headers={**AUTH_HEADERS, "X-Filename": "large-stream-copy.bin"},
        )
        large_duplicate_body = json.loads(response_body(large_duplicate_upload))
        large_duplicate_id = large_duplicate_body["id"]
        large_range = request(
            f"/objects/{large_object_id}",
            headers={**AUTH_HEADERS, "Range": "bytes=0-31"},
        )
        tmp_upload_files_after_large_upload = sorted(
            (storage_dir / "tmp_uploads").glob("*.tmp")
        )
        instant_miss = request(
            "/objects/instant",
            method="POST",
            headers={
                **AUTH_HEADERS,
                "X-Filename": "missing.txt",
                "X-Object-Sha256": "0" * 64,
                "X-Object-Size": str(upload_body["size"]),
            },
        )
        with ThreadPoolExecutor(max_workers=8) as executor:
            parallel_ids = list(executor.map(upload_parallel, range(8)))

        listing = request("/objects", headers=AUTH_HEADERS)
        download = request(f"/objects/{object_id}", headers=AUTH_HEADERS)
        range_prefix = request(
            f"/objects/{object_id}",
            headers={**AUTH_HEADERS, "Range": "bytes=0-4"},
        )
        range_open_end = request(
            f"/objects/{object_id}",
            headers={**AUTH_HEADERS, "Range": "bytes=6-"},
        )
        range_suffix = request(
            f"/objects/{object_id}",
            headers={**AUTH_HEADERS, "Range": "bytes=-3"},
        )
        range_invalid = request(
            f"/objects/{object_id}",
            headers={**AUTH_HEADERS, "Range": "bytes=999-1000"},
        )
        stop_server(proc)

        proc = start_server(config_path)
        restarted_listing = request("/objects", headers=AUTH_HEADERS)
        restarted_download = request(f"/objects/{object_id}", headers=AUTH_HEADERS)

        delete = request(f"/objects/{object_id}", method="DELETE", headers=AUTH_HEADERS)
        deleted_download = request(f"/objects/{object_id}", headers=AUTH_HEADERS)
        instant_download_after_source_delete = request(
            f"/objects/{instant_id}", headers=AUTH_HEADERS
        )
        delete_duplicate = request(
            f"/objects/{duplicate_id}", method="DELETE", headers=AUTH_HEADERS
        )
        instant_download_after_duplicate_delete = request(
            f"/objects/{instant_id}", headers=AUTH_HEADERS
        )
        delete_instant = request(
            f"/objects/{instant_id}", method="DELETE", headers=AUTH_HEADERS
        )
        deleted_instant_download = request(f"/objects/{instant_id}", headers=AUTH_HEADERS)
        metrics = request("/metrics")
        metrics_body = json.loads(response_body(metrics))
    finally:
        out, err = stop_server(proc)

    print("=== /health ===")
    print(health)
    print("=== /not-found ===")
    print(missing)
    print("=== POST /health ===")
    print(method_not_allowed)
    print("=== bad request ===")
    print(bad_request)
    print("=== POST /objects without auth ===")
    print(unauthorized_upload)
    print("=== POST /objects ===")
    print(upload)
    print("=== POST /objects duplicate body ===")
    print(duplicate_upload)
    print("=== POST /objects/instant ===")
    print(instant_upload)
    print("=== POST /objects/instant missing source ===")
    print(instant_miss)
    print("=== POST /objects large streaming upload ===")
    print(large_upload)
    print("=== POST /objects large duplicate streaming upload ===")
    print(large_duplicate_upload)
    print("=== GET /objects/{id} large Range bytes=0-31 ===")
    print(large_range)
    print("=== GET /objects ===")
    print(listing)
    print("=== GET /objects/{id} ===")
    print(download)
    print("=== GET /objects/{id} Range bytes=0-4 ===")
    print(range_prefix)
    print("=== GET /objects/{id} Range bytes=6- ===")
    print(range_open_end)
    print("=== GET /objects/{id} Range bytes=-3 ===")
    print(range_suffix)
    print("=== GET /objects/{id} invalid Range ===")
    print(range_invalid)
    print("=== concurrent upload ids ===")
    print(parallel_ids)
    print("=== GET /objects after restart ===")
    print(restarted_listing)
    print("=== GET /objects/{id} after restart ===")
    print(restarted_download)
    print("=== DELETE /objects/{id} ===")
    print(delete)
    print("=== GET deleted object ===")
    print(deleted_download)
    print("=== GET instant object after source delete ===")
    print(instant_download_after_source_delete)
    print("=== DELETE duplicate object ===")
    print(delete_duplicate)
    print("=== GET instant object after duplicate delete ===")
    print(instant_download_after_duplicate_delete)
    print("=== DELETE instant object ===")
    print(delete_instant)
    print("=== GET deleted instant object ===")
    print(deleted_instant_download)
    print("=== GET /metrics ===")
    print(metrics)
    print("=== parsed metrics ===")
    print(metrics_body)
    print("=== server stdout ===")
    print(out)
    print("=== server stderr ===")
    print(err)
    access_log = log_dir / "access.log"
    error_log = log_dir / "error.log"
    slow_log = log_dir / "slow.log"
    print("=== access.log ===")
    print(access_log.read_text(encoding="utf-8") if access_log.exists() else "")
    print("=== error.log ===")
    print(error_log.read_text(encoding="utf-8") if error_log.exists() else "")
    print("=== slow.log ===")
    print(slow_log.read_text(encoding="utf-8") if slow_log.exists() else "")

    if "HTTP/1.1 200 OK" not in health or '{"status":"ok"}' not in health:
        print("health check failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 404 Not Found" not in missing:
        print("404 check failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 405 Method Not Allowed" not in method_not_allowed:
        print("405 check failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 400 Bad Request" not in bad_request:
        print("400 check failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 401 Unauthorized" not in unauthorized_upload:
        print("unauthorized upload check failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 201 Created" not in upload or '"sha256"' not in upload:
        print("object upload failed", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 201 Created" not in duplicate_upload
        or duplicate_body.get("deduplicated") is not True
        or duplicate_body.get("instant_upload") is not False
        or duplicate_body.get("source_id") != object_id
    ):
        print("duplicate upload deduplication failed", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 201 Created" not in instant_upload
        or instant_body.get("deduplicated") is not True
        or instant_body.get("instant_upload") is not True
        or instant_body.get("source_id") != object_id
    ):
        print("instant upload failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 404 Not Found" not in instant_miss:
        print("instant upload miss check failed", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 201 Created" not in large_upload
        or large_upload_body.get("size") != len(large_body)
        or large_upload_body.get("sha256") != large_sha256
    ):
        print("large streaming upload failed", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 201 Created" not in large_duplicate_upload
        or large_duplicate_body.get("deduplicated") is not True
        or large_duplicate_body.get("instant_upload") is not False
        or large_duplicate_body.get("source_id") != large_object_id
    ):
        print("large streaming duplicate deduplication failed", file=sys.stderr)
        return 1
    if large_duplicate_id == large_object_id:
        print("large duplicate did not create a distinct metadata alias", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 206 Partial Content" not in large_range
        or f"Content-Range: bytes 0-31/{len(large_body)}" not in large_range
        or response_body(large_range).encode("utf-8") != large_body[:32]
    ):
        print("large range download failed", file=sys.stderr)
        return 1
    if tmp_upload_files_after_large_upload:
        print(
            f"temporary upload files were not cleaned: {tmp_upload_files_after_large_upload}",
            file=sys.stderr,
        )
        return 1
    if "HTTP/1.1 200 OK" not in listing:
        print("object list failed", file=sys.stderr)
        return 1
    listed_ids = {item["id"] for item in json.loads(response_body(listing))["objects"]}
    expected_ids = {object_id, duplicate_id, instant_id}
    if not expected_ids.issubset(listed_ids):
        print("object list missed uploaded/deduplicated object", file=sys.stderr)
        return 1
    expected_large_ids = {large_object_id, large_duplicate_id}
    if not expected_large_ids.issubset(listed_ids):
        print("object list missed large uploaded/deduplicated object", file=sys.stderr)
        return 1
    if len(set(parallel_ids)) != len(parallel_ids):
        print("concurrent upload ids are not unique", file=sys.stderr)
        return 1
    if not set(parallel_ids).issubset(listed_ids):
        print("concurrent uploaded objects missing from list", file=sys.stderr)
        return 1
    if "HTTP/1.1 200 OK" not in download or "hello mini oss" not in download:
        print("object download failed", file=sys.stderr)
        return 1
    if "Accept-Ranges: bytes" not in download:
        print("download accept-ranges header missing", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 206 Partial Content" not in range_prefix
        or "Content-Range: bytes 0-4/14" not in range_prefix
        or response_body(range_prefix) != "hello"
    ):
        print("range prefix download failed", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 206 Partial Content" not in range_open_end
        or "Content-Range: bytes 6-13/14" not in range_open_end
        or response_body(range_open_end) != "mini oss"
    ):
        print("range open-end download failed", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 206 Partial Content" not in range_suffix
        or "Content-Range: bytes 11-13/14" not in range_suffix
        or response_body(range_suffix) != "oss"
    ):
        print("range suffix download failed", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 416 Range Not Satisfiable" not in range_invalid
        or "Content-Range: bytes */14" not in range_invalid
        or "Accept-Ranges: bytes" not in range_invalid
    ):
        print("invalid range check failed", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 200 OK" not in restarted_listing
        or object_id not in restarted_listing
        or duplicate_id not in restarted_listing
        or instant_id not in restarted_listing
    ):
        print("metadata persistence list check failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 200 OK" not in restarted_download or "hello mini oss" not in restarted_download:
        print("metadata persistence download check failed", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 200 OK" not in delete
        or '"deleted":true' not in delete
        or '"removed_file":false' not in delete
    ):
        print("source object delete failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 404 Not Found" not in deleted_download:
        print("deleted object 404 check failed", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 200 OK" not in instant_download_after_source_delete
        or "hello mini oss" not in instant_download_after_source_delete
    ):
        print("deduplicated object lost after source delete", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 200 OK" not in delete_duplicate
        or '"deleted":true' not in delete_duplicate
        or '"removed_file":false' not in delete_duplicate
    ):
        print("duplicate object delete failed", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 200 OK" not in instant_download_after_duplicate_delete
        or "hello mini oss" not in instant_download_after_duplicate_delete
    ):
        print("instant object lost before last reference delete", file=sys.stderr)
        return 1
    if (
        "HTTP/1.1 200 OK" not in delete_instant
        or '"deleted":true' not in delete_instant
        or '"removed_file":true' not in delete_instant
    ):
        print("last deduplicated object delete failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 404 Not Found" not in deleted_instant_download:
        print("deleted instant object 404 check failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 200 OK" not in metrics:
        print("metrics endpoint failed", file=sys.stderr)
        return 1
    expected_metric_fields = {
        "active_connections",
        "total_requests",
        "success_requests",
        "failed_requests",
        "request_bytes",
        "response_bytes",
        "total_latency_ms",
        "average_latency_ms",
    }
    if not expected_metric_fields.issubset(metrics_body):
        print("metrics fields missing", file=sys.stderr)
        return 1
    if metrics_body["total_requests"] < 4:
        print("metrics total request count too small", file=sys.stderr)
        return 1
    if metrics_body["success_requests"] < 3 or metrics_body["failed_requests"] < 1:
        print("metrics success/failure count invalid", file=sys.stderr)
        return 1
    if metrics_body["active_connections"] < 1:
        print("metrics active connection count invalid", file=sys.stderr)
        return 1
    if metrics_body["request_bytes"] <= 0 or metrics_body["response_bytes"] <= 0:
        print("metrics byte counters invalid", file=sys.stderr)
        return 1
    access_text = access_log.read_text(encoding="utf-8") if access_log.exists() else ""
    error_text = error_log.read_text(encoding="utf-8") if error_log.exists() else ""
    slow_text = slow_log.read_text(encoding="utf-8") if slow_log.exists() else ""
    if "method=GET path=/health status=200" not in access_text:
        print("access log health entry missing", file=sys.stderr)
        return 1
    if "method=POST path=/objects status=401" not in access_text:
        print("access log unauthorized entry missing", file=sys.stderr)
        return 1
    if "method=POST path=/objects status=201" not in access_text:
        print("access log upload entry missing", file=sys.stderr)
        return 1
    if "method=POST path=/objects/instant status=201" not in access_text:
        print("access log instant upload entry missing", file=sys.stderr)
        return 1
    if "method=GET path=/not-found status=404" not in access_text:
        print("access log 404 entry missing", file=sys.stderr)
        return 1
    if "method=GET path=/metrics status=200" not in access_text:
        print("access log metrics entry missing", file=sys.stderr)
        return 1
    if "level=INFO" not in error_text:
        print("error log info entry missing", file=sys.stderr)
        return 1
    if "duration_ms=" not in slow_text:
        print("slow log entry missing", file=sys.stderr)
        return 1
    return 0


def start_server(config_path: Path) -> subprocess.Popen[str]:
    proc = subprocess.Popen(
        ["./build/mini_oss", "--config", str(config_path), "--threads", "4"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.8)
    return proc


def stop_server(proc: subprocess.Popen[str]) -> tuple[str, str]:
    if proc.poll() is not None:
        out, err = proc.communicate(timeout=3)
        return out, err

    proc.terminate()
    try:
        out, err = proc.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        out, err = proc.communicate(timeout=3)
    return out, err


if __name__ == "__main__":
    raise SystemExit(main())
