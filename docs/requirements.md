# Requirements

## Project Name

Linux C++ High-Concurrency Object Storage Service

## Positioning

This project targets formal C++ backend/software engineer roles. It should demonstrate:

- Linux C++ server-side development
- TCP/IP and HTTP protocol handling
- High-concurrency network programming
- Multi-threaded task processing
- Object/file storage service design
- Database metadata management
- Reliability, observability, and benchmark awareness

## MVP Scope

The first version should support:

- Start a TCP server on a configurable port.
- Parse basic HTTP requests.
- Provide a health check endpoint.
- Upload a file.
- Download a file.
- List stored files.
- Delete a file.
- Log access and error events.

Current MVP status:

- Server startup on configurable port: done.
- Basic HTTP GET parsing: done.
- `GET /health`: done.
- 404 response for unknown GET paths: done.
- `POST /objects`: done.
- `POST /objects/instant`: done.
- `GET /objects`: done.
- `GET /objects/{id}`: done.
- `GET /objects/{id}` Range download: done.
- `DELETE /objects/{id}`: done.
- SHA-256 object metadata: done.
- SHA-256 based deduplication and instant upload: done.
- Shared object reference cleanup: done.
- SQLite metadata persistence: done.
- Thread pool request processing: done.
- Config file parsing and command-line override: done.
- Access/error/slow request logs: done.
- Asynchronous bounded logging with drop counters: done.
- `/metrics` runtime monitoring endpoint: done.
- Resource and status-class metrics: done.
- Token authentication for object APIs: done.
- Benchmark script and ab/wrk report: done.
- CTest unit tests and GitHub Actions CI workflow: done.
- Large upload streaming with temporary files and incremental SHA-256: done.
- Resource guards for connection count, worker queue length, request timeout, and upload timeout: done.

Known limitations before the next phase:

- Metrics are in-process counters and reset after service restart.
- Small requests are still buffered in memory; large `POST /objects` uploads use a file-backed streaming path.
- Token auth is static shared-token auth; later versions can add users, roles, or signed URLs.
- Deduplication is single-node metadata deduplication; later versions can add content-addressed storage layout and garbage collection jobs.
- No SQLite connection pool yet; current version uses a single SQLite connection protected by a mutex.
- Large response sending still happens in the event loop; later versions can add EPOLLOUT output buffers.
- No Redis cache yet; later versions can cache object metadata and SHA-256 dedup indexes with SQLite fallback.
- No Docker deployment artifact yet; later versions can add Dockerfile, docker-compose, startup scripts, and deployment docs.

## Advanced Scope

After MVP, add:

- epoll + non-blocking IO + Reactor event loop.
- Thread pool for request processing.
- SHA-256 file integrity verification.
- SQLite metadata storage.
- Token-based authentication.
- Instant upload based on SHA-256.
- Chunked upload and merge.
- Range download.
- Metrics endpoint: QPS, active connections, error count, average latency.
- Benchmark report using ab and wrk.

## Non-Goals

- Do not build a full distributed object storage system at the beginning.
- Do not introduce Kubernetes, complex service discovery, or multi-node replication in MVP.
- Do not over-design before a stable single-node version is running.
