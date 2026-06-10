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
- Linux socket + non-blocking listening socket
- epoll event loop

## Smoke Test

```bash
./scripts/smoke_test_http.py
```

Expected checks:

- `/health` returns `HTTP/1.1 200 OK`
- `/not-found` returns `HTTP/1.1 404 Not Found`
