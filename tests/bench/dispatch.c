// dispatch.c - calls through a table of function pointers, state in globals (like basic's VM)
#include <stdio.h>

int acc; int cnt; int pos;
int prog[32];
int (*optab[4])();

int op_add() { acc = acc + prog[pos + 1]; pos = pos + 2; return 0; }
int op_mul() { acc = acc * 3 + 1; pos = pos + 1; return 0; }
int op_mask() { acc = acc & 1048575; pos = pos + 1; return 0; }
int op_loop() { cnt--; if (cnt > 0) pos = 0; else pos = -1; return 0; }

int main() {
  int t0; int op;
  optab[0] = op_add; optab[1] = op_mul; optab[2] = op_mask; optab[3] = op_loop;
  prog[0] = 0; prog[1] = 5; prog[2] = 1; prog[3] = 2; prog[4] = 0; prog[5] = 9; prog[6] = 1; prog[7] = 2; prog[8] = 3;
  t0 = millis(); acc = 1; cnt = 120000; pos = 0;
  while (pos >= 0) { op = prog[pos]; optab[op](); }
  printf("check %d\n", acc);
  printf("time %d\n", millis() - t0);
  return 0;
}
