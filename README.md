Repo label: PROD

# mirc-sqlite

Known-good testing reference for a clean-room SQLite DLL for mIRC.

mIRC scripts talk to SQLite through `$dll()` calls into a small DLL
(`msqlite.dll`). The last public build of that DLL is from 2009 —
msqlite.dll 1.3.0, bundling SQLite 3.6.17. That version ceiling is the
root cause of recurring live bugs: USING-chain join failures, no window
functions, no upserts, no partial indexes, OFFSET NULL datatype
mismatches.

This repo is the known-good reference for replacing it: the interface
spec (`docs/`) and the Wine + mIRC 7.79 proof harness (`tools/proof/`)
that diffs a candidate DLL — built against the latest SQLite
amalgamation — against the 2009 build's behavior. A DLL that passes the
proof here keeps the `$sqlite_*` script interface bit-compatible, so
existing mIRC scripts work unchanged.

## Status

Shipped. PR #2 ("Clean-room msqlite.dll: 49 exports, SQLite 3.53.4",
branch `dll/initial-build`) was merged by Dread on 2026-10-07. The
32-bit DLL (SQLite 3.54.0, DLL 2.1.0, 49 `msqlite_*` exports +
`LoadDll`/`UnloadDll`) is built with `i686-w64-mingw32-gcc`; the `build`
workflow cross-compiles it and uploads it as an artifact on every PR and
push to `main`. It was proven in real mIRC 7.79 against a Test2 snapshot
on 2026-10-06: basic ops, alias query shapes, WAL concurrency, binds,
transactions, multi-connection, handle lifecycle, large result sets, and
long SQL. Full evidence record: `docs/TEST-SPEC.md`. Fixes from the
2026-10-07 code review merged in PR #12 on 2026-10-08, which also added
the DLL version resource (`src/msqlite.rc`).

## What's in here

- `src/` — the DLL source: `msqlite.c` (clean-room C), `msqlite.def`
  (the export list) and `msqlite.rc` (the version resource).
- `tools/` — `fetch-sqlite.sh` (downloads and hash-checks the SQLite
  3.54.0 amalgamation), `build.sh` (the 32-bit build), `checks/` (the PII
  gate) and `proof/`: `proof.mrc`, the alias suite that exercises the
  query shapes in `docs/INTERFACE.md`, and `diff.sh`, which compares its
  log from the 2009 DLL with its log from the new build.
- `docs/` — interface spec (`INTERFACE.md`) and the validation record
  (`TEST-SPEC.md`).

## Rules

- Code carries no comments (house rule; see AGENTS.md).
- Standalone mIRC infrastructure. The DLL ships only after the Wine
  proof passes and Dread merges.
- The 2009 DLL's quirks are bugs, not spec — except where a script
  depends on one, which the proof must catch.

## Process

See AGENTS.md and LOOP-STATE.md. Every change: issue first, PR with
For Dread + Blast radius, CI, Wine proof, independent grader, Dread merges.
