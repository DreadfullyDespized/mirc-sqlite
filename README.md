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

Pre-build. The DLL and the proof harness do not exist yet; the
interface spec in `docs/` is the current source of truth.

## Use

Drop the built DLL alongside your mIRC scripts and call it through the
standard `$dll()` interface — the same `$sqlite_*` identifiers the 2009
DLL exposed (`msqlite_open`, `msqlite_fetch_row`, `msqlite_libversion`,
`LoadDll`/`UnloadDll`, and the rest of the ~15). The exact calling
convention for every identifier — params, return values, error
semantics — is specified in `docs/INTERFACE.md`.

## Implement

Implement `docs/INTERFACE.md`: every identifier's behavior plus the
compatibility notes (busy handling, journal mode, BLOB paths, 64-bit
value formatting). Build targets are 32-bit, clean-room C, latest
SQLite amalgamation. The 2009 DLL's quirks are bugs, not spec — except
where a script depends on one, which the proof must catch.

## Test

Correctness is proven, not asserted. The proof harness (`tools/proof/`,
in progress) runs real mIRC 7.79 under Wine on a private Xvfb display:
a candidate DLL faces a real-world mIRC alias suite, and every
script-visible behavior is diffed against the 2009 DLL. The new DLL
must match everywhere except the documented version-ceiling fixes.
Scratch databases only — nothing touches a live instance.

## What's in here

- `src/` — the DLL source (C, clean-room). Not written yet.
- `tools/proof/` — the Wine proof harness. Not built yet.
- `docs/` — interface spec (the identifier behaviors mIRC scripts rely on).

## Rules

- Code carries no comments (house rule; see AGENTS.md).
- Standalone mIRC infrastructure. The DLL ships only after the Wine
  proof passes and Dread merges.

## Process

See AGENTS.md and LOOP-STATE.md. Every change: issue first, PR with
For Dread + Blast radius, CI, Wine proof, independent grader, Dread merges.
