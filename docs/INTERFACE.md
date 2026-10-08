# mIRC SQLite DLL — Clean-Room Interface Spec

Derived from `msqlite.mrc` v1.3.0 (Reko Tiira, 18 Aug 2009), the script-side
companion to the original `msqlite.dll`. Every DLL export below is documented
from an actual `$dll($sqlite_dll, msqlite_XXX, <params>)` call site in that
script, with the exact parameter string quoted. Nothing here is guessed from
the old binary: if the script cannot distinguish two behaviors, the spec says
so and names the safe choice.

Audience: a C programmer implementing a new `msqlite.dll` (32-bit, ANSI/UTF-8
see section 1) against a modern SQLite amalgamation, such that the unmodified
`msqlite.mrc` v1.3.0 script — and the mIRC scripts built on it — work unchanged.

Conventions in this doc:

- `data` is the string mIRC hands to the exported function and the buffer the
  function overwrites with its return value (see section 1).
- `$null` in mIRC terms means the DLL wrote an empty string into `data`.
- "1" / "0" mean the literal one-character strings.

---

## 1. Transport: how `$dll()` reaches C

Each script alias calls:

```
$dll($sqlite_dll, msqlite_XXX, <param string>)
```

mIRC maps this to the exported C function:

```c
int __stdcall msqlite_XXX(HWND mWnd, HWND aWnd, char *data, char *parms,
                          BOOL show, BOOL nopause);
```

- The `<param string>` arrives in `data`. (`parms` is empty for `$dll()` calls;
  everything is packed into the one string. Parse arguments out of `data`.)
- The function writes its result string into `data` and returns **3**, which
  tells mIRC "data now holds the `$dll()` return value".
- An empty `data` on return is `$null` to the script. That is the universal
  error signal for every fallible export.
- Return 1 (continue) is never useful here; return 0 (halt) must never happen.
- All calls arrive on mIRC's main thread. No worker threads are involved
  unless the script uses `/dll -m`, which the companion script never does.

**Character encoding.** The companion script predates mIRC's Unicode support
and never opts into it, so implement the DLL as a non-Unicode (ANSI-style)
DLL: do not set `mUnicode` in `LoadDll`. On mIRC 7.x, mIRC converts between
its internal UTF-16 and the `char*` buffers, presenting UTF-8 bytes to a
non-Unicode DLL. Practical rule: treat `data`/`parms` as UTF-8 text in and
UTF-8 text out. Test with non-ASCII chat text (e.g. accented nicks) once the
proof rig exists.

**Buffer sizes.** mIRC ≥ 7.64 reports the safe byte limit in `LOADINFO.mBytes`
(double the line-length limit, always). Older mIRC used smaller fixed buffers.
Never write more than `mBytes` bytes into `data`. SQL text arrives inside that
same budget, so very long statements are truncated by mIRC itself before the
DLL ever sees them — there is nothing the DLL can do about that; just never
overflow the buffer.

---

## 2. `$sqlite_dll` resolution (msqlite.mrc line 123)

```mirc
alias -l sqlite_dll return $qt($+($scriptdir,msqlite.dll))
```

- The DLL file must be named exactly `msqlite.dll` and live in the same
  directory as `msqlite.mrc`.
- `$qt()` wraps the full path in double quotes, so paths with spaces arrive
  intact. The DLL does not parse this; it is only the `$dll()` target.

---

## 3. LoadDll / UnloadDll

```c
void __stdcall LoadDll(LOADINFO *info);
int  __stdcall UnloadDll(int mTimeout);
```

`LOADINFO` carries `mVersion`, `mHwnd` (main mIRC window handle — keep it;
section 5 explains why), `mKeep`, `mUnicode`, and on newer mIRC `mBeta` and
`mBytes`.

Required behavior:

- `LoadDll`: set `info->mKeep = TRUE` (connections, results and prepared
  statements live in DLL memory between calls; unloading would orphan them),
  leave `mUnicode = FALSE`, remember `mHwnd` and `mBytes`.
- `UnloadDll(1)` — mIRC idle-unloads after ~10 minutes of disuse. Return **0**
  (keep loaded). Letting mIRC unload here would silently destroy every open
  connection and result while scripts still hold their ids.
- `UnloadDll(0)` — user ran `/dll -u`, or `UnloadDll(2)` — mIRC is exiting.
  Close every open connection (finalizing statements and freeing results
  first), release all memory, return 1 (allow unload).

---

## 4. Global error mechanism: `%sqlite_errstr` / `%sqlite_errno`

There are no `$sqlite_errstr` / `$sqlite_errno` aliases. They are **global
mIRC variables the DLL itself must set**, and every fallible export in this
spec maintains them:

