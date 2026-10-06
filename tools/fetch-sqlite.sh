#!/bin/sh
set -eu
URL="${SQLITE_URL:-https://www.sqlite.org/2026/sqlite-amalgamation-3530400.zip}"
cd "$(dirname "$0")/.."
mkdir -p build/deps
python3 - "$URL" <<'PYEOF'
import sys, urllib.request, zipfile, io, os
url = sys.argv[1]
with urllib.request.urlopen(url, timeout=120) as r:
    data = r.read()
z = zipfile.ZipFile(io.BytesIO(data))
names = z.namelist()
c = next(n for n in names if n.endswith("/sqlite3.c"))
h = next(n for n in names if n.endswith("/sqlite3.h"))
open("build/deps/sqlite3.c", "wb").write(z.read(c))
open("build/deps/sqlite3.h", "wb").write(z.read(h))
print("fetched", c, h)
PYEOF
test -f build/deps/sqlite3.c
test -f build/deps/sqlite3.h
