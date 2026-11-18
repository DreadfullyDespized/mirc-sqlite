#!/bin/sh
set -eu
URL="${SQLITE_URL:-https://www.sqlite.org/2026/sqlite-amalgamation-3540000.zip}"
SHA3_256="${SQLITE_SHA3_256:-7b670a62fdfbd672b75fef004cb703c8a3e87d3a5cc7d675b4a08337004a2d93}"
cd "$(dirname "$0")/.."
mkdir -p build/deps
python3 - "$URL" "$SHA3_256" <<'PYEOF'
import sys, urllib.request, zipfile, io, hashlib
url = sys.argv[1]
want = sys.argv[2]
with urllib.request.urlopen(url, timeout=120) as r:
    data = r.read()
if hashlib.sha3_256(data).hexdigest() != want:
    sys.exit("sqlite amalgamation checksum mismatch")
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
