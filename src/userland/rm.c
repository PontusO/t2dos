// rm [-rf] file... : delete files; -r removes trees, -f tolerates missing paths.
#include "sys.h"

// dst = dir/name, or -1 if it does not fit (rcp.c's join)
int join(char *dst, char *dir, char *name) {
  int k;
  k = strlen(dir);
  if (k + strlen(name) + 2 > 200) { printf("rm: path too long: %s/%s\n", dir, name); return -1; }
  strcpy(dst, dir);
  if (k == 0 || dir[k - 1] != '/') { dst[k] = '/'; k++; }
  strcpy(dst + k, name);
  return 0;
}

// one path: 0 gone or tolerated, 1 failed.
// -r on a directory rescan-loops: open, take the FIRST entry, close, recurse.
// The directory is never iterated while it mutates and there is no entry-count
// cap; any child error aborts, so an entry that will not die cannot spin the loop.
int rm1(char *path, int force, int rec) {
  char child[200]; struct stat_t st; struct dirent_t e; int d;
  if (stat(path, &st) < 0) {
    if (force) return 0;
    printf("rm: %s failed\n", path); return 1;
  }
  if (!st.isdir) {
    if (unlink(path) < 0) { printf("rm: %s failed\n", path); return 1; }
    return 0;
  }
  if (!rec) { printf("rm: %s is a directory\n", path); return 1; }
  for (;;) {
    d = opendir(path);
    if (d < 0) { printf("rm: %s failed\n", path); return 1; }
    if (!readdir(d, &e)) { closedir(d); break; }
    closedir(d);
    if (join(child, path, e.name) < 0 || rm1(child, force, rec)) return 1;
  }
  if (rmdir(path) < 0) { printf("rm: %s failed\n", path); return 1; }
  return 0;
}

int main(int argc, char **argv) {
  int i; int j; int r; int force; int rec; char *p;
  r = 0; force = 0; rec = 0;
  for (i = 1; i < argc; i++) {
    p = argv[i];
    if (p[0] == '-' && p[1]) {
      for (j = 1; p[j]; j++) {
        if (p[j] == 'r') rec = 1;
        else if (p[j] == 'f') force = 1;
        else { printf("usage: rm [-rf] file...\n"); return 1; }
      }
    } else if (rm1(p, force, rec)) r = 1;
  }
  return r;
}
