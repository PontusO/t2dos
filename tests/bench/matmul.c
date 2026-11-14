// matmul.c - integer matrix multiply on flat arrays (index arithmetic, multiply-add)
#include <stdio.h>

int A[1600]; int B[1600]; int C[1600];

int mul(int n) {
  int i; int j; int k; int s;
  for (i = 0; i < n; i++)
    for (j = 0; j < n; j++) {
      s = 0;
      for (k = 0; k < n; k++) s = s + A[i * n + k] * B[k * n + j];
      C[i * n + j] = s;
    }
  return 0;
}

int main() {
  int i; int r; unsigned sum; int t0;
  for (i = 0; i < 1600; i++) { A[i] = i % 17 - 8; B[i] = i % 13 - 6; }
  t0 = millis(); sum = 0;
  for (r = 0; r < 12; r++) { mul(40); sum = sum + C[r * 41] + C[1599 - r]; A[r] = A[r] + 1; }
  printf("check %u\n", sum);
  printf("time %d\n", millis() - t0);
  return 0;
}
