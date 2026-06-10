# Architecture

## Layered Design

```text
Client
  |
HTTP API
  |
HttpServer / Router
  |
FileService / UserService / MetricsService
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
- Current MVP uses a single epoll event loop and closes each HTTP connection after sending the response.

### HTTP

- Parse request line, headers, and body.
- Build HTTP responses.
- Route URL and method to service handlers.

### Thread Pool

- Execute request handling tasks.
- Separate IO event loop from CPU/file/database work.

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

### Logging

- Access log.
- Error log.
- Slow request log.

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
