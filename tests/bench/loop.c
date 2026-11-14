// loop.c - nested counting loops on int locals (register use, compare and branch)
#include <stdio.h>

int main() {
  int i; int j; int k; unsigned s; int t0;
  t0 = millis(); s = 0;
  for (k = 0; k < 20; k++)
    for (i = 0; i < 1000; i++)
      for (j = 0; j < 100; j++) s = s + (i ^ j) + (s >> 7);
  printf("check %u\n", s);
  printf("time %d\n", millis() - t0);
  return 0;
}
