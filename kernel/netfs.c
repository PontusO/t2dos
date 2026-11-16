// netfs.c - the network filesystem: /mnt/NAME shows a directory that tools/fileserver.py serves
// from the PC (up to NFS_MOUNTS of them).  One long-lived connection per mount in session mode (README, network protocol) carries
// STAT, READ, WRITE, TRUNC, LS, MKDIR, RM, RMDIR and RENAME requests.  Open files keep the 4 KB
// blocks they have read or written in a cache (memory from nt_alloc: PSRAM on the board); the
// block functions below (blk_*, file_save, file_drop) are the cache's whole policy, so a new
// policy replaces only them.  Shared by the board kernel and host_linux.c, which supply the
// nt_* transport; not reentrant (the kernel is never preempted).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "netfs.h"

#define BLK 4096           // a cache block, also the most one READ or WRITE moves
#define NAME 200           // a remote name: the mounted directory plus the path below /mnt/NAME
#define NFILES 16
#define NDIRS 4
#define TRIES 3            // attempts per request (and per save)
#define PAUSE 10           // ms after a broken connection before the next attempt
#define CONNECT_MS 2000
#define REPLY_MS 5000
#define DOWN_MS 10000      // while down, one reconnect attempt at most this often

// One mounted server: /mnt/name shows directory dir of host:port.  Each has its own
// connection and down state, so one server going away leaves the others working.
struct mount {
  int on;
  char name[16], host[64], dir[NAME];
  int port;
  int conn;                // the session connection, -1 when closed
  int down;                // 1 after a request failed all its tries
  unsigned down_at;
};
static struct mount mounts[NFS_MOUNTS];
static char nf_line[300];     // the last reply line

struct nfile {
  int used, mode, isdir;
  struct mount *mt;
  char abs[160], name[NAME];
  int size, pos, rsize;    // rsize: the size on the server when opened (blocks below it are read from there)
  int existed;             // NFS_APPEND: the file was there when opened
  int nblk, cap;           // blocks in use, room in the arrays
  unsigned char **blk;     // block i, or 0 when not cached
  unsigned char *dirty;
};
static struct nfile nf_files[NFILES];
struct ndir { int used; struct mount *mt; char *list, *at; };
static struct ndir nf_dirs[NDIRS];

// ---------------------------------------------------------------- connection and requests
static void drop_conn(struct mount *m) { if (m->conn >= 0) { nt_close(m->conn); m->conn = -1; } }
static int send_all(struct mount *m, const void *b, int n) { return n == 0 || nt_send(m->conn, b, n) == n ? 0 : -1; }
static int recv_line(struct mount *m, char *buf, int max) {
  int n = 0; char c;
  while (n < max - 1) {
    if (nt_recv(m->conn, &c, 1, REPLY_MS) != 1) return -1;
    if (c == '\n') break;
    buf[n++] = c;
  }
  buf[n] = 0;
  return n;
}
static int recv_all(struct mount *m, void *b, int n) {
  int got = 0, r;
  while (got < n) { r = nt_recv(m->conn, (char *)b + got, n - got, REPLY_MS); if (r <= 0) return -1; got += r; }
  return 0;
}
static int open_conn(struct mount *m) {
  m->conn = nt_connect(m->host, m->port, CONNECT_MS);
  if (m->conn < 0) { m->conn = -1; return -1; }
  if (send_all(m, "SESSION\n", 8) < 0 || recv_line(m, nf_line, sizeof nf_line) < 0 || strcmp(nf_line, "OK")) { drop_conn(m); return -1; }
  return 0;
}

