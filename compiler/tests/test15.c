// test15.c - static variables and functions. Standard C: gcc builds it natively
// for reference output.
#include <stdio.h>

static int gcount;                       // static global, zero init
static int gseven = 3 * 7;               // computed init (.Linit) before main
static int arr[3] = { 1, 2, 3 };         // static array, constant init
static int v = 1;                        // shadowed below

static int helper(int x) { return x * 3; }
static int twice_helper(int x) { return helper(x) + helper(x) / 2; }

int bump(void) { static int i; i = i + 1; return i; }        // own counter
int poke(void) { static int i; i = i + 10; return i; }       // same name, other cell
int once(void) { static int c = 41; c = c + 1; return c; }   // static init once, then persists

int sum_arr(void) { int i; int s; s = 0; for (i = 0; i < 3; i = i + 1) s = s + arr[i]; return s; }

int shadow(void) {
  int out;
  { int v = 2; printf("shadow inner %d\n", v); }
  out = v;                                                    // static v again
  return out;
}

int apply(int (*f)(int), int x) { return f(x); }

int main() {
  int (*fp)(int); int i; int b1; int b2; int c1; int c2;
  printf("gcount %d\n", gcount);
  printf("gseven %d\n", gseven);
  b1 = bump(); b2 = bump();
  printf("bump %d %d\n", b1, b2);
  printf("poke %d\n", poke());
  printf("bump again %d\n", bump());
  printf("arr %d\n", sum_arr());
  c1 = once(); c2 = once();
  printf("once %d %d\n", c1, c2);
  printf("shadowed %d\n", shadow());
  fp = helper;
  printf("fp %d %d\n", fp(4), apply(twice_helper, 3));
  gcount = gcount + 1;
  for (i = 0; i < 3; i = i + 1) ;
  printf("gcount now %d v %d\n", gcount, v);
  return 0;
}
