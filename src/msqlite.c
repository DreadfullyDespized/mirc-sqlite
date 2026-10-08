#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>
#include "sqlite3.h"

#define WM_MCOMMAND (WM_USER + 200)
#define WM_MEVALUATE (WM_USER + 201)
#define MAPSIZE 65536
#define DLLVERSION "2.0.0"
#define DEF_BUSY_MS 1000
#define TMP_PATH_LEN (MAX_PATH + 48)

typedef struct {
  DWORD mVersion;
  HWND mHwnd;
  BOOL mKeep;
  BOOL mUnicode;
  DWORD mBeta;
  DWORD mBytes;
} LOADINFO;

typedef struct { char* b; size_t n; size_t cap; } dynbuf;

static void db_init(dynbuf* d) { d->b = 0; d->n = 0; d->cap = 0; }
static void db_putn(dynbuf* d, const char* s, size_t n) {
  size_t need = d->n + n + 1;
  if (need > d->cap) {
    size_t nc = d->cap ? d->cap : 64;
    char* nb;
    while (nc < need) nc *= 2;
    nb = (char*)realloc(d->b, nc);
    if (!nb) return;
    d->b = nb;
    d->cap = nc;
  }
  memcpy(d->b + d->n, s, n);
  d->n += n;
  d->b[d->n] = 0;
}
static void db_puts(dynbuf* d, const char* s) { db_putn(d, s, strlen(s)); }
static void db_putc(dynbuf* d, char c) { db_putn(d, &c, 1); }
static void db_free(dynbuf* d) { free(d->b); db_init(d); }

static HWND g_mwnd = 0;
static DWORD g_maxbytes = 4096;
static HANDLE g_map = 0;
static char* g_mapview = 0;
static unsigned long g_nextid = 1;
static unsigned long g_tmpctr = 0;
static char g_lasttmp[TMP_PATH_LEN] = "";
static int g_udf_active = 0;
static int g_udf_errset = 0;
static char g_udf_err[4096] = "";

static int map_ready(void) { return g_mwnd != 0 && g_mapview != 0; }

static void mirc_command(const char* cmd) {
  size_t n;
  if (!map_ready()) return;
  n = strlen(cmd);
  if (n >= MAPSIZE) n = MAPSIZE - 1;
  memcpy(g_mapview, cmd, n);
  g_mapview[n] = 0;
  SendMessage(g_mwnd, WM_MCOMMAND, 1, 0);
}

static int mirc_eval(const char* expr, char* out, DWORD cap) {
  size_t n;
  if (!map_ready() || cap == 0) return 0;
  n = strlen(expr);
  if (n >= MAPSIZE) n = MAPSIZE - 1;
  memcpy(g_mapview, expr, n);
  g_mapview[n] = 0;
  if (!SendMessage(g_mwnd, WM_MEVALUATE, 0, 0)) return 0;
  n = strnlen(g_mapview, MAPSIZE);
  if (n >= cap) n = cap - 1;
  memcpy(out, g_mapview, n);
  out[n] = 0;
  return 1;
}

static char* mirc_escape(const char* s) {
  size_t n = 0;
  const char* p;
  char* o;
  char* q;
  for (p = s; *p; p++) n += (*p == '$' || *p == '%' || *p == '\r' || *p == '\n') ? 8 : 1;
  o = (char*)malloc(n + 1);
  if (!o) return strdup("");
  q = o;
  for (p = s; *p; p++) {
    if (*p == '$') { memcpy(q, "$chr(36)", 8); q += 8; }
    else if (*p == '%') { memcpy(q, "$chr(37)", 8); q += 8; }
    else if (*p == '\r') { memcpy(q, "$chr(13)", 8); q += 8; }
    else if (*p == '\n') { memcpy(q, "$chr(10)", 8); q += 8; }
    else *q++ = *p;
  }
  *q = 0;
  return o;
}

static void set_err(int code, const char* msg) {
  dynbuf d;
  char num[32];
  char* e;
  if (!msg) msg = "";
  db_init(&d);
  db_puts(&d, "/set %sqlite_errno ");
  snprintf(num, sizeof(num), "%d", code);
  db_puts(&d, num);
  mirc_command(d.b ? d.b : "");
  db_free(&d);
  e = mirc_escape(msg);
  db_init(&d);
  db_puts(&d, "/set %sqlite_errstr ");
  db_puts(&d, e);
  mirc_command(d.b ? d.b : "");
  db_free(&d);
  free(e);
}

static void set_ok(void) {
  mirc_command("/set %sqlite_errno 0");
  mirc_command("/set %sqlite_errstr $null");
}

static void set_sqlite_err(sqlite3* db) {
  const char* m = sqlite3_errmsg(db);
  set_err(sqlite3_errcode(db), m ? m : "");
}

static int putstr(char* data, const char* s) {
  size_t n = strlen(s);
  if (n >= g_maxbytes) n = g_maxbytes - 1;
  memcpy(data, s, n);
  data[n] = 0;
  return 3;
}

static int putnull(char* data) {
  data[0] = 0;
  return 3;
}

static int put_id(char* data, unsigned long id) {
  char b[32];
  snprintf(b, sizeof(b), "%lu", id);
  return putstr(data, b);
}

typedef struct {
  int t;
  sqlite3_int64 i;
  double d;
  char* s;
  int n;
} cell_t;

typedef struct {
  int col;
  char var[128];
} bindcol_t;

typedef struct {
  int byname;
  char pname[128];
  int pidx;
  char var[128];
  int dtype;
} bindp_t;

typedef struct result {
  struct conn* conn;
  int buffered;
  int ncols;
  char** cols;
  int nrows;
  int cap;
  cell_t* cells;
  int pos;
  sqlite3_stmt* st;
  int done;
  int stepped;
  int fetchcount;
  bindcol_t* binds;
  int nbinds;
} result_t;

typedef struct stmt {
  struct conn* conn;
  sqlite3* db;
  sqlite3_stmt* st;
  bindp_t* binds;
  int nbinds;
} stmt_t;

typedef struct udfnode {
  void* p;
  struct udfnode* next;
} udfnode_t;

typedef struct conn {
  sqlite3* db;
  int ismem;
  char* tmppath;
  void* auth;
  result_t* unbuf;
  udfnode_t* udfs;
  int cb_active;
} conn_t;

static void free_cell(cell_t* c) {
  free(c->s);
  c->s = 0;
}

static void free_result(result_t* r) {
  int i;
  if (!r) return;
  if (r->conn && r->conn->unbuf == r) r->conn->unbuf = 0;
  for (i = 0; i < r->ncols; i++) free(r->cols[i]);
  free(r->cols);
  for (i = 0; i < r->nrows * r->ncols; i++) free_cell(&r->cells[i]);
  free(r->cells);
  free(r->binds);
  if (r->st) sqlite3_finalize(r->st);
  free(r);
}

static void free_stmt(stmt_t* s) {
  if (!s) return;
  free(s->binds);
  if (s->st) sqlite3_finalize(s->st);
  free(s);
}

static void free_conn(conn_t* c) {
  udfnode_t* u;
  if (!c) return;
  if (c->tmppath) {
    DeleteFileA(c->tmppath);
    free(c->tmppath);
  }
  free(c->auth);
  u = c->udfs;
  while (u) {
    udfnode_t* nx = u->next;
    free(u->p);
    free(u);
    u = nx;
  }
  if (c->db) sqlite3_close(c->db);
  free(c);
}

enum { HT_CONN = 0, HT_RESULT = 1, HT_STMT = 2 };

typedef struct handle {
  unsigned long id;
  int type;
  void* ptr;
  struct handle* next;
} mhandle_t;

static mhandle_t* g_handles = 0;

static mhandle_t* new_handle(int type, void* ptr) {
  mhandle_t* h = (mhandle_t*)calloc(1, sizeof(*h));
  if (!h) return 0;
  h->id = g_nextid++;
  h->type = type;
  h->ptr = ptr;
  h->next = g_handles;
  g_handles = h;
  return h;
}

static mhandle_t* find_handle(unsigned long id) {
  mhandle_t* h;
  for (h = g_handles; h; h = h->next)
    if (h->id == id) return h;
  return 0;
}

static void drop_handle(mhandle_t* h) {
  mhandle_t** pp;
  if (!h) return;
  for (pp = &g_handles; *pp; pp = &(*pp)->next) {
    if (*pp == h) {
      *pp = h->next;
      break;
    }
  }
  if (h->type == HT_CONN) free_conn((conn_t*)h->ptr);
  else if (h->type == HT_RESULT) free_result((result_t*)h->ptr);
  else free_stmt((stmt_t*)h->ptr);
  free(h);
}

static void close_conn_handle(mhandle_t* h) {
  conn_t* c = (conn_t*)h->ptr;
  mhandle_t* x = g_handles;
  while (x) {
    mhandle_t* nx = x->next;
    if (x != h && (x->type == HT_RESULT || x->type == HT_STMT)) {
      void* p = x->ptr;
      conn_t* oc = (x->type == HT_RESULT) ? ((result_t*)p)->conn : ((stmt_t*)p)->conn;
      if (oc == c) {
        if (x->type == HT_RESULT) ((result_t*)p)->conn = 0;
        else ((stmt_t*)p)->conn = 0;
        drop_handle(x);
      }
    }
    x = nx;
  }
  drop_handle(h);
}

static void close_all(void) {
  mhandle_t* h;
  int guard = 1000000;
  while (g_handles && guard-- > 0) {
    h = g_handles;
    if (h->type == HT_CONN) close_conn_handle(h);
    else drop_handle(h);
  }
  if (g_lasttmp[0]) {
    DeleteFileA(g_lasttmp);
    g_lasttmp[0] = 0;
  }
}

static const char* after_tok(const char* p) {
  while (*p && *p != ' ') p++;
  while (*p == ' ') p++;
  return p;
}

static int get_tok(const char* p, char* out, int cap) {
  int n = 0;
  while (*p && *p != ' ' && n < cap - 1) out[n++] = *p++;
  out[n] = 0;
  return n;
}

static int parse_ulong(const char* s, unsigned long* v) {
  unsigned long r = 0;
  if (!*s) return 0;
  while (*s) {
    if (*s < '0' || *s > '9') return 0;
    r = r * 10 + (unsigned long)(*s - '0');
    s++;
  }
  if (r == 0) return 0;
  *v = r;
  return 1;
}

static int parse_long_strict(const char* s, long* v) {
  long r = 0;
  int neg = 0;
  if (*s == '-') { neg = 1; s++; }
  if (!*s) return 0;
  while (*s) {
    if (*s < '0' || *s > '9') return 0;
    r = r * 10 + (*s - '0');
    s++;
  }
  *v = neg ? -r : r;
  return 1;
}

static conn_t* handle_conn(mhandle_t* h) {
  if (!h) return 0;
  if (h->type == HT_CONN) return (conn_t*)h->ptr;
  if (h->type == HT_RESULT) return ((result_t*)h->ptr)->conn;
  return ((stmt_t*)h->ptr)->conn;
}

static int conn_in_callback(conn_t* c) { return c && c->cb_active; }

