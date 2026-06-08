#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build/readest_hash"
BINARY="$BUILD_DIR/ReadestHashTest"

mkdir -p "$BUILD_DIR"

# partialMd5SampleRanges is defined inline in lib/ReadestSync/ReadestHash.h —
# the test only needs that header, no Arduino dependencies pulled in.
SOURCES=(
  "$ROOT_DIR/test/readest_hash/ReadestHashTest.cpp"
)

CXXFLAGS=(
  -std=c++20
  -O2
  -Wall
  -Wextra
  -pedantic
  -I"$ROOT_DIR"
)

c++ "${CXXFLAGS[@]}" "${SOURCES[@]}" -o "$BINARY"

"$BINARY" "$@"
