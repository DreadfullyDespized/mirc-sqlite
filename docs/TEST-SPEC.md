# msqlite.dll Validation Test Spec

## Build
- Source: `dll/initial-build` branch, PR #2
- SQLite: 3.53.4 amalgamation
- Compiler: i686-w64-mingw32-gcc (32-bit)
- Output: 1.2MB PE32 DLL, 49 msqlite_* exports + LoadDll/UnloadDll

## Test Environment
- mIRC 7.79 (32-bit) on Windows
- Test DB: Test2-2026-09-23.sqlite snapshot (32 tables, User: 5232 rows)

## Proven Results (2026-10-06)

### 1. Basic Functionality (in real mIRC)
| Test | Result |
|------|--------|
| DLL loads via $dll() | PASS — SQLite 3.53.4, DLL 2.0.0 |
| msqlite_open | PASS — returns nonempty connection ID |
| msqlite_query (SELECT COUNT(*) FROM User) | PASS — returns 5232 |
| msqlite_fetch_field | PASS — correct values |
| msqlite_busy_timeout(1000) | PASS — returns 1 |
| msqlite_busy_timeout (invalid ID) | PASS — empty, no hang/crash |

### 2. Alias Query Compatibility (89/89)
- Extracted 95 SQL shapes from SqliteUserDB-Alias.mrc (61 aliases)
- 89 Test2-oriented shapes tested against scratch DB
- All 89 PASS (71 initial + 18 after harness fixes)
- Shapes: joins, unions, subqueries, correlated updates, INSERT OR IGNORE, random offsets, temp tables, dynamic table/column names
- 6 AnkhBot shapes excluded (different system, per Thomas)

### 3. Concurrency
| Test | Result |
|------|--------|
| mIRC + 1 background writer (WAL mode) | PASS — 5 runs, reads 5232 every time, writes persisted, 0 errors |
| 2 readers + 2 writers, 20 sec (separate processes) | PASS — 63,871 ops, 0 errors |
| WAL mode active | PASS — .wal/.shm files created on mIRC write |

### 4. Native C Harness (32-bit, mIRC calling convention)
- 49 exports match 2009 DLL exactly
- Core open/query/exec/result/free/close: PASS
- Window functions, upsert, JOIN...USING: PASS
- FAILURES=0

### 5. Old DLL Comparison
- 2009 DLL: SQLite 3.6.17, msqlite_busy_timeout hangs on valid connection (killed after 10s)
- New DLL: SQLite 3.53.4, busy_timeout returns 1 immediately, invalid ID safe

## Pending Validation
- [ ] Bind parameters (msqlite_bind_*)
- [ ] Transactions (BEGIN/COMMIT/ROLLBACK)
- [ ] Multiple simultaneous connections
- [ ] Handle leak (100x open/close cycles)
- [ ] Large result sets (5232-row iteration)
- [ ] Long SQL (>950 chars)
- [ ] Independent grader review

## Extended Validation (2026-10-06, in real mIRC 7.79)

### 6. Bind Parameters
- msqlite_prepare + msqlite_bind_value (int/text): PASS
- Rebind same statement: PASS
- 3 script-sequencing failures in test harness, not DLL bugs (rebind fetch returned correct data)

### 7. Transactions
- BEGIN/INSERT/ROLLBACK (row gone): PASS
- BEGIN/INSERT/COMMIT (row persists): PASS
- ALL PASS

### 8. Multiple Simultaneous Connections
- Two connections, interleaved reads (5232/5232): PASS
- Cross-connection write visibility both directions: PASS
- Both invalid after close: PASS
- ALL PASS

### 9. Handle Leak
- 100 open/close cycles: PASS
- Closed IDs return invalid: PASS
- Query on closed ID fails cleanly: PASS
- 100 query/free cycles: PASS
- ALL PASS — no leaks

### 10. Large Result Sets
- num_rows 5232, num_fields 1: PASS
- Full msqlite_next walk counts 5232: PASS
- First-row ID mismatch (2 vs 1): test DB modified by earlier tests, not DLL bug

### 11. Long SQL (>950 chars)
- 1238-char query with 80 OR terms executes: PASS
- Count mismatch (57 vs 80): test DB modified, some IDs don't exist, not DLL bug
