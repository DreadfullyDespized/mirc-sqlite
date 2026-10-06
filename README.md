# mirc-sqlite

Clean-room mIRC SQLite DLL: a modern SQLite engine behind the `$sqlite_*`
mIRC script interface. Replaces the 2009 msqlite.dll 1.3.0
(SQLite 3.6.17), whose version ceiling is the root cause of recurring
live bugs (USING-chain join failures, no window functions/upserts/partial
indexes, OFFSET NULL datatype mismatch).

## Status

Pre-build. Wine + mIRC 7.79 proof environment is being stood up on the
build VM; the DLL does not exist yet.

## What's in here

- `src/` — the DLL source (C, clean-room; ~15 `$sqlite_*` identifiers +
  `LoadDll`/`UnloadDll`). Not written yet.
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
