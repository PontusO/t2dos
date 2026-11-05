#include <stdio.h>
typedef int num;
typedef char *str;
typedef struct { int x; int y; } Point;
typedef struct Node Node;
struct Node { int val; Node *next; };
enum { ALPHA = 1, BETA, GAMMA = 10 };

str classify(num v) {
  switch (v) {
    case 0: return "zero";
    case 1: case 2: return "small";
    case GAMMA: return "ten";
    case -1: return "minus one";
    default: return "other";
  }
}

int main() {
  num a; int i; Point pt; Point *pp; Node n1; Node n2; Node *np; str s; int *ip; int arr[4]; char buf[8]; char c;
  a = 10; a += 5; a -= 3; a *= 2; a /= 4; a %= 5; a <<= 3; a >>= 1; a |= 1; a &= 7; a ^= 2;
  printf("a=%d\n", a);
  pt.x = 1; pt.y = 2; pp = &pt; pp->x += 10; pt.y *= 7;
  printf("pt=%d,%d sizeof=%d\n", pt.x, pt.y, sizeof(Point));
  n1.val = 1; n2.val = 2; n1.next = &n2; n2.next = 0;
  for (np = &n1; np; np = np->next) printf("node %d\n", np->val);
  for (i = 0; i < 4; i++) arr[i] = i; ip = arr; ip += 2; printf("ip=%d ", *ip); ip -= 1; printf("%d\n", *ip);
  strcpy(buf, "abc"); c = 'x'; c += 1; buf[0] += 1; printf("%s %c\n", buf, c);
  for (i = -2; i <= 11; i++) printf("%d:%s ", i, classify(i));
  printf("\n");
  i = 0;
  switch (3) { case 3: i += 1; case 4: i += 10; break; case 5: i += 100; }
  printf("fallthrough=%d\n", i);
  for (i = 0; i < 6; i++) { switch (i) { case 2: continue; case 4: break; default: printf("[%d]", i); } printf("."); }
  printf("\n");
  switch (99) { case 1: printf("no"); }
  s = "done"; printf("%s\n", s);
  return 0;
}
