#include <stdio.h>
#define LIMIT 20
enum { RED, GREEN = 5, BLUE };
struct Point { int x; int y; char tag; char *name; };
int table[16];
int counter;
char buf[64];

int fib(int n) { if (n < 2) return n; return fib(n - 1) + fib(n - 2); }
int sum(int *a, int n) { int s; int i; s = 0; for (i = 0; i < n; i++) s = s + a[i]; return s; }
int strrev(char *s) {
  int i; int j; int t;
  i = 0; j = strlen(s) - 1;
  while (i < j) { t = s[i]; s[i] = s[j]; s[j] = t; i++; j--; }
  return 0;
}
int main(int argc, char **argv) {
  int i; struct Point pt; struct Point *pp; int local[8]; int big; char c;
  printf("hello from thumb-2, argc=%d argv0=%s\n", argc, argv[0]);
  for (i = 0; i < LIMIT; i++) { if (i % 7 == 3) continue; if (i > 12) break; printf("%d ", fib(i)); }
  printf("\n");
  i = 0; while (i < 16) { table[i] = i * i; local[i & 7] = -i; i++; }
  printf("sum sq=%d sumloc=%d %d %d %d\n", sum(table, 16), sum(local, 8), 17 / 5, 17 % 5, -17 % 5);
  pt.x = 3; pt.y = 4; pt.tag = 'Q'; pt.name = "pointy"; pp = &pt;
  printf("pt=(%d,%d) %c %s via ptr: %d %d\n", pt.x, pt.y, pt.tag, pt.name, pp->x * pp->y, pp->tag);
  strcpy(buf, "abcdefg"); strrev(buf);
  printf("rev=%s len=%d cmp=%d\n", buf, strlen(buf), strcmp(buf, "gfedcba"));
  big = 100000 * 300; counter = big >> 3;
  printf("big=%d ctr=%d shl=%d hex=%x and=%d or=%d xor=%d not=%d neg=%d\n", big, counter, 1 << 20, 0xBEEF, 12 & 10, 12 | 10, 12 ^ 10, !5, -counter);
  printf("cmp %d %d %d %d %d %d\n", 3 < 4, 3 > 4, 4 <= 4, 5 >= 6, 7 == 7, 7 != 7);
  printf("logic %d %d %d cond=%d\n", 0 || 2, 3 && 0, !0 && !0, big > 0 ? GREEN : BLUE);
  c = 'A'; c++; printf("c=%c pre=%c post=%c\n", c, ++c, c++);
  pp = &pt; printf("sizeof %d %d %d, ptrdiff %d\n", sizeof(int), sizeof(struct Point), sizeof(char *), (table + 5) - table);
  printf("%d\n", BLUE);
  return 42;
}