static int reject_nested_call(const char* data) {
  char sid[64];
  unsigned long id;
  mhandle_t* h;
  get_tok(data, sid, sizeof(sid));
  if (!parse_ulong(sid, &id)) return 0;
  h = find_handle(id);
  if (conn_in_callback(handle_conn(h))) { set_err(200, "reentrant call"); return 1; }
  return 0;
}

static const char* parse_bindtok(const char* p, char** out) {
  dynbuf d;
  db_init(&d);
  if (p[0] != ',' || p[1] != '"') return 0;
  p += 2;
  while (*p && *p != '"') {
    if (*p == '\\' && p[1]) { db_putc(&d, p[1]); p += 2; }
    else { db_putc(&d, *p); p++; }
  }
  if (*p != '"') { db_free(&d); return 0; }
  p++;
  if (*p != ',') { db_free(&d); return 0; }
  p++;
  *out = d.b ? d.b : strdup("");
  return p;
}

static int parse_quoted2(const char* p, char* a1, int c1, char* a2, int c2) {
  int n = 0;
  a1[0] = 0;
  a2[0] = 0;
  while (*p && n < 2) {
    char* q;
    int cap;
    while (*p == ' ') p++;
    if (!*p) break;
    q = n == 0 ? a1 : a2;
    cap = n == 0 ? c1 : c2;
    if (*p == '"') {
      int i = 0;
      p++;
      while (*p && *p != '"' && i < cap - 1) q[i++] = *p++;
      q[i] = 0;
      if (*p == '"') p++;
    } else {
      int i = 0;
      while (*p && *p != ' ' && i < cap - 1) q[i++] = *p++;
      q[i] = 0;
    }
    n++;
  }
  return n;
}

typedef struct { int n; int cap; char** v; } bindset_t;

static void bs_add(bindset_t* b, char* s) {
  if (b->n >= b->cap) {
    int nc = b->cap ? b->cap * 2 : 8;
    char** nv = (char**)realloc(b->v, nc * sizeof(char*));
    if (!nv) { free(s); return; }
    b->v = nv;
    b->cap = nc;
  }
  b->v[b->n++] = s;
}

static void bs_free(bindset_t* b) {
  int i;
  for (i = 0; i < b->n; i++) free(b->v[i]);
  free(b->v);
  b->v = 0;
  b->n = b->cap = 0;
}

static int deduce_type(const char* v) {
  char* e;
  if (!*v) return SQLITE_TEXT;
  (void)strtoll(v, &e, 10);
  if (*e == 0) return SQLITE_INTEGER;
  (void)strtod(v, &e);
  if (*e == 0) return SQLITE_FLOAT;
  return SQLITE_TEXT;
}

static int bind_value(sqlite3_stmt* st, int idx, const char* v, int dtype) {
  if (dtype == SQLITE_INTEGER) return sqlite3_bind_int64(st, idx, strtoll(v, 0, 10));
  if (dtype == SQLITE_FLOAT) return sqlite3_bind_double(st, idx, strtod(v, 0));
  if (dtype == SQLITE_BLOB) return sqlite3_bind_blob(st, idx, v, (int)strlen(v), SQLITE_TRANSIENT);
  if (dtype == SQLITE_NULL) return sqlite3_bind_null(st, idx);
  return sqlite3_bind_text(st, idx, v, -1, SQLITE_TRANSIENT);
}

static int step_done(sqlite3* db, sqlite3_stmt* st) {
  int rc;
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {}
  if (rc != SQLITE_DONE) { set_sqlite_err(db); return 0; }
  return 1;
}

static result_t* materialize(sqlite3* db, sqlite3_stmt* st, conn_t* c, int* ok) {
  result_t* r = (result_t*)calloc(1, sizeof(*r));
  int rc, i, n;
  *ok = 0;
  if (!r) { set_err(7, "out of memory"); return 0; }
  r->conn = c;
  r->buffered = 1;
  r->pos = -1;
  n = sqlite3_column_count(st);
  r->ncols = n;
  r->cols = (char**)calloc(n ? n : 1, sizeof(char*));
  for (i = 0; i < n; i++) {
    const char* nm = sqlite3_column_name(st, i);
    r->cols[i] = strdup(nm ? nm : "");
    if (!r->cols[i]) { set_err(7, "out of memory"); free_result(r); return 0; }
  }
  while ((rc = sqlite3_step(st)) == SQLITE_ROW) {
    cell_t* row;
    if (r->nrows >= r->cap) {
      int ncap = r->cap ? r->cap * 2 : 64;
      cell_t* ncells = (cell_t*)realloc(r->cells, ncap * n * sizeof(cell_t));
      if (!ncells) { set_err(7, "out of memory"); free_result(r); return 0; }
      r->cells = ncells;
      r->cap = ncap;
    }
    row = &r->cells[r->nrows * n];
    memset(row, 0, n * sizeof(cell_t));
    for (i = 0; i < n; i++) {
      int t = sqlite3_column_type(st, i);
      if (t == SQLITE_INTEGER) { row[i].t = 1; row[i].i = sqlite3_column_int64(st, i); }
      else if (t == SQLITE_FLOAT) { row[i].t = 2; row[i].d = sqlite3_column_double(st, i); }
      else if (t == SQLITE_NULL) { row[i].t = 0; }
      else {
        const void* p = sqlite3_column_blob(st, i);
        int nb = sqlite3_column_bytes(st, i);
        row[i].t = (t == SQLITE_BLOB) ? 4 : 3;
        row[i].n = nb;
        row[i].s = (char*)malloc(nb + 1);
        if (!row[i].s) { set_err(7, "out of memory"); free_result(r); return 0; }
        if (nb) memcpy(row[i].s, p, nb);
        row[i].s[nb] = 0;
      }
    }
    r->nrows++;
  }
  if (rc != SQLITE_DONE) { set_sqlite_err(db); free_result(r); return 0; }
  *ok = 1;
  return r;
}

static int run_statements(conn_t* c, const char* sql, bindset_t* binds, int want_rows, mhandle_t** out) {
  const char* tail = sql;
  mhandle_t* lasth = 0;
  int first = 1;
  if (out) *out = 0;
  while (tail && *tail) {
    sqlite3_stmt* st = 0;
    int rc = sqlite3_prepare_v2(c->db, tail, -1, &st, &tail);
    if (rc != SQLITE_OK) { set_sqlite_err(c->db); goto fail; }
    if (!st) continue;
    if (first && binds && binds->n > 0) {
      int np = sqlite3_bind_parameter_count(st);
      int i;
      for (i = 0; i < binds->n && i < np; i++) {
        rc = bind_value(st, i + 1, binds->v[i], deduce_type(binds->v[i]));
        if (rc != SQLITE_OK) { set_sqlite_err(c->db); sqlite3_finalize(st); goto fail; }
      }
    }
    first = 0;
    if (want_rows && sqlite3_column_count(st) > 0) {
      int ok = 0;
      result_t* r = materialize(c->db, st, c, &ok);
      sqlite3_finalize(st);
      if (!ok) goto fail;
      if (lasth) drop_handle(lasth);
      lasth = new_handle(HT_RESULT, r);
      if (!lasth) { free_result(r); set_err(7, "out of memory"); goto fail; }
    } else {
      if (want_rows && lasth) { drop_handle(lasth); lasth = 0; }
      if (!step_done(c->db, st)) { sqlite3_finalize(st); goto fail; }
      sqlite3_finalize(st);
    }
  }
  if (out) *out = lasth;
  else if (lasth) drop_handle(lasth);
  set_ok();
  return 1;
fail:
  if (lasth) drop_handle(lasth);
  if (out) *out = 0;
  return 0;
}

static int backup_copy(sqlite3* dst, sqlite3* src) {
  sqlite3_backup* b = sqlite3_backup_init(dst, "main", src, "main");
  int rc;
  if (!b) return sqlite3_errcode(dst);
  rc = sqlite3_backup_step(b, -1);
  sqlite3_backup_finish(b);
  return rc == SQLITE_DONE ? SQLITE_OK : rc;
}

static void make_temp_path(char* out, const char* ext) {
  char dir[MAX_PATH];
  DWORD pid = GetCurrentProcessId();
  GetTempPathA(sizeof(dir), dir);
  snprintf(out, TMP_PATH_LEN, "%smsqlite_%lu_%lu.%s", dir, (unsigned long)pid, ++g_tmpctr, ext);
}

static char* read_file(const char* path) {
  FILE* f = fopen(path, "rb");
  long n;
  char* b;
  if (!f) return 0;
  fseek(f, 0, SEEK_END);
  n = ftell(f);
  if (n < 0) { fclose(f); return 0; }
  rewind(f);
  b = (char*)malloc(n + 1);
  if (b && n > 0 && fread(b, 1, n, f) != (size_t)n) { free(b); b = 0; }
  fclose(f);
  if (b) b[n] = 0;
  return b;
}

typedef struct { int isnull; const void* p; int n; char tmp[64]; } rawval_t;

static void rawval_buffered(result_t* r, int row, int col, rawval_t* v) {
  cell_t* c = &r->cells[row * r->ncols + col];
  v->isnull = 0;
  if (c->t == 0) { v->isnull = 1; v->p = ""; v->n = 0; return; }
  if (c->t == 1) { snprintf(v->tmp, sizeof(v->tmp), "%lld", (long long)c->i); v->p = v->tmp; v->n = (int)strlen(v->tmp); return; }
  if (c->t == 2) { snprintf(v->tmp, sizeof(v->tmp), "%.15g", c->d); v->p = v->tmp; v->n = (int)strlen(v->tmp); return; }
  v->p = c->s;
  v->n = c->n;
}

static void rawval_live(sqlite3_stmt* st, int col, rawval_t* v) {
  int t = sqlite3_column_type(st, col);
  v->isnull = 0;
  if (t == SQLITE_NULL) { v->isnull = 1; v->p = ""; v->n = 0; return; }
  if (t == SQLITE_INTEGER) { snprintf(v->tmp, sizeof(v->tmp), "%lld", (long long)sqlite3_column_int64(st, col)); v->p = v->tmp; v->n = (int)strlen(v->tmp); return; }
  if (t == SQLITE_FLOAT) { snprintf(v->tmp, sizeof(v->tmp), "%.15g", sqlite3_column_double(st, col)); v->p = v->tmp; v->n = (int)strlen(v->tmp); return; }
  v->p = sqlite3_column_blob(st, col);
  v->n = sqlite3_column_bytes(st, col);
  if (!v->p) { v->p = ""; v->n = 0; }
}

static int res_step(result_t* r) {
  if (r->buffered) {
    r->pos++;
    return r->pos < r->nrows;
  }
  if (r->done) return 0;
  if (sqlite3_step(r->st) == SQLITE_ROW) {
    r->stepped = 1;
    r->fetchcount++;
    return 1;
  }
  r->done = 1;
  return 0;
}

static int res_peek_row(result_t* r) {
  if (!r->buffered) return -1;
  if (r->pos + 1 < r->nrows) return r->pos + 1;
  return -1;
}

