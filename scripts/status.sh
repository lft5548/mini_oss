#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

PID_FILE="${MINI_OSS_PID_FILE:-run/mini_oss.pid}"

if [[ ! -f "$PID_FILE" ]]; then
    echo "Mini-OSS is not running"
    exit 1
fi

pid="$(cat "$PID_FILE")"
if [[ -n "$pid" ]] && kill -0 "$pid" 2>/dev/null; then
    echo "Mini-OSS is running with pid $pid"
    exit 0
fi

echo "Mini-OSS pid file exists, but process is not running"
exit 1
