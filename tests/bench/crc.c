// crc.c - bitwise CRC-32 over a buffer (shifts, xor, unsigned arithmetic)
#include <stdio.h>

char buf[2048];

unsigned crc32(char *p, int n) {
  unsigned c; int k;
  c = 4294967295;
  while (n > 0) {
    c = c ^ (*p & 255);
    for (k = 0; k < 8; k++) { if (c & 1) c = (c >> 1) ^ 3988292384; else c = c >> 1; }
    p++; n--;
  }
  return c ^ 4294967295;
}

int main() {
  int i; int r; unsigned c; int t0;
  for (i = 0; i < 2048; i++) buf[i] = i * 31 + (i >> 5);
  t0 = millis(); c = 0;
  for (r = 0; r < 80; r++) { buf[r] = buf[r] + 1; c = c ^ crc32(buf, 2048); }
  printf("check %u\n", c);
  printf("time %d\n", millis() - t0);
  return 0;
}
