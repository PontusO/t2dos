// test11.c - float. Standard C: gcc builds it natively for reference output.
// Literals carry an f suffix: unsuffixed ones are double in C but float in T2CC.
#include <stdio.h>

struct Particle { float x; float v; int id; };
float gf = 2.5f;
float gneg = -0.125f;
float gint = 7;                          // int constant, converted
int gtrunc = 2.75f;                      // float constant, truncated
float garr[4] = { 1.0f, 0.5f, 0.25f };
struct Particle gp = { 1.5f, -2.0f, 3 };
float gexpr = 1.0f / 3.0f;               // computed before main

float half(float x) { return x / 2; }
float avg(float a, float b) { return (a + b) / 2; }
int to_int(float f) { return f; }
float from_int(int i) { return i; }
float newton_sqrt(float a) { float x = a; int i; for (i = 0; i < 20; i++) x = (x + a / x) / 2; return x; }
float poly(float x) { return 3.0f * x * x - 2.0f * x + 1.0f; }

int main() {
  float a = 1.5f, b = -4.25f, c, zero = 0.0f, nan, inf; int i; unsigned u; float (*fp)(float); struct Particle p;
  float sum; float arr[5];

  printf("basic %f %f %f %f %f\n", a + b, a - b, a * b, b / a, -a);
  printf("mixed %f %f %f %f\n", a * 2, 10 / a, a + 1, 7 - b);
  c = a; c += 2; c *= b; c -= 1; c /= 2; printf("compound %f\n", c);
  c = 1.5f; c++; printf("inc %f ", c); c--; --c; printf("%f ", c); printf("%f ", c++); printf("%f ", c); printf("%f\n", ++c + 1);
  printf("casts %d %d %d %d %f %f\n", (int)3.99f, (int)-3.99f, (int)a, to_int(-0.5f), (float)7, from_int(-3));
  i = 16777217; printf("rounding %f %f\n", (float)i, (float)(i + 2));
  u = 4000000000u; printf("unsigned %f %u %u\n", (float)u, (unsigned)3000000000.0f, (unsigned)(float)u);
  printf("compare %d %d %d %d %d %d\n", a < b, a > b, a <= 1.5f, a >= 2, a == 1.5f, a != 1.5f);
  nan = zero / zero; inf = 1.0f / zero;
  printf("nan %d %d %d %d %d %d inf %f %f %d\n", nan == nan, nan != nan, nan < 1, nan > 1, nan <= nan, nan >= 1, inf, -inf, inf > 1e38f);
  printf("truth %d %d %d %d %d", !zero, !-zero, !a, zero && a, zero || a);
  if (-zero) printf(" bad"); else printf(" ok");
  if (nan) printf(" nan-true\n"); else printf(" nan-false\n");
  printf("funcs %f %f %f %f %f\n", half(5), avg(1, 2), newton_sqrt(2), poly(0.5f), half(half(10)));
  fp = half; printf("fnptr %f\n", fp(9));
  printf("globals %f %f %f %d %f %f %f %f\n", gf, gneg, gint, gtrunc, garr[1], garr[3], gp.x, gp.v);
  printf("gexpr %.7f\n", gexpr);
  p.x = 0; p.v = 0.5f; p.id = 1; for (i = 0; i < 4; i++) p.x += p.v * i; printf("struct %f %d\n", p.x, p.id);
  sum = 0; for (i = 0; i < 5; i++) { arr[i] = i * 0.1f; sum += arr[i]; } printf("array %f %f\n", sum, arr[3]);
  printf("ternary %f %f\n", a > 0 ? a : b, a < 0 ? 1.0f : 2);
  printf("formats [%.2f] [%e] [%g] [%10.3f] [%-8.1f] [%08.2f] [%.0f]\n", 3.14159f, 12345.678f, 0.0001f, b, a, b, 2.5f);
  printf("tiny %e %e huge %e\n", 1e-40f, 1.4e-45f, 3.4e38f);
  printf("sizeof %d %d\n", sizeof(float), sizeof(struct Particle));
  c = 0; while (c < 1) c += 0.25f; printf("loop %f\n", c);
  return 0;
}
