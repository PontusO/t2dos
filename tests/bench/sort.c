// sort.c - quicksort of an int array (array indexing through a pointer, swaps, recursion)
#include <stdio.h>

int a[4000];
unsigned seed;

unsigned rnd() { seed = seed * 1103515245 + 12345; return (seed >> 8) & 1048575; }

int qsort_int(int *v, int lo, int hi) {
  int i; int j; int p; int t;
  while (lo < hi) {
    p = v[(lo + hi) / 2]; i = lo; j = hi;
    while (i <= j) {
      while (v[i] < p) i++;
      while (v[j] > p) j--;
      if (i <= j) { t = v[i]; v[i] = v[j]; v[j] = t; i++; j--; }
    }
    if (j - lo < hi - i) { qsort_int(v, lo, j); lo = i; }
    else { qsort_int(v, i, hi); hi = j; }
  }
  return 0;
}

int main() {
  int r; int i; int n; unsigned sum; int t0;
  t0 = millis(); sum = 0; seed = 1; n = 4000;
  for (r = 0; r < 20; r++) {
    for (i = 0; i < n; i++) a[i] = rnd();
    qsort_int(a, 0, n - 1);
    for (i = 1; i < n; i++) if (a[i - 1] > a[i]) { printf("not sorted\n"); return 1; }
    sum = sum + a[n / 3] + a[n - 1];
  }
  printf("check %u\n", sum);
  printf("time %d\n", millis() - t0);
  return 0;
}
