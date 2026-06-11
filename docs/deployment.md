# Mini-OSS Deployment

This document describes two reproducible ways to run Mini-OSS: local script deployment and Docker deployment. Redis can be enabled as an optional metadata cache.

## Deployment Goals

- Keep build and runtime steps repeatable.
- Separate configuration, object data, and logs.
- Provide simple start/stop/status commands for local debugging.
- Provide Docker artifacts for containerized delivery.
- Make health check and smoke verification explicit.

## Local Script Deployment

Build and start the service:

```bash
cmake -S . -B build
cmake --build build
./scripts/start.sh
```

The start script:

- creates `config.ini` from `config.example.ini` if missing
- builds `build/mini_oss` if needed
- writes the process id to `run/mini_oss.pid`
- writes stdout/stderr to `run/mini_oss.out` and `run/mini_oss.err`

Check status:

```bash
./scripts/status.sh
```

Stop:

```bash
./scripts/stop.sh
```

Use a custom config:

```bash
MINI_OSS_CONFIG=config.example.ini ./scripts/start.sh
```

Run a quick health check:

```bash
curl -i http://127.0.0.1:8080/health
```

Run the full smoke test:

```bash
./scripts/smoke_test_http.py
```

Run the Redis cache smoke test when Redis is available:

```bash
redis-cli ping
./scripts/smoke_test_redis_cache.py
```

## Docker Image

Build the image:

```bash
docker build -t mini-oss:latest .
```

Run with named volumes:

```bash
docker run --rm --name mini-oss \
  -p 8080:8080 \
  -v mini_oss_storage:/data/storage \
  -v mini_oss_logs:/data/logs \
  mini-oss:latest
```

The image uses a multi-stage build:

- build stage installs compiler dependencies, builds `mini_oss`, and runs CTest
- runtime stage keeps only the binary, runtime libraries, curl for health check, and Docker config

Runtime paths:

```text
/app/config.ini
/data/storage
/data/logs
```

The Docker config uses `dev-token` for local object API authentication and enables Redis metadata cache with host `redis`:

```bash
curl -i -X POST http://127.0.0.1:8080/objects \
  -H "Authorization: Bearer dev-token" \
  -H "X-Filename: hello.txt" \
  --data-binary "hello mini oss"
```

## Docker Compose

Start:

```bash
docker compose up --build
```

Start in background:

```bash
docker compose up -d --build
```

Check health:

```bash
docker compose ps
curl -i http://127.0.0.1:8080/health
```

View logs:

```bash
docker compose logs -f mini-oss
docker compose logs -f redis
```

Stop:

```bash
docker compose down
```

Remove persisted data and logs:

```bash
docker compose down -v
```

## Configuration Strategy

`config/docker.ini` is the default container config:

- listens on `8080`
- stores objects under `/data/storage`
- writes logs under `/data/logs`
- enables token auth with `dev-token`
- enables Redis metadata cache with `redis:6379`

For real deployment, mount a custom config:

```bash
docker run --rm --name mini-oss \
  -p 8080:8080 \
  -v "$PWD/config/prod.ini:/app/config.ini:ro" \
  -v mini_oss_storage:/data/storage \
  -v mini_oss_logs:/data/logs \
  mini-oss:latest
```

## Operational Checks

Recommended checks after deployment:

```bash
curl -i http://127.0.0.1:8080/health
curl -i http://127.0.0.1:8080/metrics
redis-cli ping
```

For local non-container runs:

```bash
ctest --test-dir build --output-on-failure
./scripts/smoke_test_http.py
./scripts/smoke_test_redis_cache.py
./scripts/benchmark_http.py
```

## Interview Talking Points

- Docker multi-stage build separates compile-time dependencies from runtime dependencies.
- CTest runs during image build, so a broken core module fails before producing a deployable image.
- Storage and logs are mounted as volumes, which keeps runtime data outside the container layer.
- Health checks use `/health`, matching the service's public liveness endpoint.
- Startup scripts provide a lightweight local deployment path for debugging without Docker.
- Redis is treated as a cache, not the source of truth; SQLite fallback keeps object APIs available when Redis is unavailable.
