#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PID_FILE="${MINI_OSS_PID_FILE:-run/mini_oss.pid}"

if [[ ! -f "$PID_FILE" ]]; then
    echo "Mini-OSS pid file not found"
    exit 0
fi

pid="$(cat "$PID_FILE")"
if [[ -z "$pid" ]]; then
    rm -f "$PID_FILE"
    echo "Mini-OSS pid file was empty"
    exit 0
fi

if ! kill -0 "$pid" 2>/dev/null; then
    rm -f "$PID_FILE"
    echo "Mini-OSS is not running"
    exit 0
fi

kill "$pid"

for _ in {1..30}; do
    if ! kill -0 "$pid" 2>/dev/null; then
        rm -f "$PID_FILE"
        echo "Mini-OSS stopped"
        exit 0
    fi
    sleep 0.2
done

kill -9 "$pid" 2>/dev/null || true
rm -f "$PID_FILE"
echo "Mini-OSS force stopped"
