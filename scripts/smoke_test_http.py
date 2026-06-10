#!/usr/bin/env python3
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


def upload_parallel(index: int) -> str:
    response = request(
        "/objects",
        method="POST",
        body=f"parallel object {index}".encode("utf-8"),
        headers={"X-Filename": f"parallel-{index}.txt"},
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
        "[server]\nport = 18080\nthreads = 2\nslow_request_ms = 0\n\n"
        "[storage]\ndir = tmp/smoke_storage\n\n"
        "[logging]\ndir = tmp/smoke_logs\n",
        encoding="utf-8",
    )

    proc = start_server(config_path)

    try:
        health = request("/health")
        missing = request("/not-found")
        method_not_allowed = request("/health", method="POST")
        bad_request = request("/", raw_request=b"BAD_REQUEST\r\n\r\n")
        upload = request(
            "/objects",
            method="POST",
            body=b"hello mini oss",
            headers={"X-Filename": "hello.txt"},
        )
        upload_body = json.loads(response_body(upload))
        object_id = upload_body["id"]
        with ThreadPoolExecutor(max_workers=8) as executor:
            parallel_ids = list(executor.map(upload_parallel, range(8)))

        listing = request("/objects")
        download = request(f"/objects/{object_id}")
        stop_server(proc)

        proc = start_server(config_path)
        restarted_listing = request("/objects")
        restarted_download = request(f"/objects/{object_id}")

        delete = request(f"/objects/{object_id}", method="DELETE")
        deleted_download = request(f"/objects/{object_id}")
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
    print("=== POST /objects ===")
    print(upload)
    print("=== GET /objects ===")
    print(listing)
    print("=== GET /objects/{id} ===")
    print(download)
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
    if "HTTP/1.1 201 Created" not in upload or '"sha256"' not in upload:
        print("object upload failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 200 OK" not in listing:
        print("object list failed", file=sys.stderr)
        return 1
    listed_ids = {item["id"] for item in json.loads(response_body(listing))["objects"]}
    if object_id not in listed_ids:
        print("object list missed uploaded object", file=sys.stderr)
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
    if "HTTP/1.1 200 OK" not in restarted_listing or object_id not in restarted_listing:
        print("metadata persistence list check failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 200 OK" not in restarted_download or "hello mini oss" not in restarted_download:
        print("metadata persistence download check failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 200 OK" not in delete or '"deleted":true' not in delete:
        print("object delete failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 404 Not Found" not in deleted_download:
        print("deleted object 404 check failed", file=sys.stderr)
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
    if "method=POST path=/objects status=201" not in access_text:
        print("access log upload entry missing", file=sys.stderr)
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
