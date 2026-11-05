#include "sys.h"
char buf[4096];
char dst[160];
int main(int argc, char **argv) {
  int fi; int fo; int n; struct stat_t st; char *b;
  if (argc < 3) { printf("usage: cp src dst\n"); return 1; }
  strcpy(dst, argv[2]);
  if (stat(dst, &st) == 0 && st.isdir) {      // copy into directory
    b = argv[1]; while (strchr(b, '/')) b = strchr(b, '/') + 1;
    sprintf(dst, "%s/%s", argv[2], b);
  }
  fi = open(argv[1], OPEN_READ); if (fi < 0) { printf("cp: %s: not found\n", argv[1]); return 1; }
  fo = open(dst, OPEN_WRITE); if (fo < 0) { printf("cp: cannot write %s\n", dst); return 1; }
  while ((n = read(fi, buf, 4096)) > 0) write(fo, buf, n);
  close(fi); close(fo);
  return 0;
}