- `%sqlite_errno` — numeric code of the last operation. `0` (`$SQLITE_OK`) on
  success, a SQLite result code (1–26, or 200/201/202 for the mSQLite-specific
  codes) on failure.
- `%sqlite_errstr` — human-readable text for the last operation
  (`sqlite3_errmsg()` text on failure).

Script failure detection is always one of these two patterns:

```mirc
var %db = $sqlite_open(test.db)
if (%db) { ... }
else { echo -a Error opening database: %sqlite_errstr }
```

```mirc
var %v = $sqlite_fetch_single(%res)
; $null here is ambiguous: check %sqlite_errno
; 0 ($SQLITE_OK)        -> the value really was NULL
; 201 ($SQLITE_NOMOREROWS) -> no more rows
; anything else         -> an error; %sqlite_errstr has the text
```

How the DLL sets mIRC globals: a `$dll()` call cannot set variables through
its return value, so the DLL must send commands to mIRC itself — the
equivalent of executing `/set %sqlite_errno <n>` and
`/set %sqlite_errstr <text>` — via `SendMessage` to the main window handle
saved from `LoadDll`. Use mIRC's documented DLL-to-mIRC command channel for
this. The same channel is how `msqlite_fetch_row` writes hash tables
(section 8) and how bound-variable exports (`msqlite_fetch_bound`,
`msqlite_bind_param`) read and write mIRC variables. Every use of that
channel must be synchronous within the `$dll()` call: when the function
returns 3, the variables and hash tables are already updated.

On success, set `%sqlite_errno` to 0. Set `%sqlite_errstr` to empty on
success; scripts only read it on failure.

---

## 5. ID numbering scheme

`msqlite_open` returns a **positive numeric connection id**;
`msqlite_query` returns a **positive numeric result id**; `msqlite_prepare`
returns a **positive numeric statement id**. `0` is never a valid id —
`$sqlite_query` uses bare `0` to mean "succeeded, no rows" (section 8).

Rules:

- Ids start at 1 and increase. Never reuse an id while its handle is live.
- Ids must be unique across all three kinds at any moment. The script
  sometimes probes an id with `$sqlite_is_valid_statement()` before deciding
  whether it is a connection or a statement (`$sqlite_exec`/`$sqlite_query`
  do exactly this), so a single incrementing counter with a per-id type tag
  is the simplest correct design. A connection id must never collide with a
  live result or statement id.
- After `msqlite_free` / `msqlite_close`, the id becomes invalid; later use
  of it must fail cleanly (`$null` return, `%sqlite_errno` set) — never crash,
  never alias to a new handle that recycled the number while the script still
  holds the old one. (Monotonic ids with no reuse inside a session satisfy
  this trivially.)

---

## 6. Parameter encoding conventions

Three encodings appear in `data`; the DLL must parse all three.

**a. Bare tokens.** Most exports take space-separated tokens:
`<conn_id>`, `<result_id> <field_index>`, `<conn_id> <milliseconds>`.
The script pre-trims with `$gettok($1, 1, 32)`, so the first token never has
leading spaces. A missing token is an empty string, not `$null` — e.g.
`$sqlite_close` with no argument passes an empty `data`.

**b. `$qt()`-quoted paths.** `msqlite_open` and `msqlite_write_to_file`
receive double-quoted filenames:

```mirc
return $dll($sqlite_dll, msqlite_open, $qt($1) $iif($2, $qt($2)))
```

`data` looks like `"C:\mirc\Test2.sqlite"` or `":memory:" "C:\mirc\from.db"`.
The DLL needs a quote-aware tokenizer here: a `"` opens a quoted span that
may contain spaces, closed by the next `"`. `$qt($null)` produces `""`
(two quote characters, empty inside) — treat that as "argument omitted".

**c. Bind-value tokens.** `$sqlite_exec` / `$sqlite_query` /
`$sqlite_unbuffered_query` build bind parameters in script:

```mirc
var %bind, %i = 2
while (%i <= $0) {
  var %value = $[ $+ [ %i ] ]
  %bind = %bind $+(",$replace(%value,\,\\,",\"),")
  inc %i
}
```

Each value becomes `,"<escaped>",` — wrapped in double quotes, prefixed by a
comma, suffixed by a comma, with `\` escaped to `\\` and `"` escaped to `\"`,
concatenated with no spaces. Two values `a"b` and `c\d` arrive as:

```
,"a\"b",,"c\\d",
```

