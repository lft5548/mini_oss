# Mini-OSS: Linux C++ High-Concurrency Object Storage Service

Mini-OSS is a Linux C++ backend project designed for C++ backend/software engineer roles.
It implements a single-node object storage service with high-concurrency networking,
HTTP APIs, object metadata, file integrity verification, logging, monitoring, and pressure testing.

## Goals

- Build a Linux C++ backend service instead of a desktop demo.
- Practice TCP/IP, non-blocking IO, epoll/Reactor, thread pool, HTTP parsing, storage, logging, and metrics.
- Produce a project that can be explained clearly in interviews for C++ backend roles.

## Planned Tech Stack

- C++17
- Linux socket, non-blocking IO, epoll, Reactor
- Thread pool
- HTTP request parsing and response building
- SQLite first, MySQL later if needed
- OpenSSL SHA-256
- Redis metadata cache with SQLite fallback
- CMake
- curl, ab/wrk for testing and benchmarking

## Project Documents

- [Architecture](docs/architecture.md): system design, request lifecycle, threading model, storage design, resource guards, observability, testing, and interview explanation outline.
- [Requirements](docs/requirements.md): project scope, current status, limitations, and advanced roadmap.
- [Benchmark Report](docs/benchmark.md): repeatable ab/wrk benchmark commands, results, raw output, and metrics snapshot.
- [Deployment](docs/deployment.md): local scripts, Docker image, docker compose, runtime paths, and operational checks.
- [Interview Guide](docs/interview_guide.md): interview narrative, design tradeoffs, verification evidence, and common follow-up answers.

## Roadmap

1. MVP HTTP server: listen, accept, parse basic HTTP, return responses.
2. File service: upload, download, list, delete files.
3. Metadata: store object metadata in SQLite.
4. Reliability: SHA-256 verification, timeout cleanup, error handling.
5. Engineering: async-style logging, config file, metrics endpoint, benchmark report.
6. Extensions: token auth, instant upload by SHA-256, Redis metadata cache, chunk upload.

## Build

```bash
cmake -S . -B build
cmake --build build
```

## Run

```bash
cp config.example.ini config.ini
./build/mini_oss --config config.ini
```

Local script deployment:

```bash
./scripts/start.sh
./scripts/status.sh
./scripts/stop.sh
```

Docker deployment:

```bash
docker compose up --build
```

Command-line options override the config file:

```bash
./build/mini_oss --config config.ini --port 8080 --threads 4 \
  --storage-dir storage --log-dir logs --log-queue-limit 8192 \
  --slow-request-ms 200 \
  --max-request-bytes 10485760 --max-upload-bytes 134217728 \
  --stream-upload-threshold-bytes 1048576 --max-connections 1024 \
  --thread-queue-limit 1024 --request-timeout-ms 5000 \
  --upload-timeout-ms 30000 --auth-token dev-token \
  --redis-enabled true --redis-host 127.0.0.1 --redis-port 6379
```

The current MVP supports:

- `GET /health`
- `404 Not Found` for unknown GET paths
- `POST /objects` upload an object body
- `POST /objects/instant` create an object by existing SHA-256 metadata
- `GET /objects` list persisted object metadata
- `GET /objects/{id}` download an object
- `GET /objects/{id}` with `Range: bytes=start-end` download part of an object
- `DELETE /objects/{id}` delete an object
- `GET /metrics` expose runtime metrics
- Linux socket + non-blocking listening socket
- epoll event loop
- worker thread pool for HTTP request handling
- config file and command-line override support
- asynchronous access/error/slow request logs under configurable log directory
- optional Token authentication for object APIs
- SHA-256 object integrity metadata
- content deduplication and instant upload based on SHA-256
- CTest unit tests for HTTP parsing, routing, config parsing, object storage, deduplication, and Range download
- repeatable benchmark report generated with ab and wrk
- configurable upload size limits and stream-upload threshold
- large `POST /objects` request bodies streamed to `storage/tmp_uploads` before metadata processing
- large object downloads are sent through EPOLLOUT + sendfile instead of loading the full file into memory
- resource guards for max connections, worker queue length, slow request timeout, and slow upload timeout
- SQLite metadata persistence under `storage/metadata.db`
- optional Redis metadata cache for object lookup and SHA-256 dedup index lookup

