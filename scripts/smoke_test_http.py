#!/usr/bin/env python3
import socket
import subprocess
import sys
import time


def request(path: str, method: str = "GET", raw_request: bytes | None = None) -> str:
    with socket.create_connection(("127.0.0.1", 18080), timeout=3) as sock:
        sock.settimeout(3)
        if raw_request is None:
            raw = (
                f"{method} {path} HTTP/1.1\r\n"
                "Host: 127.0.0.1\r\n"
                "Connection: close\r\n"
                "\r\n"
            ).encode("ascii")
        else:
            raw = raw_request
        sock.sendall(raw)
        chunks = []
        while True:
            data = sock.recv(4096)
            if not data:
                break
            chunks.append(data)
    return b"".join(chunks).decode("utf-8", "replace")


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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
