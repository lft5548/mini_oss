#!/usr/bin/env python3
import socket
import subprocess
import sys
import time


def request(path: str) -> str:
    with socket.create_connection(("127.0.0.1", 18080), timeout=3) as sock:
        sock.settimeout(3)
        raw = (
            f"GET {path} HTTP/1.1\r\n"
            "Host: 127.0.0.1\r\n"
            "Connection: close\r\n"
            "\r\n"
        ).encode("ascii")
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
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