// One request: req (a full line), then dlen bytes of data.  An "OK n" reply's n bytes go to in
// (at most inmax) or, with alloc, to a new nt_alloc'd, 0-terminated list.
struct op {
  const char *req; const void *data; int dlen;
  void *in; int inmax; int alloc;
  char *list; int got; int sent;
};
// one attempt: 0 for OK, -2 for an ERR reply (in nf_line), -1 if the connection failed (closed)
static int attempt(struct mount *m, struct op *o) {
  int n;
  o->sent = 0;
  if (m->conn < 0 && open_conn(m) < 0) return -1;
  if (send_all(m, o->req, strlen(o->req)) < 0 || send_all(m, o->data, o->dlen) < 0) goto broken;
  o->sent = 1;
  if (recv_line(m, nf_line, sizeof nf_line) < 0) goto broken;
  if (strncmp(nf_line, "OK", 2)) return -2;
  o->got = 0;
  if (o->in || o->alloc) {
    n = nf_line[2] == ' ' ? atoi(nf_line + 3) : 0;
    if (n < 0) goto broken;
    if (o->alloc) {
      o->list = (char *)nt_alloc(n + 1);
      if (!o->list) goto broken;               // no memory: the listing cannot be skipped cheaply
      if (recv_all(m, o->list, n) < 0) { nt_free(o->list); o->list = 0; goto broken; }
      o->list[n] = 0;
    } else {
      if (n > o->inmax || recv_all(m, o->in, n) < 0) goto broken;
    }
    o->got = n;
  }
  return 0;
broken:
  drop_conn(m);
  return -1;
}
// attempt() with retries: up to TRIES, PAUSE ms apart; after that the mount is down and requests
// fail at once, except one attempt every DOWN_MS.  once: no repeat after the request was sent
// (RENAME is not safe to repeat).  0, -2 (ERR) or -1.
static int request(struct mount *m, struct op *o, int once) {
  int k, r;
  if (!m->on) return -1;
  if (m->down) {
    if (nt_millis() - m->down_at < DOWN_MS) return -1;
    r = attempt(m, o);
    if (r == -1) { m->down_at = nt_millis(); return -1; }
    m->down = 0;
    return r;
  }
  for (k = 0; k < TRIES; k++) {
    if (k) nt_sleep(PAUSE);
    r = attempt(m, o);
    if (r != -1) return r;
    if (once && o->sent) return -1;
  }
  m->down = 1; m->down_at = nt_millis();
  return -1;
}
// a request without payload or data: 0, -2 or -1
static int simple(struct mount *m, const char *req, int once) {
  struct op o; memset(&o, 0, sizeof o); o.req = req;
  return request(m, &o, once);
}

// ---------------------------------------------------------------- names
// the mount a resolved path is on (/mnt/name or below it), with *rest the part after /mnt/name
// ("" or "/x/y"); 0 if it is on none
static struct mount *mnt_of(const char *abs, const char **rest) {
  const char *n = abs + 5; int k, i;
  if (strncmp(abs, "/mnt/", 5)) return 0;
  for (k = 0; n[k] && n[k] != '/'; k++) {}
  for (i = 0; i < NFS_MOUNTS; i++)
    if (mounts[i].on && (int)strlen(mounts[i].name) == k && !strncmp(mounts[i].name, n, k)) {
      if (rest) *rest = n + k;
      return &mounts[i];
    }
  return 0;
}
int nfs_mounted(void) { int i; for (i = 0; i < NFS_MOUNTS; i++) if (mounts[i].on) return 1; return 0; }
int nfs_path(const char *abs) { return mnt_of(abs, 0) != 0; }
int nfs_same(const char *a, const char *b) { struct mount *m = mnt_of(a, 0); return m && m == mnt_of(b, 0); }
// 1 for a mount's own directory (/mnt/name), which cannot be removed, renamed or written
static int mroot(const char *abs) { const char *r; return mnt_of(abs, &r) && !*r; }
// the remote name for a resolved path under a mount, and that mount; 0 if it is on none, too
// long, or holds a line break, which would end the request line and start another one
static struct mount *rname(const char *abs, char *out) {
  const char *rest; struct mount *m = mnt_of(abs, &rest);
  if (!m || strlen(m->dir) + strlen(rest) + 1 > NAME || strpbrk(rest, "\r\n")) return 0;
  if (!strcmp(m->dir, "/") && *rest) strcpy(out, rest);
  else { strcpy(out, m->dir); strcat(out, rest); }
  return m;
}
// "VERB name\n" and its mount, or 0 if it does not fit
static struct mount *req1(char *req, int sz, const char *verb, const char *abs) {
  char n[NAME]; struct mount *m = rname(abs, n);
  if (!m) return 0;
  return snprintf(req, sz, "%s %s\n", verb, n) < sz ? m : 0;
}

