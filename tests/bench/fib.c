// fib.c - recursive calls (prologue, epilogue, argument passing)
#include <stdio.h>

int fib(int n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }

int main() {
  int t0; int r;
  t0 = millis();
  r = fib(29);
  printf("check %d\n", r);
  printf("time %d\n", millis() - t0);
  return 0;
}
