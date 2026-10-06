#!/bin/sh
set -eu
URL="${SQLITE_URL:-https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip}"
cd "$(dirname "$0")/.."
mkdir -p build/deps
tmp="$(mktemp -d)"
trap 'rm -rf "$tmp"' EXIT INT TERM
curl -fsSL -o "$tmp/am.zip" "$URL"
unzip -q -o "$tmp/am.zip" -d "$tmp"
cp "$tmp"/sqlite-amalgamation-*/sqlite3.c "$tmp"/sqlite-amalgamation-*/sqlite3.h build/deps/
test -f build/deps/sqlite3.c
test -f build/deps/sqlite3.h
