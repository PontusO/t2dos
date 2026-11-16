// netfs.cpp - /mnt/fs as an arduino-pico fs::FSImpl over the core in netfs.c (which holds the
// protocol, the cache and the save rules).  A file's changes are saved when it is closed;
// FileImpl::close() cannot return a result, so it leaves it in netfs_close_result for
// SYS_close and k_rename.
#include <Arduino.h>
#include <FS.h>
#include <FSImpl.h>
#include <memory>
#include <string.h>
#include "netfs.h"
#include "netfs_fs.h"

using namespace fs;

int netfs_close_result;

class NFile : public FileImpl {
  int h; bool open_, dir; char path[160];
public:
  NFile(int handle) : h(handle), open_(true), dir(nfs_isdir(handle)) { strlcpy(path, nfs_name(handle), sizeof path); }
  ~NFile() override { close(); }
  size_t write(const uint8_t *buf, size_t size) override { if (!open_) return 0; int r = nfs_write(h, buf, size); return r < 0 ? 0 : r; }
  int read(uint8_t *buf, size_t size) override { return open_ ? nfs_read(h, buf, size) : -1; }
  void flush() override {}             // nothing calls it; changes are saved at close
  bool seek(uint32_t pos, SeekMode mode) override {
    if (!open_) return false;
    int p = mode == SeekSet ? (int)pos : mode == SeekCur ? nfs_tell(h) + (int32_t)pos : nfs_size(h) + (int32_t)pos;
    return nfs_seek(h, p) >= 0;
  }
  size_t position() const override { return open_ ? nfs_tell(h) : 0; }
  size_t size() const override { return open_ ? nfs_size(h) : 0; }
  bool truncate(uint32_t) override { return false; }
  void close() override { if (open_) { open_ = false; netfs_close_result = nfs_close(h); } }
  const char *name() const override { const char *s = strrchr(path, '/'); return s ? s + 1 : path; }
  const char *fullName() const override { return path; }
  bool isFile() const override { return !dir; }
  bool isDirectory() const override { return dir; }
};

class NDir : public DirImpl {
  int d; bool at; char nm[64]; int sz, isd;
public:
  NDir(int dd) : d(dd), at(false), sz(0), isd(0) { nm[0] = 0; }
  ~NDir() override { nfs_closedir(d); }
  FileImplPtr openFile(OpenMode, AccessMode) override { return FileImplPtr(); }
  const char *fileName() override { return at ? nm : ""; }
  size_t fileSize() override { return at ? sz : 0; }
  bool isFile() const override { return at && !isd; }
  bool isDirectory() const override { return at && isd; }
  bool next() override { at = nfs_readdir(d, nm, sizeof nm, &sz, &isd) == 1; return at; }
  bool rewind() override { return false; }
};

class NetFS : public FSImpl {
public:
  bool setConfig(const FSConfig &) override { return true; }
  bool begin() override { return true; }
  void end() override {}
  bool format() override { return false; }
  bool info(FSInfo &) override { return false; }
  FileImplPtr open(const char *path, OpenMode om, AccessMode am) override {
    int h = nfs_open(path, !(am & AM_WRITE) ? NFS_READ : (om & OM_APPEND) ? NFS_APPEND : NFS_WRITE);
    if (h < 0) return FileImplPtr();
    return std::make_shared<NFile>(h);
  }
  bool exists(const char *path) override { int s, d; return nfs_stat(path, &s, &d) == 0; }
  DirImplPtr openDir(const char *path) override {
    int d = nfs_opendir(path);
    if (d < 0) return DirImplPtr();
    return std::make_shared<NDir>(d);
  }
  bool rename(const char *a, const char *b) override { return nfs_rename(a, b) == 0; }
  bool remove(const char *path) override { return nfs_remove(path) == 0; }
  bool mkdir(const char *path) override { return nfs_mkdir(path) == 0; }
  bool rmdir(const char *path) override { return nfs_rmdir(path) == 0; }
  bool stat(const char *path, FSStat *st) override {
    int s, d; if (nfs_stat(path, &s, &d)) return false;
    st->size = s; st->blocksize = 4096; st->ctime = st->atime = 0; st->isDir = d;
    return true;
  }
};

FS *netfs_begin(void) {
  static FS *fs;
  if (!fs) fs = new FS(std::make_shared<NetFS>());
  return fs;
}
