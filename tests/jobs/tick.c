// tick.c - background job test program: tick [n] [ms]
// Every ms milliseconds (default 200) prints "tick K", n times (default 5; 0 = forever),
// then returns 7. It waits with sleep(), a switch point, so other tasks run meanwhile.
#include "sys.h"

int main(int argc, char **argv) {
  int n; int ms; int k;
  n = 5; ms = 200;
  if (argc > 1) n = atoi(argv[1]);
  if (argc > 2) ms = atoi(argv[2]);
  k = 1;
  while (n == 0 || k <= n) {
    sleep(ms);
    printf("tick %d\n", k);
    k = k + 1;
  }
  return 7;
}
