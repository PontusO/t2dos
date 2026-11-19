// check_licm.py: cc -O's loop-invariant code motion (tests/opt/licm.c)
#include "sys.h"
int B[256];
int n;
int sum(int m) {
  int i; int s; int *p;
  p = B; s = 0; i = 0;
  while (i < m) { s = s + p[n]; i++; }   // p + &n is the same address every round
  return s;
}
int two(int m) {                          // a loop inside a loop: both hoist their own base
  int i; int j; int s;
  i = 0; s = 0;
  while (i < m) {
    j = 0;
    while (j < m) { s = s + B[n] + B[n + 1]; j++; }
    i++;
  }
  return s;
}
int nohoist(int m) {                      // the base moves every round: nothing may hoist
  int i; int s; int *p;
  p = B; s = 0; i = 0;
  while (i < m) { s = s + p[0]; p = p + 1; i++; }
  return s;
}
int main() {
  int a; int b; int c;
  n = 5; B[5] = 11; B[6] = 22;
  a = sum(4);                             // 4 * 11
  b = two(3);                             // 9 * (11 + 22)
  c = nohoist(8);                         // 0 + ... + 7, B[0..7]
  printf("%d %d %d\n", a, b, c);
  return 0;
}
