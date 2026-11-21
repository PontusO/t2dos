// test7.c - unsigned, short, long. Standard C: gcc builds it natively for reference output.
// (long is 32 bits on T2DOS and 64 on a PC, so only values that fit 32 bits are printed.)
#include <stdio.h>

struct Mixed { char c; short s; int i; unsigned char uc; unsigned short us; };
unsigned short tab[4] = { 1, 65535, 40000, 7 };
short stab[] = { -1, -32768, 32767 };
unsigned char bytes[] = { 0, 127, 128, 255 };
unsigned big = 0xFFFFFFF0;
#define MASK 0x80000000u

unsigned char trunc8(int v) { return v; }
short trunc16(int v) { return v; }
int takes_short(short s) { return s; }
unsigned int udivide(unsigned a, unsigned b) { return a / b; }

int main() {
  unsigned u; unsigned int ui; int i; short s; unsigned short us; unsigned char uc; char c;
  signed char sc; short int si; long l; unsigned long ul; long int li; struct Mixed m; unsigned short *pus;

  u = 0xFFFFFFFF; i = -1;
  printf("compare %d %d %d %d %d\n", u > 1, i < 1u, -1 < 1, (unsigned)i > 5, MASK > 0);
  printf("div %u %u %u %d %d\n", big / 16, big % 7, udivide(4000000000u, 3), -7 / 2, -7 % 2);
  printf("shift %u %d %u\n", u >> 28, -16 >> 2, big >> 4);
  s = 32767; s++; us = 65535; us++; uc = 255; c = 200;
  printf("wrap %d %u %d %d ", s, us, uc + 1, c); uc++; printf("%d\n", uc);
  us = 65530; us += 10; s = -32768; s -= 1; uc = 250; uc += 10;
  printf("compound %u %d %u\n", us, s, uc);
  printf("casts %d %d %d %d %u\n", (unsigned char)300, (short)70000, (unsigned short)-1, (char)200, (unsigned)-1);
  printf("returns %d %d %d\n", trunc8(300), trunc16(70000), takes_short(65535));
  printf("arrays %u %u %u %d %d %d %u %u\n", tab[1], tab[2], tab[3], stab[0], stab[1], stab[2], bytes[2], bytes[3]);
  pus = tab; pus++; printf("ptr %u %u\n", *pus, pus[1]);
  m.c = -3; m.s = -1000; m.i = 123456; m.uc = 250; m.us = 60000;
  printf("struct %d %d %d %u %u size %d\n", m.c, m.s, m.i, m.uc, m.us, sizeof(struct Mixed));
  printf("sizeof %d %d %d %d %d\n", sizeof(short), sizeof(unsigned short), sizeof(unsigned char), sizeof(unsigned), sizeof(short int));
  sc = -5; si = -300; l = 100000; ul = 4000000000u; li = -2;
  printf("spellings %d %d %d %u %d\n", sc, si, (int)l, (unsigned)ul, (int)li);
  for (u = 3; u > 0; u--) printf("down %u ", u);
  printf("\n");
  ui = 65536u; printf("overflow %u %u\n", ui * ui, ui * ui - 1);
  u = 10; i = -20; printf("mixed %d %u\n", u + i > 0, u + i);
  us = 65535; printf("promote %d %d\n", us > -1, uc < -1);
  printf("unary %u %u\n", -u, ~u);
  return 0;
}
