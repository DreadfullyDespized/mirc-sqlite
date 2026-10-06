#!/bin/bash
set -euo pipefail
[ $# -eq 2 ] || { echo "usage: diff.sh old-proof.log new-proof.log" >&2; exit 2; }
[ -f "$1" ] || { echo "missing: $1" >&2; exit 2; }
[ -f "$2" ] || { echo "missing: $2" >&2; exit 2; }
norm() {
  {
    grep -v '^STATUS ' "$1" | sed -E \
      -e 's/^(BEGIN variant=)[^ ]*/\1?/' \
      -e 's/ (dllver|libver)=[^ ]*/ \1=?/g' \
      -e 's/^(PROBE [^ ]+).*/\1/' \
      -e 's/^BUSY badid=.*$/BUSY badid=?/' \
      -e 's/ errstr=.*$/ errstr=?/'
    printf 'STATUS-COUNT %s\n' "$(grep -c '^STATUS ' "$1" || true)"
  }
}
N1=$(mktemp)
N2=$(mktemp)
trap 'rm -f "$N1" "$N2"' EXIT
norm "$1" > "$N1"
norm "$2" > "$N2"
if diff -u "$N1" "$N2"; then
  echo "PROOF-DIFF PASS: no unexpected differences"
else
  echo "PROOF-DIFF FAIL: unexpected differences above" >&2
  exit 1
fi
