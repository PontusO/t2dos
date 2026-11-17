// check_frame.py: function entry and exit shapes with -O (and that the code still works)
#include "sys.h"
int g;
int leaf() { g = g + 1; return g; }                  // no parameters, no locals: no frame pointer
int small(int n) { char buf[16]; int i; i = 0; while (i < 16) { buf[i] = n + i; i++; } return buf[15]; }   // sub sp, #N
int big() { char b[1000]; b[999] = 7; return b[999]; }   // over 508 bytes: the long form stays
int early(int x) { if (x < 2) return x; return early(x - 1) + 1; }   // the early return pops on the spot
int noret() { g = g + 2; }                           // falls off the end
int main() {
  int a; int b; int c; int d;
  a = leaf(); b = small(3); c = big(); d = early(5); noret();
  printf("%d %d %d %d %d\n", a, b, c, d, g);
  return 0;
}
