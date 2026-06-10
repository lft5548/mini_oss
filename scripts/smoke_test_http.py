#!/usr/bin/env python3
import json
import socket
import subprocess
import sys
import time


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


def main() -> int:
    proc = subprocess.Popen(
        ["./build/mini_oss", "--port", "18080"],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )
    time.sleep(0.8)

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
        listing = request("/objects")
        download = request(f"/objects/{object_id}")
        delete = request(f"/objects/{object_id}", method="DELETE")
        deleted_download = request(f"/objects/{object_id}")
    finally:
        proc.terminate()
        try:
            out, err = proc.communicate(timeout=3)
        except subprocess.TimeoutExpired:
            proc.kill()
            out, err = proc.communicate(timeout=3)

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
    print("=== DELETE /objects/{id} ===")
    print(delete)
    print("=== GET deleted object ===")
    print(deleted_download)
    print("=== server stdout ===")
    print(out)
    print("=== server stderr ===")
    print(err)

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
    if "HTTP/1.1 200 OK" not in listing or object_id not in listing:
        print("object list failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 200 OK" not in download or "hello mini oss" not in download:
        print("object download failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 200 OK" not in delete or '"deleted":true' not in delete:
        print("object delete failed", file=sys.stderr)
        return 1
    if "HTTP/1.1 404 Not Found" not in deleted_download:
        print("deleted object 404 check failed", file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
