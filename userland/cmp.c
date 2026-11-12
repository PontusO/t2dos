// cmp [-s] a b : compares two files byte by byte.  Prints where they first differ
// ("a b differ: byte N, line L", or "cmp: EOF on a after byte N, in line L" when a is a
// shorter copy of b); -s prints nothing.  Exit 0 same, 1 different, 2 error.
#include "sys.h"
char ba[2048];
char bb[2048];

int open_file(char *name, int quiet) {
  struct stat_t st; int fd;
  if (stat(name, &st) != 0) { if (!quiet) printf("cmp: %s: not found\n", name); return -1; }
  if (st.isdir) { if (!quiet) printf("cmp: %s: is a directory\n", name); return -1; }
  fd = open(name, OPEN_READ);
  if (fd < 0 && !quiet) printf("cmp: %s: cannot open\n", name);
  return fd;
}

int main(int argc, char **argv) {
  int quiet; int fa; int fb; int na; int nb; int ia; int ib; int pos; int line; int r;
  char *a; char *b;
  quiet = 0;
  if (argc > 1 && !strcmp(argv[1], "-s")) { quiet = 1; argc--; argv++; }
  if (argc != 3) { printf("usage: cmp [-s] a b\n"); return 2; }
  a = argv[1]; b = argv[2];
  fa = open_file(a, quiet); if (fa < 0) return 2;
  fb = open_file(b, quiet); if (fb < 0) { close(fa); return 2; }
  na = 0; nb = 0; ia = 0; ib = 0; pos = 0; line = 1; r = 0;
  while (1) {
    if (ia == na) { na = read(fa, ba, 2048); ia = 0; if (na < 0) { if (!quiet) printf("cmp: %s: read error\n", a); r = 2; break; } }
    if (ib == nb) { nb = read(fb, bb, 2048); ib = 0; if (nb < 0) { if (!quiet) printf("cmp: %s: read error\n", b); r = 2; break; } }
    if (na == 0 && nb == 0) break;                        // same length, same bytes
    if (na == 0 || nb == 0) {
      if (quiet) {}
      else if (pos == 0) printf("cmp: EOF on %s which is empty\n", na == 0 ? a : b);
      else printf("cmp: EOF on %s after byte %d, in line %d\n", na == 0 ? a : b, pos, line);
      r = 1; break;
    }
    while (ia < na && ib < nb) {
      if (ba[ia] != bb[ib]) {
        if (!quiet) printf("%s %s differ: byte %d, line %d\n", a, b, pos + 1, line);
        r = 1; break;
      }
      if (ba[ia] == '\n') line++;
      ia++; ib++; pos++;
    }
    if (r) break;
  }
  close(fa); close(fb);
  return r;
}
