// fexec.c - check_jobs builds it with cc -O, which keeps float locals in s16-s31: a parent's
// floats must survive an exec'd child that ends by exit() (a longjmp past the child's own
// vpop), so the kernel restores s16-s31 itself. "fexec" execs "fexec child", whose deep()
// puts other values in s16-s18 and exits with 5; the parent then checks its floats.
#include "sys.h"

char *args[3];

int ip(float f) { return (int)(f * 1000.0); }

int deep(float x) {
  float y; float z; float w;
  y = x * 3.0; z = y + 7.0; w = z * y;
  if (w > 0.0) exit(5);
  return ip(y + z + w);
}

int main(int argc, char **argv) {
  float a; float b; float c; int r; int k;
  if (argc > 1) return deep(2.5);
  a = 1.5; b = -2.25; c = a * b;
  args[0] = "fexec"; args[1] = "child"; args[2] = 0;
  r = exec("/src/jobs/fexec.bin", 2, args);
  k = ip(a) == 1500 && ip(b) == -2250 && ip(c) == -3375;
  printf("fexec: child exit %d, floats %d %d %d (want 1500 -2250 -3375)\n", r, ip(a), ip(b), ip(c));
  if (r != 5 || !k) { printf("FAIL\n"); return 1; }
  printf("PASS\n");
  return 0;
}