// ---------------------------------------------------------------- mount
static int busy(struct mount *m) {
  int i;
  for (i = 0; i < NFILES; i++) if (nf_files[i].used && nf_files[i].mt == m) return 1;
  for (i = 0; i < NDIRS; i++) if (nf_dirs[i].used && nf_dirs[i].mt == m) return 1;
  return 0;
}
static int good_name(const char *n) {
  int k = strlen(n), i;
  if (k < 1 || k > 15 || !strcmp(n, ".") || !strcmp(n, "..")) return 0;
  for (i = 0; i < k; i++) {
    char c = n[i];
    if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_' || c == '-' || c == '.')) return 0;
  }
  return 1;
}
int nfs_mountat(const char *name, const char *host, int port, const char *dir, int check) {
  struct mount *m = 0, old; char req[NAME + 8]; int i, r, k, had;
  if (!name || !good_name(name)) return -5;
  if (!dir) dir = "/";
  for (i = 0; i < NFS_MOUNTS; i++) if (mounts[i].on && !strcmp(mounts[i].name, name)) m = &mounts[i];
  if (!host || !*host) {                       // unmount
    if (!m) return -5;
    if (busy(m)) return -3;
    drop_conn(m); m->on = 0;
    return 0;
  }
  if (strlen(host) >= sizeof m->host || strlen(dir) + 2 > NAME) return -1;
  had = m != 0;
  if (m) {
    if (busy(m)) return -3;
    drop_conn(m);
  } else {
    for (i = 0; i < NFS_MOUNTS && mounts[i].on; i++) {}
    if (i == NFS_MOUNTS) return -4;
    m = &mounts[i];
  }
  old = *m;
  memset(m, 0, sizeof *m);
  strcpy(m->name, name); strcpy(m->host, host); m->port = port; m->conn = -1;
  if (dir[0] != '/') { m->dir[0] = '/'; strcpy(m->dir + 1, dir); } else strcpy(m->dir, dir);
  k = strlen(m->dir); while (k > 1 && m->dir[k - 1] == '/') m->dir[--k] = 0;
  m->on = 1;
  if (!check) return 0;
  snprintf(req, sizeof req, "STAT %s\n", m->dir);
  r = simple(m, req, 0);
  if (r == 0 && !strcmp(nf_line, "OK d")) return 0;
  r = r == -1 ? -1 : -2;                       // unreachable, or not a directory
  drop_conn(m);
  if (had) { *m = old; m->conn = -1; m->down = 0; } else m->on = 0;   // the old mount, as it was
  return r;
}
// one "host[:port]" and dir, from /etc/mount
static void boot_one(const char *name, char *host, char *dir) {
  int port = 7777; char *c = strchr(host, ':');
  if (c) { *c = 0; if (c[1]) port = atoi(c + 1); }
  nfs_mountat(name, host, port, *dir ? dir : "/", 0);
}
void nfs_boot_text(char *t) {
  char *nl = strchr(t, '\n'), *sp = strchr(t, ' '), *e, *host, *dir;
  if (nl && (!sp || sp > nl)) {                // the old format: host[:port], then dir
    *nl = 0; e = strchr(nl + 1, '\n'); if (e) *e = 0;
    boot_one("fs", t, nl + 1);
    return;
  }
  while (*t) {                                 // name host[:port] dir
    nl = strchr(t, '\n'); if (nl) *nl = 0;
    host = strchr(t, ' ');
    if (host) {
      *host++ = 0;
      dir = strchr(host, ' ');
      if (dir) { *dir++ = 0; boot_one(t, host, dir); }
    }
    if (!nl) break;
    t = nl + 1;
  }
}
int nfs_mount(const char *host, int port, const char *dir, int check) {
  int r = nfs_mountat("fs", host, port, dir, check);
  return (!host || !*host) && r == -5 ? 0 : r;   // unmounting /mnt/fs when nothing is there
}

int nfs_stat(const char *abs, int *size, int *isdir) {
  char req[NAME + 8]; struct mount *m = req1(req, sizeof req, "STAT", abs);
  if (!m || simple(m, req, 0)) return -1;
  *isdir = nf_line[3] == 'd';
  *size = *isdir ? 0 : atoi(nf_line + 5);         // "OK d" or "OK f size"
  return 0;
}

