# LOOP-STATE.md — mirc-sqlite

## Test bed

Wine (32-bit) + real mIRC 7.79 + candidate DLL on a private Xvfb display,
driven by `tools/proof/` (adapted from the mirc `tools/holiday-proof`
harness). Each run: fresh Wine prefix, scratch SQLite copies only, real-world mIRC alias files, scripted steps with per-step logging. The 2009
msqlite.dll 1.3.0 is the behavior baseline — the new DLL must produce
identical script-visible behavior except for the documented version
fixes (USING-chain joins, window functions, upserts, partial indexes).

## Merge authority

Dread merges. Agent may open PRs; nothing merges without his approval,
and nothing ships to a live environment without his explicit go-ahead.

## State

1. DLL source: merged in PR #2 on 2026-10-07. `src/msqlite.c` is
   clean-room C, 32-bit, on the SQLite 3.53.4 amalgamation, with 49
   `msqlite_*` exports + `LoadDll`/`UnloadDll` (`src/msqlite.def`).
   Code-review fixes, and the 2.0.0 version resource `src/msqlite.rc`,
   merged in PR #12 on 2026-10-08.
2. Validation: the 2026-10-06 run in real mIRC 7.79 is recorded in
   `docs/TEST-SPEC.md`.
3. Proof files: `tools/proof/proof.mrc` (alias suite) and
   `tools/proof/diff.sh` (log diff between the 2009 DLL and the new build).
4. Baseline behaviors: the `$sqlite_*` semantics the scripts rely on,
   including `$sqlite_result` and the global variables `%sqlite_errstr` /
   `%sqlite_errno`, are specified in `docs/INTERFACE.md`.
5. A SQLite 3.54.0 bump (DLL 2.1.0) is proposed in PR #16 (issue #15).
