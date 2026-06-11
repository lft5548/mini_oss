#!/usr/bin/env bash
set -Eeuo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
cd "$ROOT_DIR"

CONFIG_PATH="${MINI_OSS_CONFIG:-config.ini}"
PID_FILE="${MINI_OSS_PID_FILE:-run/mini_oss.pid}"
STDOUT_LOG="${MINI_OSS_STDOUT:-run/mini_oss.out}"
STDERR_LOG="${MINI_OSS_STDERR:-run/mini_oss.err}"

if [[ ! -f "$CONFIG_PATH" ]]; then
    mkdir -p "$(dirname "$CONFIG_PATH")"
    cp config.example.ini "$CONFIG_PATH"
fi

if [[ ! -x build/mini_oss ]]; then
    cmake -S . -B build
    cmake --build build
fi

mkdir -p "$(dirname "$PID_FILE")" "$(dirname "$STDOUT_LOG")" "$(dirname "$STDERR_LOG")"

if [[ -f "$PID_FILE" ]]; then
    old_pid="$(cat "$PID_FILE")"
    if [[ -n "$old_pid" ]] && kill -0 "$old_pid" 2>/dev/null; then
        echo "Mini-OSS is already running with pid $old_pid"
        exit 0
    fi
fi

nohup ./build/mini_oss --config "$CONFIG_PATH" "$@" >"$STDOUT_LOG" 2>"$STDERR_LOG" &
pid="$!"
echo "$pid" > "$PID_FILE"

echo "Mini-OSS started with pid $pid"
echo "Config: $CONFIG_PATH"
echo "Stdout: $STDOUT_LOG"
echo "Stderr: $STDERR_LOG"
