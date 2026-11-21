// test9.c - macros: text, function-like, multi-line, #, ##, variadic, #undef.
// Standard C: gcc builds it natively for reference output.
#include <stdio.h>

#define VERSION "1.2"
#define BANNER "T2DOS " VERSION
#define SIZE (4 * 2)
#define NEG -3
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define SQUARE(x) ((x) * (x))
#define CUBE(x) (SQUARE(x) * (x))
#define STR(x) #x
#define XSTR(x) STR(x)
#define CAT(a, b) a ## b
#define FIELD(n) field_ ## n
#define SWAP(t, a, b) {   \
    t tmp_ = (a);         \
    (a) = (b);            \
    (b) = tmp_;           \
  }
#define SUM3(a, b, c)     \
  ((a) +                  \
   (b) +                  \
   (c))
#define LOG(fmt, ...) printf("[log] " fmt "\n", __VA_ARGS__)
#define SHOW(...) printf(__VA_ARGS__)
#define EMPTY()
#define NOARGS() 42
#define APPLY(f, x) f(x)
#define TWICE(x) ((x) * 2)
#define FLAG 1
#undef FLAG
#define FLAG 2
#define printf_alias printf

int field_a; int field_b;
int table[SIZE];
int calls;
int bump() { calls++; return calls; }
int count = 5;
#define count count + 1          /* not expanded inside itself */

int main() {
  int i = 3, j = 9, k; int CAT(var, 1) = 11; char *s = BANNER;
  printf("%s %s %d %d\n", s, VERSION, SIZE, NEG);
  printf("max %d %d square %d cube %d\n", MAX(i, j), MAX(j - 10, -2), SQUARE(i + 1), CUBE(2));
  printf("str %s %s %s\n", STR(hello world), STR(a "q" b), XSTR(SIZE));
  printf("str2 %s|%s|%s\n", STR(a     b), XSTR(MAX(1, 2)), STR(SIZE));
  printf("rescan %d %d\n", CAT(SI, ZE), CAT(TWI, CE)(4));
  FIELD(a) = 1; FIELD(b) = 2; printf("paste %d %d %d\n", field_a, field_b, var1);
  SWAP(int, i, j) printf("swap %d %d\n", i, j);
  printf("multi %d\n", SUM3(1,
                            2,
                            3));
  LOG("%d + %d = %d", 2, 3, 2 + 3);
  SHOW("variadic %s %d\n", "ok", 7);
  EMPTY() printf("noargs %d\n", NOARGS());
  printf("self %d\n", count);
  printf("nested %d %d\n", APPLY(TWICE, 5), MAX(SQUARE(2), CUBE(1)));
  printf("args with parens %d %d\n", MAX((1, 9), 4), MAX(bump(), 0));
  printf("flag %d\n", FLAG);
  for (k = 0; k < SIZE; k++) table[k] = SQUARE(k);
  printf("array %d %d\n", table[SIZE - 1], table[3]);
  printf_alias("alias %d\n", MAX(1, 2));
  switch (2) { case FLAG: printf("case %d\n", FLAG); break; }
  return 0;
}