static int resolve_col(result_t* r, int isname, const char* field) {
  int i;
  long v;
  if (isname) {
    for (i = 0; i < r->ncols; i++)
      if (sqlite3_stricmp(r->cols[i], field) == 0) return i;
    return -1;
  }
  if (!parse_long_strict(field, &v) || v < 1 || v > r->ncols) return -1;
  return (int)v - 1;
}

static void rawval_to_cstr(rawval_t* v, char* out, int cap) {
  int n = v->n;
  if (cap <= 0) return;
  if (n >= cap) n = cap - 1;
  if (n > 0) memcpy(out, v->p, n);
  out[n] = 0;
}

static int hash_clear(const char* table) {
  char expr[512];
  char res[64];
  char* e = mirc_escape(table);
  dynbuf d;
  _snprintf(expr, sizeof(expr) - 1, "$hget(%s,0)", e);
  expr[sizeof(expr) - 1] = 0;
  free(e);
  if (!mirc_eval(expr, res, sizeof(res))) return 0;
  if (!res[0]) return 1;
  e = mirc_escape(table);
  db_init(&d);
  db_puts(&d, "/hfree ");
  db_puts(&d, e);
  mirc_command(d.b ? d.b : "");
  db_free(&d);
  free(e);
  return 1;
}

static void hash_add(const char* table, const char* item, rawval_t* v) {
  dynbuf d;
  char* et = mirc_escape(table);
  char* ei = mirc_escape(item);
  char* ed = 0;
  db_init(&d);
  db_puts(&d, "/hadd -m ");
  db_puts(&d, et);
  db_putc(&d, ' ');
  db_puts(&d, ei);
  if (!v->isnull) {
    char* txt = (char*)malloc(v->n + 1);
    if (txt) {
      memcpy(txt, v->p, v->n);
      txt[v->n] = 0;
      ed = mirc_escape(txt);
      free(txt);
      db_putc(&d, ' ');
      db_puts(&d, ed);
      free(ed);
    }
  }
  mirc_command(d.b ? d.b : "");
  db_free(&d);
  free(et);
  free(ei);
}

static int hash_write_row(result_t* r, int row, const char* table, int type) {
  int i;
  char num[16];
  rawval_t v;
  if (!hash_clear(table)) return 0;
  if (type == 1 || type == 3) {
    for (i = 0; i < r->ncols; i++) {
      if (r->buffered) rawval_buffered(r, row, i, &v);
      else rawval_live(r->st, i, &v);
      hash_add(table, r->cols[i], &v);
    }
  }
  if (type == 1 || type == 2) {
    for (i = 0; i < r->ncols; i++) {
      if (r->buffered) rawval_buffered(r, row, i, &v);
      else rawval_live(r->st, i, &v);
      snprintf(num, sizeof(num), "%d", i + 1);
      hash_add(table, num, &v);
    }
  }
  return 1;
}

static int temp_bin_path(char* out) {
  char dir[MAX_PATH];
  char shrt[MAX_PATH];
  DWORD pid = GetCurrentProcessId();
  GetTempPathA(sizeof(dir), dir);
  if (GetShortPathNameA(dir, shrt, sizeof(shrt)) && !strchr(shrt, ' '))
    snprintf(out, TMP_PATH_LEN, "%smsqlite_%lu_%lu.tmp", shrt, (unsigned long)pid, ++g_tmpctr);
  else
    snprintf(out, TMP_PATH_LEN, "%smsqlite_%lu_%lu.tmp", dir, (unsigned long)pid, ++g_tmpctr);
  return strchr(out, ' ') == 0;
}

static void temp_bin_cleanup(void) {
  if (g_lasttmp[0]) {
    DeleteFileA(g_lasttmp);
    g_lasttmp[0] = 0;
  }
}

static int write_temp_bin(const void* p, int n, char* outpath) {
  FILE* f;
  temp_bin_cleanup();
  if (!temp_bin_path(outpath)) return 0;
  f = fopen(outpath, "wb");
  if (!f) return 0;
  if (n > 0) fwrite(p, 1, n, f);
  fclose(f);
  strcpy(g_lasttmp, outpath);
  return 1;
}

static int parse_exec_shape(const char* data, unsigned long* id, int* is_stmt, int* file, bindset_t* binds, const char** sql) {
  char tok[64];
  const char* p = data;
  mhandle_t* h;
  get_tok(p, tok, sizeof(tok));
  if (!parse_ulong(tok, id)) { set_err(200, "invalid id"); return 0; }
  h = find_handle(*id);
  if (h && h->type == HT_STMT) {
    *is_stmt = 1;
    p = after_tok(p);
    while (p[0] == ',' && p[1] == '"') {
      char* val = 0;
      const char* np = parse_bindtok(p, &val);
      if (!np) { bs_free(binds); set_err(200, "invalid argument"); return 0; }
      bs_add(binds, val);
      p = np;
    }
    if (*p) { bs_free(binds); set_err(200, "invalid argument"); return 0; }
    *sql = 0;
    return 1;
  }
  *is_stmt = 0;
  p = after_tok(p);
  get_tok(p, tok, sizeof(tok));
  if (strcmp(tok, "0") != 0 && strcmp(tok, "1") != 0) { set_err(200, "invalid argument"); return 0; }
  *file = tok[0] - '0';
  p = after_tok(p);
  get_tok(p, tok, sizeof(tok));
  {
    long np = 0;
    const char* q = tok;
    long i;
    if (!*q) { set_err(200, "invalid argument"); return 0; }
    while (*q) {
      if (*q < '0' || *q > '9') { set_err(200, "invalid argument"); return 0; }
      np = np * 10 + (*q - '0');
      if (np > 1000000) { set_err(200, "invalid argument"); return 0; }
      q++;
    }
    p = after_tok(p);
    for (i = 0; i < np; i++) {
      char* val = 0;
      const char* nx = parse_bindtok(p, &val);
      if (!nx) { bs_free(binds); set_err(200, "invalid argument"); return 0; }
      bs_add(binds, val);
      p = nx;
    }
  }
  if (*p == ' ') p++;
  *sql = p;
  return 1;
}

static int apply_param_binds(stmt_t* s) {
  int i;
  for (i = 0; i < s->nbinds; i++) {
    bindp_t* b = &s->binds[i];
    int idx = b->byname ? sqlite3_bind_parameter_index(s->st, b->pname) : b->pidx;
    int rc;
    if (idx <= 0) { set_err(200, "invalid parameter"); return 0; }
    if (b->var[0] == '&') {
      char expr[256];
      char res[8192];
      const char* q;
      dynbuf vals;
      _snprintf(expr, sizeof(expr) - 1, "$bvar(%s,1-)", b->var);
      expr[sizeof(expr) - 1] = 0;
      if (!mirc_eval(expr, res, sizeof(res))) { sqlite3_bind_null(s->st, idx); continue; }
      db_init(&vals);
      q = res;
      while (*q) {
        char* e;
        long v = strtol(q, &e, 10);
        if (e == q) break;
        db_putc(&vals, (char)(v & 0xff));
        q = e;
        while (*q == ' ') q++;
      }
      rc = sqlite3_bind_blob(s->st, idx, vals.b ? vals.b : "", (int)vals.n, SQLITE_TRANSIENT);
      db_free(&vals);
    } else {
      char expr[256];
      char res[16384];
      _snprintf(expr, sizeof(expr) - 1, "%%%s", b->var);
      expr[sizeof(expr) - 1] = 0;
      if (!mirc_eval(expr, res, sizeof(res))) rc = sqlite3_bind_null(s->st, idx);
      else rc = bind_value(s->st, idx, res, b->dtype ? b->dtype : deduce_type(res));
    }
    if (rc != SQLITE_OK) { set_sqlite_err(s->db); return 0; }
  }
  return 1;
}

static int run_prepared_stmt(stmt_t* s, bindset_t* inline_b, int want_rows, mhandle_t** out) {
  int i, rc;
  int np = sqlite3_bind_parameter_count(s->st);
  if (out) *out = 0;
  if (!apply_param_binds(s)) return 0;
  for (i = 0; i < inline_b->n && i < np; i++) {
    rc = bind_value(s->st, i + 1, inline_b->v[i], deduce_type(inline_b->v[i]));
    if (rc != SQLITE_OK) { set_sqlite_err(s->db); return 0; }
  }
  if (want_rows) {
    int ok = 0;
    result_t* r = materialize(s->db, s->st, s->conn, &ok);
    sqlite3_reset(s->st);
    if (!ok) return 0;
    *out = new_handle(HT_RESULT, r);
    if (!*out) { free_result(r); set_err(7, "out of memory"); return 0; }
  } else {
    if (!step_done(s->db, s->st)) { sqlite3_reset(s->st); return 0; }
    sqlite3_reset(s->st);
  }
  set_ok();
  return 1;
}

int __stdcall msqlite_libversion(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  set_ok();
  return putstr(data, sqlite3_libversion());
}

int __stdcall msqlite_dllversion(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  set_ok();
  return putstr(data, DLLVERSION);
}

int __stdcall msqlite_error_string(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  int code = atoi(data);
  const char* m;
  set_ok();
  if (code == 200) m = "invalid argument";
  else if (code == 201) m = "no more rows";
  else if (code == 202) m = "not a memory database";
  else m = sqlite3_errstr(code);
  return putstr(data, m ? m : "unknown error");
}

int __stdcall msqlite_open(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  char db[512], from[512];
  int nargs = parse_quoted2(data, db, sizeof(db), from, sizeof(from));
  conn_t* c = 0;
  sqlite3* sq = 0;
  int rc;
  mhandle_t* h;
  if (nargs == 0 || db[0] == 0) {
    char path[TMP_PATH_LEN];
    make_temp_path(path, "db");
    rc = sqlite3_open_v2(path, &sq, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0);
    if (rc != SQLITE_OK) { set_err(rc, sq ? sqlite3_errmsg(sq) : "cannot open database"); if (sq) sqlite3_close(sq); return putnull(data); }
    c = (conn_t*)calloc(1, sizeof(*c));
    if (c) { c->db = sq; c->tmppath = strdup(path); }
  } else if (strcmp(db, ":memory:") == 0) {
    rc = sqlite3_open(":memory:", &sq);
    if (rc != SQLITE_OK) { set_err(rc, sq ? sqlite3_errmsg(sq) : "cannot open database"); if (sq) sqlite3_close(sq); return putnull(data); }
    if (nargs == 2 && from[0]) {
      FILE* f = fopen(from, "ab");
      sqlite3* fdb = 0;
      if (f) fclose(f);
      rc = sqlite3_open_v2(from, &fdb, SQLITE_OPEN_READWRITE, 0);
      if (rc == SQLITE_OK) rc = backup_copy(sq, fdb);
      if (fdb) sqlite3_close(fdb);
      if (rc != SQLITE_OK) { set_err(rc, sqlite3_errmsg(sq)); sqlite3_close(sq); return putnull(data); }
    }
    c = (conn_t*)calloc(1, sizeof(*c));
    if (c) { c->db = sq; c->ismem = 1; }
  } else {
    if (nargs == 2 && from[0]) { set_err(200, "invalid argument"); return putnull(data); }
    rc = sqlite3_open_v2(db, &sq, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0);
    if (rc != SQLITE_OK) { set_err(rc, sq ? sqlite3_errmsg(sq) : "cannot open database"); if (sq) sqlite3_close(sq); return putnull(data); }
    c = (conn_t*)calloc(1, sizeof(*c));
    if (c) c->db = sq;
  }
  if (!c || !c->db) { if (sq) sqlite3_close(sq); free(c); set_err(7, "out of memory"); return putnull(data); }
  sqlite3_busy_timeout(sq, DEF_BUSY_MS);
  h = new_handle(HT_CONN, c);
  if (!h) { free_conn(c); set_err(7, "out of memory"); return putnull(data); }
  set_ok();
  return put_id(data, h->id);
}

