// licm torture cases: the guards must fail closed, and what hoists must stay correct.
#include "sys.h"
int G[64];
int n;
int call(int x);
int enter(int m) {                      // a body branch lands above the head's test: no hoist
  int i; int s;
  i = 0; s = 0;
  while (i < m) {
    if (s == 1000) { s = s + G[n]; i++; continue; }   // joins mid-test
    s = s + G[n] + G[n + 1];
    i++;
  }
  return s;
}
int callloop(int m) {                   // the body calls: r0-r3 values die at bl
  int i; int s;
  i = 0; s = 0;
  while (i < m) { s = s + call(G[n]) + G[n]; i++; }
  return s;
}
int pushpop(int m) {                    // the value lives in a stack slot across a call
  int i; int s; int t;
  i = 0; s = 0;
  while (i < m) {
    t = G[n];
    s = s + call(t) + t;                 // push/pop traffic around the call, and t is re-read
    i++;
  }
  return s;
}
int nested(int m) {                     // three deep, same base read in all three loops
  int i; int j; int k; int s;
  i = 0; s = 0;
  while (i < m) {
    j = 0;
    while (j < m) {
      k = 0;
      while (k < m) { s = s + G[n]; k++; }
      j++;
    }
    i++;
  }
  return s;
}
int flags(int m) {                      // an S-setting ALU line near the head's test
  int i; int s; int t;
  i = 0; s = 0;
  while (i < m) {
    t = G[n];
    s = s + t;                           // adds sets flags: only dead ones may be folded
    if (s > 1000000) s = s - 1000000;
    i++;
  }
  return s;
}
int reentry(int m) {                    // a continue lands on the test again mid-body, and
  int i; int j; int k; int s;            // the inner loop exits to a label below the outer
  i = 0; s = 0;                          // loop's body: an entry into it from outside would
  while (i < m) {                        // run a hoist twice per round. The exit at s > 900
    j = 0;                               // branches below the outer head: licm() must refuse
    while (j < m) {                      // this loop's candidates
      k = G[n];
      s = s + k;
      if (s > 900) { i = m; break; }     // leaves both loops through a label under the outer
      j++;                               // head's test
    }
    i++;
  }
  return s;
}
int call(int x) { return x + G[n]; }
int main() {
  int a; int b; int c; int d; int e; int f;
  n = 3; G[3] = 7; G[4] = 11;
  a = enter(6);                          // 6*(7+11) = 108
  b = callloop(5);                       // 5*((7+7)+7) = 105
  c = pushpop(4);                        // 4*(7+7+7) = 84
  d = nested(3);                         // 9*7 = 63
  e = flags(20);                         // 20*7 = 140
  f = reentry(4);                        // 16*7 = 112 (the break path stays cold)
  printf("%d %d %d %d %d %d\n", a, b, c, d, e, f);
  return 0;
}
