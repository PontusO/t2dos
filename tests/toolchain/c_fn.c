// c_fn.c - with asm_calls_c.s: C functions called from assembly. c_msg needs
// run-before-main initializer code, which ld's entry stub calls before asm's main.
int c_count;
char *c_msg = "report";

int c_mul(int a, int b) { return a * b; }
int report(int v) { printf("%s %d\n", c_msg, v); return 0; }
int c_bump() { c_count = c_count + 1; return c_count; }
