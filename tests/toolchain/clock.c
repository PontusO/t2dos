// clock.c - the time(int*) builtin (SYS_time): the running second and, through the
// pointer, the valid flag. QEMU_RUN: host_linux returns the real clock; the board's
// is only synced opportunistically, so board mode skips this case.
#include "sys.h"
int main() {
  int v = 0;
  int t = time(&v);
  if (t > 1700000000 && v == 1) { printf("ok\n"); return 0; }
  printf("clock %d valid %d\n", t, v);
  return 1;
}
