// test8.c - the comma operator. Standard C: gcc builds it natively for reference output.
#include <stdio.h>

int calls;
int tick(int v) { calls++; return v; }
int pair(int a, int b) { return a * 10 + b; }
int last(int a, int b) { return (a++, b++, a + b); }
char *src = "abc";
int pos;
int getc_() { return src[pos] ? src[pos++] : 0; }

int main() {
  int i; int j; int k; int c; int a[5]; int t; int x;

  for (i = 0, j = 9; i < j; i++, j--) printf("%d%d ", i, j);
  printf("\n");
  k = (tick(1), tick(2), tick(3)); printf("value %d calls %d\n", k, calls);
  x = 5; k = (x += 2, x * 3); printf("side effects %d %d\n", x, k);
  printf("args %d %d\n", pair((tick(7), 1), 2), pair(3, (4, 5)));
  printf("return %d\n", last(1, 2));
  while (c = getc_(), c != 0) printf("[%c]", c);
  printf("\n");
  for (i = 0; i < 5; i++) a[i] = i * i;
  printf("index %d\n", a[tick(0), 3]);
  t = 1; printf("ternary %d %d\n", t ? (tick(8), 9) : 0, !t ? 0 : tick(1), 4);
  if (tick(0), t) printf("if takes the last\n");
  switch (i = 2, i + 1) { case 3: printf("switch %d\n", i); break; default: printf("bad\n"); }
  x = (i = 10, j = 20, i + j); printf("chain %d %d %d\n", x, i, j);
  i = 0; j = 0;
  for (; i < 3; i++, j += 2) {}
  printf("for clauses %d %d calls %d\n", i, j, calls);
  return 0;
}