// ---------------------------------------------------------------- the cache
// makes room for one block: the first policy drops the clean blocks of every open file
static void blk_make_room(void) {
  int i, k;
  for (i = 0; i < NFILES; i++)
    for (k = 0; nf_files[i].used && k < nf_files[i].nblk; k++)
      if (nf_files[i].blk[k] && !nf_files[i].dirty[k]) { nt_free(nf_files[i].blk[k]); nf_files[i].blk[k] = 0; }
}
// room in f's arrays for block i; -1 without memory
static int blk_slots(struct nfile *f, int i) {
  unsigned char **b; unsigned char *d; int c;
  if (i < f->cap) return 0;
  c = f->cap ? f->cap : 8;
  while (c <= i) c *= 2;
  b = (unsigned char **)nt_alloc(c * sizeof *b); d = (unsigned char *)nt_alloc(c);
  if (!b || !d) { if (b) nt_free(b); if (d) nt_free(d); return -1; }
  memset(b, 0, c * sizeof *b); memset(d, 0, c);
  if (f->cap) { memcpy(b, f->blk, f->cap * sizeof *b); memcpy(d, f->dirty, f->cap); nt_free(f->blk); nt_free(f->dirty); }
  f->blk = b; f->dirty = d; f->cap = c;
  return 0;
}
// block i of f: cached, read from the server, or new zeros past its end there; 0 on failure
static unsigned char *blk_get(struct nfile *f, int i) {
  unsigned char *p; char req[NAME + 32]; struct op o; int k;
  if (blk_slots(f, i) < 0) return 0;
  if (i >= f->nblk) f->nblk = i + 1;
  if (f->blk[i]) return f->blk[i];
  p = (unsigned char *)nt_alloc(BLK);
  if (!p) { blk_make_room(); p = (unsigned char *)nt_alloc(BLK); if (!p) return 0; }
  memset(p, 0, BLK);
  if ((long)i * BLK < f->rsize) {
    if (snprintf(req, sizeof req, "READ %d %d %s\n", i * BLK, BLK, f->name) >= (int)sizeof req) { nt_free(p); return 0; }
    memset(&o, 0, sizeof o); o.req = req; o.in = p; o.inmax = BLK;
    // fewer bytes than the server had when the file was opened: it shrank there meanwhile,
    // and this block cannot be read (zeros would pass for data)
    k = f->rsize - i * BLK; if (k > BLK) k = BLK;
    if (request(f->mt, &o, 0) || o.got < k) { nt_free(p); return 0; }
  }
  f->blk[i] = p;
  return p;
}
// frees f's blocks and arrays
static void file_drop(struct nfile *f) {
  int k;
  for (k = 0; k < f->nblk; k++) if (f->blk[k]) nt_free(f->blk[k]);
  if (f->blk) nt_free(f->blk);
  if (f->dirty) nt_free(f->dirty);
  f->blk = 0; f->dirty = 0; f->nblk = f->cap = 0;
}
// one try at saving f, without retries of its own: 0, -2 (the server refused) or -1
static int save_once(struct nfile *f, const char *tmp) {
  static unsigned char zero[BLK];
  char req[NAME + 32], to[NAME + 2]; struct op o; int i, r, n;
  const char *dst = f->mode == NFS_WRITE ? tmp : f->name;
  memset(&o, 0, sizeof o); o.req = req;
  if (f->mode == NFS_WRITE || !f->existed) {
    snprintf(req, sizeof req, "TRUNC 0 %s\n", dst);
    if ((r = attempt(f->mt, &o))) return r;
  }
  for (i = 0; i * BLK < f->size; i++) {
    if (f->mode == NFS_APPEND && (i >= f->nblk || !f->dirty[i])) continue;
    n = f->size - i * BLK; if (n > BLK) n = BLK;
    snprintf(req, sizeof req, "WRITE %d %d %s\n", i * BLK, n, dst);
    o.data = i < f->nblk && f->blk[i] ? f->blk[i] : zero; o.dlen = n;
    if ((r = attempt(f->mt, &o))) return r;
  }
  if (f->mode == NFS_WRITE) {
    snprintf(req, sizeof req, "RENAME %s\n", tmp);
    snprintf(to, sizeof to, "%s\n", f->name);
    o.data = to; o.dlen = strlen(to);
    if ((r = attempt(f->mt, &o))) return r;
  }
  return 0;
}
// Saves f's changes.  A file opened with NFS_WRITE goes to ".NAME.t2tmp" in the same directory
// and is then renamed over NAME, so a broken save leaves the old file; an appended file gets
// its changed blocks written in place.  The whole save is tried up to TRIES times, each on a
// new connection.  0, or -1 (the mount is down after TRIES failures).
static int file_save(struct nfile *f) {
  char tmp[NAME]; const char *b; int k, r, tries = TRIES;
  if (f->mode == NFS_READ || f->isdir) return 0;
  if (f->mode == NFS_WRITE) {                  // dir/.base.t2tmp
    b = strrchr(f->name, '/'); b = b ? b + 1 : f->name;
    if (strlen(f->name) + 8 > NAME) return -1;
    memcpy(tmp, f->name, b - f->name); tmp[b - f->name] = 0;
    strcat(tmp, "."); strcat(tmp, b); strcat(tmp, ".t2tmp");
  }
  if (f->mt->down) {
    if (nt_millis() - f->mt->down_at < DOWN_MS) return -1;
    tries = 1;
  }
  for (k = 0; k < tries; k++) {
    if (k) { drop_conn(f->mt); nt_sleep(PAUSE); }
    r = save_once(f, tmp);
    if (r == 0) { f->mt->down = 0; return 0; }
    if (r == -2) return -1;                    // the server refused: trying again will not help
  }
  f->mt->down = 1; f->mt->down_at = nt_millis();
  return -1;
}

