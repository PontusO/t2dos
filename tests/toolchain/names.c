// names.c - with names_lib.c: uses the functions sp, add, b, push and the globals pc
// and r1 defined there, directly, through function addresses and from an initializer
extern int pc;
extern int r1;
extern int sp(int x);
extern int add(int a, int b);
extern int b(int x);
extern int push(int x);
int (*gadd)(int, int) = add;            // a function address in run-before-main code
int *gpc = &pc;                         // and a data address

int main() {
  int (*f)(int);
  printf("sp %d add %d\n", sp(1), add(20, 22));
  b(5); b(6);
  printf("r1 %d pc %d\n", r1, pc);
  pc = 10;
  r1 = r1 + 100;
  f = push;
  printf("push %d gadd %d gpc %d\n", f(4), gadd(2, 3), *gpc);
  f = b;
  return f(1);
}
