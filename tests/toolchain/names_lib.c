// names_lib.c - with names.c: functions and globals whose names are also register
// names or instruction mnemonics in the assembly (sp, pc, r1, b, add, push)
int pc = 3;                             // globals
int r1;
int sp(int x) { return x + pc; }        // functions
int add(int a, int b) { return a + b; }
int b(int x) { r1 = r1 + x; return r1; }
int push(int x) { return add(x, sp(x)); }