// ---------------------------------------------------------------- files
static struct nfile *fh(int h) { return h >= 0 && h < NFILES && nf_files[h].used ? &nf_files[h] : 0; }

int nfs_open(const char *abs, int mode) {
  int h, size, isdir; struct nfile *f;
  for (h = 0; h < NFILES && nf_files[h].used; h++) {}
  if (h == NFILES || strlen(abs) >= sizeof f->abs) return -1;
  f = &nf_files[h];
  memset(f, 0, sizeof *f);
  if (!(f->mt = rname(abs, f->name))) return -1;
  strcpy(f->abs, abs);
  f->mode = mode;
  if (mode == NFS_READ) {
    if (nfs_stat(abs, &size, &isdir) < 0) return -1;
    f->isdir = isdir; f->size = f->rsize = size;
  } else {                                     // a file (not a directory) in an existing directory
    char parent[160]; char *sl;
    if (mroot(abs)) return -1;
    if (mode == NFS_WRITE && strlen(f->name) + 8 > NAME) return -1;   // no room for its temp name (file_save)
    if (nfs_stat(abs, &size, &isdir) == 0) {
      if (isdir) return -1;
      if (mode == NFS_APPEND) { f->existed = 1; f->size = f->rsize = f->pos = size; }
    } else {
      strcpy(parent, abs); sl = strrchr(parent, '/'); *sl = 0;
      if (nfs_stat(parent, &size, &isdir) < 0 || !isdir) return -1;
    }
  }
  f->used = 1;
  return h;
}
int nfs_read(int h, void *b, int n) {
  struct nfile *f = fh(h); int done = 0, k; unsigned char *p;
  if (!f || f->isdir || f->mode != NFS_READ) return -1;
  if (n > f->size - f->pos) n = f->size - f->pos;
  while (done < n) {
    p = blk_get(f, f->pos / BLK);
    if (!p) return done ? done : -1;
    k = BLK - f->pos % BLK; if (k > n - done) k = n - done;
    memcpy((char *)b + done, p + f->pos % BLK, k);
    f->pos += k; done += k;
  }
  return done;
}
int nfs_write(int h, const void *b, int n) {
  struct nfile *f = fh(h); int done = 0, k, i; unsigned char *p;
  if (!f || f->mode == NFS_READ) return -1;
  if (f->mode == NFS_APPEND) f->pos = f->size;
  while (done < n) {
    i = f->pos / BLK;
    p = blk_get(f, i);                         // an appended file's last block is read first
    if (!p) break;                             // no room: a short count, like /tmp
    k = BLK - f->pos % BLK; if (k > n - done) k = n - done;
    memcpy(p + f->pos % BLK, (const char *)b + done, k);
    f->dirty[i] = 1;
    f->pos += k; done += k;
    if (f->pos > f->size) f->size = f->pos;
  }
  return done;
}
int nfs_seek(int h, int pos) {
  struct nfile *f = fh(h);
  if (!f || pos < 0 || pos > f->size) return -1;
  f->pos = pos;
  return pos;
}
int nfs_tell(int h) { struct nfile *f = fh(h); return f ? f->pos : -1; }
int nfs_size(int h) { struct nfile *f = fh(h); return f ? f->size : -1; }
int nfs_isdir(int h) { struct nfile *f = fh(h); return f ? f->isdir : 0; }
const char *nfs_name(int h) { struct nfile *f = fh(h); return f ? f->abs : ""; }
int nfs_close(int h) {
  struct nfile *f = fh(h); int r;
  if (!f) return -1;
  r = file_save(f);
  file_drop(f);
  f->used = 0;
  return r;
}

