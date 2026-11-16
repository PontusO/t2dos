// ramfs.cpp - see ramfs.h.  Every file and directory is a node in one list, named by its full
// path ("/tmp/cc12_1_a.s"); "/tmp" is the root node and is never removed.  File data lives in
// 4 KB blocks, so a growing file never copies what it already holds.  A file removed while
// still open stays in the list, marked gone (invisible to lookups), until it is closed.  Only the kernel calls this, never preempted, so no locking.
#include <Arduino.h>
#include <FS.h>
#include <FSImpl.h>
#include <memory>
#include <string.h>
#include "ramfs.h"

using namespace fs;

#if defined(PICO_RP2350)

#define RBLK 4096
#define RPATH 160
#define ROOT "/tmp"

struct RNode {
  RNode *next;
  char path[RPATH];
  bool dir, gone;
  int refs;
  uint32_t size;
  uint8_t **blk; uint32_t nblk, cap;
};

static RNode *nodes;                  // nodes[0] is the root
static size_t margin;
static uint32_t used;                 // bytes in data blocks

static bool room(size_t n) { return rp2040.getFreePSRAMHeap() >= margin + n; }

static RNode *find(const char *path) {
  for (RNode *n = nodes; n; n = n->next) if (!n->gone && !strcmp(n->path, path)) return n;
  return nullptr;
}
static const char *base(const char *path) { const char *s = strrchr(path, '/'); return s ? s + 1 : path; }
// is p directly inside dir?
static bool child_of(const char *p, const char *dir) {
  size_t l = strlen(dir);
  return !strncmp(p, dir, l) && p[l] == '/' && p[l + 1] && !strchr(p + l + 1, '/');
}
static bool parent_dir(const char *path) {
  char d[RPATH]; strlcpy(d, path, sizeof d);
  char *s = strrchr(d, '/'); if (!s || s == d) return false;
  *s = 0; RNode *n = find(d); return n && n->dir;
}
static bool has_children(const char *dir) {
  for (RNode *n = nodes; n; n = n->next) if (!n->gone && child_of(n->path, dir)) return true;
  return false;
}
static bool inside(const char *path) { return !strcmp(path, ROOT) || !strncmp(path, ROOT "/", 5); }

static void free_from(RNode *n, uint32_t keep) {   // drop the blocks past the first keep
  while (n->nblk > keep) { free(n->blk[--n->nblk]); used -= RBLK; }
  if (!n->nblk && n->blk) { free(n->blk); n->blk = nullptr; n->cap = 0; }
}
// blocks for size bytes; false (with as many as there was room for) when PSRAM runs short
static bool grow(RNode *n, uint32_t size) {
  uint32_t want = (size + RBLK - 1) / RBLK;
  while (n->nblk < want) {
    if (n->nblk == n->cap) {
      uint32_t c = n->cap ? n->cap * 2 : 8;
      if (!room(c * sizeof(uint8_t *))) return false;
      uint8_t **b = (uint8_t **)pmalloc(c * sizeof(uint8_t *)); if (!b) return false;
      if (n->nblk) memcpy(b, n->blk, n->nblk * sizeof(uint8_t *));
      free(n->blk); n->blk = b; n->cap = c;
    }
    if (!room(RBLK)) return false;
    uint8_t *p = (uint8_t *)pmalloc(RBLK); if (!p) return false;
    memset(p, 0, RBLK);
    n->blk[n->nblk++] = p; used += RBLK;
  }
  return true;
}
static void set_size(RNode *n, uint32_t size) {    // shrinks, or grows with zeros
  if (size < n->size) {
    free_from(n, (size + RBLK - 1) / RBLK);
    if (size % RBLK && size / RBLK < n->nblk) memset(n->blk[size / RBLK] + size % RBLK, 0, RBLK - size % RBLK);
  }
  n->size = size;
}
static void unlist(RNode *n) {
  for (RNode **pp = &nodes; *pp; pp = &(*pp)->next) if (*pp == n) { *pp = n->next; break; }
  free_from(n, 0); free(n);
}
static void release(RNode *n) { if (--n->refs == 0 && n->gone) unlist(n); }
static void drop(RNode *n) { if (n->refs) n->gone = true; else unlist(n); }
static RNode *make(const char *path, bool dir) {
  if (strlen(path) >= RPATH || !parent_dir(path) || !room(sizeof(RNode))) return nullptr;
  RNode *n = (RNode *)pmalloc(sizeof(RNode)); if (!n) return nullptr;
  memset(n, 0, sizeof *n); strlcpy(n->path, path, RPATH); n->dir = dir;
  n->next = nodes->next; nodes->next = n;            // after the root
  return n;
}

