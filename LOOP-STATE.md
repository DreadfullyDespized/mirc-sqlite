# LOOP-STATE.md — mirc-sqlite

## Test bed

Wine (32-bit) + real mIRC 7.79 + candidate DLL on a private Xvfb display,
driven by `tools/proof/` (adapted from the mirc `tools/holiday-proof`
harness). Each run: fresh Wine prefix, scratch SQLite copies only, the
bot's real alias files, scripted steps with per-step logging. The 2009
msqlite.dll 1.3.0 is the behavior baseline — the new DLL must produce
identical script-visible behavior except for the documented version
fixes (USING-chain joins, window functions, upserts, partial indexes).

## Merge authority

Thomas merges. Agent may open PRs; nothing merges without his approval,
and nothing ships to the live bot without his explicit go-ahead.

## Open state

1. Wine + mIRC 7.79 environment: being stood up on the build VM
   (2026-10-05).
2. DLL source: not written yet. Target: ~15 `$sqlite_*` identifiers +
   `LoadDll`/`UnloadDll`, clean-room C, 32-bit, modern SQLite amalgamation.
3. Proof harness: not built yet.
4. Baseline behaviors to preserve: the exact `$sqlite_result` /
   `$sqlite_errstr` / `$sqlite_errno` semantics the scripts rely on
   (spec to be extracted into `docs/`).
