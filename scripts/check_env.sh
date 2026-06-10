#!/usr/bin/env bash
set -euo pipefail

tools=(
  gcc
  g++
  cmake
  make
  git
  gdb
  valgrind
  curl
  openssl
  sqlite3
  ab
  wrk
  pkg-config
)

for tool in "${tools[@]}"; do
  if command -v "${tool}" >/dev/null 2>&1; then
    printf "%-12s OK (%s)\n" "${tool}" "$(command -v "${tool}")"
  else
    printf "%-12s MISSING\n" "${tool}"
  fi
done
