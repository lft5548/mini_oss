# Mini-OSS Architecture

Mini-OSS is a single-node Linux C++ object storage service. The project is intentionally designed as an interview-ready backend system rather than a feature-only demo: each module has a clear responsibility, observable behavior, resource boundary, and verification method.

## Design Goals

- Build a Linux C++ backend service with non-blocking network IO and worker-thread request processing.
- Support common object-storage workflows: upload, list, download, Range download, delete, deduplication, and instant upload.
- Keep the service stable under imperfect clients through connection limits, bounded queues, request timeouts, upload timeouts, and temporary-file cleanup.
- Make runtime behavior explainable through access logs, slow logs, error logs, `/metrics`, smoke tests, unit tests, and benchmark reports.
- Keep the current version single-node and understandable, while providing reproducible deployment artifacts and Redis metadata cache integration, and leaving clear extension points for EPOLLOUT response buffers and future distributed storage features.

## High-Level Architecture

```text
Client / curl / ab / wrk
        |
        v
Linux socket + non-blocking fd + epoll
        |
        v
HttpServer
  - accept/read
  - connection lifecycle
  - timeout scan
  - large-upload streaming
  - auth gate
        |
        v
Router + worker ThreadPool
        |
        v
ObjectStore
  - upload/download/list/delete
  - SHA-256
  - dedup/instant upload
  - Range download
        |
        v
Redis Metadata Cache(optional)
        |
        | miss/fallback
        v
MetadataStore(SQLite WAL) + Linux File System

Side channels:
  Config -> HttpServer / ObjectStore / Logger
  Async Logger -> access.log / error.log / slow.log
  Metrics -> GET /metrics
  Tests -> CTest + smoke_test_http.py
  Benchmark -> ab/wrk report
```

The service separates IO, business logic, persistence, and observability:

- The epoll loop is responsible for accepting connections and reading request bytes.
- Worker threads handle HTTP routing, file operations, SHA-256 calculation, and SQLite metadata operations.
- Object files are stored in `storage/objects`, while metadata is stored in SQLite.
- Logs and metrics are maintained outside the business path as much as possible.

## Request Lifecycle

### Normal HTTP Request

```text
1. client connects
2. epoll detects readable fd
3. HttpServer reads bytes into connection buffer
4. HTTP parser validates request line, headers, and Content-Length
5. auth check runs before object API routing
6. request is submitted to ThreadPool
7. Router dispatches to ObjectStore
8. ObjectStore reads/writes file system and SQLite metadata
9. worker serializes HttpResponse
10. eventfd wakes epoll loop
11. epoll loop sends response and closes connection
12. access log, slow log, and metrics are updated
```

The current implementation supports one request per connection. This keeps the server easier to reason about and avoids persistent-connection state complexity while the project focuses on backend fundamentals.

### Large Upload Request

Small request bodies are buffered in memory. Large `POST /objects` bodies switch to a file-backed path:

```text
1. epoll loop parses headers first
2. if Content-Length > stream_upload_threshold_bytes, create temp file
3. incoming body chunks are written to storage/tmp_uploads
4. upload timeout protects slow or stalled clients
5. completed temp file is submitted to worker thread
6. ObjectStore calculates SHA-256 incrementally in 64KB chunks
7. ObjectStore checks dedup metadata by sha256 + size
8. duplicate upload creates metadata alias and deletes temp file
9. new upload renames temp file into storage/objects/{id}
10. failed/disconnected upload is cleaned through connection cleanup
```

This design avoids keeping large request bodies in memory and gives a clear story around memory control, temporary-file lifecycle, and failure cleanup.

### Download and Range Download

`GET /objects/{id}` uses metadata to locate the physical object file. `ObjectStore` validates the object id, opens the file while holding the store lock, and returns a file-backed HTTP response descriptor instead of reading the full file into memory. The epoll thread then sends headers plus file bytes through `EPOLLOUT` and `sendfile`.

Normal download returns the full file with `Accept-Ranges: bytes`. A valid single-range request returns:

- `206 Partial Content`
- `Content-Range: bytes start-end/total`
- `Accept-Ranges: bytes`

Invalid ranges return `416 Range Not Satisfiable` with `Content-Range: bytes */total`.

Range support is useful because it demonstrates protocol detail, partial file reads, and object-service behavior beyond simple upload/download.

The file descriptor is owned by the response object. Even if another request deletes the object metadata and unlinks the file, Linux keeps the opened file valid until the download response is released. This keeps in-flight downloads stable while still allowing normal delete semantics.

Download send path:

```text
1. worker thread builds HttpResponse with opened file fd, offset, and length
2. response is pushed to the epoll thread through eventfd
3. epoll thread registers client fd for EPOLLOUT
4. response headers are sent first
5. file body is sent in bounded chunks with sendfile
6. EAGAIN/EWOULDBLOCK pauses sending until the next writable event
7. metrics and access logs are recorded after the response is fully sent
```

