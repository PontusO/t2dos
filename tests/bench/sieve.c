// sieve.c - sieve of Eratosthenes on a global char array (byte loads and stores, indexing)
#include <stdio.h>

char flags[16384];

int sieve(int n) {
  int i; int j; int count;
  count = 0;
  for (i = 2; i < n; i++) flags[i] = 1;
  for (i = 2; i < n; i++) {
    if (flags[i]) {
      count++;
      for (j = i + i; j < n; j = j + i) flags[j] = 0;
    }
  }
  return count;
}

int main() {
  int r; int total; int t0;
  t0 = millis(); total = 0;
  for (r = 0; r < 40; r++) total = total + sieve(16384);
  printf("check %d\n", total);
  printf("time %d\n", millis() - t0);
  return 0;
}
