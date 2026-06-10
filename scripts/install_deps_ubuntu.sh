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
  curl

sudo apt install -y \
  libssl-dev \
  sqlite3 \
  libsqlite3-dev \
  apache2-utils \
  wrk

echo "Base dependencies installed."
echo "Optional later:"
echo "  sudo apt install -y redis-server"
echo "  sudo apt install -y mysql-server libmysqlclient-dev"
