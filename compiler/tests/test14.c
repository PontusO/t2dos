// test14.c - floats in locals, parameters and constants: what cc -O keeps in s16-s31 and turns
// into vmov.f32 immediates. Standard C: gcc builds it natively for reference output
// (gcc -fsingle-precision-constant, as cc's float constants are single precision).
#include <stdio.h>

float g;
float garr[4];
struct P { float x; float y; };

int ip(float f) { return (int)(f * 1000.0); }

// a float that lives across calls, in a function whose callee uses s16-s31 as well
float sq(float a) { float r; r = a * a; return r; }
float hyp2(float a, float b) { float s; s = sq(a); s = s + sq(b); return s; }

// more float locals than s16-s31 hold
float many(float a) {
  float f0; float f1; float f2; float f3; float f4; float f5; float f6; float f7; float f8; float f9;
  float f10; float f11; float f12; float f13; float f14; float f15; float f16; float f17; float f18; float f19;
  int i;
  f0 = a; f1 = a + 1.0; f2 = a + 2.0; f3 = a + 3.0; f4 = a + 4.0; f5 = a + 5.0; f6 = a + 6.0; f7 = a + 7.0;
  f8 = a + 8.0; f9 = a + 9.0; f10 = a + 10.0; f11 = a + 11.0; f12 = a + 12.0; f13 = a + 13.0; f14 = a + 14.0;
  f15 = a + 15.0; f16 = a + 16.0; f17 = a + 17.0; f18 = a + 18.0; f19 = a + 19.0;
  for (i = 0; i < 3; i++) {
    f0 = f0 + f19 * 0.5; f1 = f1 - f18 * 0.25; f2 = f2 * 1.5; f3 = f3 / 2.0; f4 = f4 + f0;
    f5 = f5 + f1; f6 = f6 - f2; f7 = f7 + f3 * f4; f8 = -f8; f9 = f9 + f8; f10 = f10 * 0.125;
    f11 = f11 + f10; f12 = f12 - 1.0; f13 = f13 + 31.0; f14 = f14 * -1.5; f15 = f15 + f14;
    f16 = f16 + f15; f17 = f17 - f16; f18 = f18 + f17; f19 = f19 * 0.9375;
  }
  return f0 + f1 + f2 + f3 + f4 + f5 + f6 + f7 + f8 + f9 + f10 + f11 + f12 + f13 + f14 + f15 + f16 + f17 + f18 + f19;
}

// parameters: floats between ints, used in a loop
float mix(int n, float a, int k, float b) {
  float s; int i;
  s = 0.0;
  for (i = 0; i < n; i++) { s = s + a * i - b; if (s > 100.0) s = s - k; }
  return s;
}

// recursion keeps the caller's floats
float rec(float x, int d) {
  float y;
  y = x * 0.5;
  if (d > 0) y = y + rec(x + 1.0, d - 1);
  return y + x;
}

// address taken: stays in memory
void addto(float *p, float v) { *p = *p + v; }
float addr(void) { float a; a = 1.5; addto(&a, 2.25); addto(&a, a); return a; }

// conversions, compares, ++, compound assignments, ternaries, && and ||
int misc(int n) {
  float f; float h; unsigned u; int i; int c;
  f = n; h = 0.0; u = 7; c = 0;
  for (i = 0; i < n; i++) {
    f++; h += f; h -= 0.5; h *= 1.0625; h /= 1.03125;
    if (f > 3.0 && h < 1000.0) c = c + 1;
    if (f < 2.0 || h > 50.0) c = c + 10;
    h = h > 200.0 ? h - 150.0 : h;
    u = u + (unsigned)f;
  }
  f = u;
  return c * 1000 + ip(f) + ip(h) + (int)(f > h) + (int)-f;
}

// constants: every value vmov.f32 has as an immediate, and some it has not
float consts(void) {
  float s;
  s = 0.0;
  s = s + 2.0 + 2.125 + 2.25 + 3.875 + 4.0 + 4.25 + 7.75 + 8.0 + 15.5 + 16.0 + 31.0;
  s = s + 0.125 + 0.1328125 + 0.2421875 + 0.25 + 0.375 + 0.5 + 0.96875 + 1.0 + 1.0625 + 1.9375;
  s = s - 2.0 - 0.125 - 31.0 - 1.5 - 0.1328125;
  s = s + 0.1 + 32.0 + 0.0 + 100.0 - 0.0625 + 1.03125;
  return s;
}

int main() {
  float a; float b; struct P p; int i;
  a = 3.0; b = 4.0;
  printf("hyp2 %d\n", ip(hyp2(a, b)));
  printf("many %d\n", ip(many(1.25)));
  printf("mix %d\n", ip(mix(20, 1.5, 3, 0.75)));
  printf("rec %d\n", ip(rec(2.0, 5)));
  printf("addr %d\n", ip(addr()));
  printf("misc %d\n", misc(12));
  printf("consts %d\n", ip(consts()));
  g = a * b; garr[2] = g + 0.5; p.x = garr[2] * 2.0; p.y = p.x - a;
  for (i = 0; i < 4; i++) garr[i] = garr[i] + p.y * i;
  printf("mem %d %d %d %d\n", ip(g), ip(p.x), ip(p.y), ip(garr[3]));
  return 0;
}
