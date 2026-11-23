// mtime.c - dirent_t.mtime: readdir reports the file's last write time in seconds UTC.
// QEMU_RUN: under host_linux /tmp is a real directory in the harness root, so the stamp
// is the real st_mtime of the file just written; on the board /tmp is ramfs (no times,
// mtime 0), so board mode skips this case.
#include "sys.h"
int main() {
  int t0 = time(0);
  mkdir("tmp/mtf");   // an error means it is there already
  int f = open("tmp/mtf/x", OPEN_WRITE);
  if (f < 0) { printf("open %d\n", f); return 1; }
  write(f, "hi\n", 3);
  close(f);
  int d = opendir("tmp/mtf");
  if (d < 0) { printf("opendir %d\n", d); return 1; }
  struct dirent_t e;
  while (readdir(d, &e)) {
    if (strcmp(e.name, "x") == 0) {
      closedir(d);
      if (e.mtime < t0 || e.mtime > time(0) + 2) { printf("mtime %d t0 %d\n", e.mtime, t0); return 1; }
      printf("ok\n");
      return 0;
    }
  }
  closedir(d);
  printf("x not listed\n");
  return 1;
}
