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
- `GET /objects`: done.
- `GET /objects/{id}`: done.
- `DELETE /objects/{id}`: done.
- SHA-256 object metadata: done.
- SQLite metadata persistence: done.

Known limitations before the next phase:

- File upload currently accepts the full HTTP body in memory.
- No user authentication yet.
- No thread pool yet; request handling still runs in the event loop.
- No SQLite connection pool yet; current version uses a single SQLite connection protected by a mutex.

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
- Benchmark report using ab or wrk.

## Non-Goals

- Do not build a full distributed object storage system at the beginning.
- Do not introduce Kubernetes, complex service discovery, or multi-node replication in MVP.
- Do not over-design before a stable single-node version is running.