class RFile : public FileImpl {
  RNode *n; uint32_t pos; bool rd, wr, app, open_;
public:
  RFile(RNode *node, AccessMode am, bool append) : n(node), pos(0), rd(am & AM_READ), wr(am & AM_WRITE), app(append), open_(true) { n->refs++; }
  ~RFile() override { close(); }
  size_t write(const uint8_t *buf, size_t size) override {
    if (!open_ || !wr || n->dir) return 0;
    if (app) pos = n->size;
    if (!grow(n, pos + size)) { uint32_t cap = n->nblk * RBLK; size = cap > pos ? cap - pos : 0; }
    for (size_t done = 0; done < size; ) {
      uint32_t o = pos % RBLK, k = RBLK - o; if (k > size - done) k = size - done;
      memcpy(n->blk[pos / RBLK] + o, buf + done, k); pos += k; done += k;
    }
    if (pos > n->size) n->size = pos;
    return size;
  }
  int read(uint8_t *buf, size_t size) override {
    if (!open_ || !rd || n->dir) return -1;
    if (pos >= n->size) return 0;
    if (size > n->size - pos) size = n->size - pos;
    for (size_t done = 0; done < size; ) {
      uint32_t o = pos % RBLK, k = RBLK - o; if (k > size - done) k = size - done;
      memcpy(buf + done, n->blk[pos / RBLK] + o, k); pos += k; done += k;
    }
    return size;
  }
  void flush() override {}
  bool seek(uint32_t p, SeekMode mode) override {
    if (!open_) return false;
    int64_t t = mode == SeekSet ? (int64_t)p : mode == SeekCur ? (int64_t)pos + (int32_t)p : (int64_t)n->size + (int32_t)p;
    if (t < 0 || t > n->size) return false;
    pos = t; return true;
  }
  size_t position() const override { return pos; }
  size_t size() const override { return n->size; }
  bool truncate(uint32_t size) override {
    if (!open_ || !wr || n->dir) return false;
    if (size > n->size && !grow(n, size)) return false;
    set_size(n, size); if (pos > size) pos = size; return true;
  }
  void close() override { if (open_) { open_ = false; release(n); } }
  const char *name() const override { return base(n->path); }
  const char *fullName() const override { return n->path; }
  bool isFile() const override { return !n->dir; }
  bool isDirectory() const override { return n->dir; }
};

// Entries come in name order, like LittleFS: each next() takes the smallest name after the last
// one, so removing entries while listing (rm *) is safe and nothing is held between calls.
class RDir : public DirImpl {
  char path[RPATH], last[RPATH];      // last: the full path of the current entry, "" before the first
  RNode *at;                          // the current entry, valid until the next call into ramfs
public:
  RDir(const char *p) : at(nullptr) { strlcpy(path, p, sizeof path); last[0] = 0; }
  FileImplPtr openFile(OpenMode om, AccessMode am) override {
    RNode *n = last[0] ? find(last) : nullptr;
    if (!n || ((am & AM_WRITE) && n->dir)) return FileImplPtr();
    if ((om & OM_TRUNCATE) && !n->dir) set_size(n, 0);
    return std::make_shared<RFile>(n, am, om & OM_APPEND);
  }
  const char *fileName() override { return at ? base(last) : ""; }
  size_t fileSize() override { return at && !at->dir ? at->size : 0; }
  bool isFile() const override { return at && !at->dir; }
  bool isDirectory() const override { return at && at->dir; }
  bool next() override {
    RNode *best = nullptr;
    for (RNode *n = nodes; n; n = n->next)
      if (!n->gone && child_of(n->path, path) && (!last[0] || strcmp(n->path, last) > 0) && (!best || strcmp(n->path, best->path) < 0)) best = n;
    at = best;
    if (best) strlcpy(last, best->path, sizeof last);
    return best != nullptr;
  }
  bool rewind() override { at = nullptr; last[0] = 0; return true; }
};

