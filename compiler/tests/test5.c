// test5.c - initializers. Standard C: gcc builds it natively for reference output.
#include <stdio.h>
#include <string.h>

#define N 4
enum { RED = 2, GREEN, BLUE = 10 };
struct Point { int x; int y; };
struct Rec { char *name; int vals[3]; struct Point at; char tag; };
struct Op { char *name; int (*fn)(int, int); };

int add(int a, int b) { return a + b; }
int sub(int a, int b) { return a - b; }

int gi = 42;
int gneg = -7, genum = GREEN, gchar = 'A';
int gexpr = 3 * 4 + 1;                    // not a constant here: computed before main
char *gs = "global string";
char garr[] = "abc";
char gbuf[8] = "hi";
char gexact[3] = "xyz";                   // no terminator, as in C
int gpart[5] = { 1, 2, 3 };               // rest zero
int ginf[] = { 10, 20, 30, 40, };         // size from the list, trailing comma
int gdef[N] = { N, N * 2 };
struct Point gp = { 3, -4 };
struct Rec grec = { "rec", { 7, 8, 9 }, { 1, 2 }, 'r' };
struct Point gpts[] = { { 1, 2 }, { 3, 4 }, { 5, 6 } };
struct Op gops[] = { { "add", add }, { "sub", sub } };
int (*gtable[])(int, int) = { add, sub, 0 };
int *gptr = &gi;
int *gmid = ginf + 2;
char *gnames[] = { "zero", "one", "two" };
int gbrace = { 99 };
int later = 5;                            // initialized after functions that use it

int show() { return later * 2; }

int main() {
  int i = 5, j = i * 2;
  char *p = "local string";
  int la[4] = { 1, 2 };
  char ls[] = "local";
  char lb[10] = "ab";
  struct Point lp = { 11, 12 };
  struct Rec lr = { "lrec", { 4, 5 }, { 6, 7 } };
  int lsum[] = { i, j, i + j };
  int (*lf)(int, int) = sub;
  int k;

  printf("scalars %d %d %d %d %d\n", gi, gneg, genum, gchar, gexpr);
  printf("strings %s %s %s %d %c%c%c\n", gs, garr, gbuf, strlen(gbuf), gexact[0], gexact[1], gexact[2]);
  printf("gpart %d %d %d %d %d\n", gpart[0], gpart[1], gpart[2], gpart[3], gpart[4]);
  printf("ginf %d %d %d %d gdef %d %d %d %d\n", ginf[0], ginf[1], ginf[2], ginf[3], gdef[0], gdef[1], gdef[2], gdef[3]);
  printf("gp %d %d grec %s %d %d %d %d %d %c\n", gp.x, gp.y, grec.name, grec.vals[0], grec.vals[1], grec.vals[2], grec.at.x, grec.at.y, grec.tag);
  printf("gpts %d %d %d %d %d %d\n", gpts[0].x, gpts[0].y, gpts[1].x, gpts[1].y, gpts[2].x, gpts[2].y);
  printf("gops %s=%d %s=%d table %d %d %d\n", gops[0].name, gops[0].fn(5, 3), gops[1].name, gops[1].fn(5, 3), gtable[0](1, 2), gtable[1](1, 2), gtable[2] == 0);
  printf("pointers %d %d names %s %s %s brace %d later %d\n", *gptr, *gmid, gnames[0], gnames[1], gnames[2], gbrace, show());
  printf("locals %d %d %s la %d %d %d %d\n", i, j, p, la[0], la[1], la[2], la[3]);
  printf("ls %s %d lb %s %d %d\n", ls, strlen(ls), lb, lb[2], lb[9]);
  printf("lp %d %d lr %s %d %d %d %d %d %d\n", lp.x, lp.y, lr.name, lr.vals[0], lr.vals[1], lr.vals[2], lr.at.x, lr.at.y, lr.tag);
  printf("lsum %d %d %d lf %d\n", lsum[0], lsum[1], lsum[2], lf(9, 4));
  for (k = 0; k < 3; k++) { int fresh = 100; int arr[2] = { k }; fresh += k; printf("loop %d %d %d ", fresh, arr[0], arr[1]); arr[1] = 55; }
  printf("\n");
  gi = 1; printf("globals writable %d\n", gi);
  return 0;
}
