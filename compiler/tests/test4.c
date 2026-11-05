// test4.c - function pointers. Standard C: gcc builds it natively for reference output.
#include <stdio.h>
#include <string.h>

typedef int (*binop)(int, int);
struct Op { char *name; binop fn; int (*unary)(int); };
struct Op ops[3];
int (*table[4])(int, int);
int (*gfp)(int, int);
int calls;

int add(int a, int b) { calls++; return a + b; }
int sub(int a, int b) { calls++; return a - b; }
int mul(int a, int b) { calls++; return a * b; }
int neg(int a) { return -a; }
int twice(int a) { return a * 2; }
char *greet(int n) { if (n) return "hello"; return "bye"; }

int apply(int (*f)(int, int), int a, int b) { return f(a, b); }
int apply_star(binop f, int a, int b) { return (*f)(a, b); }
int later(int x);                         // address taken before definition
binop pick(int i) { if (i) return mul; return sub; }
int fold(int *v, int n, binop f, int init) { int i; int acc; acc = init; for (i = 0; i < n; i++) acc = f(acc, v[i]); return acc; }
int fact(int n);
int (*rec)(int);
int fact(int n) { if (n < 2) return 1; return n * rec(n - 1); }

int main() {
  int (*fp)(int, int); int (*u)(int); char *(*g)(int); binop b; struct Op *op; int i; int v[5];
  int (*loc[2])(int);

  fp = add; printf("fp=add %d\n", fp(2, 3));
  fp = &sub; printf("fp=&sub %d %d\n", fp(10, 4), (*fp)(1, 2));
  printf("apply %d %d %d\n", apply(add, 5, 6), apply(mul, 5, 6), apply_star(sub, 5, 6));
  table[0] = add; table[1] = sub; table[2] = mul; table[3] = 0;
  for (i = 0; table[i]; i++) printf("table[%d]=%d ", i, table[i](7, 3));
  printf("\n");
  ops[0].name = "neg"; ops[0].unary = neg; ops[0].fn = add;
  ops[1].name = "twice"; ops[1].unary = twice; ops[1].fn = mul;
  op = ops + 1;
  printf("member %s %d %d, via ptr %s %d %d\n", ops[0].name, ops[0].unary(9), ops[0].fn(1, 1), op->name, op->unary(9), (*op->fn)(4, 5));
  g = greet; printf("char* return %s %s len=%d\n", g(1), g(0), strlen(g(1)));
  u = later; printf("forward %d\n", u(20));
  printf("returned fp %d %d, chained %d\n", pick(0)(9, 4), pick(1)(9, 4), pick(1)(pick(0)(9, 4), 3));
  for (i = 0; i < 5; i++) v[i] = i + 1;
  printf("fold sum=%d prod=%d\n", fold(v, 5, add, 0), fold(v, 5, mul, 1));
  rec = fact; printf("recursion via pointer %d\n", rec(6));
  gfp = 0; printf("null %d %d\n", gfp == 0, !gfp);
  gfp = add; b = add; printf("compare %d %d %d\n", gfp == b, gfp == add, gfp != sub);
  loc[0] = neg; loc[1] = twice; printf("local array %d %d\n", loc[0](3), loc[1](3));
  b = (binop)fp; printf("cast %d sizeof %d %d\n", b(8, 2), sizeof(binop), sizeof(int (*)(int)));
  printf("calls=%d\n", calls);
  return 0;
}

int later(int x) { return x + 1; }