## Tests

Mini-OSS uses CTest for C++ unit tests and keeps the Python smoke test for end-to-end HTTP workflow checks.

```bash
cmake -S . -B build
cmake --build build
ctest --test-dir build --output-on-failure
./scripts/smoke_test_http.py
```

Sanitizer quality gate:

```bash
cmake -S . -B build-sanitize -DCMAKE_BUILD_TYPE=Debug -DMINI_OSS_ENABLE_SANITIZERS=ON
cmake --build build-sanitize
ctest --test-dir build-sanitize --output-on-failure
MINI_OSS_BINARY=./build-sanitize/mini_oss ./scripts/smoke_test_http.py
MINI_OSS_BINARY=./build-sanitize/mini_oss ./scripts/smoke_test_redis_cache.py
```

## Smoke Test

```bash
./scripts/smoke_test_http.py
```

Expected checks:

- `/health` returns `HTTP/1.1 200 OK`
- `/not-found` returns `HTTP/1.1 404 Not Found`
- object upload/list/download/delete flow passes
- object metadata survives a service restart
- duplicate object bodies reuse the existing physical file
- instant upload creates metadata without resending object content
- shared physical files are removed only after the last object reference is deleted
- Range download returns `206 Partial Content` and invalid ranges return `416 Range Not Satisfiable`
- concurrent object uploads return unique persisted object IDs
- config file startup and command-line thread override work
- large upload can exceed the normal in-memory request limit and still complete through the streaming path
- temporary upload files are cleaned after success or deduplication
- slow/incomplete HTTP requests and uploads return `408 Request Timeout`
- connection overflow returns `503 Service Unavailable`
- `access.log`, `error.log`, and `slow.log` are generated
- `/metrics` exposes runtime counters for connections, status classes, resource rejections, timeouts, upload traffic, bytes, and latency
- CTest unit tests and a GitHub Actions workflow provide build, unit-test, and smoke-test quality gates
- ASan/UBSan CI checks catch memory errors and undefined behavior in C++ code paths
- Dockerfile, docker compose, and local start/stop/status scripts provide repeatable deployment paths
- object APIs return `401 Unauthorized` when auth token is configured and missing
- Redis cache smoke test validates cache hit/miss/error metrics and SQLite fallback

## Benchmark

Mini-OSS includes a repeatable benchmark driver based on ApacheBench and wrk. The script starts a local server, uploads small and large seed objects, runs read/write benchmark cases, probes Redis cache hit/miss behavior when enabled, fetches `/metrics`, and writes a Markdown report.

```bash
cmake --build build
redis-cli ping  # start redis-server first if this is not PONG
./scripts/benchmark_http.py --redis-enabled
```

The generated report is written to `docs/benchmark.md` and includes QPS, mean latency, percentile latency, failure count, transfer rate, Redis cache probe results, raw ab/wrk output, and a metrics snapshot. The default scenarios cover `/health`, small object download, large EPOLLOUT + sendfile download, Range download, instant upload, and streamed upload.

## Authentication

If `auth.token` or `--auth-token` is set, object APIs require either `Authorization: Bearer <token>` or `X-Auth-Token: <token>`. `GET /health` and `GET /metrics` remain public.

## Redis Metadata Cache

Redis is an optional acceleration layer for object metadata. SQLite remains the source of truth, so uploads, downloads, and deletes continue to work when Redis is disabled or temporarily unavailable.

Enable it in `config.ini`:

```ini
[redis]
enabled = true
host = 127.0.0.1
port = 6379
db = 0
key_prefix = mini_oss
ttl_seconds = 300
connect_timeout_ms = 100
io_timeout_ms = 100
```

Cached keys:

- `mini_oss:object:{id}` stores serialized object metadata.
- `mini_oss:sha:{sha256}:{size}` maps a content hash and size to an object id for deduplication and instant upload.

`GET /metrics` exposes `metadata_cache_hits`, `metadata_cache_misses`, and `metadata_cache_errors`.

Redis smoke test:

```bash
redis-cli ping
./scripts/smoke_test_redis_cache.py
```

## Large Uploads

`POST /objects` uses two upload paths:

- Small bodies are parsed into memory with the regular HTTP request path.
- Bodies larger than `stream_upload_threshold_bytes` are streamed to a temporary file under `storage/tmp_uploads` while the socket is being read.

After the upload completes, Mini-OSS computes SHA-256 from the temporary file in 64KB chunks, checks deduplication metadata, and either removes the temporary file for duplicate content or renames it into `storage/objects/{id}` for new content.

The related resource controls are:

- `max_request_bytes`: maximum size for regular in-memory HTTP requests.
- `max_upload_bytes`: maximum accepted object upload size.
- `stream_upload_threshold_bytes`: threshold for switching `POST /objects` to the file-backed upload path.

## Streaming Downloads

`GET /objects/{id}` and Range downloads use file-backed responses. `ObjectStore` opens the object file and returns a response descriptor containing the file descriptor, offset, and length. The epoll thread then sends headers and streams the file body with `EPOLLOUT` and `sendfile`.

This avoids loading large files into memory and keeps slow clients from blocking the event loop. The response owns the opened file descriptor, so an in-flight download can finish even if another request deletes the object's metadata and unlinks the file.

## Resource Guards

Mini-OSS exposes defensive limits that are common in backend services:

- `max_connections`: maximum active client connections accepted by the event loop.
- `thread_queue_limit`: maximum queued tasks waiting for worker threads. `0` means unlimited.
- `request_timeout_ms`: idle timeout for incomplete HTTP headers or normal request bodies. `0` disables it.
- `upload_timeout_ms`: idle timeout for file-backed large uploads. `0` disables it.

Timed-out clients receive `408 Request Timeout`; connection overload or a full worker queue returns `503 Service Unavailable`.


```bash
curl -i -X POST http://127.0.0.1:8080/objects \
  -H "Authorization: Bearer dev-token" \
  -H "X-Filename: hello.txt" \
  --data-binary "hello mini oss"
```

## Metrics

`GET /metrics` returns runtime service counters in JSON format, including active/peak/total connections, rejected connections, total requests, success/failure counts, HTTP status-class distribution, worker-queue rejections, request/upload timeouts, object upload/download counters, streamed upload/download bytes, Redis metadata cache hit/miss/error counters, async log dropped entries, request/response bytes, total latency, and average latency.

```bash
curl -i http://127.0.0.1:8080/metrics
```

## Logs

Mini-OSS writes logs asynchronously to the configured log directory. Request threads enqueue completed log lines and return quickly, while a background thread batches file writes and flushes during shutdown. `logging.queue_limit` or `--log-queue-limit` controls the in-memory queue; `0` means unlimited. When the queue is full, Mini-OSS drops new log entries, increments `log_dropped_entries` in `/metrics`, and writes a final drop summary to `error.log`.

Mini-OSS writes three log files:

- `access.log`: remote address, method, path, status code, response bytes, request latency.
- `error.log`: startup/shutdown events and server-side errors.
- `slow.log`: requests whose latency is greater than or equal to `slow_request_ms`.

## Example Object APIs

Start the server:

```bash
./build/mini_oss --port 8080
```

Upload:

```bash
curl -i -X POST http://127.0.0.1:8080/objects \
  -H "X-Filename: hello.txt" \
  --data-binary "hello mini oss"
```

Instant upload by SHA-256:

```bash
curl -i -X POST http://127.0.0.1:8080/objects/instant \
  -H "X-Filename: hello-copy.txt" \
  -H "X-Object-Sha256: f40026b1d6ecc4c661b3913acae18b8e19f4a223ab34f1d816caeeec497e2e93" \
  -H "X-Object-Size: 14"
```

List:

```bash
curl -i http://127.0.0.1:8080/objects
```

Download:

```bash
curl -i http://127.0.0.1:8080/objects/1
```

Range download:

```bash
curl -i http://127.0.0.1:8080/objects/1 \
  -H "Range: bytes=0-1023"
```

Delete:

```bash
curl -i -X DELETE http://127.0.0.1:8080/objects/1
```
