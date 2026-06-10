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
- Redis cache as an optional extension
- CMake
- curl, ab/wrk for testing and benchmarking

## Roadmap

1. MVP HTTP server: listen, accept, parse basic HTTP, return responses.
2. File service: upload, download, list, delete files.
3. Metadata: store object metadata in SQLite.
4. Reliability: SHA-256 verification, timeout cleanup, error handling.
5. Engineering: async-style logging, config file, metrics endpoint, benchmark report.
6. Extensions: token auth, instant upload by SHA-256, chunk upload, Redis cache.

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

Command-line options override the config file:

```bash
./build/mini_oss --config config.ini --port 8080 --threads 4 \
  --storage-dir storage --log-dir logs --slow-request-ms 200 \
  --auth-token dev-token
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
- access/error/slow request logs under configurable log directory
- optional Token authentication for object APIs
- SHA-256 object integrity metadata
- content deduplication and instant upload based on SHA-256
- repeatable benchmark report generated with ab and wrk
- SQLite metadata persistence under `storage/metadata.db`

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
- `access.log`, `error.log`, and `slow.log` are generated
- `/metrics` exposes runtime counters for requests, status results, bytes, latency, and active connections
- object APIs return `401 Unauthorized` when auth token is configured and missing

## Benchmark

Mini-OSS includes a repeatable benchmark driver based on ApacheBench and wrk. The script starts a local server, uploads a seed object, runs read/write benchmark cases, fetches `/metrics`, and writes a Markdown report.

```bash
cmake --build build
./scripts/benchmark_http.py
```

The generated report is written to `docs/benchmark.md` and includes QPS, mean latency, percentile latency, failure count, transfer rate, raw ab/wrk output, and a metrics snapshot.

## Authentication

If `auth.token` or `--auth-token` is set, object APIs require either `Authorization: Bearer <token>` or `X-Auth-Token: <token>`. `GET /health` and `GET /metrics` remain public.

```bash
curl -i -X POST http://127.0.0.1:8080/objects \
  -H "Authorization: Bearer dev-token" \
  -H "X-Filename: hello.txt" \
  --data-binary "hello mini oss"
```

## Metrics

`GET /metrics` returns runtime service counters in JSON format, including total requests, success/failure counts, active connections, request/response bytes, total latency, and average latency.

```bash
curl -i http://127.0.0.1:8080/metrics
```

## Logs

Mini-OSS writes logs to the configured log directory:

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