class RamFS : public FSImpl {
public:
  bool setConfig(const FSConfig &) override { return true; }
  bool begin() override { return true; }
  void end() override {}
  bool format() override {
    for (RNode *n = nodes->next, *nx; n; n = nx) { nx = n->next; if (!n->gone) drop(n); }
    return true;
  }
  bool info(FSInfo &fi) override {
    size_t f = rp2040.getFreePSRAMHeap();
    fi.usedBytes = used; fi.totalBytes = used + (f > margin ? f - margin : 0);
    fi.blockSize = RBLK; fi.pageSize = RBLK; fi.maxOpenFiles = 255; fi.maxPathLength = RPATH - 1;
    return true;
  }
  FileImplPtr open(const char *path, OpenMode om, AccessMode am) override {
    if (!inside(path)) return FileImplPtr();
    RNode *n = find(path);
    if (!n) { if (!(om & OM_CREATE) || !(n = make(path, false))) return FileImplPtr(); }
    else if (n->dir && (am & AM_WRITE)) return FileImplPtr();
    else if ((om & OM_TRUNCATE) && !n->dir) set_size(n, 0);
    return std::make_shared<RFile>(n, am, om & OM_APPEND);
  }
  bool exists(const char *path) override { return find(path) != nullptr; }
  DirImplPtr openDir(const char *path) override {
    RNode *n = find(path);
    if (!n || !n->dir) return DirImplPtr();
    return std::make_shared<RDir>(path);
  }
  bool rename(const char *from, const char *to) override {
    RNode *a = find(from); if (!a || a == nodes || !inside(to) || strlen(to) >= RPATH) return false;
    if (!strcmp(from, to)) return true;
    size_t lf = strlen(from);
    if (a->dir && !strncmp(to, from, lf) && to[lf] == '/') return false;     // into itself
    if (!parent_dir(to)) return false;
    if (RNode *b = find(to)) { if (b->dir || a->dir) return false; drop(b); }
    if (a->dir) {                     // the paths of everything below it change too
      size_t lt = strlen(to);
      for (RNode *n = nodes; n; n = n->next)
        if (!n->gone && !strncmp(n->path, from, lf) && n->path[lf] == '/' && strlen(n->path) - lf + lt >= RPATH) return false;
      for (RNode *n = nodes; n; n = n->next)
        if (!n->gone && !strncmp(n->path, from, lf) && n->path[lf] == '/') {
          char t[RPATH]; strlcpy(t, to, sizeof t); strlcat(t, n->path + lf, sizeof t); strlcpy(n->path, t, RPATH);
        }
    }
    strlcpy(a->path, to, RPATH);
    return true;
  }
  bool remove(const char *path) override {
    RNode *n = find(path); if (!n || n == nodes || (n->dir && has_children(path))) return false;
    drop(n); return true;
  }
  bool mkdir(const char *path) override { return inside(path) && !find(path) && make(path, true); }
  bool rmdir(const char *path) override {
    RNode *n = find(path); if (!n || !n->dir) return false;
    return remove(path);
  }
  bool stat(const char *path, FSStat *st) override {
    RNode *n = find(path); if (!n) return false;
    st->size = n->dir ? 0 : n->size; st->blocksize = RBLK; st->ctime = st->atime = 0; st->isDir = n->dir;
    return true;
  }
};

static FS *ramfs;

FS *ramfs_begin(size_t m) {
  if (ramfs) return ramfs;
  if (rp2040.getPSRAMSize() == 0) return nullptr;
  margin = m;
  nodes = (RNode *)pmalloc(sizeof(RNode)); if (!nodes) return nullptr;
  memset(nodes, 0, sizeof *nodes); strlcpy(nodes->path, ROOT, RPATH); nodes->dir = true; nodes->refs = 1;
  ramfs = new FS(std::make_shared<RamFS>());
  return ramfs;
}

#else
FS *ramfs_begin(size_t) { return nullptr; }
#endif
