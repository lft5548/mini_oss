# Architecture

## Layered Design

```text
Client
  |
HTTP API
  |
HttpServer / Router
  |
Config / Auth / Logger / FileService / UserService / MetricsService
  |
MetadataStore / ObjectStore / Logger
  |
Linux File System / SQLite / Redis(optional)
```

## Modules

### Network

- Create listening socket.
- Set socket to non-blocking mode.
- Use epoll to monitor readable/writable events.
- Manage connection lifecycle.
- The epoll event loop accepts connections and reads complete HTTP requests.
- For large `POST /objects` bodies, the event loop parses headers first and streams the body into `storage/tmp_uploads` instead of keeping the whole body in memory.
- Completed file-backed upload requests pass a temporary file path to the worker, while small requests keep the existing in-memory body path.
- Completed requests are dispatched to the worker thread pool.
- The event loop enforces max active connections and periodically closes idle incomplete requests or uploads.
- Timeout cleanup reuses the connection lifecycle path, so unfinished upload temporary files are removed.
- Worker responses wake the event loop through eventfd, and the current version closes each HTTP connection after sending the response.

### HTTP

- Parse request line, headers, and body.
- Build HTTP responses.
- Route URL and method to service handlers.
- Current implementation supports one request per connection.

### Thread Pool

- Execute HTTP routing, object storage, and metadata tasks outside the epoll loop.
- Keep the IO event loop responsive while file and SQLite operations are running.
- Use a fixed worker count derived from hardware concurrency by default, with `--threads` for manual tuning.
- Bound the pending task queue with `thread_queue_limit`; when the queue is full, new requests fail fast with `503`.

### Object Store

- Save uploaded files.
- Generate object IDs.
- Support download, list, and delete.
- Verify SHA-256 integrity.
- Reuse existing physical files when uploaded content has the same SHA-256 and size.
- Support instant upload by creating new metadata for an existing SHA-256 without resending the file body.
- Support single-range object download with `206 Partial Content`, `Content-Range`, and `Accept-Ranges`.
- Delete metadata first and remove the physical file only when no remaining object references point to it.
- Large upload processing uses temporary files, incremental SHA-256 calculation, deduplication lookup, and atomic-style rename into the final object path.
- Duplicate large uploads remove the temporary body file after creating a new metadata alias.
- Failed or disconnected uploads clean temporary files through connection lifecycle cleanup.
- Current MVP stores object files under `storage/objects` and persists metadata through SQLite.

### Metadata Store

- Store object metadata: id, filename, path, size, sha256, owner, created_at.
- Current implementation uses SQLite and stores metadata in `storage/metadata.db`.
- SQLite WAL mode is enabled for the local single-node metadata store.
- `sha256 + size` and `path` indexes support dedup lookup and reference-count style cleanup.

### Auth

- Optional token authentication protects object APIs when an auth token is configured.
- Supports `Authorization: Bearer <token>` and `X-Auth-Token: <token>` headers.
- Public endpoints such as `/health` and `/metrics` bypass authentication.

### Config

- Load simple INI-style config files.
- Support `server`, `storage`, and `logging` sections.
- Command-line options override config-file values.
- Current configurable values: port, worker threads, storage directory, log directory, slow request threshold,
  connection limit, thread queue limit, request/upload timeout, in-memory request limit, upload size limit,
  stream-upload threshold, and auth token.

### Logging

- Thread-safe file logger.
- `access.log` records remote address, method, path, status code, response bytes, and latency.
- `error.log` records startup/shutdown events and server-side errors.
- `slow.log` records requests whose latency reaches the configured threshold.

### Metrics

- Thread-safe atomic counters for runtime service statistics.
- Count active/peak/total connections, rejected connections, total requests, success/failure requests, HTTP status classes, request bytes, response bytes, total latency, and average latency.
- Count worker-queue rejections, incomplete request timeouts, streaming upload timeouts, object upload requests, streamed upload requests, and uploaded bytes.
- `HttpServer` updates metrics on connection lifecycle, resource guard rejection, timeout, upload completion, and request completion, then exposes snapshots through `GET /metrics`.

### Benchmarking

- `scripts/benchmark_http.py` starts a local Mini-OSS instance and drives benchmark cases through ab and wrk.
- ab is used for fixed request-count tests, while wrk is used for fixed-duration throughput and latency distribution tests.
- The generated `docs/benchmark.md` records commands, QPS, mean latency, percentile latency, failure count, transfer rate, raw tool output, and a `/metrics` snapshot.

## MVP API Draft

```text
GET    /health
POST   /objects
POST   /objects/instant
GET    /objects
GET    /objects/{id}
GET    /objects/{id}  Range: bytes=start-end
DELETE /objects/{id}
GET /metrics
GET    /metrics
```

## Implemented API

```text
GET /health
POST /objects
POST /objects/instant
GET /objects
GET /objects/{id}
GET /objects/{id}  Range: bytes=start-end
DELETE /objects/{id}
```

Response:

```json
{"status":"ok"}
```

Object upload response:

```json
{
  "id": "1",
  "filename": "hello.txt",
  "size": 14,
  "sha256": "...",
  "created_at": "2026-06-10T10:30:58Z"
}
```
