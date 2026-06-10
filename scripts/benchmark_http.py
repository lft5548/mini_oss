#!/usr/bin/env python3
import argparse
import json
import platform
import re
import shlex
import shutil
import socket
import subprocess
import sys
import time
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path


AUTH_TOKEN = "benchmark-token"
AUTH_HEADER = f"Authorization: Bearer {AUTH_TOKEN}"


@dataclass
class BenchmarkResult:
    tool: str
    name: str
    command: list[str]
    output: str
    requests: int
    failed: int
    qps: float
    mean_ms: float
    p50_ms: float
    p95_ms: float
    p99_ms: float
    transfer: str
    notes: str


def build_request(
    path: str,
    method: str = "GET",
    headers: dict[str, str] | None = None,
    body: bytes = b"",
) -> bytes:
    lines = [
        f"{method} {path} HTTP/1.1",
        "Host: 127.0.0.1",
        "Connection: close",
    ]
    for key, value in (headers or {}).items():
        lines.append(f"{key}: {value}")
    if body:
        lines.append(f"Content-Length: {len(body)}")
    return ("\r\n".join(lines) + "\r\n\r\n").encode("ascii") + body


def http_request(
    port: int,
    path: str,
    method: str = "GET",
    headers: dict[str, str] | None = None,
    body: bytes = b"",
    timeout: float = 5.0,
) -> tuple[int, bytes]:
    with socket.create_connection(("127.0.0.1", port), timeout=timeout) as sock:
        sock.settimeout(timeout)
        sock.sendall(build_request(path, method, headers, body))
        chunks: list[bytes] = []
        while True:
            data = sock.recv(16384)
            if not data:
                break
            chunks.append(data)

    raw = b"".join(chunks)
    first_line = raw.split(b"\r\n", 1)[0].decode("ascii", "replace")
    parts = first_line.split()
    status = int(parts[1]) if len(parts) >= 2 and parts[1].isdigit() else 0
    body_bytes = raw.split(b"\r\n\r\n", 1)[1] if b"\r\n\r\n" in raw else b""
    return status, body_bytes


def write_config(
    path: Path,
    port: int,
    threads: int,
    max_request_bytes: int,
    max_upload_bytes: int,
    stream_upload_threshold_bytes: int,
    max_connections: int,
    thread_queue_limit: int,
    request_timeout_ms: int,
    upload_timeout_ms: int,
) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(
        f"[server]\nport = {port}\nthreads = {threads}\nslow_request_ms = 100\n"
        f"max_request_bytes = {max_request_bytes}\n"
        f"max_upload_bytes = {max_upload_bytes}\n"
        f"stream_upload_threshold_bytes = {stream_upload_threshold_bytes}\n"
        f"max_connections = {max_connections}\n"
        f"thread_queue_limit = {thread_queue_limit}\n"
        f"request_timeout_ms = {request_timeout_ms}\n"
        f"upload_timeout_ms = {upload_timeout_ms}\n\n"
        "[storage]\ndir = tmp/benchmark_storage\n\n"
        "[logging]\ndir = tmp/benchmark_logs\n\n"
        f"[auth]\ntoken = {AUTH_TOKEN}\n",
        encoding="utf-8",
    )


def start_server(config_path: Path, threads: int) -> subprocess.Popen[str]:
    return subprocess.Popen(
        ["./build/mini_oss", "--config", str(config_path), "--threads", str(threads)],
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
    )


def wait_for_server(port: int, proc: subprocess.Popen[str]) -> None:
    deadline = time.time() + 5
    while time.time() < deadline:
        if proc.poll() is not None:
            out, err = proc.communicate(timeout=1)
            raise RuntimeError(f"server exited early\nstdout:\n{out}\nstderr:\n{err}")
        try:
            status, _ = http_request(port, "/health", timeout=1)
            if status == 200:
                return
        except OSError:
            pass
        time.sleep(0.1)
    raise RuntimeError("server did not become ready")


