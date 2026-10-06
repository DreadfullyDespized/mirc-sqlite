# mirc-sqlite

A clean-room SQLite DLL for mIRC, built on the latest SQLite.

mIRC scripts talk to SQLite through `$dll()` calls into a small DLL
(`msqlite.dll`). The last public build of that DLL is from 2009 —
msqlite.dll 1.3.0, bundling SQLite 3.6.17. Seventeen years of missing
SQLite features later, that version ceiling is the root cause of
recurring live bugs: USING-chain join failures, no window functions,
no upserts, no partial indexes, OFFSET NULL datatype mismatches.

This project rebuilds that DLL from scratch (clean-room C, ~15
`$sqlite_*` identifiers plus `LoadDll`/`UnloadDll`) against the latest
SQLite amalgamation, keeping the `$sqlite_*` script interface
bit-compatible so existing mIRC scripts work unchanged.

## Status

Pre-build. Wine + mIRC 7.79 proof environment is being stood up on the
build VM; the DLL does not exist yet.

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
