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
./build/mini_oss --config config.ini
```

The current MVP supports:

- `GET /health`
- `404 Not Found` for unknown GET paths
- `POST /objects` upload an object body
- `GET /objects` list persisted object metadata
- `GET /objects/{id}` download an object
- `DELETE /objects/{id}` delete an object
- Linux socket + non-blocking listening socket
- epoll event loop
- SHA-256 object integrity metadata
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

List:

```bash
curl -i http://127.0.0.1:8080/objects
```

Download:

```bash
curl -i http://127.0.0.1:8080/objects/1
```

Delete:

```bash
curl -i -X DELETE http://127.0.0.1:8080/objects/1
```