This avoids loading large downloads into memory and prevents slow clients from blocking the entire event loop.

## Threading Model

```text
Main thread
  |
  +-- epoll event loop
  |     - accept
  |     - read
  |     - timeout scan
  |     - streaming upload writes
  |     - response send
  |     - EPOLLOUT file download streaming
  |
  +-- worker ThreadPool
  |     - route request
  |     - SQLite operations
  |     - file read/write
  |     - SHA-256
  |
  +-- async logger thread
        - batch log writes
        - flush on shutdown
```

Important design choices:

- The epoll loop avoids blocking business work by pushing completed requests to the worker pool.
- The worker queue is bounded by `thread_queue_limit`; queue overflow fails fast with `503`.
- Large uploads are streamed while reading the socket, but CPU-heavy and metadata-heavy processing still happens in worker threads after the file is complete.
- Large downloads are represented as file-backed responses and streamed by the epoll loop using `EPOLLOUT` and `sendfile`.
- Logging is asynchronous so request threads do not directly pay file flush latency.

## Storage Design

### Physical Layout

```text
storage/
  metadata.db
  objects/
    1
    2
  tmp_uploads/
    upload_*.tmp
```

### Metadata Schema

SQLite stores:

- `id`: object id
- `filename`: sanitized original filename
- `path`: physical file path
- `size`: object size
- `sha256`: content hash
- `created_at`: creation time

Indexes:

- `sha256 + size`: dedup and instant upload lookup
- `path`: reference-count cleanup for shared physical files

SQLite WAL mode is enabled to improve local single-node metadata behavior. The current implementation uses one SQLite connection protected by a mutex; this is simple and stable for a single-node project. A future version can add a connection pool or move metadata to MySQL/PostgreSQL.

### Deduplication and Instant Upload

Mini-OSS deduplicates by `(sha256, size)`:

- If uploaded content is new, it writes a physical file and inserts metadata.
- If content already exists, it inserts a new metadata row pointing to the existing physical file.
- Instant upload skips body transfer and creates metadata only when the client provides a known SHA-256 and size.
- Delete removes metadata first and deletes the physical file only when no metadata rows reference it.

This design is easy to explain: metadata can have multiple logical objects pointing to one physical file.

### Redis Metadata Cache

Redis is an optional cache in front of SQLite metadata reads. SQLite remains the authoritative store.

Cached keys:

- `prefix:object:{id}` stores serialized `ObjectInfo` for download and delete lookup.
- `prefix:sha:{sha256}:{size}` stores an object id for deduplication and instant upload lookup.

Read strategy:

```text
1. try Redis object/SHA index
2. on hit, return cached metadata
3. on miss or Redis error, query SQLite
4. when SQLite succeeds, backfill Redis
```

Write/delete strategy:

- New object metadata is inserted into SQLite first, then written to Redis.
- Delete removes SQLite metadata first, then invalidates object and SHA index keys.
- Cache keys use TTL, so a failed invalidation has a bounded stale window.
- If Redis is down, the request still succeeds through SQLite and increments cache error metrics.

This gives a clear backend tradeoff: correctness depends on SQLite, while Redis improves hot metadata and dedup lookup latency.

## API Surface

```text
GET    /health
GET    /metrics
POST   /objects
POST   /objects/instant
GET    /objects
GET    /objects/{id}
GET    /objects/{id} Range: bytes=start-end
DELETE /objects/{id}
```

Auth behavior:

- `/health` and `/metrics` are public.
- Object APIs require a token when `auth.token` or `--auth-token` is configured.
- Supported headers: `Authorization: Bearer <token>` and `X-Auth-Token: <token>`.

## Resource Guards

Mini-OSS implements defensive limits commonly seen in backend services:

| Guard | Config | Behavior |
| --- | --- | --- |
| Max connections | `max_connections` | Reject overflow with `503` |
| Worker queue limit | `thread_queue_limit` | Fail fast with `503` when worker backlog is full |
| Request timeout | `request_timeout_ms` | Close incomplete header/body requests with `408` |
| Upload timeout | `upload_timeout_ms` | Close stalled file-backed uploads with `408` |
| In-memory request limit | `max_request_bytes` | Reject oversized normal requests with `413` |
| Upload limit | `max_upload_bytes` | Reject oversized object uploads with `413` |
| Log queue limit | `logging.queue_limit` | Drop and count log entries instead of blocking request threads |

The project exposes resource guard events through logs and `/metrics`, so overload behavior is visible instead of silent.

## Observability

### Logs

The logger writes three files:

- `access.log`: remote address, method, path, status, bytes, latency
- `error.log`: startup/shutdown, server errors, async logger drop summary
- `slow.log`: requests whose latency reaches `slow_request_ms`

The logger uses a bounded queue and a background thread. Queue overflow uses drop-and-count instead of blocking request threads. Dropped entries are exposed as `log_dropped_entries` in `/metrics`.

