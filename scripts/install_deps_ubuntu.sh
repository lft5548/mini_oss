#!/usr/bin/env bash
set -euo pipefail

sudo apt update

sudo apt install -y \
  build-essential \
  cmake \
  git \
  gdb \
  valgrind \
  pkg-config \
  curl \
  redis-server \
  redis-tools

sudo apt install -y \
  libssl-dev \
  sqlite3 \
  libsqlite3-dev \
  libhiredis-dev \
  apache2-utils \
  wrk

echo "Base dependencies installed."
echo "Optional later:"
echo "  sudo apt install -y mysql-server libmysqlclient-dev"
