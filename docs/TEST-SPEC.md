# msqlite.dll Validation Test Spec

## Build
- Source: `dll/initial-build` branch, PR #2
- SQLite: 3.53.4 amalgamation, compiled as C (a G++ attempt was wrong and produced C++ errors)
- Compiler: i686-w64-mingw32-gcc (32-bit)
- Build fixes: the 48 unnamed C parameters in `src/msqlite.c` had to be named for the mingw build; the stdcall linker notes are warnings only; C sources are LF-normalized via `.gitattributes`
- Output: PE32/i386 DLL, 49 `msqlite_*` exports + `LoadDll`/`UnloadDll`

## Test Environment
- mIRC 7.79 (32-bit) on Windows
- Test DB: copy of the Test2-2026-09-23.sqlite snapshot (32 tables, User: 5232 rows); the live DB was never touched
- Fixture fact, verified 2026-10-06 against the pristine snapshot: `User.idUser` starts at 2, is non-contiguous (max 5708), and exactly 57 of the ids 1..80 exist. Test oracles that assumed ids 1..80 all exist were wrong; the DLL's answers were right.

## Proven Results (2026-10-06, real mIRC 7.79)

### 1. Basic Functionality
| Test | Result |
|------|--------|
| DLL loads via $dll() | PASS — SQLite 3.53.4, DLL 2.0.0 |
| msqlite_open | PASS — returns nonempty connection ID |
| msqlite_query (SELECT COUNT(*) FROM User) | PASS — 5232, matches snapshot |
| msqlite_fetch_field | PASS — correct values |
| msqlite_busy_timeout(1000) | PASS — returns 1 |
| msqlite_busy_timeout (invalid ID) | PASS — empty, no hang/crash |

### 2. Alias Query Compatibility — 71/89 through the DLL
- 95 SQL shapes extracted from SqliteUserDB-Alias.mrc (61 aliases); 6 AnkhBot shapes excluded per Dread (different system)
- 71 of the 89 Test2-oriented shapes PASS through `msqlite.dll`
- The other 18 stopped on harness fixture gaps (missing temp tables, dynamic table/column names, unresolved mIRC variables) — not DLL errors
- Those 18 shapes were re-validated 18/18 with corrected fixtures via stock Python sqlite3
- Rerun of the 18 through `msqlite.dll` itself is PENDING — 89/89 through the DLL is not claimed

### 3. Concurrency
| Test | Result |
|------|--------|
| mIRC + 1 background writer (WAL mode) | PASS — 5 runs, reads returned 5232 every time, writes persisted, 0 mIRC-side errors |
| 2 readers + 2 writers, 20 sec (separate processes) | PASS — 63,871 ops, 0 errors |
| WAL mode active | PASS — mIRC write created .sqlite-wal (4152B) and .sqlite-shm (32768B) |

### 4. Native C Harness (32-bit, mIRC calling convention)
- 49 exports match the 2009 DLL exactly
- Core open/query/exec/result/free/close: PASS
- Window functions, upsert, JOIN...USING: PASS
- FAILURES=0

### 5. Old DLL Comparison
- 2009 DLL (SQLite 3.6.17): `msqlite_busy_timeout` on a valid connection hung in the isolated harness (killed after 10s)
- New DLL (SQLite 3.53.4): busy_timeout returns 1 immediately; invalid ID safe

### 6. Bind Parameters
- `msqlite_prepare` (int and text): PASS
- `msqlite_bind_value` int + query + fetch: PASS — bound idUser=2 returned `oldmanfishing`, matching the snapshot
- Rebind same statement with a new int: PASS — correct row again
- The 3 first-run failures traced to the fixture assuming idUser=1 exists (snapshot: MIN(idUser)=2): int fetch "no more rows", empty setup name, and the cascading text-fetch failure. Not DLL defects.
- Text bind was accepted (returned 1) but its end-to-end fetch never ran against a real value — rerun with a valid id PENDING

### 7. Transactions — ALL PASS
- BEGIN/INSERT/ROLLBACK: row gone after rollback
- BEGIN/INSERT/COMMIT: row persists after commit

### 8. Multiple Simultaneous Connections — ALL PASS
- Two connections, interleaved reads 5232/5232
- Cross-connection write visibility both directions
- Both handles invalid after close

### 9. Handle Lifecycle — ALL PASS, no leaks
- 100 open/close cycles; closed IDs invalid; query on closed ID fails cleanly
- 100 query/free cycles; freed results invalid

### 10. Large Result Sets — PASS
- num_rows 5232, num_fields 1: PASS
- Full `msqlite_next` walk: 5232 rows: PASS
- First-row spot check returned idUser=2; the oracle expected 1, but the snapshot's MIN(idUser) is 2 — the DLL was right, the oracle was wrong

### 11. Long SQL — PASS
- 1238-char query (80 OR terms) parsed and executed, returned a result id
- Count returned 57; the oracle expected 80 assuming ids 1..80 all exist — the snapshot holds exactly 57 of those ids — the DLL was right, the oracle was wrong

## Pending
- [ ] Text-bind end-to-end fetch with a valid fixture value (rerun)
- [ ] 18 alias shapes rerun through `msqlite.dll` (proven via stock sqlite3 only so far)
- [ ] Independent grader review (requested 2026-10-06)
- [ ] Dread's merge decision on PR #2

## Cleanup
- Test directory `D:\mirc-dll-test\` removed and verified gone 2026-10-06 after testing
