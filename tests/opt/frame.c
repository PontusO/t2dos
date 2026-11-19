// check_frame.py: function entry and exit shapes with -O (and that the code still works)
#include "sys.h"
int g;
int leaf() { g = g + 1; return g; }                  // no parameters, no locals: no frame pointer
int framed(int n) { return n + 1; }                  // reads its parameter with [r7, #off]: the
                                                     // frame stays even when a frameless leaf
                                                     // sits next to it on the same stack (the
                                                     // crash witness of the frameless ruling:
                                                     // drop r7 here and the read sees leaf's lr)
int small(int n) { char buf[16]; int i; i = 0; while (i < 16) { buf[i] = n + i; i++; } return buf[15]; }   // sub sp, #N
int big() { char b[1000]; b[999] = 7; return b[999]; }   // over 508 bytes: the long form stays
int early(int x) { if (x < 2) return x; return early(x - 1) + 1; }   // the early return pops on the spot
int noret() { g = g + 2; }                           // falls off the end
float fpm(float a, int n, float b) {                 // floats in s registers: promoted param and locals
  float x; float y; int i;
  x = a * 2.0; y = b + 1.5;
  for (i = 0; i < n; i++) { x = x + y; }
  return x - y;                                      // an early-free path: the exit vpop must stay
}
float fpint(int n) {                                 // float locals in arrays stay in memory
  float t[4]; float s; int i;
  s = 0.0; for (i = 0; i < 4; i++) t[i] = n + i;
  for (i = 0; i < 4; i++) s = s + t[i];
  return s;
}
float fmany(float a) {                               // more float locals than s16-s31: the rest stay in memory
  float f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18;
  f1 = a + 1; f2 = a + 2; f3 = a + 3; f4 = a + 4; f5 = a + 5; f6 = a + 6; f7 = a + 7; f8 = a + 8; f9 = a + 9;
  f10 = a + 10; f11 = a + 11; f12 = a + 12; f13 = a + 13; f14 = a + 14; f15 = a + 15; f16 = a + 16; f17 = a + 17; f18 = a + 18;
  return f1 + f9 + f18;
}
int plarge(int a, int b, int c, int d, int e, int f, int g) {   // seven parameters: the best ones promote
  int s;
  s = a + b + c + d + e + f + g;
  if (s > 3) s = s - 1;
  return s * 2;
}
int bigprom(int n) {                                 // a long frame with promoted locals and an early return
  char b[600]; int i; int j;
  b[599] = n; i = 3;
  if (n > 0) return b[599] + i;                      // pops on the spot, past the frame it never frees
  j = 1; return j;
}
int amem(int n) {                                    // a local array: its pointer word must never promote
  int a[8]; int i; int s;
  a[0] = n; for (i = 1; i < 8; i++) a[i] = a[i - 1] + 1;
  s = 0; for (i = 0; i < 8; i++) s = s + a[i];
  return s;
}
int fearly(float x) {                                // float locals and an early return: the vpop stays on the exit path
  float y;
  y = x * 2.0;
  if (x > 0.5) return 7;
  return (int)y;
}
int main() {
  int a; int b; int c; int d; float e; float fx; int p; int q; int r; int t; int f;
  a = leaf(); b = small(3); c = big(); d = early(5); noret(); f = framed(17);
  e = fpm(1.5, 3, 2.5);                               // (1.5*2 + 3*(4+1.5)) - 4.0 = 12.0
  fx = fpint(2);                                     // 2+3+4+5 = 14.0
  p = fpm(1.5, 3, 2.5) > e; q = plarge(1, 2, 3, 4, 5, 6, 7); r = amem(1);   // 0, 26, 60
  t = fearly(1.25) + 2 * fearly(0.25);               // 7 + 2*2 = 11
  printf("%d %d %d %d %d %d %d %d %d %d %d\n", a, b, c, d, g, (int)e, (int)fx, p, q + r + bigprom(1), (int)fmany(0.5), f);   // 1 18 7 5 3 11 14 0 94 29 18
  return 0;
}
