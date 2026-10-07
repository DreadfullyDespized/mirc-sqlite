#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
if [ ! -f build/deps/sqlite3.c ] || [ ! -f build/deps/sqlite3.h ]; then
  echo "missing build/deps/sqlite3.c or sqlite3.h; run tools/fetch-sqlite.sh first" >&2
  exit 1
fi
mkdir -p build
i686-w64-mingw32-gcc -c -o build/sqlite3.o build/deps/sqlite3.c -Ibuild/deps -O2 \
  -DSQLITE_ENABLE_LOAD_EXTENSION -DSQLITE_ENABLE_COLUMN_METADATA
i686-w64-mingw32-gcc -c -o build/msqlite.o src/msqlite.c -Ibuild/deps -O2 -Wall
i686-w64-mingw32-gcc -shared -o build/msqlite.dll build/msqlite.o build/sqlite3.o src/msqlite.def \
  -s -static-libgcc
