#include "sys.h"
char b[16];
int main(int argc, char **argv) {
  int fd; int n; int off; int i; int c;
  if (argc < 2) { printf("usage: hexdump file\n"); return 1; }
  fd = open(argv[1], OPEN_READ);
  if (fd < 0) { printf("hexdump: %s: not found\n", argv[1]); return 1; }
  off = 0;
  while ((n = read(fd, b, 16)) > 0) {
    printf("%06x ", off);
    for (i = 0; i < 16; i++) { if (i < n) printf(" %02x", b[i] & 255); else printf("   "); }
    printf("  ");
    for (i = 0; i < n; i++) { c = b[i] & 255; putchar(c >= 32 && c < 127 ? c : '.'); }
    printf("\n"); off = off + n;
  }
  close(fd);
  return 0;
}
