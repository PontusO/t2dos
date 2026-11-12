// spin.c - preemption test program: spin n [expected]
// Runs n rounds (0 = forever) of integer and float arithmetic with no syscalls inside the
// loop, so only preemption lets other tasks run meanwhile. Prints "spin RESULT"; with
// expected given it returns 1 if RESULT equals it, else 2 (without it, 0).
#include "sys.h"

int spin(int n) {
  int i; int a; int b; int c; float x; float y; float z;
  a = 1; b = 7; c = 0; x = 1.0; y = 0.5; z = 0.0;
  i = 0;
  while (n == 0 || i < n) {
    a = a * 1103515245 + 12345;
    b = b ^ (a >> 7);
    c = b & 255;
    z = c;
    x = x * 1.0001 + y;
    if (x > 1000.0) x = x - 999.0;
    y = y * 0.999 + z * 0.001;
    i = i + 1;
  }
  return a ^ b ^ (int)(x * 1000.0) ^ (int)(y * 1000.0);
}

int main(int argc, char **argv) {
  int r;
  if (argc < 2) { printf("usage: spin n [expected]\n"); return 3; }
  r = spin(atoi(argv[1]));
  printf("spin %d\n", r);
  if (argc > 2) { if (r == atoi(argv[2])) return 1; return 2; }
  return 0;
}
