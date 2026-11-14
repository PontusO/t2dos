// mandel.c - float arithmetic in a loop (Mandelbrot iteration counts)
#include <stdio.h>

int main() {
  int x; int y; int it; int total; int r; float cr; float ci; float zr; float zi; float t; int t0;
  t0 = millis(); total = 0;
  for (r = 0; r < 8; r++)
  for (y = 0; y < 48; y++)
    for (x = 0; x < 64; x++) {
      cr = x * 0.046875 - 2.0 + r * 0.001; ci = y * 0.0520833 - 1.25;
      zr = 0.0; zi = 0.0; it = 0;
      while (it < 64 && zr * zr + zi * zi < 4.0) { t = zr * zr - zi * zi + cr; zi = 2.0 * zr * zi + ci; zr = t; it++; }
      total = total + it;
    }
  printf("check %d\n", total);
  printf("time %d\n", millis() - t0);
  return 0;
}