The DLL parses exactly `%params` such tokens (the count is given explicitly,
section 8), unescaping `\"` → `"` and `\\` → `\`. Values cannot contain
newlines (mIRC `$dll()` data is a single line) — same limitation as the
original; document it, do not try to work around it.

---

## 7. Script-side-only aliases (no DLL export)

These exist in `msqlite.mrc` but never call `$dll()`. They are listed so the
implementer does not go looking for exports that do not exist.

- `$sqlite_escape_string` — pure script: `return $replace($1-, ', '')`
  (doubles single quotes). The 67 mIRC call sites use this, not the DLL.
- `$sqlite_qt` — `return $+(',$1-,')` (wraps in single quotes).
- `$sqlite_exec_file(...)` — rewrites itself into `$sqlite_exec(...).file`.
- `$sqlite_begin` / `$sqlite_commit` / `$sqlite_rollback` — call
  `$sqlite_exec($1, BEGIN TRANSACTION)` etc.
- `$sqlite_finalize` — calls `$sqlite_free($1)`.
- `$sqlite_rewind` — calls `$sqlite_seek($1, 1, $SQLITE_BEG)` (the third
  argument is dropped by `$sqlite_seek`; see section 9).
- `$sqlite_fetch_num` / `$sqlite_fetch_assoc` — call `$sqlite_fetch_row`
  with `$SQLITE_NUM` / `$SQLITE_ASSOC`.
- `$sqlite_bind_column` — calls `$sqlite_bind_field`.
- `$sqlite_bind_null` — calls `$sqlite_bind_value($1, $2, $null, $SQLITE_NULL)`.
- `$sqlite_open_memory([from])` — calls `$sqlite_open(:memory:, $1)`.

There is no `msqlite_escape_string` export. Do not create one.

---

## 8. Group A — exports mIRC scripts actually use

These ~15 exports carry the scripts' entire database traffic. Implement and
proof-test these first.

### msqlite_libversion

```mirc
return $dll($sqlite_dll, msqlite_libversion, $null)
```

- Params: none (`data` is empty).
- Returns: the SQLite library version string, e.g. `3.51.2` (dotted
  major.minor.revision). For the new DLL this is the amalgamation's version
  via `sqlite3_libversion()`.
- Never fails.

### msqlite_dllversion

```mirc
return $dll($sqlite_dll, msqlite_dllversion, $null)
```

- Params: none.
- Returns: the DLL's own version string, dotted, e.g. `2.0.0`. The original
  returned `1.3.0`; the new DLL returns its own version. Scripts may display
  or compare it, so keep the `major.minor.revision` shape.
- Never fails.

### msqlite_error_string

```mirc
return $dll($sqlite_dll, msqlite_error_string, $1)
```

- Params: `<errcode>` — a numeric SQLite result code.
- Returns: the textual description (`sqlite3_errstr()` equivalent), e.g.
  code 1 → a "SQL error"-style string. Must cover the mSQLite-specific codes
  too: 200 (`$SQLITE_INVALIDARG`), 201 (`$SQLITE_NOMOREROWS`), 202
  (`$SQLITE_NOTMEMORYDB`).
- Never fails (unknown codes get a generic message, not `$null`).

### msqlite_open

```mirc
return $dll($sqlite_dll, msqlite_open, $qt($1) $iif($2, $qt($2)))
```

- Params (quote-aware, section 6b): `"db"` or `"db" "from"`. `$1` may be
  `$null`/empty → `data` is `""` (empty quoted string).
- Returns: positive numeric connection id, or `$null` with `%sqlite_errstr`
  / `%sqlite_errno` set.
- Semantics:
  - No `db` (empty): open a transient database in a temp file.
  - `db` given, file missing: create it.
  - `db` is `:memory:`: open a memory database. If `from` is also given,
    the memory db starts as a copy of that file (missing `from` file →
    create it empty, then copy — i.e. start empty). The original used the
    SQLite backup API for this; `sqlite3_backup_*` is the correct modern
    mechanism.
  - `from` given without `:memory:`: error (`$null`,
    `%sqlite_errno` = `$SQLITE_INVALIDARG` (200)).
- Apply the configured default busy timeout to every new connection
  (section 10). The original read a default from its config file; keep a
  sensible built-in default and document it.

### msqlite_close

```mirc
return $dll($sqlite_dll, msqlite_close, $1)
```

- Params: `<conn_id>`.
- Returns: `"1"` on success; `$null` if the id is not a live connection
  (the only documented error case).
- Closing a connection must first finalize its prepared statements and free
  its results — a script that closes a connection with live results must not
  crash or leak.

### msqlite_exec

```mirc
alias sqlite_exec {
  var %id = $gettok($1, 1, 32), %file = 0
  if ($sqlite_is_valid_statement(%id)) {
    var %bind, %i = 2
    while (%i <= $0) {
      var %value = $[ $+ [ %i ] ]
      %bind = %bind $+(",$replace(%value,\,\\,",\"),")
      inc %i
    }
    return $dll($sqlite_dll, msqlite_exec, %id %bind)
  }
  else if ($isid) {
    var %params = $calc($0 - 2)
    if ($prop == file) { %file = 1 }
    if (%params > 0) {
      var %bind, %i = 3
      while (%i <= $0) {
        var %value = $[ $+ [ %i ] ]
        %bind = %bind $+(",$replace(%value,\,\\,",\"),")
        inc %i
      }
      return $dll($sqlite_dll, msqlite_exec, %id %file %params %bind $2)
    }
  }
  return $dll($sqlite_dll, msqlite_exec, %id %file 0 $2-)
}
```

Three shapes arrive in `data`; distinguish by parsing:

1. **Statement form** — `%id` passed `$sqlite_is_valid_statement()`, so
   `data` = `<stmt_id> <bind tokens...>` (bind tokens per section 6c, may be
   absent). Bind each value to the prepared statement's `?` / `:name`
   parameters in order, then step it to completion and reset it.
2. **Identifier-with-binds form** — `data` =
   `<conn_id> <file 0|1> <nparams> <n bind tokens> <SQL>`.
   Parse the first three space-separated tokens, then exactly `nparams` bind
   tokens, then the remainder (ltrim one space) is the SQL. Prepare the SQL,
   bind the values positionally, step to completion.
3. **Plain form** — `data` = `<conn_id> <file 0|1> 0 <SQL>` where `<SQL>` is
   `$2-`, the raw remainder of the script line, spaces intact.

- `file` = 1 (only via the `.file` property / `$sqlite_exec_file`): the SQL
  argument is a filename; read the file and execute its contents.
- Multiple statements separated by `;` are all executed, in order
  (prepare/step loop over `sqlite3_prepare` tail).
- Returns: `"1"` on success; `$null` on error with `%sqlite_errstr` /
  `%sqlite_errno` set (use `sqlite3_errmsg()` of the connection).
- Datatype deduction for bound values (no explicit datatype in this path):
  mirror the original — integers bind as INTEGER, floats as FLOAT, everything
  else as TEXT. (`$sqlite_bind_value` documents this deduction rule; the
  inline-bind path follows it.)

### msqlite_query

Identical three shapes to `msqlite_exec` (the alias body is the same shape
with `msqlite_query`), identical parsing, identical multi-statement handling.

- Returns: **positive numeric result id** if the (last) statement returns
  rows — a SELECT always yields an id on success, even for zero rows;
  **`"0"`** if the statement ran successfully but returns no rows (INSERT /
  UPDATE / etc.); **`$null`** on error with `%sqlite_errstr` /
  `%sqlite_errno` set.
- "The returned result is the data returned by the last SQL query" — when
  several statements are given, earlier result sets are discarded (freed)
  and only the last statement's rows are kept.
- Buffered: all rows are materialized at query time, so `$sqlite_num_rows`,
  `$sqlite_seek`, `$sqlite_result` (random access) all work. The cursor starts
  *before* the first row; `$sqlite_fetch_row` / `$sqlite_fetch_single` /
  `$sqlite_fetch_field` advance it.

### msqlite_free

```mirc
return $dll($sqlite_dll, msqlite_free, $1)
```

- Params: `<id>` — a result id or a statement id (both are freed through this
  one export; `$sqlite_finalize` is just a script alias for it).
- Returns: `"1"`; `$null` if the id is not a live result or statement.
- Freeing a result that belongs to an open connection is fine. After free,
  the id is invalid (section 5).

### msqlite_num_rows

```mirc
return $dll($sqlite_dll, msqlite_num_rows, $1)
```

- Params: `<result_id>`.
- Returns: decimal row count (`"0"` for an empty SELECT — note this is the
  string zero, which is *truthy* where it matters; the script checks
  `$sqlite_num_rows(%res) == 0` numerically); `$null` for an invalid id.
- Buffered results only; for unbuffered results the original documented this
  as unsupported (Group B).

### msqlite_num_fields

```mirc
return $dll($sqlite_dll, msqlite_num_fields, $1)
```

- Params: `<result_id>`.
- Returns: decimal column count; `$null` for an invalid id.

### msqlite_changes

```mirc
return $dll($sqlite_dll, msqlite_changes, $1)
```

- Params: `<conn_id>`.
- Returns: decimal string of `sqlite3_changes()` for the connection;
  `$null` for an invalid id.

### msqlite_last_insert_rowid

```mirc
return $dll($sqlite_dll, msqlite_last_insert_rowid, $1)
```

- Params: `<conn_id>`.
- Returns: decimal string of `sqlite3_last_insert_rowid()`; `$null` for an
  invalid id. Rowids are 64-bit — format with `%lld`; never truncate to 32.

### msqlite_fetch_row

```mirc
return $dll($sqlite_dll, msqlite_fetch_row, $gettok($1, 1, 32) $gettok($2, 1, 32) $3)
```

- Params: `<result_id> <hashtable_name> <result_type>` where result_type is
  `1` (`$SQLITE_BOTH`, default), `2` (`$SQLITE_NUM`), `3` (`$SQLITE_ASSOC`),
  or empty (= BOTH).
- This is the one export that **writes into mIRC state directly**: the DLL
  must populate the named hash table itself, via the DLL→mIRC command
  channel (section 4), *before* returning:
  - If the table does not exist, create it; if it exists, clear it first.
  - `$SQLITE_NUM`: items are field ordinals `"1".."N"` → text values.
  - `$SQLITE_ASSOC`: items are field names → text values.
  - `$SQLITE_BOTH`: both. On a name/index collision (a column literally
    named `"1"`), the **index wins**.
  - Values are the columns' text representation. NULL columns: the original
    stored them as empty items (hash tables cannot hold `$null`); keep that.
- Cursor: fetches the current row, then advances past it (the next call gets
  the next row).
- Returns: `"1"` if a row was fetched; `"0"` if no more rows;
  `$null` on error (invalid id). `"0"` vs `$null` matters: scripts loop
  `while ($sqlite_fetch_row(%res, row, $SQLITE_ASSOC))` and stop on either,
  but error diagnosis after the loop reads `%sqlite_errno`.

### msqlite_fetch_field  (also serves `$sqlite_result`)

This single export backs two script aliases. Quote both call sites:

```mirc
alias sqlite_fetch_field {
  if ($0 < 3) {
    return $dll($sqlite_dll, msqlite_fetch_field, $1 $iif($2 !isnum || $prop == name, 1, 0) $2 1)
  }
  else {
    tokenize 32 $dll($sqlite_dll, msqlite_fetch_field, $1 $iif($2 !isnum || $prop == name, 1, 0) $2 1 $3)
    ...
  }
}
alias sqlite_result {
  if ($0 < 3) {
    return $dll($sqlite_dll, msqlite_fetch_field, $1 $iif($2 !isnum || $prop == name, 1, 0) $2 0)
  }
  ...
}
```

- Params (text form): `<result_id> <is_name 1|0> <field> <advance 1|0>`.
  - `is_name` = 1 when the field argument is non-numeric or the `.name`
    property was used; then `<field>` is a column name, else a 1-based
    ordinal.
  - `advance` = 1 for `$sqlite_fetch_field` (cursor moves past the row),
    0 for `$sqlite_result` (cursor stays).
- Returns the column value as text. `$null` is returned in three distinct
  cases — the script disambiguates via `%sqlite_errno`:
  - value really is NULL → `%sqlite_errno` = 0 (`$SQLITE_OK`);
  - no more rows → `%sqlite_errno` = 201 (`$SQLITE_NOMOREROWS`);
  - error (bad id, bad field) → other code, `%sqlite_errstr` set.
- **Binvar form** (both aliases, when a third argument like `&b` is given):
  params are `<result_id> <is_name> <field> <advance> <binvar_name>`.
  The DLL writes the column's **raw bytes** to a temp file and returns
  exactly three space-separated tokens: `<temp_filepath> <byte_count>
  <binvar_name>`. The script then runs `bread <file> 0 <count> <binvar>`
  and returns `$bvar(<binvar>,0)` (the byte count). If the DLL returns
  anything other than 3 tokens, the script treats it as failure (`$null`).
  Temp file paths must not contain spaces (the script `tokenize 32`s the
  reply) — use the system temp dir with a space-free generated name, and
  delete the file after a grace period or on the next call; the script reads
  it synchronously inside the same `$dll()` evaluation via `bread`, so the
  file must exist when the function returns.
- `$sqlite_fetch_single` (Group B, the adjacent reader) is the same
  export family with field fixed to column 1; see section 9.

### msqlite_busy_timeout

```mirc
return $dll($sqlite_dll, msqlite_busy_timeout, $gettok($1, 1, 32) $2)
```

- Params: `<conn_id> <milliseconds>`. `0` disables the busy handler.
- Returns: `"1"`; `$null` for an invalid id.
- Implement with `sqlite3_busy_timeout()`. Note: the original 1.3.0 DLL had
  a fatal bug in this export (0xc0000417 crash on some inputs) — the new
  implementation must validate the id *before* touching SQLite and must
  never crash on missing/empty/non-numeric arguments; those yield `$null`.

---

## 9. Group B — remaining exports (LATER)

Implement after Group A is proven. Each entry gives the call site and the
contract; parsing conventions from sections 6–8 apply.

- **msqlite_unbuffered_query** — same three shapes/parsing as
  `msqlite_query`, same return contract, but the result streams:
  rows are stepped on demand, not materialized. Random access
  (`$sqlite_num_rows`, `$sqlite_seek`, `$sqlite_result`, navigation below)
  is unsupported on these results — return `$null` with
  `%sqlite_errno` = `$SQLITE_MISUSE` (21) if attempted. The result must be
  freed before the connection runs another query.
- **msqlite_prepare** —
  `return $dll($sqlite_dll, msqlite_prepare, %id %file $2-)`
  i.e. `data` = `<conn_id> <file 0|1> <SQL>` (raw remainder, spaces intact;
  `file`=1 reads SQL from the named file). Prepares **only the first**
  statement; trailing statements are ignored. Returns statement id /
  `$null`.
- **msqlite_bind_field** —
  `$dll($sqlite_dll, msqlite_bind_field, $gettok($1,1,32) $iif($2 !isnum || $prop == name,1,0) $gettok($2,1,32) $gettok($3,1,32))`
  i.e. `<result_id> <is_name 1|0> <column> <varname>`. Binds a result column
  for `$sqlite_fetch_bound`: `varname` without `%` prefix is a regular mIRC
  global, with `&` prefix a binvar. Returns `"1"` / `$null`.
- **msqlite_bind_param** —
  `$dll($sqlite_dll, msqlite_bind_param, $gettok($1,1,32) $gettok($2,1,32) $gettok($3,1,32) $4)`
  i.e. `<stmt_id> <param> <varname> <datatype>`. **By-reference** binding:
  the DLL records the mIRC variable name and re-reads its value at each
  execution (requires evaluating mIRC variables from the DLL at exec time).
  `param` is a 1-based index or a `:named` parameter (colon included).
  `datatype` empty = deduce at execution; else 1–5
  (`$SQLITE_INTEGER/FLOAT/TEXT/BLOB/NULL`). Returns `"1"` / `$null`.
- **msqlite_bind_value** — two shapes:
  `$dll($sqlite_dll, msqlite_bind_value, $gettok($1,1,32) $2)` (binds NULL
  when called with two args) and
  `$dll($sqlite_dll, msqlite_bind_value, $gettok($1,1,32) $gettok($2,1,32) $iif($0 < 4,-1,$gettok($4,1,32)) $3)`
  i.e. `<stmt_id> <param> <datatype|-1> <value>` where the value is `$3`,
  the raw remainder — **note the reordering**: datatype comes before the
  value in `data`, and `$3` (not `$3-`) means multi-word values only arrive
  via the identifier form. `-1` = deduce type. Returns `"1"` / `$null`.
- **msqlite_clear_bindings** — `data` = `<id>` (result or statement).
  Clears `$sqlite_bind_field` bindings on a result, or
  `$sqlite_bind_param`/`$sqlite_bind_value` bindings on a statement
  (back to NULL). Returns `"1"` / `$null`.
- **msqlite_autocommit** —
  `return $dll($sqlite_dll, msqlite_autocommit, $gettok($1, 1, 32) $2)`.
  `data` = `<conn_id> [<mode>]`. No mode: return `"1"` if autocommit is on,
  `"0"` if off. Mode given: set and return `"1"` / `$null`.
- **msqlite_create_function** —
  `<conn_id> <sql_func_name> <mirc_alias> <num_args> <prop>` (num_args/prop
  may be empty; default num_args = -1 = any arity). Registers an SQLite
  scalar function that calls back into the mIRC alias with the SQL
  arguments. **Requires calling mIRC aliases from inside SQLite execution**
  — re-entrant into mIRC's evaluator from the DLL. Returns `"1"` / `$null`.
- **msqlite_create_aggregate** —
  `<conn_id> <sql_name> <step_alias> <finalize_alias> <num_args> <step_prop> <finalize_prop>`.
  The step alias receives `<context> <args...>` and returns the new context;
  the finalize alias receives `<context>` and returns the result. Same
  re-entrancy requirement as above. Returns `"1"` / `$null`.
- **msqlite_signal_error** — `data` = `$1-`, the raw error text. Only valid
  when called from inside a user-defined-function callback; records the
  error so the in-flight query fails with it. Returns `"1"` / `$null`
  (not in a UDF → `$null`).
- **msqlite_set_authorizer** — `<conn_id> <alias> <prop>` to register,
  `<conn_id>` alone (second token empty) to unregister. The alias receives
  `<action_code> <arg2> <arg3> <dbname> <trigger>` and returns 0/1/2
  (`$SQLITE_OK/DENY/IGNORE`). Same re-entrancy requirement. Returns
  `"1"` / `$null`.
- **msqlite_load_extension** —
  `<conn_id> <filename> <entrypoint>` (entrypoint may be empty → default
  `sqlite3_extension_init`). Returns `"1"` / `$null`.
- **msqlite_is_valid_conn / msqlite_is_valid_result /
  msqlite_is_valid_statement** — `data` = `<id>`. Return `"1"` if live,
  `"0"` if not, `$null` if the argument is missing entirely.
- **msqlite_is_memory** — `data` = `<conn_id>`. `"1"` if memory db, `"0"` if
  file db, `$null` if invalid.
- **msqlite_write_to_file** —
  `$dll($sqlite_dll, msqlite_write_to_file, $gettok($1,1,32) $qt($2-))`
  i.e. `<conn_id> "<file>"` (quote-aware). Dumps a memory database to disk
  (backup API, reverse of `msqlite_open`'s `from`). Returns `"1"` / `$null`;
  non-memory conn → `$null` with `%sqlite_errno` = 202
  (`$SQLITE_NOTMEMORYDB`).
- **msqlite_reload** — `data` empty/`$null`. Reloads the DLL's config file.
  Return value ignored by the script (`noop`).
- **msqlite_field_name** —
  `$dll($sqlite_dll, msqlite_field_name, $gettok($1,1,32) $2)`:
  `<result_id> <field_index_1based>`. Returns the column name / `$null`.
- **msqlite_field_type** — same shape as field_name. Returns `1`–`5`
  (`$SQLITE_INTEGER/FLOAT/TEXT/BLOB/NULL`) for the value's storage class /
  `$null`. Note the script never passes the documented third `row_index`
  argument — the DLL always reports the current row.
- **msqlite_fetch_single** — text form `data` = `<result_id>`; binvar form
  `data` = `<result_id> <binvar>` with the same 3-token temp-file reply as
  `msqlite_fetch_field`'s binvar form. Column 1, advances the cursor.
  `$null` + `%sqlite_errno` disambiguation exactly as in section 8.
- **msqlite_fetch_all** —
  `$dll($sqlite_dll, msqlite_fetch_all, $gettok($1,1,32) %delim $2)`:
  `<result_id> <delim_ascii> <file>`. Writes every row as one line,
  fields separated by the delimiter character (default 9 = TAB), escaping
  `\`, newline, CR, NUL bytes and the delimiter itself as `\xNN`
  (two-digit hex). Returns `"1"` / `$null`.
- **msqlite_has_more / msqlite_has_prev** — `<result_id>` → `"1"` / `"0"` /
  `$null`.
- **msqlite_next / msqlite_prev** — `<result_id>` → `"1"` moved / `"0"`
  at end / `$null`.
- **msqlite_seek** —
  `return $dll($sqlite_dll, msqlite_seek, $gettok($1, 1, 32) $2)`:
  `<result_id> <row_index>`. **Note:** the script documents an optional
  third `seek_type` (`$SQLITE_BEG/CUR/END`) but the alias never passes it to
  the DLL, and `$sqlite_rewind` calls `$sqlite_seek($1, 1, $SQLITE_BEG)` —
  so the DLL always seeks 1-based from the start. Returns `"1"` / `"0"`
  (not seekable) / `$null`.
- **msqlite_key** — `<result_id>` → current 1-based row number / `$null`.
- **msqlite_current** —
  `$dll($sqlite_dll, msqlite_current, $gettok($1,1,32) $gettok($2,1,32) $3)`:
  `<result_id> <hashtable> <result_type>` — identical to
  `msqlite_fetch_row` except the cursor does **not** advance. Returns
  `"1"` / `"0"` (past end) / `$null`.
- **msqlite_fetch_bound** — `data` = `<result_id> <bind_type>` (bind_type
  `1`=`$SQLITE_ALL`, `2`=`$SQLITE_BOUND`, empty = BOUND). Fetches the row
  into the variables registered by `msqlite_bind_field` (text vars set as
  mIRC globals via the command channel; binary columns spooled to a temp
  file). Reply contract, from the script:
  - single token (`"1"`, `"0"`, or empty): returned to the script as-is —
    `"1"` row fetched, `"0"` no more rows, empty `$null` error;
  - three tokens `<tempfile> <sizes> <bvars>`: `sizes` and `bvars` are
    `|`-separated parallel lists; the script `bread`s each byte range into
    the named binvar and returns 1.
  
  This is the most intricate export; implement last, against the script's
  tokenize/bread logic quoted in section 8's binvar discussion.
- **msqlite_current_bound** — same as `msqlite_fetch_bound` without
  advancing the cursor. Call shape:
  `$dll($sqlite_dll, msqlite_current_bound, $gettok($1,1,32) $gettok($2,1,32) $3)`.
- **msqlite_safe_encode** —
  `return $dll($sqlite_dll, msqlite_safe_encode, %delim $1)` where
  `%delim` is an ASCII code or `-1`: `data` = `<delim|-1> <datum>`.
  **Quirk:** the script passes `$1` (first space-delimited token), not
  `$1-` — multi-word input is truncated by the script before the DLL sees
  it. Reproduce the escaping exactly: `\`, `\n`, `\r`, NUL, and the
  delimiter char become `\xNN`. Returns encoded text / `$null` (empty input
  also yields `$null` with `%sqlite_errno` = 0 — not an error).
- **msqlite_safe_decode** — `data` = `<datum>` (`$1`, first token; encoded
  data never contains raw spaces). Reverses the above. Same `$null`/errno
  convention.
- **msqlite_field_metadata** — `data` = `$1-`: `<conn_id> [database]
  <table> <column> <hashtable>`. Writes five items into the named hash
  table via the command channel: `dattype`, `collseq`, `notnull` (1/0),
  `primkey` (1/0), `autoinc` (1/0). Returns `"1"` / `$null`.

---

## 10. Limits and edge cases the implementation must respect

1. **Never crash on bad input.** Every export with an id argument must
   validate the id first and return `$null` (empty `data`) with
   `%sqlite_errno`/`%sqlite_errstr` set. Non-numeric, missing, or recycled
   ids are routine in buggy scripts — they must not AV the DLL. (The
   original's `msqlite_busy_timeout` violated this; do not reproduce the
   bug.)
2. **64-bit integers.** `msqlite_last_insert_rowid` and any integer column
   values must be formatted from 64-bit (`%lld`). mIRC numbers are doubles
   on the script side, but the DLL must not truncate at 32 bits.
3. **BLOBs in text paths.** `msqlite_fetch_row` / text-form `msqlite_fetch_field`
   convert to text; embedded NULs terminate the C string — the value is
   truncated there. That matches the original's `$dll()` string semantics.
   Full-fidelity binary goes through the binvar/temp-file forms only.
4. **Busy handling.** Long-held read transactions block writers. The typical workload is open → query → free → close per command, so contention is
   minimal, but set a default busy timeout on every connection opened by
   `msqlite_open` (the original used its config file default; a built-in
   1000 ms default is reasonable — record the choice).
5. **Journal mode.** Scripts may share their database files with other processes. The DLL must not enable WAL or change
   journal modes on its own; default rollback-journal behavior keeps the
   file interoperable.
6. **No config-file surprises.** The original read `msqlite.ini` for the
   default busy timeout. The new DLL may keep an ini for that one setting,
   but it must work correctly with no ini present.
7. **mIRC evaluation re-entrancy (Group B).** `msqlite_create_function`,
   `msqlite_create_aggregate`, `msqlite_set_authorizer`, and
   `msqlite_bind_param` call back into mIRC while SQLite is executing.
   mIRC's evaluator is re-entrant from the main thread for this purpose,
   but keep the callbacks short and never call them with the SQLite mutex
   in a state that deadlocks if the script itself runs another query on the
   same connection (document the restriction; the original had it too).
8. **Temp files.** The binvar forms and `msqlite_fetch_bound`'s binary path
   return temp-file paths that the script `tokenize 32`s — **no spaces
   allowed** in those paths. Generate them under the system temp dir with a
   space-free name, and clean them up (on next call or via a small LRU;
   the script `bread`s them synchronously before the `$dll()` result is
   even assigned, so prompt deletion after return is safe as long as it is
   not before return).
9. **Line-length ceiling.** `data` cannot exceed mIRC's line limit
   (`LOADINFO.mBytes` on ≥7.64). A single column value longer than that is
   truncated by mIRC itself on the way in and on the way out. Nothing the
   DLL can do — but it must not overflow the buffer trying.
10. **Same-connection re-entrancy is refused.** Extending item 7: a `$dll()`
    call made from inside a UDF/authorizer alias that targets the *same*
    connection is refused with `%sqlite_errno` 200 ("reentrant call").
    Calls on *other* connections are unaffected, and `msqlite_signal_error`
    keeps working inside the callback.
11. **Buffer ceilings.** Values are silently truncated at: 16384 bytes for
    `msqlite_fetch_field` text results, `%var` bind evaluations, and UDF
    return values; 65536 bytes for commands sent back into mIRC; 4096
    characters of `msqlite_safe_encode`/`msqlite_safe_decode` input;
    511 characters for `msqlite_open` database paths (a longer path is
    truncated and may open a different file — keep paths short).
12. **Bind-count cap.** More than 1,000,000 bind values on one call is
    rejected with error 200.
13. **UDF blob arguments.** Blob arguments passed to a script alias
    truncate at the first embedded NUL (extends item 3 to the UDF
    argument path).