### Metrics

`GET /metrics` returns JSON counters such as:

- active, peak, total, and rejected connections
- total, success, and failed requests
- HTTP status-class counters
- worker queue rejections
- request and upload timeouts
- object upload and streamed upload counters
- uploaded bytes and streamed uploaded bytes
- file download and streamed downloaded byte counters
- metadata cache hits, misses, and errors
- request and response bytes
- total and average latency
- async log dropped entries

Metrics are in-process counters and reset when the service restarts. This is acceptable for the current single-node project; later versions can export Prometheus-style metrics or persist aggregates.

## Testing Strategy

Mini-OSS uses layered verification:

```text
CTest unit tests
  - HTTP parser
  - response serialization
  - router matching
  - config parsing
  - object upload/dedup/range/delete

Python smoke test
  - real local server process
  - socket IO
  - auth
  - object workflow
  - metadata restart persistence
  - large upload streaming
  - resource guards
  - logs and metrics

Sanitizer CI
  - AddressSanitizer for memory safety issues
  - UndefinedBehaviorSanitizer for undefined behavior
  - unit tests and smoke tests run against the sanitizer-built binary

Benchmark script
  - ab fixed-count tests
  - wrk fixed-duration tests
  - generated docs/benchmark.md
```

The unit-test stage found and fixed a real route-boundary bug: `/objects/` should not match the `/objects/{id}` prefix route. This is a useful interview point because it shows the tests are not decorative; they catch protocol-edge regressions.

## Benchmarking Strategy

`scripts/benchmark_http.py` starts a local server and runs:

- `GET /health`
- `GET /objects/{id}`
- `GET /objects/{id}` with Range
- `POST /objects/instant`
- `POST /objects`

The generated report records:

- requests
- failed requests
- QPS
- mean latency
- percentile latency
- transfer rate
- raw ab/wrk output
- `/metrics` snapshot

The benchmark is local WSL loopback, so it should be used for regression comparison rather than production capacity claims.

## Design Tradeoffs

### Why epoll + thread pool?

The epoll loop handles connection readiness efficiently, while worker threads isolate blocking file and SQLite work. This is a common backend pattern: IO multiplexing for connection scale, worker pool for CPU/blocking tasks.

### Why SQLite first?

SQLite keeps the project deployable and easy to test while still demonstrating metadata persistence, indexes, WAL mode, and query-based dedup. It is enough for a single-node object store. The metadata layer is separated, so it can later move to MySQL/PostgreSQL.

### Why file-backed large uploads?

Reading entire upload bodies into memory is simple but unsafe for large files. The stream-to-temp-file path controls memory, supports cleanup on timeout/disconnect, and still lets ObjectStore calculate SHA-256 incrementally after upload completion.

### Why async logging?

Synchronous log writes add IO latency to the request path. Async logging decouples request handling from disk flushes. A bounded queue prevents memory blowup; drop counters and final summaries preserve observability.

### Why fail fast on overload?

When connection count or worker queue limits are reached, returning `503` is clearer than letting latency grow without bound. This makes overload behavior predictable and measurable.

## Current Limitations

- One request per connection; keep-alive and pipelining are not implemented.
- Large response sending still happens synchronously after worker completion; EPOLLOUT output buffers can improve this.
- SQLite uses one mutex-protected connection; a connection pool or external metadata database can improve concurrency.
- Token auth is static shared-token auth; users, roles, signed URLs, or JWT can be added later.
- Metrics are in-process counters and reset on restart.
- Object storage is single-node; no replication, erasure coding, compaction, or background garbage collection.

## Planned Extensions

Recommended next phases:

1. **Redis metadata cache**
   - cache `object:meta:{id}`
   - cache `object:sha:{sha256}:{size}`
   - fallback to SQLite when Redis is unavailable
   - invalidation on delete
   - metrics: hits, misses, errors

2. **Network output buffer**
   - EPOLLOUT-driven response writing
   - avoid blocking the event loop during large response send

## Interview Explanation Outline

When explaining the project, use this order:

1. Mini-OSS is a Linux C++ single-node object storage service.
2. Network layer uses non-blocking socket + epoll; worker pool handles blocking tasks.
3. Object layer supports upload, download, Range, delete, SHA-256, dedup, and instant upload.
4. Metadata is stored in SQLite with indexes for dedup and reference cleanup.
5. Large uploads are streamed to temp files to avoid high memory usage.
6. Resource guards protect connection count, worker backlog, request timeout, upload timeout, and log queue size.
7. Observability includes async logs, `/metrics`, slow logs, benchmark reports, and test results.
8. Engineering quality is shown through CTest, smoke tests, GitHub Actions, and ab/wrk benchmarks.
9. Current deployment is covered by local scripts and Docker artifacts; future extensions are Redis metadata cache and EPOLLOUT output buffers.
