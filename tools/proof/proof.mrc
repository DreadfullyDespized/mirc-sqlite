alias t.log { write $qt(%t.log) $1- }
alias t.probe {
  var %name = $1
  var %r = $sqlite_query(%t.db, $2-)
  if (%r) { t.log PROBE %name ok=1 rows= $+ $sqlite_num_rows(%r) | sqlite_free %r }
  else { t.log PROBE %name ok=0 err= $+ %sqlite_errstr }
}
alias t.step {
  goto s $+ $1
  :s1
  t.log BEGIN variant= $+ %t.variant mirc= $+ $version dllver= $+ $sqlite_dllversion libver= $+ $sqlite_libversion
  return
  :s2
  var %db = $sqlite_open(%t.dbdir $+ \proof.db)
  set %t.db %db
  t.log OPEN file ok= $+ $iif(%db, 1, 0) errno= $+ %sqlite_errno
  var %m = $sqlite_open(:memory:)
  t.log OPEN memory ok= $+ $iif(%m, 1, 0)
  if (%m) { sqlite_close %m }
  t.log CLOSE badid= $+ $iif($sqlite_close(999999), 1, 0)
  return
  :s3
  var %ok = $sqlite_exec(%t.db, CREATE TABLE IF NOT EXISTS t(id INTEGER PRIMARY KEY, name TEXT, score REAL))
  t.log EXEC create ok= $+ $iif(%ok, 1, 0) errno= $+ %sqlite_errno
  var %ok2 = $sqlite_exec(%t.db, INSERT INTO t(name, score) VALUES('alice', 1.5); INSERT INTO t(name, score) VALUES('bob', 2.5))
  t.log EXEC multi ok= $+ $iif(%ok2, 1, 0) changes= $+ $sqlite_changes(%t.db)
  return
  :s4
  var %ok = $sqlite_exec(%t.db, INSERT INTO t(name, score) VALUES(?, ?), o'brien, 3.25)
  t.log BIND ok= $+ $iif(%ok, 1, 0) changes= $+ $sqlite_changes(%t.db) rowid= $+ $sqlite_last_insert_rowid(%t.db)
  var %ok2 = $sqlite_exec(%t.db, UPDATE t SET score = score + 1 WHERE name = 'alice')
  t.log EXEC update ok= $+ $iif(%ok2, 1, 0) changes= $+ $sqlite_changes(%t.db)
  var %ok3 = $sqlite_exec(%t.db, DELETE FROM t WHERE name = 'nobody')
  t.log EXEC deletenone ok= $+ $iif(%ok3, 1, 0) changes= $+ $sqlite_changes(%t.db)
  return
  :s5
  var %r = $sqlite_query(%t.db, SELECT id, name, score FROM t ORDER BY id)
  t.log QUERY ok= $+ $iif(%r, 1, 0) rows= $+ $iif(%r, $sqlite_num_rows(%r), -) fields= $+ $iif(%r, $sqlite_num_fields(%r), -)
  if (%r) {
    t.log RESULT byname= $+ $sqlite_result(%r, name) byidx= $+ $sqlite_result(%r, 1)
    var %f = $sqlite_fetch_row(%r, trow, $SQLITE_ASSOC)
    t.log FETCH assoc ok= $+ %f name= $+ $hget(trow, name) score= $+ $hget(trow, score)
    var %f2 = $sqlite_fetch_row(%r, trow2, $SQLITE_NUM)
    t.log FETCH num ok= $+ %f2 f2= $+ $hget(trow2, 2)
    var %drained = 0
    while ($sqlite_fetch_row(%r, trow3, $SQLITE_ASSOC)) { inc %drained }
    t.log DRAINED extra= $+ %drained nomore= $+ $iif($sqlite_fetch_row(%r, trow4, $SQLITE_ASSOC), 1, 0)
    sqlite_free %r
  }
  t.log FREE badid= $+ $iif($sqlite_free(999999), 1, 0)
  var %rb = $sqlite_query(999999, SELECT 1)
  t.log QUERY badid ok= $+ $iif(%rb, 1, 0)
  return
  :s6
  t.log ESCAPE quote= $+ $sqlite_escape_string(o'clock) backslash= $+ $sqlite_escape_string(a\b) uni= $+ $sqlite_escape_string($chr(233))
  return
  :s7
  var %r = $sqlite_query(%t.db, SELECT * FROM nosuchtable)
  t.log BADQUERY ok= $+ $iif(%r, 1, 0) errno= $+ %sqlite_errno errstr= $+ %sqlite_errstr
  var %e = $sqlite_exec(%t.db, THIS IS NOT SQL)
  t.log BADEXEC ok= $+ $iif(%e, 1, 0) errno= $+ %sqlite_errno
  var %r2 = $sqlite_query(%t.db, SELECT 1 AS one)
  t.log ERRNORESET ok= $+ $iif(%r2, 1, 0) errno= $+ %sqlite_errno one= $+ $sqlite_result(%r2, one)
  if (%r2) { sqlite_free %r2 }
  return
  :s8
  write $qt(%t.dbdir $+ \batch.sql) CREATE TABLE IF NOT EXISTS f(id INTEGER PRIMARY KEY, v TEXT);
  write $qt(%t.dbdir $+ \batch.sql) INSERT INTO f(v) VALUES('fromfile');
  var %ok = $sqlite_exec_file(%t.db, %t.dbdir $+ \batch.sql)
  var %n = $sqlite_query(%t.db, SELECT COUNT(*) AS n FROM f)
  t.log EXECFILE ok= $+ $iif(%ok, 1, 0) rows= $+ $sqlite_result(%n, n)
  if (%n) { sqlite_free %n }
  return
  :s9
  t.log BUSY ms1000= $+ $iif($sqlite_busy_timeout(%t.db, 1000), 1, 0) ms0= $+ $iif($sqlite_busy_timeout(%t.db, 0), 1, 0)
  if (%t.variant == new) { t.log BUSY badid= $+ $iif($sqlite_busy_timeout(999999, 1000), 1, 0) }
  else { t.log BUSY badid=skipped }
  return
  :s10
  var %e1 = $sqlite_exec(%t.db, CREATE TABLE IF NOT EXISTS u1(id INTEGER PRIMARY KEY, v TEXT))
  var %e2 = $sqlite_exec(%t.db, CREATE TABLE IF NOT EXISTS u2(id INTEGER PRIMARY KEY, w TEXT))
  t.probe using-chain SELECT u1.id, v, w FROM u1 JOIN u2 USING (id)
  return
  :s11
  t.probe window-fn SELECT row_number() OVER (ORDER BY id) AS rn FROM u1
  return
  :s12
  t.probe upsert INSERT INTO u1(id, v) VALUES(1, 'x') ON CONFLICT(id) DO UPDATE SET v = excluded.v
  return
  :s13
  t.probe offset-null SELECT 1 AS n LIMIT 1 OFFSET NULL
  return
  :s14
  sqlite_close %t.db
  var %db = $sqlite_open(%t.dbdir $+ \proof.db)
  var %r = $sqlite_query(%db, SELECT COUNT(*) AS n FROM t)
  t.log PERSIST rows= $+ $sqlite_result(%r, n)
  if (%r) { sqlite_free %r }
  sqlite_close %db
  return
  :s15
  var %i = 1, %n = $line(Status Window, 0)
  while (%i <= %n) {
    if (error isin $line(Status Window, %i)) { t.log STATUS $line(Status Window, %i) }
    inc %i
  }
  return
  :s16
  t.log END
  exit -n
  return
  :s
}
alias t.run {
  inc %t.i
  if (%t.i > 16) { return }
  .timerTRUN -m 1 100 t.run
  t.step %t.i
  return
  :error
  t.log STEP %t.i SCRIPT-ERROR $error
  reseterror
}
on *:START: {
  t.cfg
  set %t.i 0
  .timerTRUN -m 1 500 t.run
}
