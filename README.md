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

Built and validated 2026-10-06. The 32-bit DLL (SQLite 3.53.4, 49
`msqlite_*` exports + `LoadDll`/`UnloadDll`) was built with
`i686-w64-mingw32-gcc` and proven in real mIRC 7.79 against a Test2
snapshot: basic ops, alias query shapes, WAL concurrency, binds,
transactions, multi-connection, handle lifecycle, large result sets,
and long SQL. Full evidence record: `docs/TEST-SPEC.md`. PR #2
(`dll/initial-build`) is under independent review; Dread merges.

## What's in here

- `src/` — the DLL source (C, clean-room). Not written yet.
- `tools/proof/` — the Wine proof harness: real mIRC 7.79 + a candidate
  DLL on a private Xvfb display, running a real-world mIRC alias suite
  and diffing behavior against the 2009 DLL. Not built yet.
- `docs/` — interface spec (the identifier behaviors mIRC scripts rely on).

## Rules

- Code carries no comments (house rule; see AGENTS.md).
- Standalone mIRC infrastructure. The DLL ships only after the Wine
  proof passes and Dread merges.
- The 2009 DLL's quirks are bugs, not spec — except where a script
  depends on one, which the proof must catch.

## Process

See AGENTS.md and LOOP-STATE.md. Every change: issue first, PR with
For Dread + Blast radius, CI, Wine proof, independent grader, Dread merges.
