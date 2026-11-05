// c_calls_asm.c - with asm_fn.s: C calls assembly functions and uses assembly data
extern int asm_val;                     // a word in asm_fn.s's data
extern int asm_sub3(int a, int b, int c);
extern int asm_get();
extern int asm_apply(int (*f)(int), int x);

int twice(int x) { return 2 * x; }      // called back from asm_apply

int main() {
  printf("sub3 %d\n", asm_sub3(100, 20, 3));   // argument order: 100 - 20 - 3
  printf("val %d\n", asm_val);
  asm_val = asm_val + 5;                // a store through the extern, read back by asm
  printf("get %d\n", asm_get());
  printf("apply %d\n", asm_apply(twice, 21));
  return asm_sub3(10, 2, 1);
}
