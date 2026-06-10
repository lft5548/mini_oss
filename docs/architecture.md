# Architecture

## Layered Design

```text
Client
  |
HTTP API
  |
HttpServer / Router
  |
Config / Logger / FileService / UserService / MetricsService
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
- Completed requests are dispatched to the worker thread pool.
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

### Object Store

- Save uploaded files.
- Generate object IDs.
- Support download, list, and delete.
- Verify SHA-256 integrity.
- Current MVP stores object files under `storage/objects` and persists metadata through SQLite.

### Metadata Store

- Store object metadata: id, filename, path, size, sha256, owner, created_at.
- Current implementation uses SQLite and stores metadata in `storage/metadata.db`.
- SQLite WAL mode is enabled for the local single-node metadata store.

### Config

- Load simple INI-style config files.
- Support `server`, `storage`, and `logging` sections.
- Command-line options override config-file values.
- Current configurable values: port, worker threads, storage directory, log directory, slow request threshold.

### Logging

- Thread-safe file logger.
- `access.log` records remote address, method, path, status code, response bytes, and latency.
- `error.log` records startup/shutdown events and server-side errors.
- `slow.log` records requests whose latency reaches the configured threshold.

### Metrics

- Total requests.
- Failed requests.
- Active connections.
- Average latency.
- Bytes uploaded/downloaded.

## MVP API Draft

```text
GET    /health
POST   /objects
GET    /objects
GET    /objects/{id}
DELETE /objects/{id}
GET    /metrics
```

## Implemented API

```text
GET /health
POST /objects
GET /objects
GET /objects/{id}
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