int __stdcall msqlite_close(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h;
  if (!parse_ulong(data, &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  close_conn_handle(h);
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_exec(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  int is_stmt = 0, file = 0;
  bindset_t b = { 0, 0, 0 };
  const char* sql = 0;
  mhandle_t* h;
  int ok;
  if (!parse_exec_shape(data, &id, &is_stmt, &file, &b, &sql)) return putnull(data);
  h = find_handle(id);
  if (is_stmt) {
    ok = run_prepared_stmt((stmt_t*)h->ptr, &b, 0, 0);
    bs_free(&b);
    return ok ? putstr(data, "1") : putnull(data);
  }
  if (!h || h->type != HT_CONN) { bs_free(&b); set_err(200, "invalid connection id"); return putnull(data); }
  {
    conn_t* c = (conn_t*)h->ptr;
    char* fsql = 0;
    if (c->unbuf) { bs_free(&b); set_err(21, sqlite3_errstr(21)); return putnull(data); }
    if (file) {
      fsql = read_file(sql);
      if (!fsql) { bs_free(&b); set_err(14, "cannot open file"); return putnull(data); }
      sql = fsql;
    }
    ok = run_statements(c, sql, &b, 0, 0);
    free(fsql);
    bs_free(&b);
    return ok ? putstr(data, "1") : putnull(data);
  }
}

int __stdcall msqlite_query(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  int is_stmt = 0, file = 0;
  bindset_t b = { 0, 0, 0 };
  const char* sql = 0;
  mhandle_t* h;
  int ok;
  mhandle_t* rh = 0;
  if (!parse_exec_shape(data, &id, &is_stmt, &file, &b, &sql)) return putnull(data);
  h = find_handle(id);
  if (is_stmt) {
    ok = run_prepared_stmt((stmt_t*)h->ptr, &b, 1, &rh);
    bs_free(&b);
    if (!ok) return putnull(data);
    return put_id(data, rh->id);
  }
  if (!h || h->type != HT_CONN) { bs_free(&b); set_err(200, "invalid connection id"); return putnull(data); }
  {
    conn_t* c = (conn_t*)h->ptr;
    char* fsql = 0;
    if (c->unbuf) { bs_free(&b); set_err(21, sqlite3_errstr(21)); return putnull(data); }
    if (file) {
      fsql = read_file(sql);
      if (!fsql) { bs_free(&b); set_err(14, "cannot open file"); return putnull(data); }
      sql = fsql;
    }
    ok = run_statements(c, sql, &b, 1, &rh);
    free(fsql);
    bs_free(&b);
    if (!ok) return putnull(data);
    if (rh) return put_id(data, rh->id);
    return putstr(data, "0");
  }
}

int __stdcall msqlite_free(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h;
  if (!parse_ulong(data, &id)) { set_err(200, "invalid result id"); return putnull(data); }
  h = find_handle(id);
  if (!h || (h->type != HT_RESULT && h->type != HT_STMT)) { set_err(200, "invalid result id"); return putnull(data); }
  drop_handle(h);
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_num_rows(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h;
  result_t* r;
  char b[32];
  if (!parse_ulong(data, &id)) { set_err(200, "invalid result id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_RESULT) { set_err(200, "invalid result id"); return putnull(data); }
  r = (result_t*)h->ptr;
  if (!r->buffered) { set_err(21, sqlite3_errstr(21)); return putnull(data); }
  set_ok();
  snprintf(b, sizeof(b), "%d", r->nrows);
  return putstr(data, b);
}

int __stdcall msqlite_num_fields(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h;
  result_t* r;
  char b[32];
  if (!parse_ulong(data, &id)) { set_err(200, "invalid result id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_RESULT) { set_err(200, "invalid result id"); return putnull(data); }
  r = (result_t*)h->ptr;
  set_ok();
  snprintf(b, sizeof(b), "%d", r->ncols);
  return putstr(data, b);
}

int __stdcall msqlite_changes(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h;
  char b[32];
  if (!parse_ulong(data, &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  set_ok();
  snprintf(b, sizeof(b), "%d", sqlite3_changes(((conn_t*)h->ptr)->db));
  return putstr(data, b);
}

int __stdcall msqlite_last_insert_rowid(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h;
  char b[32];
  if (!parse_ulong(data, &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  set_ok();
  snprintf(b, sizeof(b), "%lld", (long long)sqlite3_last_insert_rowid(((conn_t*)h->ptr)->db));
  return putstr(data, b);
}

int __stdcall msqlite_fetch_row(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h;
  result_t* r;
  char sid[64], stab[128], stype[16];
  const char* p = data;
  int type;
  int row;
  get_tok(p, sid, sizeof(sid)); p = after_tok(p);
  get_tok(p, stab, sizeof(stab)); p = after_tok(p);
  get_tok(p, stype, sizeof(stype));
  if (!parse_ulong(sid, &id)) { set_err(200, "invalid result id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_RESULT) { set_err(200, "invalid result id"); return putnull(data); }
  r = (result_t*)h->ptr;
  type = stype[0] ? atoi(stype) : 1;
  if (type < 1 || type > 3) { set_err(200, "invalid argument"); return putnull(data); }
  if (!stab[0]) { set_err(200, "invalid argument"); return putnull(data); }
  if (!res_step(r)) { set_err(201, "no more rows"); return putstr(data, "0"); }
  row = r->buffered ? r->pos : -1;
  if (!hash_write_row(r, row, stab, type)) { set_err(200, "hash table error"); return putnull(data); }
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_fetch_field(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sres[64], ssecond[256];
  const char* p = data;
  const char* q;
  int is_binvar = 0, isname = 0, advance = 1;
  char binvar[128] = "";
  dynbuf field;
  unsigned long id;
  mhandle_t* h;
  result_t* r;
  int col, row;
  rawval_t v;
  char out[16384];
  get_tok(p, sres, sizeof(sres));
  p = after_tok(p);
  if (!parse_ulong(sres, &id)) { set_err(200, "invalid result id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_RESULT) { set_err(200, "invalid result id"); return putnull(data); }
  r = (result_t*)h->ptr;
  db_init(&field);
  if (!*p) {
    col = 0;
  } else {
    get_tok(p, ssecond, sizeof(ssecond));
    q = after_tok(p);
    if (ssecond[0] == '&' && !*q) {
      is_binvar = 1;
      col = 0;
      get_tok(ssecond, binvar, sizeof(binvar));
    } else {
      const char* end = data + strlen(data);
      const char* t;
      char sadv[16];
      if (strcmp(ssecond, "0") != 0 && strcmp(ssecond, "1") != 0) { db_free(&field); set_err(200, "invalid argument"); return putnull(data); }
      isname = ssecond[0] - '0';
      while (end > q && *(end - 1) == ' ') end--;
      t = end;
      while (t > q && *(t - 1) != ' ') t--;
      if (*t == '&') {
        const char* e2;
        is_binvar = 1;
        get_tok(t, binvar, sizeof(binvar));
        e2 = t;
        while (e2 > q && *(e2 - 1) == ' ') e2--;
        t = e2;
        while (t > q && *(t - 1) != ' ') t--;
      }
      get_tok(t, sadv, sizeof(sadv));
      if (strcmp(sadv, "0") != 0 && strcmp(sadv, "1") != 0) { db_free(&field); set_err(200, "invalid argument"); return putnull(data); }
      advance = sadv[0] - '0';
      {
        const char* e2 = t;
        while (e2 > q && *(e2 - 1) == ' ') e2--;
        db_putn(&field, q, e2 - q);
      }
      col = resolve_col(r, isname, field.b ? field.b : "");
      db_free(&field);
      if (col < 0 || col >= r->ncols) { set_err(200, "invalid field"); return putnull(data); }
    }
  }
  if (advance) {
    if (!res_step(r)) { set_err(201, "no more rows"); return putnull(data); }
    row = r->buffered ? r->pos : -1;
  } else {
    if (!r->buffered) { set_err(21, sqlite3_errstr(21)); return putnull(data); }
    row = res_peek_row(r);
    if (row < 0) { set_err(201, "no more rows"); return putnull(data); }
  }
  if (r->buffered) rawval_buffered(r, row, col, &v);
  else rawval_live(r->st, col, &v);
  if (is_binvar) {
    char path[TMP_PATH_LEN];
    char rb[TMP_PATH_LEN + 160];
    if (!write_temp_bin(v.p, v.n, path)) { set_err(14, "cannot write temp file"); return putnull(data); }
    _snprintf(rb, sizeof(rb) - 1, "%s %d %s", path, v.n, binvar);
    rb[sizeof(rb) - 1] = 0;
    set_ok();
    return putstr(data, rb);
  }
  if (v.isnull) { set_ok(); return putnull(data); }
  rawval_to_cstr(&v, out, sizeof(out));
  set_ok();
  return putstr(data, out);
}

int __stdcall msqlite_busy_timeout(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sid[64], sms[64];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  long ms;
  get_tok(p, sid, sizeof(sid)); p = after_tok(p);
  get_tok(p, sms, sizeof(sms));
  if (!parse_ulong(sid, &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  if (!parse_long_strict(sms, &ms)) { set_err(200, "invalid argument"); return putnull(data); }
  sqlite3_busy_timeout(((conn_t*)h->ptr)->db, (int)ms);
  set_ok();
  return putstr(data, "1");
}

static int run_unbuffered(conn_t* c, const char* sql, bindset_t* binds, mhandle_t** out) {
  const char* tail = sql;
  sqlite3_stmt* kept = 0;
  int first = 1;
  int i, n;
  result_t* r;
  *out = 0;
  while (tail && *tail) {
    sqlite3_stmt* st = 0;
    int rc = sqlite3_prepare_v2(c->db, tail, -1, &st, &tail);
    if (rc != SQLITE_OK) { set_sqlite_err(c->db); goto fail; }
    if (!st) continue;
    if (first && binds && binds->n > 0) {
      int np = sqlite3_bind_parameter_count(st);
      for (i = 0; i < binds->n && i < np; i++) {
        rc = bind_value(st, i + 1, binds->v[i], deduce_type(binds->v[i]));
        if (rc != SQLITE_OK) { set_sqlite_err(c->db); sqlite3_finalize(st); goto fail; }
      }
    }
    first = 0;
    if (sqlite3_column_count(st) > 0) {
      if (kept) {
        if (!step_done(c->db, kept)) { sqlite3_finalize(kept); sqlite3_finalize(st); goto fail; }
        sqlite3_finalize(kept);
      }
      kept = st;
    } else {
      if (!step_done(c->db, st)) { sqlite3_finalize(st); goto fail; }
      sqlite3_finalize(st);
    }
  }
  if (kept) {
    r = (result_t*)calloc(1, sizeof(*r));
    if (!r) { sqlite3_finalize(kept); set_err(7, "out of memory"); return 0; }
    r->conn = c;
    r->buffered = 0;
    r->pos = -1;
    n = sqlite3_column_count(kept);
    r->ncols = n;
    r->cols = (char**)calloc(n ? n : 1, sizeof(char*));
    for (i = 0; i < n; i++) {
      const char* nm = sqlite3_column_name(kept, i);
      r->cols[i] = strdup(nm ? nm : "");
      if (!r->cols[i]) { sqlite3_finalize(kept); free_result(r); set_err(7, "out of memory"); return 0; }
    }
    r->st = kept;
    *out = new_handle(HT_RESULT, r);
    if (!*out) { free_result(r); set_err(7, "out of memory"); return 0; }
    c->unbuf = r;
  }
  set_ok();
  return 1;
fail:
  if (kept) sqlite3_finalize(kept);
  return 0;
}

int __stdcall msqlite_unbuffered_query(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  int is_stmt = 0, file = 0;
  bindset_t b = { 0, 0, 0 };
  const char* sql = 0;
  mhandle_t* h;
  int ok;
  mhandle_t* rh = 0;
  if (!parse_exec_shape(data, &id, &is_stmt, &file, &b, &sql)) return putnull(data);
  h = find_handle(id);
  if (is_stmt) {
    ok = run_prepared_stmt((stmt_t*)h->ptr, &b, 1, &rh);
    bs_free(&b);
    if (!ok) return putnull(data);
    return put_id(data, rh->id);
  }
  if (!h || h->type != HT_CONN) { bs_free(&b); set_err(200, "invalid connection id"); return putnull(data); }
  {
    conn_t* c = (conn_t*)h->ptr;
    char* fsql = 0;
    if (c->unbuf) { bs_free(&b); set_err(21, sqlite3_errstr(21)); return putnull(data); }
    if (file) {
      fsql = read_file(sql);
      if (!fsql) { bs_free(&b); set_err(14, "cannot open file"); return putnull(data); }
      sql = fsql;
    }
    ok = run_unbuffered(c, sql, &b, &rh);
    free(fsql);
    bs_free(&b);
    if (!ok) return putnull(data);
    if (rh) return put_id(data, rh->id);
    return putstr(data, "0");
  }
}

int __stdcall msqlite_prepare(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sid[64], sfile[16];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  conn_t* c;
  const char* sql;
  char* fsql = 0;
  sqlite3_stmt* st = 0;
  int rc;
  stmt_t* s;
  mhandle_t* nh;
  get_tok(p, sid, sizeof(sid)); p = after_tok(p);
  get_tok(p, sfile, sizeof(sfile)); p = after_tok(p);
  if (!parse_ulong(sid, &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  c = (conn_t*)h->ptr;
  if (strcmp(sfile, "0") != 0 && strcmp(sfile, "1") != 0) { set_err(200, "invalid argument"); return putnull(data); }
  sql = p;
  if (c->unbuf) { set_err(21, sqlite3_errstr(21)); return putnull(data); }
  if (sfile[0] == '1') {
    fsql = read_file(sql);
    if (!fsql) { set_err(14, "cannot open file"); return putnull(data); }
    sql = fsql;
  }
  rc = sqlite3_prepare_v2(c->db, sql, -1, &st, 0);
  free(fsql);
  if (rc != SQLITE_OK) { set_sqlite_err(c->db); return putnull(data); }
  if (!st) { set_err(200, "invalid argument"); return putnull(data); }
  s = (stmt_t*)calloc(1, sizeof(*s));
  if (!s) { sqlite3_finalize(st); set_err(7, "out of memory"); return putnull(data); }
  s->conn = c;
  s->db = c->db;
  s->st = st;
  nh = new_handle(HT_STMT, s);
  if (!nh) { free_stmt(s); set_err(7, "out of memory"); return putnull(data); }
  set_ok();
  return put_id(data, nh->id);
}

int __stdcall msqlite_bind_field(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sres[64], sisname[16], svar[128];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  result_t* r;
  int isname, col;
  dynbuf colname;
  bindcol_t* nb;
  const char* end;
  const char* t;
  const char* q;
  get_tok(p, sres, sizeof(sres)); p = after_tok(p);
  get_tok(p, sisname, sizeof(sisname)); p = after_tok(p);
  if (!parse_ulong(sres, &id)) { set_err(200, "invalid result id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_RESULT) { set_err(200, "invalid result id"); return putnull(data); }
  r = (result_t*)h->ptr;
  if (strcmp(sisname, "0") != 0 && strcmp(sisname, "1") != 0) { set_err(200, "invalid argument"); return putnull(data); }
  isname = sisname[0] - '0';
  end = data + strlen(data);
  while (end > p && *(end - 1) == ' ') end--;
  t = end;
  while (t > p && *(t - 1) != ' ') t--;
  get_tok(t, svar, sizeof(svar));
  if (!svar[0]) { set_err(200, "invalid argument"); return putnull(data); }
  q = t;
  while (q > p && *(q - 1) == ' ') q--;
  db_init(&colname);
  db_putn(&colname, p, q - p);
  col = resolve_col(r, isname, colname.b ? colname.b : "");
  db_free(&colname);
  if (col < 0) { set_err(200, "invalid column"); return putnull(data); }
  nb = (bindcol_t*)realloc(r->binds, (r->nbinds + 1) * sizeof(bindcol_t));
  if (!nb) { set_err(7, "out of memory"); return putnull(data); }
  r->binds = nb;
  r->binds[r->nbinds].col = col;
  strcpy(r->binds[r->nbinds].var, svar);
  r->nbinds++;
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_bind_param(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sstmt[64], sparam[128], svar[128], sdt[16];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  stmt_t* s;
  bindp_t* nb;
  bindp_t b;
  long v;
  memset(&b, 0, sizeof(b));
  get_tok(p, sstmt, sizeof(sstmt)); p = after_tok(p);
  get_tok(p, sparam, sizeof(sparam)); p = after_tok(p);
  get_tok(p, svar, sizeof(svar)); p = after_tok(p);
  get_tok(p, sdt, sizeof(sdt));
  if (!parse_ulong(sstmt, &id)) { set_err(200, "invalid statement id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_STMT) { set_err(200, "invalid statement id"); return putnull(data); }
  s = (stmt_t*)h->ptr;
  if (!sparam[0] || !svar[0]) { set_err(200, "invalid argument"); return putnull(data); }
  if (sparam[0] == ':') {
    b.byname = 1;
    memcpy(b.pname, sparam, sizeof(b.pname) - 1);
  } else {
    if (!parse_long_strict(sparam, &v) || v < 1) { set_err(200, "invalid argument"); return putnull(data); }
    b.pidx = (int)v;
  }
  strcpy(b.var, svar);
  if (!sdt[0]) b.dtype = 0;
  else {
    if (!parse_long_strict(sdt, &v) || v < 1 || v > 5) { set_err(200, "invalid argument"); return putnull(data); }
    b.dtype = (int)v;
  }
  nb = (bindp_t*)realloc(s->binds, (s->nbinds + 1) * sizeof(bindp_t));
  if (!nb) { set_err(7, "out of memory"); return putnull(data); }
  s->binds = nb;
  s->binds[s->nbinds++] = b;
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_bind_value(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sstmt[64], sparam[128], sdt[16];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  stmt_t* s;
  int idx, dtype, rc;
  long v;
  const char* val;
  get_tok(p, sstmt, sizeof(sstmt)); p = after_tok(p);
  get_tok(p, sparam, sizeof(sparam)); p = after_tok(p);
  if (!parse_ulong(sstmt, &id)) { set_err(200, "invalid statement id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_STMT) { set_err(200, "invalid statement id"); return putnull(data); }
  s = (stmt_t*)h->ptr;
  if (sparam[0] == ':') idx = sqlite3_bind_parameter_index(s->st, sparam);
  else {
    if (!parse_long_strict(sparam, &v) || v < 1) { set_err(200, "invalid argument"); return putnull(data); }
    idx = (int)v;
  }
  if (idx <= 0) { set_err(200, "invalid parameter"); return putnull(data); }
  if (!*p) {
    rc = sqlite3_bind_null(s->st, idx);
    if (rc != SQLITE_OK) { set_sqlite_err(s->db); return putnull(data); }
    set_ok();
    return putstr(data, "1");
  }
  get_tok(p, sdt, sizeof(sdt)); p = after_tok(p);
  if (!parse_long_strict(sdt, &v) || (v != -1 && (v < 1 || v > 5))) { set_err(200, "invalid argument"); return putnull(data); }
  dtype = (int)v;
  val = p;
  if (dtype == -1) dtype = deduce_type(val);
  rc = bind_value(s->st, idx, val, dtype);
  if (rc != SQLITE_OK) { set_sqlite_err(s->db); return putnull(data); }
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_clear_bindings(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h;
  if (!parse_ulong(data, &id)) { set_err(200, "invalid id"); return putnull(data); }
  h = find_handle(id);
  if (!h || (h->type != HT_RESULT && h->type != HT_STMT)) { set_err(200, "invalid id"); return putnull(data); }
  if (h->type == HT_RESULT) {
    result_t* r = (result_t*)h->ptr;
    free(r->binds);
    r->binds = 0;
    r->nbinds = 0;
  } else {
    stmt_t* s = (stmt_t*)h->ptr;
    free(s->binds);
    s->binds = 0;
    s->nbinds = 0;
    sqlite3_clear_bindings(s->st);
  }
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_autocommit(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sid[64], smode[16];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  sqlite3* db;
  long m;
  int rc;
  get_tok(p, sid, sizeof(sid)); p = after_tok(p);
  get_tok(p, smode, sizeof(smode));
  if (!parse_ulong(sid, &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  db = ((conn_t*)h->ptr)->db;
  if (!smode[0]) {
    set_ok();
    return putstr(data, sqlite3_get_autocommit(db) ? "1" : "0");
  }
  if (!parse_long_strict(smode, &m)) { set_err(200, "invalid argument"); return putnull(data); }
  if (m) {
    if (!sqlite3_get_autocommit(db)) {
      rc = sqlite3_exec(db, "COMMIT", 0, 0, 0);
      if (rc != SQLITE_OK) { set_sqlite_err(db); return putnull(data); }
    }
  } else {
    if (sqlite3_get_autocommit(db)) {
      rc = sqlite3_exec(db, "BEGIN", 0, 0, 0);
      if (rc != SQLITE_OK) { set_sqlite_err(db); return putnull(data); }
    }
  }
  set_ok();
  return putstr(data, "1");
}

typedef struct { conn_t* conn; char sqlname[128]; char alias[128]; char prop[128]; int nargs; } udf_t;
typedef struct { udf_t b; char stepalias[128]; char stepprop[128]; char finalias[128]; char finprop[128]; } uag_t;
typedef struct { conn_t* conn; char alias[128]; char prop[128]; } auth_t;
typedef struct { char* s; } actx_t;

static void udf_arg_text(sqlite3_value* v, char* out, int cap) {
  int t = sqlite3_value_type(v);
  int n;
  if (cap <= 0) return;
  if (t == SQLITE_INTEGER) n = snprintf(out, cap, "%lld", (long long)sqlite3_value_int64(v));
  else if (t == SQLITE_FLOAT) n = snprintf(out, cap, "%.15g", sqlite3_value_double(v));
  else if (t == SQLITE_NULL) { out[0] = 0; return; }
  else {
    const void* p = sqlite3_value_blob(v);
    n = sqlite3_value_bytes(v);
    if (!p) n = 0;
    if (n >= cap) n = cap - 1;
    if (n > 0) memcpy(out, p, n);
    out[n < 0 ? 0 : n] = 0;
    return;
  }
  if (n >= cap) out[cap - 1] = 0;
}

static void udf_build_call(dynbuf* d, const char* alias, const char* prop, const char* first, int argc, sqlite3_value** argv) {
  int i;
  char ab[1024];
  char* e;
  db_putc(d, '$');
  db_puts(d, alias);
  db_putc(d, '(');
  if (first) { e = mirc_escape(first); db_puts(d, e); free(e); }
  for (i = 0; i < argc; i++) {
    if (i || first) db_putc(d, ',');
    udf_arg_text(argv[i], ab, sizeof(ab));
    e = mirc_escape(ab);
    db_puts(d, e);
    free(e);
  }
  db_putc(d, ')');
  if (prop && prop[0]) { db_putc(d, '.'); db_puts(d, prop); }
}

static void udf_func(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  udf_t* u = (udf_t*)sqlite3_user_data(ctx);
  dynbuf d;
  char out[16384];
  db_init(&d);
  udf_build_call(&d, u->alias, u->prop, 0, argc, argv);
  g_udf_active = 1;
  g_udf_errset = 0;
  u->conn->cb_active = 1;
  if (!mirc_eval(d.b ? d.b : "", out, sizeof(out))) {
    u->conn->cb_active = 0;
    g_udf_active = 0;
    db_free(&d);
    sqlite3_result_error(ctx, "mIRC evaluation failed", -1);
    return;
  }
  db_free(&d);
  u->conn->cb_active = 0;
  g_udf_active = 0;
  if (g_udf_errset) sqlite3_result_error(ctx, g_udf_err, -1);
  else sqlite3_result_text(ctx, out, -1, SQLITE_TRANSIENT);
}

static void uag_step(sqlite3_context* ctx, int argc, sqlite3_value** argv) {
  uag_t* u = (uag_t*)sqlite3_user_data(ctx);
  actx_t* a = (actx_t*)sqlite3_aggregate_context(ctx, sizeof(*a));
  dynbuf d;
  char out[16384];
  if (!a) return;
  db_init(&d);
  udf_build_call(&d, u->stepalias, u->stepprop, a->s ? a->s : "", argc, argv);
  g_udf_active = 1;
  g_udf_errset = 0;
  u->b.conn->cb_active = 1;
  if (mirc_eval(d.b ? d.b : "", out, sizeof(out))) {
    free(a->s);
    a->s = strdup(out);
  }
  db_free(&d);
  u->b.conn->cb_active = 0;
  g_udf_active = 0;
  if (g_udf_errset) sqlite3_result_error(ctx, g_udf_err, -1);
  else if (!a->s) sqlite3_result_error_nomem(ctx);
}

static void uag_final(sqlite3_context* ctx) {
  uag_t* u = (uag_t*)sqlite3_user_data(ctx);
  actx_t* a = (actx_t*)sqlite3_aggregate_context(ctx, sizeof(*a));
  dynbuf d;
  char out[16384];
  int ok;
  db_init(&d);
  udf_build_call(&d, u->finalias, u->finprop, a && a->s ? a->s : "", 0, 0);
  g_udf_active = 1;
  g_udf_errset = 0;
  u->b.conn->cb_active = 1;
  ok = mirc_eval(d.b ? d.b : "", out, sizeof(out));
  db_free(&d);
  u->b.conn->cb_active = 0;
  g_udf_active = 0;
  if (a) { free(a->s); a->s = 0; }
  if (!ok) sqlite3_result_error(ctx, "mIRC evaluation failed", -1);
  else if (g_udf_errset) sqlite3_result_error(ctx, g_udf_err, -1);
  else sqlite3_result_text(ctx, out, -1, SQLITE_TRANSIENT);
}

static int auth_cb(void* p, int action, const char* a1, const char* a2, const char* dbn, const char* trig) {
  auth_t* a = (auth_t*)p;
  dynbuf d;
  char out[256];
  char num[16];
  long v;
  char* e;
  db_init(&d);
  db_putc(&d, '$');
  db_puts(&d, a->alias);
  db_putc(&d, '(');
  snprintf(num, sizeof(num), "%d", action);
  db_puts(&d, num);
  e = mirc_escape(a1 ? a1 : ""); db_putc(&d, ','); db_puts(&d, e); free(e);
  e = mirc_escape(a2 ? a2 : ""); db_putc(&d, ','); db_puts(&d, e); free(e);
  e = mirc_escape(dbn ? dbn : ""); db_putc(&d, ','); db_puts(&d, e); free(e);
  e = mirc_escape(trig ? trig : ""); db_putc(&d, ','); db_puts(&d, e); free(e);
  db_putc(&d, ')');
  if (a->prop[0]) { db_putc(&d, '.'); db_puts(&d, a->prop); }
  a->conn->cb_active = 1;
  if (!mirc_eval(d.b ? d.b : "", out, sizeof(out))) { a->conn->cb_active = 0; db_free(&d); return SQLITE_DENY; }
  a->conn->cb_active = 0;
  db_free(&d);
  v = strtol(out, &e, 10);
  if (e == out || (v != 0 && v != 1 && v != 2)) return SQLITE_DENY;
  return (int)v;
}

static udfnode_t* udf_track(conn_t* c, void* p) {
  udfnode_t* un = (udfnode_t*)calloc(1, sizeof(*un));
  if (!un) { free(p); return 0; }
  un->p = p;
  un->next = c->udfs;
  c->udfs = un;
  return un;
}

int __stdcall msqlite_create_function(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sid[64], sname[128], salias[128], snargs[16], sprop[128];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  conn_t* c;
  long nargs = -1;
  udf_t* u;
  int rc;
  get_tok(p, sid, sizeof(sid)); p = after_tok(p);
  get_tok(p, sname, sizeof(sname)); p = after_tok(p);
  get_tok(p, salias, sizeof(salias)); p = after_tok(p);
  get_tok(p, snargs, sizeof(snargs)); p = after_tok(p);
  get_tok(p, sprop, sizeof(sprop));
  if (!parse_ulong(sid, &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  c = (conn_t*)h->ptr;
  if (!sname[0] || !salias[0]) { set_err(200, "invalid argument"); return putnull(data); }
  if (snargs[0] && !parse_long_strict(snargs, &nargs)) { set_err(200, "invalid argument"); return putnull(data); }
  u = (udf_t*)calloc(1, sizeof(*u));
  if (!u) { set_err(7, "out of memory"); return putnull(data); }
  u->conn = c;
  strcpy(u->sqlname, sname);
  strcpy(u->alias, salias);
  strcpy(u->prop, sprop);
  u->nargs = (int)nargs;
  rc = sqlite3_create_function(c->db, sname, (int)nargs, SQLITE_UTF8, u, udf_func, 0, 0);
  if (rc != SQLITE_OK) { free(u); set_sqlite_err(c->db); return putnull(data); }
  if (!udf_track(c, u)) { set_err(7, "out of memory"); return putnull(data); }
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_create_aggregate(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sid[64], sname[128], sstep[128], sfin[128], snargs[16], sstepprop[128], sfinprop[128];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  conn_t* c;
  long nargs = -1;
  uag_t* u;
  int rc;
  get_tok(p, sid, sizeof(sid)); p = after_tok(p);
  get_tok(p, sname, sizeof(sname)); p = after_tok(p);
  get_tok(p, sstep, sizeof(sstep)); p = after_tok(p);
  get_tok(p, sfin, sizeof(sfin)); p = after_tok(p);
  get_tok(p, snargs, sizeof(snargs)); p = after_tok(p);
  get_tok(p, sstepprop, sizeof(sstepprop)); p = after_tok(p);
  get_tok(p, sfinprop, sizeof(sfinprop));
  if (!parse_ulong(sid, &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  c = (conn_t*)h->ptr;
  if (!sname[0] || !sstep[0] || !sfin[0]) { set_err(200, "invalid argument"); return putnull(data); }
  if (snargs[0] && !parse_long_strict(snargs, &nargs)) { set_err(200, "invalid argument"); return putnull(data); }
  u = (uag_t*)calloc(1, sizeof(*u));
  if (!u) { set_err(7, "out of memory"); return putnull(data); }
  u->b.conn = c;
  strcpy(u->b.sqlname, sname);
  strcpy(u->stepalias, sstep);
  strcpy(u->stepprop, sstepprop);
  strcpy(u->finalias, sfin);
  strcpy(u->finprop, sfinprop);
  u->b.nargs = (int)nargs;
  rc = sqlite3_create_function(c->db, sname, (int)nargs, SQLITE_UTF8, u, 0, uag_step, uag_final);
  if (rc != SQLITE_OK) { free(u); set_sqlite_err(c->db); return putnull(data); }
  if (!udf_track(c, u)) { set_err(7, "out of memory"); return putnull(data); }
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_signal_error(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (!g_udf_active) { set_err(200, "not in user function"); return putnull(data); }
  strncpy(g_udf_err, data, sizeof(g_udf_err) - 1);
  g_udf_err[sizeof(g_udf_err) - 1] = 0;
  g_udf_errset = 1;
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_set_authorizer(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sid[64], salias[128], sprop[128];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  conn_t* c;
  auth_t* a;
  get_tok(p, sid, sizeof(sid)); p = after_tok(p);
  if (!parse_ulong(sid, &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  c = (conn_t*)h->ptr;
  free(c->auth);
  c->auth = 0;
  if (!*p) {
    sqlite3_set_authorizer(c->db, 0, 0);
    set_ok();
    return putstr(data, "1");
  }
  get_tok(p, salias, sizeof(salias)); p = after_tok(p);
  get_tok(p, sprop, sizeof(sprop));
  if (!salias[0]) { set_err(200, "invalid argument"); return putnull(data); }
  a = (auth_t*)calloc(1, sizeof(*a));
  if (!a) { set_err(7, "out of memory"); return putnull(data); }
  a->conn = c;
  strcpy(a->alias, salias);
  strcpy(a->prop, sprop);
  sqlite3_set_authorizer(c->db, auth_cb, a);
  c->auth = a;
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_load_extension(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sid[64], sfile[512], sentry[256];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  conn_t* c;
  char* errmsg = 0;
  int rc;
  get_tok(p, sid, sizeof(sid)); p = after_tok(p);
  get_tok(p, sfile, sizeof(sfile)); p = after_tok(p);
  get_tok(p, sentry, sizeof(sentry));
  if (!parse_ulong(sid, &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  c = (conn_t*)h->ptr;
  if (!sfile[0]) { set_err(200, "invalid argument"); return putnull(data); }
  rc = sqlite3_load_extension(c->db, sfile, sentry[0] ? sentry : 0, &errmsg);
  if (rc != SQLITE_OK) {
    set_err(rc, errmsg ? errmsg : "load extension failed");
    sqlite3_free(errmsg);
    return putnull(data);
  }
  set_ok();
  return putstr(data, "1");
}

static int is_valid_x(char* data, int type) {
  char sid[64];
  unsigned long id;
  mhandle_t* h;
  get_tok(data, sid, sizeof(sid));
  if (!sid[0]) { set_err(200, "invalid argument"); return putnull(data); }
  if (!parse_ulong(sid, &id)) { set_ok(); return putstr(data, "0"); }
  h = find_handle(id);
  set_ok();
  return putstr(data, (h && h->type == type) ? "1" : "0");
}

int __stdcall msqlite_is_valid_conn(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  return is_valid_x(data, HT_CONN);
}

int __stdcall msqlite_is_valid_result(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  return is_valid_x(data, HT_RESULT);
}

int __stdcall msqlite_is_valid_statement(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  return is_valid_x(data, HT_STMT);
}

int __stdcall msqlite_is_memory(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h;
  if (!parse_ulong(data, &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  set_ok();
  return putstr(data, ((conn_t*)h->ptr)->ismem ? "1" : "0");
}

int __stdcall msqlite_write_to_file(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sid[64], sfile[512], dummy[8];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  conn_t* c;
  sqlite3* fdb = 0;
  int rc;
  int nargs;
  get_tok(p, sid, sizeof(sid)); p = after_tok(p);
  nargs = parse_quoted2(p, sfile, sizeof(sfile), dummy, sizeof(dummy));
  if (!parse_ulong(sid, &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  c = (conn_t*)h->ptr;
  if (nargs == 0 || !sfile[0]) { set_err(200, "invalid argument"); return putnull(data); }
  if (!c->ismem) { set_err(202, "not a memory database"); return putnull(data); }
  rc = sqlite3_open_v2(sfile, &fdb, SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE, 0);
  if (rc == SQLITE_OK) rc = backup_copy(fdb, c->db);
  if (fdb) sqlite3_close(fdb);
  if (rc != SQLITE_OK) { set_err(rc, "backup failed"); return putnull(data); }
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_reload(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_field_name(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sres[64], sidx[16];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  result_t* r;
  long idx;
  get_tok(p, sres, sizeof(sres)); p = after_tok(p);
  get_tok(p, sidx, sizeof(sidx));
  if (!parse_ulong(sres, &id)) { set_err(200, "invalid result id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_RESULT) { set_err(200, "invalid result id"); return putnull(data); }
  r = (result_t*)h->ptr;
  if (!parse_long_strict(sidx, &idx) || idx < 1 || idx > r->ncols) { set_err(200, "invalid field"); return putnull(data); }
  set_ok();
  return putstr(data, r->cols[idx - 1]);
}

int __stdcall msqlite_field_type(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sres[64], sidx[16];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  result_t* r;
  long idx;
  int t;
  char b[16];
  get_tok(p, sres, sizeof(sres)); p = after_tok(p);
  get_tok(p, sidx, sizeof(sidx));
  if (!parse_ulong(sres, &id)) { set_err(200, "invalid result id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_RESULT) { set_err(200, "invalid result id"); return putnull(data); }
  r = (result_t*)h->ptr;
  if (!parse_long_strict(sidx, &idx) || idx < 1 || idx > r->ncols) { set_err(200, "invalid field"); return putnull(data); }
  if (r->buffered) {
    int row = res_peek_row(r);
    if (row < 0) { set_err(201, "no more rows"); return putnull(data); }
    t = r->cells[row * r->ncols + idx - 1].t;
    t = (t == 0) ? 5 : t;
  } else {
    if (!r->stepped || r->done) { set_err(201, "no more rows"); return putnull(data); }
    t = sqlite3_column_type(r->st, (int)idx - 1);
  }
  set_ok();
  snprintf(b, sizeof(b), "%d", t);
  return putstr(data, b);
}

static void fetch_all_row(FILE* f, result_t* r, int row, int delim) {
  int i, j;
  rawval_t v;
  for (i = 0; i < r->ncols; i++) {
    const unsigned char* p;
    int n;
    if (i) fputc(delim, f);
    if (r->buffered) rawval_buffered(r, row, i, &v);
    else rawval_live(r->st, i, &v);
    if (v.isnull) continue;
    p = (const unsigned char*)v.p;
    n = v.n;
    for (j = 0; j < n; j++) {
      unsigned char ch = p[j];
      if (ch == '\\' || ch == '\n' || ch == '\r' || ch == 0 || ch == delim) fprintf(f, "\\x%02x", ch);
      else fputc(ch, f);
    }
  }
  fputc('\n', f);
}

int __stdcall msqlite_fetch_all(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sres[64], sdelim[16], sfile[512];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  result_t* r;
  int delim = 9;
  FILE* f;
  int i;
  long dv;
  get_tok(p, sres, sizeof(sres)); p = after_tok(p);
  get_tok(p, sdelim, sizeof(sdelim)); p = after_tok(p);
  get_tok(p, sfile, sizeof(sfile));
  if (!parse_ulong(sres, &id)) { set_err(200, "invalid result id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_RESULT) { set_err(200, "invalid result id"); return putnull(data); }
  r = (result_t*)h->ptr;
  if (sdelim[0]) {
    if (!parse_long_strict(sdelim, &dv) || dv <= 0 || dv > 255) { set_err(200, "invalid argument"); return putnull(data); }
    delim = (int)dv;
  }
  if (!sfile[0]) { set_err(200, "invalid argument"); return putnull(data); }
  f = fopen(sfile, "wb");
  if (!f) { set_err(14, "cannot open file"); return putnull(data); }
  if (r->buffered) {
    for (i = 0; i < r->nrows; i++) fetch_all_row(f, r, i, delim);
  } else {
    while (!r->done) {
      if (sqlite3_step(r->st) != SQLITE_ROW) { r->done = 1; break; }
      r->stepped = 1;
      r->fetchcount++;
      fetch_all_row(f, r, -1, delim);
    }
  }
  fclose(f);
  set_ok();
  return putstr(data, "1");
}

static mhandle_t* valid_result(char* data, unsigned long* id) {
  if (!parse_ulong(data, id)) { set_err(200, "invalid result id"); return 0; }
  {
    mhandle_t* h = find_handle(*id);
    if (!h || h->type != HT_RESULT) { set_err(200, "invalid result id"); return 0; }
    return h;
  }
}

int __stdcall msqlite_has_more(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h = valid_result(data, &id);
  result_t* r;
  if (!h) return putnull(data);
  r = (result_t*)h->ptr;
  if (!r->buffered) { set_err(21, sqlite3_errstr(21)); return putnull(data); }
  set_ok();
  return putstr(data, r->pos + 1 < r->nrows ? "1" : "0");
}

int __stdcall msqlite_has_prev(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h = valid_result(data, &id);
  result_t* r;
  if (!h) return putnull(data);
  r = (result_t*)h->ptr;
  if (!r->buffered) { set_err(21, sqlite3_errstr(21)); return putnull(data); }
  set_ok();
  return putstr(data, r->pos >= 1 ? "1" : "0");
}

int __stdcall msqlite_next(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h = valid_result(data, &id);
  result_t* r;
  if (!h) return putnull(data);
  r = (result_t*)h->ptr;
  if (!r->buffered) { set_err(21, sqlite3_errstr(21)); return putnull(data); }
  if (r->pos + 1 < r->nrows) {
    r->pos++;
    set_ok();
    return putstr(data, "1");
  }
  set_err(201, "no more rows");
  return putstr(data, "0");
}

int __stdcall msqlite_prev(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h = valid_result(data, &id);
  result_t* r;
  if (!h) return putnull(data);
  r = (result_t*)h->ptr;
  if (!r->buffered) { set_err(21, sqlite3_errstr(21)); return putnull(data); }
  if (r->pos >= 1) {
    r->pos--;
    set_ok();
    return putstr(data, "1");
  }
  set_err(201, "no more rows");
  return putstr(data, "0");
}

int __stdcall msqlite_seek(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sres[64], sidx[16];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  result_t* r;
  long idx;
  get_tok(p, sres, sizeof(sres)); p = after_tok(p);
  get_tok(p, sidx, sizeof(sidx));
  h = valid_result(sres, &id);
  if (!h) return putnull(data);
  r = (result_t*)h->ptr;
  if (!r->buffered) { set_ok(); return putstr(data, "0"); }
  if (!parse_long_strict(sidx, &idx) || idx < 1 || idx > r->nrows) {
    set_err(201, "no more rows");
    return putstr(data, "0");
  }
  r->pos = (int)idx - 1;
  set_ok();
  return putstr(data, "1");
}

int __stdcall msqlite_key(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  unsigned long id;
  mhandle_t* h = valid_result(data, &id);
  result_t* r;
  char b[32];
  if (!h) return putnull(data);
  r = (result_t*)h->ptr;
  if (r->buffered) {
    if (r->pos < 0 || r->pos >= r->nrows) { set_err(201, "no more rows"); return putnull(data); }
    set_ok();
    snprintf(b, sizeof(b), "%d", r->pos + 1);
    return putstr(data, b);
  }
  if (r->fetchcount <= 0) { set_err(201, "no more rows"); return putnull(data); }
  set_ok();
  snprintf(b, sizeof(b), "%d", r->fetchcount);
  return putstr(data, b);
}

int __stdcall msqlite_current(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sres[64], stab[128], stype[16];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  result_t* r;
  int type, row;
  get_tok(p, sres, sizeof(sres)); p = after_tok(p);
  get_tok(p, stab, sizeof(stab)); p = after_tok(p);
  get_tok(p, stype, sizeof(stype));
  h = valid_result(sres, &id);
  if (!h) return putnull(data);
  r = (result_t*)h->ptr;
  type = stype[0] ? atoi(stype) : 1;
  if (type < 1 || type > 3) { set_err(200, "invalid argument"); return putnull(data); }
  if (!stab[0]) { set_err(200, "invalid argument"); return putnull(data); }
  if (r->buffered) {
    row = res_peek_row(r);
    if (row < 0) { set_err(201, "no more rows"); return putstr(data, "0"); }
  } else {
    if (!r->stepped || r->done) { set_err(201, "no more rows"); return putstr(data, "0"); }
    row = -1;
  }
  if (!hash_write_row(r, row, stab, type)) { set_err(200, "hash table error"); return putnull(data); }
  set_ok();
  return putstr(data, "1");
}

static int do_fetch_bound(result_t* r, int advance, int all, char* data) {
  int i, j, nbin = 0;
  int row;
  dynbuf bdata, sizes, bvars;
  char tmppath[TMP_PATH_LEN];
  if (advance) {
    if (!res_step(r)) { set_err(201, "no more rows"); return putnull(data); }
  } else {
    if (r->buffered) {
      if (res_peek_row(r) < 0) { set_err(201, "no more rows"); return putnull(data); }
    } else if (!r->stepped || r->done) { set_err(201, "no more rows"); return putnull(data); }
  }
  row = r->buffered ? (advance ? r->pos : r->pos + 1) : -1;
  db_init(&bdata);
  db_init(&sizes);
  db_init(&bvars);
  for (i = 0; i < r->ncols; i++) {
    const char* var = 0;
    char autobin[140];
    char autoname[132];
    rawval_t v;
    if (r->buffered) rawval_buffered(r, row, i, &v);
    else rawval_live(r->st, i, &v);
    for (j = r->nbinds - 1; j >= 0; j--) {
      if (r->binds[j].col == i) { var = r->binds[j].var; break; }
    }
    if (!var) {
      int isblob;
      if (!all) continue;
      if (r->buffered) isblob = !v.isnull && r->cells[row * r->ncols + i].t == 4;
      else isblob = !v.isnull && sqlite3_column_type(r->st, i) == SQLITE_BLOB;
      if (isblob) {
        _snprintf(autobin, sizeof(autobin) - 1, "&%s", r->cols[i]);
        autobin[sizeof(autobin) - 1] = 0;
        var = autobin;
      } else {
        strncpy(autoname, r->cols[i], sizeof(autoname) - 1);
        autoname[sizeof(autoname) - 1] = 0;
        var = autoname;
      }
    }
    if (var[0] == '&') {
      char nb[32];
      db_putn(&bdata, (const char*)v.p, v.n);
      snprintf(nb, sizeof(nb), "%d|", v.n);
      db_puts(&sizes, nb);
      db_puts(&bvars, var);
      db_putc(&bvars, '|');
      nbin++;
    } else {
      dynbuf cmd;
      char* evar = mirc_escape(var);
      db_init(&cmd);
      db_puts(&cmd, "/set %");
      db_puts(&cmd, evar);
      free(evar);
      if (v.isnull) {
        db_puts(&cmd, " $null");
      } else {
        char* txt = (char*)malloc(v.n + 1);
        if (txt) {
          char* etxt;
          memcpy(txt, v.p, v.n);
          txt[v.n] = 0;
          etxt = mirc_escape(txt);
          free(txt);
          db_putc(&cmd, ' ');
          db_puts(&cmd, etxt);
          free(etxt);
        }
      }
      mirc_command(cmd.b ? cmd.b : "");
      db_free(&cmd);
    }
  }
  if (nbin > 0) {
    dynbuf reply;
    FILE* f;
    temp_bin_cleanup();
    if (!temp_bin_path(tmppath)) {
      db_free(&bdata); db_free(&sizes); db_free(&bvars);
      set_err(200, "invalid temp path");
      return putnull(data);
    }
    f = fopen(tmppath, "wb");
    if (!f) {
      db_free(&bdata); db_free(&sizes); db_free(&bvars);
      set_err(14, "cannot write temp file");
      return putnull(data);
    }
    if (bdata.n) fwrite(bdata.b, 1, bdata.n, f);
    fclose(f);
    strcpy(g_lasttmp, tmppath);
    if (sizes.n) sizes.b[sizes.n - 1] = 0;
    if (bvars.n) bvars.b[bvars.n - 1] = 0;
    db_init(&reply);
    db_puts(&reply, tmppath);
    db_putc(&reply, ' ');
    db_puts(&reply, sizes.b ? sizes.b : "");
    db_putc(&reply, ' ');
    db_puts(&reply, bvars.b ? bvars.b : "");
    set_ok();
    putstr(data, reply.b ? reply.b : "");
    db_free(&reply);
  } else {
    set_ok();
    putstr(data, "1");
  }
  db_free(&bdata);
  db_free(&sizes);
  db_free(&bvars);
  return 3;
}

int __stdcall msqlite_fetch_bound(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sres[64], stype[16];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  int all;
  get_tok(p, sres, sizeof(sres)); p = after_tok(p);
  get_tok(p, stype, sizeof(stype));
  h = valid_result(sres, &id);
  if (!h) return putnull(data);
  if (!stype[0] || strcmp(stype, "2") == 0) all = 0;
  else if (strcmp(stype, "1") == 0) all = 1;
  else { set_err(200, "invalid argument"); return putnull(data); }
  return do_fetch_bound((result_t*)h->ptr, 1, all, data);
}

int __stdcall msqlite_current_bound(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char sres[64], stype[16];
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  int all;
  get_tok(p, sres, sizeof(sres)); p = after_tok(p);
  get_tok(p, stype, sizeof(stype));
  h = valid_result(sres, &id);
  if (!h) return putnull(data);
  if (!stype[0] || strcmp(stype, "2") == 0) all = 0;
  else if (strcmp(stype, "1") == 0) all = 1;
  else { set_err(200, "invalid argument"); return putnull(data); }
  return do_fetch_bound((result_t*)h->ptr, 0, all, data);
}

int __stdcall msqlite_safe_encode(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  char sdelim[16], sdatum[4096];
  const char* p = data;
  int delim = -1;
  dynbuf out;
  const unsigned char* q;
  long dv;
  get_tok(p, sdelim, sizeof(sdelim)); p = after_tok(p);
  get_tok(p, sdatum, sizeof(sdatum));
  if (!sdatum[0]) { set_ok(); return putnull(data); }
  if (sdelim[0] && strcmp(sdelim, "-1") != 0) {
    if (!parse_long_strict(sdelim, &dv) || dv <= 0 || dv > 255) { set_err(200, "invalid argument"); return putnull(data); }
    delim = (int)dv;
  }
  db_init(&out);
  for (q = (const unsigned char*)sdatum; *q; q++) {
    if (*q == '\\' || *q == '\n' || *q == '\r' || *q == delim) {
      char eb[8];
      snprintf(eb, sizeof(eb), "\\x%02x", *q);
      db_puts(&out, eb);
    } else db_putc(&out, (char)*q);
  }
  set_ok();
  putstr(data, out.b ? out.b : "");
  db_free(&out);
  return 3;
}

int __stdcall msqlite_safe_decode(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  char sdatum[4096];
  const unsigned char* q;
  dynbuf out;
  get_tok(data, sdatum, sizeof(sdatum));
  if (!sdatum[0]) { set_ok(); return putnull(data); }
  db_init(&out);
  q = (const unsigned char*)sdatum;
  while (*q) {
    if (q[0] == '\\' && q[1] == 'x' && isxdigit(q[2]) && isxdigit(q[3])) {
      char hx[3];
      hx[0] = (char)q[2]; hx[1] = (char)q[3]; hx[2] = 0;
      db_putc(&out, (char)strtol(hx, 0, 16));
      q += 4;
    } else {
      db_putc(&out, (char)*q);
      q++;
    }
  }
  set_ok();
  putstr(data, out.b ? out.b : "");
  db_free(&out);
  return 3;
}

int __stdcall msqlite_field_metadata(HWND mWnd, HWND aWnd, char* data, char* parms, BOOL show, BOOL nopause) {
  if (reject_nested_call(data)) return putnull(data);
  char toks[5][256];
  int ntok = 0;
  const char* p = data;
  unsigned long id;
  mhandle_t* h;
  sqlite3* db;
  const char* dbname;
  const char* table;
  const char* column;
  const char* htable;
  const char* dt = 0;
  const char* cs = 0;
  int nn = 0, pk = 0, ai = 0;
  int rc;
  rawval_t v;
  while (*p && ntok < 5) {
    get_tok(p, toks[ntok], sizeof(toks[ntok]));
    p = after_tok(p);
    ntok++;
  }
  if (ntok != 4 && ntok != 5) { set_err(200, "invalid argument"); return putnull(data); }
  if (!parse_ulong(toks[0], &id)) { set_err(200, "invalid connection id"); return putnull(data); }
  h = find_handle(id);
  if (!h || h->type != HT_CONN) { set_err(200, "invalid connection id"); return putnull(data); }
  db = ((conn_t*)h->ptr)->db;
  if (ntok == 5) { dbname = toks[1]; table = toks[2]; column = toks[3]; htable = toks[4]; }
  else { dbname = 0; table = toks[1]; column = toks[2]; htable = toks[3]; }
  rc = sqlite3_table_column_metadata(db, dbname, table, column, &dt, &cs, &nn, &pk, &ai);
  if (rc != SQLITE_OK) { set_sqlite_err(db); return putnull(data); }
  if (!hash_clear(htable)) { set_err(200, "hash table error"); return putnull(data); }
  v.isnull = !dt; v.p = dt ? dt : ""; v.n = dt ? (int)strlen(dt) : 0;
  hash_add(htable, "dattype", &v);
  v.isnull = !cs; v.p = cs ? cs : ""; v.n = cs ? (int)strlen(cs) : 0;
  hash_add(htable, "collseq", &v);
  v.isnull = 0; v.p = nn ? "1" : "0"; v.n = 1;
  hash_add(htable, "notnull", &v);
  v.p = pk ? "1" : "0";
  hash_add(htable, "primkey", &v);
  v.p = ai ? "1" : "0";
  hash_add(htable, "autoinc", &v);
  set_ok();
  return putstr(data, "1");
}

void __stdcall LoadDll(LOADINFO* li) {
  DWORD v;
  li->mKeep = TRUE;
  li->mUnicode = FALSE;
  g_mwnd = li->mHwnd;
  v = li->mVersion;
  if (LOWORD(v) > 7 || (LOWORD(v) == 7 && HIWORD(v) >= 64)) {
    if (li->mBytes >= 4096 && li->mBytes <= (1 << 20)) g_maxbytes = li->mBytes;
  }
  g_map = CreateFileMapping(INVALID_HANDLE_VALUE, 0, PAGE_READWRITE, 0, MAPSIZE, "mIRC");
  if (g_map) g_mapview = (char*)MapViewOfFile(g_map, FILE_MAP_ALL_ACCESS, 0, 0, 0);
}

int __stdcall UnloadDll(int t) {
  if (t == 1) return 0; /* mIRC idle-timeout probe: 0 keeps the DLL loaded */
  close_all();
  if (g_mapview) { UnmapViewOfFile(g_mapview); g_mapview = 0; }
  if (g_map) { CloseHandle(g_map); g_map = 0; }
  g_mwnd = 0;
  return 1;
}

int __stdcall msqlite_fetch_single(HWND a, HWND b, char* data, char* c, BOOL d, BOOL e) {
  char ndata[512];
  char sres[64], svar[128];
  const char* p = data;
  get_tok(p, sres, sizeof(sres)); p = after_tok(p);
  get_tok(p, svar, sizeof(svar));
  if (svar[0] == '&') _snprintf(ndata, sizeof(ndata) - 1, "%s 0 1 1 %s", sres, svar);
  else _snprintf(ndata, sizeof(ndata) - 1, "%s 0 1 1", sres);
  ndata[sizeof(ndata) - 1] = 0;
  return msqlite_fetch_field(a, b, ndata, c, d, e);
}