def stop_server(proc: subprocess.Popen[str]) -> tuple[str, str]:
    if proc.poll() is not None:
        return proc.communicate(timeout=3)

    proc.terminate()
    try:
        return proc.communicate(timeout=3)
    except subprocess.TimeoutExpired:
        proc.kill()
        return proc.communicate(timeout=3)


def upload_seed_object(port: int, size: int) -> dict:
    body = (b"mini-oss-benchmark-" * ((size // 19) + 1))[:size]
    status, response_body = http_request(
        port,
        "/objects",
        method="POST",
        headers={"Authorization": f"Bearer {AUTH_TOKEN}", "X-Filename": "benchmark-seed.bin"},
        body=body,
    )
    if status != 201:
        raise RuntimeError(f"seed upload failed: status={status} body={response_body!r}")
    return json.loads(response_body.decode("utf-8"))


def fetch_metrics(port: int) -> dict:
    status, body = http_request(port, "/metrics")
    if status != 200:
        return {}
    return json.loads(body.decode("utf-8"))


def parse_int(pattern: str, text: str) -> int:
    match = re.search(pattern, text, re.MULTILINE)
    return int(match.group(1)) if match else 0


def parse_float(pattern: str, text: str) -> float:
    match = re.search(pattern, text, re.MULTILINE)
    return float(match.group(1)) if match else 0.0


def latency_to_ms(value: str, unit: str) -> float:
    number = float(value)
    if unit == "us":
        return number / 1000
    if unit == "s":
        return number * 1000
    return number


def parse_ab_percentile(percent: int, text: str) -> float:
    match = re.search(rf"^\s*{percent}%\s+([0-9.]+)$", text, re.MULTILINE)
    return float(match.group(1)) if match else 0.0


def parse_wrk_latency(pattern: str, text: str) -> float:
    match = re.search(pattern, text, re.MULTILINE)
    return latency_to_ms(match.group(1), match.group(2)) if match else 0.0


def parse_wrk_percentile(percent: int, text: str) -> float:
    return parse_wrk_latency(rf"^\s*{percent}%\s+([0-9.]+)\s*(us|ms|s)$", text)


def parse_wrk_socket_errors(text: str) -> int:
    match = re.search(
        r"Socket errors:\s+connect\s+(\d+),\s+read\s+(\d+),\s+write\s+(\d+),\s+timeout\s+(\d+)",
        text,
    )
    if not match:
        return 0
    return sum(int(group) for group in match.groups())


def run_command(command: list[str], name: str) -> str:
    completed = subprocess.run(command, check=False, text=True, capture_output=True)
    output = completed.stdout + completed.stderr
    if completed.returncode != 0:
        raise RuntimeError(f"{name} failed\ncommand: {command_text(command)}\n{output}")
    return output


def run_ab_case(name: str, command: list[str], notes: str) -> BenchmarkResult:
    output = run_command(command, f"ab {name}")
    transfer_kbps = parse_float(r"Transfer rate:\s+([0-9.]+)\s+\[Kbytes/sec\] received", output)
    return BenchmarkResult(
        tool="ab",
        name=name,
        command=command,
        output=output,
        requests=parse_int(r"Complete requests:\s+(\d+)", output),
        failed=parse_int(r"Failed requests:\s+(\d+)", output),
        qps=parse_float(r"Requests per second:\s+([0-9.]+)", output),
        mean_ms=parse_float(r"Time per request:\s+([0-9.]+)\s+\[ms\]\s+\(mean\)", output),
        p50_ms=parse_ab_percentile(50, output),
        p95_ms=parse_ab_percentile(95, output),
        p99_ms=parse_ab_percentile(99, output),
        transfer=f"{transfer_kbps:.2f} KB/s",
        notes=notes,
    )


def run_wrk_case(name: str, command: list[str], notes: str) -> BenchmarkResult:
    output = run_command(command, f"wrk {name}")
    non_2xx = parse_int(r"Non-2xx or 3xx responses:\s+(\d+)", output)
    socket_errors = parse_wrk_socket_errors(output)
    transfer_match = re.search(r"Transfer/sec:\s+([0-9.]+[KMG]?B)", output)
    transfer = transfer_match.group(1) + "/sec" if transfer_match else ""
    return BenchmarkResult(
        tool="wrk",
        name=name,
        command=command,
        output=output,
        requests=parse_int(r"^\s*(\d+)\s+requests in", output),
        failed=non_2xx + socket_errors,
        qps=parse_float(r"Requests/sec:\s+([0-9.]+)", output),
        mean_ms=parse_wrk_latency(r"Latency\s+([0-9.]+)(us|ms|s)", output),
        p50_ms=parse_wrk_percentile(50, output),
        p95_ms=parse_wrk_percentile(90, output),
        p99_ms=parse_wrk_percentile(99, output),
        transfer=transfer,
        notes=notes,
    )


def ab_base(total: int, concurrency: int) -> list[str]:
    return ["ab", "-l", "-n", str(total), "-c", str(concurrency)]


def wrk_base(args: argparse.Namespace) -> list[str]:
    return [
        "wrk",
        "-t",
        str(args.wrk_threads),
        "-c",
        str(args.wrk_connections),
        "-d",
        args.wrk_duration,
        "--timeout",
        args.wrk_timeout,
        "--latency",
    ]


def wrk_download_base(args: argparse.Namespace) -> list[str]:
    return [
        "wrk",
        "-t",
        str(min(args.wrk_threads, 1)),
        "-c",
        str(min(args.wrk_connections, 4)),
        "-d",
        args.wrk_duration,
        "--timeout",
        args.wrk_timeout,
        "--latency",
    ]


def wrk_range_base(args: argparse.Namespace) -> list[str]:
    return [
        "wrk",
        "-t",
        str(min(args.wrk_threads, 1)),
        "-c",
        str(min(args.wrk_connections, 4)),
        "-d",
        args.wrk_duration,
        "--timeout",
        args.wrk_timeout,
        "--latency",
    ]


def lua_long_string(value: str) -> str:
    return "[==[" + value + "]==]"


def write_wrk_scripts(tmp_dir: Path, seed: dict, upload_body: bytes) -> tuple[Path, Path]:
    tmp_dir.mkdir(parents=True, exist_ok=True)
    instant_script = tmp_dir / "benchmark_instant.lua"
    upload_script = tmp_dir / "benchmark_upload.lua"
    instant_script.write_text(
        "\n".join(
            [
                'wrk.method = "POST"',
                f'wrk.headers["Authorization"] = "Bearer {AUTH_TOKEN}"',
                'wrk.headers["X-Filename"] = "wrk-instant.bin"',
                f'wrk.headers["X-Object-Sha256"] = "{seed["sha256"]}"',
                f'wrk.headers["X-Object-Size"] = "{seed["size"]}"',
                'wrk.body = ""',
                "",
            ]
        ),
        encoding="utf-8",
    )
    upload_script.write_text(
        "\n".join(
            [
                'wrk.method = "POST"',
                f'wrk.headers["Authorization"] = "Bearer {AUTH_TOKEN}"',
                'wrk.headers["X-Filename"] = "wrk-upload.bin"',
                'wrk.headers["Content-Type"] = "application/octet-stream"',
                f"wrk.body = {lua_long_string(upload_body.decode('ascii'))}",
                "",
            ]
        ),
        encoding="utf-8",
    )
    return instant_script, upload_script


def command_text(command: list[str]) -> str:
    return " ".join(shlex.quote(part) for part in command)


def strip_trailing_whitespace(text: str) -> str:
    return "\n".join(line.rstrip() for line in text.strip().splitlines())


def render_report(
    path: Path,
    args: argparse.Namespace,
    ab_version: str,
    wrk_version: str,
    seed: dict,
    results: list[BenchmarkResult],
    metrics: dict,
    server_stdout: str,
    server_stderr: str,
) -> None:
    generated_at = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")
    lines = [
        "# Mini-OSS Benchmark Report",
        "",
        "This report is generated by `scripts/benchmark_http.py` with ApacheBench (`ab`) and `wrk`.",
        "The goal is to provide repeatable engineering evidence for throughput, latency, and failure rate.",
        "",
        "## Environment",
        "",
        f"- Generated at: {generated_at}",
        f"- Platform: {platform.platform()}",
        f"- Python: {platform.python_version()}",
        f"- ab: {ab_version}",
        f"- wrk: {wrk_version}",
        f"- Server port: {args.port}",
        f"- Worker threads: {args.threads}",
        f"- ab read requests/concurrency: {args.read_requests}/{args.concurrency}",
        f"- ab write requests/concurrency: {args.write_requests}/{args.write_concurrency}",
        f"- wrk threads/connections/duration/timeout: {args.wrk_threads}/{args.wrk_connections}/{args.wrk_duration}/{args.wrk_timeout}",
        f"- wrk full-download threads/connections/duration/timeout: {min(args.wrk_threads, 1)}/{min(args.wrk_connections, 4)}/{args.wrk_duration}/{args.wrk_timeout}",
        f"- wrk Range threads/connections/duration/timeout: {min(args.wrk_threads, 1)}/{min(args.wrk_connections, 4)}/{args.wrk_duration}/{args.wrk_timeout}",
        f"- Seed object size: {args.object_size} bytes",
        f"- Max request bytes: {args.max_request_bytes}",
        f"- Max upload bytes: {args.max_upload_bytes}",
        f"- Stream upload threshold bytes: {args.stream_upload_threshold_bytes}",
        f"- Max connections: {args.max_connections}",
        f"- Thread queue limit: {args.thread_queue_limit}",
        f"- Request timeout ms: {args.request_timeout_ms}",
        f"- Upload timeout ms: {args.upload_timeout_ms}",
        "",
        "## Summary",
        "",
        "| Tool | Case | Requests | Failed | QPS | Mean(ms) | P50(ms) | P95/90(ms) | P99(ms) | Transfer | Notes |",
        "| --- | --- | ---: | ---: | ---: | ---: | ---: | ---: | ---: | --- | --- |",
    ]
    for item in results:
        lines.append(
            f"| {item.tool} | {item.name} | {item.requests} | {item.failed} | "
            f"{item.qps:.2f} | {item.mean_ms:.2f} | {item.p50_ms:.2f} | "
            f"{item.p95_ms:.2f} | {item.p99_ms:.2f} | {item.transfer} | {item.notes} |"
        )

    lines.extend(
        [
            "",
            "## Seed Object",
            "",
            "```json",
            json.dumps(seed, indent=2),
            "```",
            "",
            "## Server Metrics Snapshot",
            "",
            "```json",
            json.dumps(metrics, indent=2, sort_keys=True),
            "```",
            "",
            "## Commands",
            "",
        ]
    )
    for item in results:
        lines.extend(
            [
                f"### {item.tool} {item.name}",
                "",
                "```bash",
                command_text(item.command),
                "```",
                "",
            ]
        )

    lines.extend(
        [
            "## Interpretation",
            "",
            "- `GET /health` reflects the networking, epoll, HTTP parsing, routing, and worker dispatch baseline.",
            "- `GET /objects/{id}` includes metadata lookup and object file read path.",
            "- `POST /objects/instant` validates SHA-256 lookup and SQLite metadata insertion without sending file content.",
            "- `POST /objects` sends an object body; repeated same-body requests also exercise the deduplication path.",
            "- When `object_size` is larger than `stream_upload_threshold_bytes`, upload cases also exercise the temporary-file streaming path.",
            "- Resource guards are enabled during benchmark startup, so the report records the tested admission-control and timeout configuration.",
            "- `ab` provides fixed request-count results; `wrk` provides fixed-duration latency distribution and throughput.",
            "",
            "## Limitations",
            "",
            "- This is a local WSL loopback benchmark; use it for regression comparison, not production capacity claims.",
            "- `ab` and `wrk` use different client models, so numbers should be compared within the same tool.",
            "- Repeated upload benchmarking uses the same body for each request, so deduplication affects write-path results.",
            "- Large full-object downloads use a lower wrk connection count because the current server sends responses synchronously after worker completion.",
            "- Later stages can add EPOLLOUT output buffers, longer duration tests, mixed traffic Lua scripts, flamegraphs, and memory profiling.",
            "",
            "## Raw Output",
            "",
        ]
    )
    for item in results:
        lines.extend(
            [
                f"### {item.tool} {item.name}",
                "",
                "```text",
                strip_trailing_whitespace(item.output),
                "```",
                "",
            ]
        )

    lines.extend(["## Server Output", "", "```text", strip_trailing_whitespace(server_stdout), "```"])
    if server_stderr.strip():
        lines.extend(["", "## Server Stderr", "", "```text", strip_trailing_whitespace(server_stderr), "```"])

    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text("\n".join(lines) + "\n", encoding="utf-8")


def tool_version(command: list[str]) -> str:
    try:
        completed = subprocess.run(command, check=False, text=True, capture_output=True)
        output = (completed.stdout + completed.stderr).strip().splitlines()
        return output[0] if output else ""
    except FileNotFoundError:
        return ""


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Run Mini-OSS HTTP benchmark with ab and wrk")
    parser.add_argument("--port", type=int, default=18081)
    parser.add_argument("--threads", type=int, default=4)
    parser.add_argument("--concurrency", type=int, default=16)
    parser.add_argument("--write-concurrency", type=int, default=4)
    parser.add_argument("--read-requests", type=int, default=120)
    parser.add_argument("--write-requests", type=int, default=40)
    parser.add_argument("--wrk-threads", type=int, default=2)
    parser.add_argument("--wrk-connections", type=int, default=12)
    parser.add_argument("--wrk-duration", default="3s")
    parser.add_argument("--wrk-timeout", default="10s")
    parser.add_argument("--object-size", type=int, default=65536)
    parser.add_argument("--max-request-bytes", type=int, default=4096)
    parser.add_argument("--max-upload-bytes", type=int, default=2 * 1024 * 1024)
    parser.add_argument("--stream-upload-threshold-bytes", type=int, default=1024)
    parser.add_argument("--max-connections", type=int, default=512)
    parser.add_argument("--thread-queue-limit", type=int, default=1024)
    parser.add_argument("--request-timeout-ms", type=int, default=5000)
    parser.add_argument("--upload-timeout-ms", type=int, default=30000)
    parser.add_argument("--report", type=Path, default=Path("docs/benchmark.md"))
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if shutil.which("ab") is None:
        print("ab not found; install apache2-utils first", file=sys.stderr)
        return 1
    if shutil.which("wrk") is None:
        print("wrk not found; install wrk first", file=sys.stderr)
        return 1
    if not Path("./build/mini_oss").exists():
        print("build/mini_oss not found; run cmake --build build first", file=sys.stderr)
        return 1

    storage_dir = Path("tmp/benchmark_storage")
    log_dir = Path("tmp/benchmark_logs")
    config_path = Path("tmp/benchmark_config.ini")
    upload_body_path = Path("tmp/benchmark_upload.bin")
    empty_body_path = Path("tmp/benchmark_empty.bin")
    shutil.rmtree(storage_dir, ignore_errors=True)
    shutil.rmtree(log_dir, ignore_errors=True)
    config_path.parent.mkdir(parents=True, exist_ok=True)
    if args.object_size > args.max_upload_bytes:
        print("object size must not exceed max upload bytes", file=sys.stderr)
        return 1
    upload_body = (b"mini-oss-upload-body-" * ((args.object_size // 21) + 1))[: args.object_size]
    upload_body_path.write_bytes(upload_body)
    empty_body_path.write_bytes(b"")
    write_config(
        config_path,
        args.port,
        args.threads,
        args.max_request_bytes,
        args.max_upload_bytes,
        args.stream_upload_threshold_bytes,
        args.max_connections,
        args.thread_queue_limit,
        args.request_timeout_ms,
        args.upload_timeout_ms,
    )

    proc = start_server(config_path, args.threads)
    server_stdout = ""
    server_stderr = ""
    try:
        wait_for_server(args.port, proc)
        seed = upload_seed_object(args.port, args.object_size)
        instant_script, upload_script = write_wrk_scripts(Path("tmp"), seed, upload_body)
        url_base = f"http://127.0.0.1:{args.port}"
        object_id = seed["id"]

        results = [
            run_ab_case(
                "GET /health",
                ab_base(args.read_requests, args.concurrency) + [f"{url_base}/health"],
                "fixed-count baseline",
            ),
            run_ab_case(
                "GET /objects/{id}",
                ab_base(args.read_requests, args.concurrency)
                + ["-H", AUTH_HEADER, f"{url_base}/objects/{object_id}"],
                "fixed-count metadata lookup + file read",
            ),
            run_ab_case(
                "GET /objects/{id} Range",
                ab_base(args.read_requests, args.concurrency)
                + [
                    "-H",
                    AUTH_HEADER,
                    "-H",
                    "Range: bytes=0-1023",
                    f"{url_base}/objects/{object_id}",
                ],
                "fixed-count partial object read",
            ),
            run_ab_case(
                "POST /objects/instant",
                ab_base(args.write_requests, args.write_concurrency)
                + [
                    "-p",
                    str(empty_body_path),
                    "-T",
                    "application/octet-stream",
                    "-H",
                    AUTH_HEADER,
                    "-H",
                    "X-Filename: ab-instant.bin",
                    "-H",
                    f"X-Object-Sha256: {seed['sha256']}",
                    "-H",
                    f"X-Object-Size: {seed['size']}",
                    f"{url_base}/objects/instant",
                ],
                "fixed-count SHA-256 lookup + metadata insert",
            ),
            run_ab_case(
                "POST /objects",
                ab_base(args.write_requests, args.write_concurrency)
                + [
                    "-p",
                    str(upload_body_path),
                    "-T",
                    "application/octet-stream",
                    "-H",
                    AUTH_HEADER,
                    "-H",
                    "X-Filename: ab-upload.bin",
                    f"{url_base}/objects",
                ],
                "fixed-count upload + SHA-256 + dedup",
            ),
            run_wrk_case(
                "GET /health",
                wrk_base(args) + [f"{url_base}/health"],
                "duration baseline",
            ),
            run_wrk_case(
                "GET /objects/{id}",
                wrk_download_base(args) + ["-H", AUTH_HEADER, f"{url_base}/objects/{object_id}"],
                "duration metadata lookup + full object read",
            ),
            run_wrk_case(
                "GET /objects/{id} Range",
                wrk_range_base(args)
                + [
                    "-H",
                    AUTH_HEADER,
                    "-H",
                    "Range: bytes=0-1023",
                    f"{url_base}/objects/{object_id}",
                ],
                "duration partial object read",
            ),
            run_wrk_case(
                "POST /objects/instant",
                wrk_base(args) + ["-s", str(instant_script), f"{url_base}/objects/instant"],
                "duration SHA-256 lookup + metadata insert",
            ),
            run_wrk_case(
                "POST /objects",
                wrk_base(args) + ["-s", str(upload_script), f"{url_base}/objects"],
                "duration upload + SHA-256 + dedup",
            ),
        ]
        metrics = fetch_metrics(args.port)
    except Exception as exc:
        server_stdout, server_stderr = stop_server(proc)
        print(f"benchmark failed: {exc}", file=sys.stderr)
        return 1
    finally:
        if proc.poll() is None:
            server_stdout, server_stderr = stop_server(proc)

    render_report(
        args.report,
        args,
        tool_version(["ab", "-V"]),
        tool_version(["wrk", "-v"]),
        seed,
        results,
        metrics,
        server_stdout,
        server_stderr,
    )
    print(f"benchmark report written to {args.report}")
    for item in results:
        print(
            f"{item.tool} {item.name}: requests={item.requests} failed={item.failed} "
            f"qps={item.qps:.2f} mean_ms={item.mean_ms:.2f} p99_ms={item.p99_ms:.2f}"
        )

    if any(item.failed != 0 for item in results):
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
