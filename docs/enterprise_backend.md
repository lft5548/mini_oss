# Enterprise Backend Extension

This document records the RBAC, audit log, admin query, verification, and resume-facing scope added for enterprise internal-platform backend roles.

## Scope

Mini-OSS keeps the original Linux C++ high-concurrency object-storage core and adds a small enterprise backend layer:

- User login with PBKDF2-HMAC-SHA256 password verification.
- Role-based access control with `admin` and `user` roles.
- Object ownership checks for regular users.
- Operation audit logs for login, object APIs, and admin APIs.
- Admin statistics APIs for object totals, storage usage, upload/download counts, failed requests, Redis hit rate, and status-code distribution.
- Backward-compatible legacy token support for the original object API smoke and benchmark flows.

Default development accounts are initialized on first startup:

| Username | Password | Role |
| --- | --- | --- |
| `admin` | `admin123` | `admin` |
| `user` | `user123` | `user` |

These defaults are for local project demonstration only.

## Database Design

Object metadata is migrated in place:

```sql
objects(
  id INTEGER PRIMARY KEY,
  filename TEXT NOT NULL,
  path TEXT NOT NULL,
  size INTEGER NOT NULL,
  sha256 TEXT NOT NULL,
  created_at TEXT NOT NULL,
  owner_user_id INTEGER NOT NULL DEFAULT 0,
  upload_count INTEGER NOT NULL DEFAULT 1,
  download_count INTEGER NOT NULL DEFAULT 0
)
```

RBAC and sessions:

```sql
users(id, username, password_hash, password_salt, status, created_at, updated_at, last_login_at)
roles(id, role_name, description, created_at)
user_roles(user_id, role_id, created_at)
auth_sessions(token, user_id, created_at, expires_at)
```

Audit logs:

```sql
audit_logs(
  id, request_id, user_id, username, method, path, action, file_id,
  status_code, result, error_message, client_ip, latency_ms, created_at
)
```

Indexes cover object owner lookup, SHA-256 dedup lookup, audit query by user/action/time, and file-id tracing.

## API Reference

### Login

```http
POST /auth/login
Content-Length: ...

{"username":"admin","password":"admin123"}
```

Response:

```json
{
  "token": "<token>",
  "token_type": "Bearer",
  "expires_in": 86400,
  "user": {
    "id": 1,
    "username": "admin",
    "roles": ["admin"]
  }
}
```

### Object APIs

Existing object APIs now accept either:

- `Authorization: Bearer <login-token>`
- `Authorization: Bearer <legacy-auth-token>`
- `X-Auth-Token: <legacy-auth-token>`

Regular users can list, download, and delete their own objects. Admin and legacy-token requests can access all objects.

### Admin APIs

All `/admin/*` APIs require an admin login token.

| Method | Path | Purpose |
| --- | --- | --- |
| `GET` | `/admin/users` | List users and roles |
| `PUT` | `/admin/users/{id}/roles` | Update a user role with JSON `{"role":"admin"}` or `{"role":"user"}` |
| `GET` | `/admin/audit-logs?limit=20&offset=0&user_id=1&action=upload_object` | Query audit logs |
| `GET` | `/admin/stats/overview` | Object, request, Redis, and status-code overview |
| `GET` | `/admin/stats/status-codes` | Exact HTTP status-code distribution |
| `GET` | `/admin/stats/redis` | Redis hit/miss/error counters and hit rate |

Example stats response:

```json
{
  "file_total": 13,
  "storage_bytes": 65686,
  "upload_count": 13,
  "download_count": 6,
  "failed_request_count": 7,
  "redis_hit_count": 0,
  "redis_miss_count": 0,
  "redis_error_count": 0,
  "redis_hit_rate": 0.0,
  "status_codes": {
    "200": 7,
    "201": 13,
    "206": 4,
    "401": 1,
    "403": 1
  }
}
```

## Deployment

Build and run:

```bash
cmake -S . -B build
cmake --build build
cp config.example.ini config.ini
./build/mini_oss --config config.ini
```

Local scripts still work:

```bash
./scripts/start.sh
./scripts/status.sh
./scripts/stop.sh
```

Docker deployment remains unchanged:

```bash
docker compose up --build
```

## Verification

Verified locally on 2026-06-27:

```bash
cmake --build build
ctest --test-dir build --output-on-failure
./scripts/smoke_test_http.py
```

Results:

- C++ unit tests passed.
- HTTP smoke test passed.
- Smoke coverage includes admin/user login, ordinary-user `403` on admin stats, admin user list, admin overview stats, status-code stats, audit-log query, object upload/download/list/delete, Range download, resource guards, and metrics.

## Admin API Benchmark

Command:

```bash
wrk -t1 -c4 -d3s -H "Authorization: Bearer <admin-token>" \
  http://127.0.0.1:18083/admin/stats/overview
```

Local result on 2026-06-27:

```text
Running 3s test @ http://127.0.0.1:18083/admin/stats/overview
  1 threads and 4 connections
  Thread Stats   Avg      Stdev     Max   +/- Stdev
    Latency    18.26ms   11.16ms  77.07ms   76.17%
    Req/Sec   228.77     91.99   333.00     56.67%
  685 requests in 3.01s, 206.60KB read
Requests/sec:    227.87
Transfer/sec:     68.72KB
```

Metrics snapshot after the benchmark:

```json
{
  "file_total": 5,
  "storage_bytes": 30,
  "upload_count": 5,
  "download_count": 0,
  "failed_request_count": 0,
  "redis_hit_count": 0,
  "redis_miss_count": 0,
  "redis_error_count": 0,
  "redis_hit_rate": 0.0,
  "status_codes": {
    "200": 687,
    "201": 5
  }
}
```

## Resume Wording

Project title:

> Mini-OSS: Linux C++ High-Concurrency Object Storage Service with Admin Backend Capabilities

Resume bullets:

- Built a Linux C++ HTTP object-storage service with non-blocking sockets, epoll, worker thread pool, SQLite metadata, Redis cache-aside lookup, SHA-256 deduplication, streamed upload, Range download, and EPOLLOUT + sendfile download.
- Extended the service with enterprise-backend modules: user login, role-based access control, admin/user permission separation, object ownership checks, and backward-compatible token authentication.
- Designed SQLite tables for users, roles, user-role relations, sessions, object ownership counters, and audit logs; added admin APIs for audit-log query, file totals, storage usage, upload/download counts, failed requests, Redis hit rate, and HTTP status-code statistics.
- Added CTest unit coverage and Python smoke tests for login, RBAC, audit query, admin statistics, object workflow, resource guards, and metrics; recorded wrk benchmark evidence for the admin statistics endpoint.