static int verb(const char *v, const char *abs) {
  char req[NAME + 8]; struct mount *m;
  if (mroot(abs) || !(m = req1(req, sizeof req, v, abs))) return -1;
  return simple(m, req, 0) ? -1 : 0;
}
int nfs_remove(const char *abs) { return verb("RM", abs); }
int nfs_mkdir(const char *abs) { return verb("MKDIR", abs); }
int nfs_rmdir(const char *abs) { return verb("RMDIR", abs); }
int nfs_rename(const char *a, const char *b) {
  char req[NAME + 10], to[NAME + 2]; struct op o; struct mount *m;
  if (mroot(a) || mroot(b) || !nfs_same(a, b)) return -1;     // between mounts: the caller copies
  if (!(m = req1(req, sizeof req, "RENAME", a)) || !rname(b, to)) return -1;
  strcat(to, "\n");
  memset(&o, 0, sizeof o); o.req = req; o.data = to; o.dlen = strlen(to);
  return request(m, &o, 1) ? -1 : 0;              // not repeated once sent: it may have happened
}

// ---------------------------------------------------------------- directories
int nfs_opendir(const char *abs) {
  char req[NAME + 8]; struct op o; int d; struct mount *m;
  for (d = 0; d < NDIRS && nf_dirs[d].used; d++) {}
  if (d == NDIRS || !(m = req1(req, sizeof req, "LS", abs))) return -1;
  memset(&o, 0, sizeof o); o.req = req; o.alloc = 1;
  if (request(m, &o, 0)) return -1;
  nf_dirs[d].used = 1; nf_dirs[d].mt = m; nf_dirs[d].list = nf_dirs[d].at = o.list;
  return d;
}
// LS lines are "   <dir>  name/" or "%8d  name"
int nfs_readdir(int d, char *name, int namesz, int *size, int *isdir) {
  char *s, *e, *nl; int k;
  if (d < 0 || d >= NDIRS || !nf_dirs[d].used) return 0;
  for (;;) {
    s = nf_dirs[d].at;
    if (!*s) return 0;
    nl = strchr(s, '\n');
    nf_dirs[d].at = nl ? nl + 1 : s + strlen(s);
    e = nl ? nl : s + strlen(s);
    while (s < e && *s == ' ') s++;
    *isdir = !strncmp(s, "<dir>", 5);
    *size = *isdir ? 0 : atoi(s);
    while (s < e && *s != ' ') s++;
    while (s < e && *s == ' ') s++;
    k = e - s;
    if (*isdir && k > 0 && s[k - 1] == '/') k--;
    if (k <= 0) continue;
    if (s[0] == '.' && k > 7 && !strncmp(s + k - 6, ".t2tmp", 6)) continue;   // left by a failed save
    if (k > namesz - 1) k = namesz - 1;
    memcpy(name, s, k); name[k] = 0;
    return 1;
  }
}
void nfs_closedir(int d) {
  if (d < 0 || d >= NDIRS || !nf_dirs[d].used) return;
  nt_free(nf_dirs[d].list);
  nf_dirs[d].used = 0;
}
