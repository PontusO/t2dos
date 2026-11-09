// vm.c - runs the bytecode: one handler per opcode (optab[], filled in vm_init()), the
// value stack, the slot store and strings. The handlers work on the globals pc (next
// code word), vs/fs/sp (the value stack) and read their arguments from code[pc].
#include "basic.h"

int pc;
int *vs;
float *fs;
int sp;
int *cs;
int csp;
int running;
void (*optab[NOPS])();
int *ivar;
float *fvar;
char **svar;
int dptr;                        // the next DATA item
int nalloc[4];                   // slots allocated per type (vm_alloc keeps them on growth)

// ---------------------------------------------------------------- strings
char *str_new(int len) {
  int *b;
  if (len <= 0) return 0;
  b = malloc(len + 5);           // length, data, a 0 (malloc zeroes)
  b[0] = len;
  return (char *)(b + 1);
}

void str_free(char *s) { if (s) free(s - 4); }
int str_len(char *s) { if (!s) return 0; return *(int *)(s - 4); }

char *str_from(char *p, int len) {
  char *s;
  s = str_new(len);
  if (s) memcpy(s, p, len);
  return s;
}

char *str_dup(char *s) { return str_from(s, str_len(s)); }

int str_cmp(char *a, char *b) {
  int la; int lb; int i; int x; int y;
  la = str_len(a); lb = str_len(b);
  for (i = 0; i < la && i < lb; i++) {
    x = a[i] & 255; y = b[i] & 255;
    if (x != y) return x - y;
  }
  return la - lb;
}

// ---------------------------------------------------------------- errors, conversions
int vm_err(char *msg) { return err_at(pc - 1, msg); }

int f2i(float f) {
  int i; float r;
  if (!(f < 2147483648.0 && f >= -2147483648.0)) { vm_err("Overflow"); return 0; }
  i = (int)f;
  r = f - i;
  if (r > 0.5) i++;
  else if (r < -0.5) i--;
  else if (r == 0.5) { if (i & 1) i++; }
  else if (r == -0.5) { if (i & 1) i--; }
  return i;
}

// ---------------------------------------------------------------- handlers
void op_end() { running = 0; }
void op_pushi() { sp++; vs[sp] = code[pc]; pc++; }
void op_pushs() { sp++; vs[sp] = (int)str_dup((char *)code[pc]); pc++; }
void op_ldi() { sp++; vs[sp] = ivar[code[pc]]; pc++; }
void op_ldf() { sp++; fs[sp] = fvar[code[pc]]; pc++; }
void op_lds() { sp++; vs[sp] = (int)str_dup(svar[code[pc]]); pc++; }
void op_sti() { ivar[code[pc]] = vs[sp]; sp--; pc++; }
void op_stf() { fvar[code[pc]] = fs[sp]; sp--; pc++; }
void op_sts() { int n; n = code[pc]; pc++; str_free(svar[n]); svar[n] = (char *)vs[sp]; sp--; }
void op_refi() { sp++; vs[sp] = (int)&ivar[code[pc]]; pc++; }
void op_reff() { sp++; vs[sp] = (int)&fvar[code[pc]]; pc++; }
void op_refs() { sp++; vs[sp] = (int)&svar[code[pc]]; pc++; }

void op_swap() {
  int *a; int *b; int t;
  a = (int *)vs[sp - 1]; b = (int *)vs[sp]; sp = sp - 2;
  t = *a; *a = *b; *b = t;
}

void op_i2f() { fs[sp] = vs[sp]; }
void op_i2f2() { fs[sp - 1] = vs[sp - 1]; }
void op_f2i() { vs[sp] = f2i(fs[sp]); }

void op_addi() { sp--; vs[sp] = vs[sp] + vs[sp + 1]; }
void op_subi() { sp--; vs[sp] = vs[sp] - vs[sp + 1]; }
void op_muli() { sp--; vs[sp] = vs[sp] * vs[sp + 1]; }

void op_idiv() {
  int b; unsigned u;
  b = vs[sp]; sp--;
  if (b == 0) { vm_err("Division by zero"); return; }
  if (b == -1) { u = 0; u = u - vs[sp]; vs[sp] = u; return; }   // no trap on -2147483648 \ -1
  vs[sp] = vs[sp] / b;
}

void op_modi() {
  int b;
  b = vs[sp]; sp--;
  if (b == 0) { vm_err("Division by zero"); return; }
  if (b == -1) { vs[sp] = 0; return; }
  vs[sp] = vs[sp] % b;
}

void op_negi() { unsigned u; u = 0; u = u - vs[sp]; vs[sp] = u; }

void op_addf() { sp--; fs[sp] = fs[sp] + fs[sp + 1]; }
void op_subf() { sp--; fs[sp] = fs[sp] - fs[sp + 1]; }
void op_mulf() { sp--; fs[sp] = fs[sp] * fs[sp + 1]; }

void op_divf() {
  sp--;
  if (fs[sp + 1] == 0) { vm_err("Division by zero"); return; }
  fs[sp] = fs[sp] / fs[sp + 1];
}

void op_negf() { fs[sp] = -fs[sp]; }

void op_powf() {
  float b; float e;
  e = fs[sp]; sp--; b = fs[sp];
  if (b < 0 && floor(e) != e) { vm_err("Illegal function call"); return; }
  if (b == 0 && e < 0) { vm_err("Division by zero"); return; }
  fs[sp] = pow(b, e);
}

// float ^ int by repeated squaring: exact while the products are (2 ^ 24 is 16777216)
void op_powi() {
  int n; unsigned m; float b; float r;
  n = vs[sp]; sp--; b = fs[sp];
  m = n;
  if (n < 0) { m = 0; m = m - n; }
  r = 1;
  while (m) {
    if (m & 1) r = r * b;
    m = m >> 1;
    if (m) b = b * b;
  }
  if (n < 0) {
    if (r == 0) { vm_err("Division by zero"); return; }
    r = 1 / r;
  }
  fs[sp] = r;
}

void op_cat() {
  char *a; char *b; char *s; int la; int lb;
  a = (char *)vs[sp - 1]; b = (char *)vs[sp]; sp--;
  la = str_len(a); lb = str_len(b);
  if (la + lb > STR_MAX) { str_free(b); vm_err("Out of string space"); return; }
  if (lb == 0) return;
  if (la == 0) { vs[sp] = (int)b; return; }
  s = str_new(la + lb);
  memcpy(s, a, la); memcpy(s + la, b, lb);
  str_free(a); str_free(b);
  vs[sp] = (int)s;
}

void op_eqi() { sp--; vs[sp] = 0 - (vs[sp] == vs[sp + 1]); }
void op_nei() { sp--; vs[sp] = 0 - (vs[sp] != vs[sp + 1]); }
void op_lti() { sp--; vs[sp] = 0 - (vs[sp] < vs[sp + 1]); }
void op_gti() { sp--; vs[sp] = 0 - (vs[sp] > vs[sp + 1]); }
void op_lei() { sp--; vs[sp] = 0 - (vs[sp] <= vs[sp + 1]); }
void op_gei() { sp--; vs[sp] = 0 - (vs[sp] >= vs[sp + 1]); }
void op_eqf() { sp--; vs[sp] = 0 - (fs[sp] == fs[sp + 1]); }
void op_nef() { sp--; vs[sp] = 0 - (fs[sp] != fs[sp + 1]); }
void op_ltf() { sp--; vs[sp] = 0 - (fs[sp] < fs[sp + 1]); }
void op_gtf() { sp--; vs[sp] = 0 - (fs[sp] > fs[sp + 1]); }
void op_lef() { sp--; vs[sp] = 0 - (fs[sp] <= fs[sp + 1]); }
void op_gef() { sp--; vs[sp] = 0 - (fs[sp] >= fs[sp + 1]); }

void op_cmps() {
  int c; int r; char *a; char *b;
  c = code[pc]; pc++;
  a = (char *)vs[sp - 1]; b = (char *)vs[sp]; sp--;
  r = str_cmp(a, b);
  str_free(a); str_free(b);
  if (c == TK_EQ) r = r == 0;
  else if (c == TK_NE) r = r != 0;
  else if (c == TK_LT) r = r < 0;
  else if (c == TK_GT) r = r > 0;
  else if (c == TK_LE) r = r <= 0;
  else r = r >= 0;
  vs[sp] = 0 - r;
}

void op_and() { sp--; vs[sp] = vs[sp] & vs[sp + 1]; }
void op_or() { sp--; vs[sp] = vs[sp] | vs[sp + 1]; }
void op_xor() { sp--; vs[sp] = vs[sp] ^ vs[sp + 1]; }
void op_eqv() { sp--; vs[sp] = ~(vs[sp] ^ vs[sp + 1]); }
void op_imp() { sp--; vs[sp] = ~vs[sp] | vs[sp + 1]; }
void op_not() { vs[sp] = ~vs[sp]; }

// a number as PRINT shows it: a space (or the -), the digits, a space; a number that does
// not fit on the rest of an 80-column line starts a new one
void print_num(char *s, int n) {
  if (s[0] != '-') n++;
  if (out_col > 0 && out_col + n > 80) out_char('\n');
  if (s[0] != '-') out_char(' ');
  out_cstr(s);
  out_char(' ');
}

void op_printi() { char b[16]; int n; n = fmt_int(b, vs[sp]); sp--; print_num(b, n); }
void op_printf() { char b[32]; int n; n = fmt_sng(b, fs[sp]); sp--; print_num(b, n); }

void op_prints() {
  char *s;
  s = (char *)vs[sp]; sp--;
  out_str(s, str_len(s));
  str_free(s);
}

void op_prcomma() {
  int n;
  n = (out_col / 14 + 1) * 14;
  if (n >= 80) { out_char('\n'); return; }
  while (out_col < n) out_char(' ');
}

void op_prnl() { out_char('\n'); }

// ---------------------------------------------------------------- jumps and the control stack
void op_jmp() { pc = code[pc]; }
void op_jz() { int v; v = vs[sp]; sp--; if (v == 0) pc = code[pc]; else pc++; }
void op_jnz() { int v; v = vs[sp]; sp--; if (v != 0) pc = code[pc]; else pc++; }

// The frame of this kind nearest the top, as the index of its top word, or 0. kind CK_FOR
// looks for the loop variable addr, but not below a GOSUB frame; CK_GOSUB for any GOSUB. Frames of other kinds end the
// search (a SUB call hides the frames of its caller).
int cs_find(int kind, int addr) {
  int i; int k;
  i = csp;
  while (i > 0) {
    k = cs[i];
    if (k == CK_FOR) {
      if (kind == CK_FOR && cs[i - 4] == addr) return i;
      i = i - 5;
    }
    else if (k == CK_GOSUB) {
      if (kind == CK_GOSUB) return i;
      if (kind == CK_FOR) return 0;            // frames below a GOSUB belong to its caller
      i = i - 2;
    }
    else return 0;
  }
  return 0;
}

// FOR: pops step, limit and the variable's address. A loop that does not run at all leaves no
// frame; a frame of the same variable (left by a GOTO out of the loop) is replaced.
void for_enter(int *ref, int limit, int step, int skip, int over) {
  int i; int j;
  i = cs_find(CK_FOR, (int)ref);
  if (i) {
    for (j = i + 1; j <= csp; j++) cs[j - 5] = cs[j];
    csp = csp - 5;
  }
  if (over) { pc = skip; return; }
  if (csp + 5 >= CSTACK) { vm_err("Out of stack space"); return; }
  cs[csp + 1] = (int)ref; cs[csp + 2] = limit; cs[csp + 3] = step; cs[csp + 4] = pc;
  cs[csp + 5] = CK_FOR;
  csp = csp + 5;
}

void op_fori() {
  int step; int limit; int *ref; int skip; int over;
  skip = code[pc]; pc++;
  step = vs[sp]; limit = vs[sp - 1]; ref = (int *)vs[sp - 2]; sp = sp - 3;
  if (step >= 0) over = *ref > limit; else over = *ref < limit;
  for_enter(ref, limit, step, skip, over);
}

void op_forf() {
  float step; float limit; float *ref; int skip; int over;
  skip = code[pc]; pc++;
  step = fs[sp]; limit = fs[sp - 1]; ref = (float *)vs[sp - 2];
  over = 0;
  if (step >= 0) { if (*ref > limit) over = 1; } else { if (*ref < limit) over = 1; }
  sp = sp - 3;
  for_enter((int *)ref, fbits(limit), fbits(step), skip, over);
}

// NEXT: step the variable; loop back or pop the frame (and any frames a GOTO left above it)
void op_nexti() {
  int *ref; int i; int step; int limit; int v; int nv; int done; unsigned u;
  ref = (int *)vs[sp]; sp--;
  i = cs_find(CK_FOR, (int)ref);
  if (!i) { vm_err("NEXT without FOR"); return; }
  csp = i;
  limit = cs[i - 3]; step = cs[i - 2];
  v = *ref;
  u = v; u = u + step; nv = u;
  *ref = nv;
  done = 0;
  if (step >= 0) { if (nv > limit || nv < v) done = 1; }
  else { if (nv < limit || nv > v) done = 1; }
  if (done) csp = i - 5; else pc = cs[i - 1];
}

void op_nextf() {
  float *ref; int i; float step; float limit; float nv; int done;
  ref = (float *)vs[sp]; sp--;
  i = cs_find(CK_FOR, (int)ref);
  if (!i) { vm_err("NEXT without FOR"); return; }
  csp = i;
  limit = bitsf(cs[i - 3]); step = bitsf(cs[i - 2]);
  nv = *ref + step;
  *ref = nv;
  done = 0;
  if (step >= 0) { if (nv > limit) done = 1; }
  else { if (nv < limit) done = 1; }
  if (done) csp = i - 5; else pc = cs[i - 1];
}

void op_exitfor() {
  int *ref; int i;
  ref = (int *)vs[sp]; sp--;
  i = cs_find(CK_FOR, (int)ref);
  if (i) csp = i - 5;
}

// GOSUB: at most 1000 nested
void op_gosub() {
  int target; int n; int i; int k;
  target = code[pc]; pc++;
  n = 0; i = csp;
  while (i > 0) {
    k = cs[i];
    if (k == CK_FOR) i = i - 5;
    else if (k == CK_GOSUB) { n++; i = i - 2; }
    else break;
  }
  if (n >= 1000 || csp + 2 >= CSTACK) { vm_err("Out of stack space"); return; }
  cs[csp + 1] = pc; cs[csp + 2] = CK_GOSUB;
  csp = csp + 2;
  pc = target;
}

void op_return() {
  int i;
  i = cs_find(CK_GOSUB, 0);
  if (!i) { vm_err("RETURN without GOSUB"); return; }
  pc = cs[i - 1];
  csp = i - 2;
}

// ---------------------------------------------------------------- DATA
// An item as a number: 1 when it is one (an empty item is 0), -1 for Overflow, else 0;
// *type T_INT / T_SNG and *val the int or the float's bits.
int data_num(char *s, int n, int quoted, int *type, int *val) {
  int i; int neg; int j;
  *type = T_INT; *val = 0;
  if (quoted) return 0;
  if (n == 0) return 1;
  i = 0; neg = 0;
  if (s[0] == '-') { neg = 1; i = 1; } else if (s[0] == '+') i = 1;
  j = num_scan(s + i, n - i, type, val);
  if (j == 0 || i + j != n) return 0;
  if (*type == T_NONE) return -1;
  if (neg) { if (*type == T_INT) *val = 0 - *val; else *val = *val ^ 0x80000000; }
  return 1;
}

// the next item as a number; 0 after an error
int read_num(int *type, int *val) {
  int r; char *s;
  if (dptr >= ndata) { vm_err("Out of DATA"); return 0; }
  s = d_item[dptr];
  r = data_num(s, str_len(s), d_quoted[dptr], type, val);
  dptr++;
  if (r == 0) { vm_err("Syntax error"); return 0; }
  if (r < 0) { vm_err("Overflow"); return 0; }
  return 1;
}

void op_readi() {
  int type; int val;
  if (!read_num(&type, &val)) return;
  if (type == T_SNG) val = f2i(bitsf(val));
  sp++; vs[sp] = val;
}

void op_readf() {
  int type; int val;
  if (!read_num(&type, &val)) return;
  sp++;
  if (type == T_INT) fs[sp] = val; else vs[sp] = val;
}

void op_reads() {
  if (dptr >= ndata) { vm_err("Out of DATA"); return; }
  sp++; vs[sp] = (int)str_dup(d_item[dptr]);
  dptr++;
}

void op_restore() { dptr = code[pc]; pc++; }

// ---------------------------------------------------------------- the machine
int vm_init() {
  vs = malloc(VSTACK * 4); fs = (float *)vs; sp = 0;
  cs = malloc(CSTACK * 4); csp = 0;
  optab[OP_END] = op_end; optab[OP_HALT] = op_end;
  optab[OP_PUSHI] = op_pushi; optab[OP_PUSHF] = op_pushi; optab[OP_PUSHS] = op_pushs;
  optab[OP_LDI] = op_ldi; optab[OP_LDF] = op_ldf; optab[OP_LDS] = op_lds;
  optab[OP_STI] = op_sti; optab[OP_STF] = op_stf; optab[OP_STS] = op_sts;
  optab[OP_REFI] = op_refi; optab[OP_REFF] = op_reff; optab[OP_REFS] = op_refs;
  optab[OP_SWAP] = op_swap;
  optab[OP_I2F] = op_i2f; optab[OP_I2F2] = op_i2f2; optab[OP_F2I] = op_f2i;
  optab[OP_ADDI] = op_addi; optab[OP_SUBI] = op_subi; optab[OP_MULI] = op_muli;
  optab[OP_IDIV] = op_idiv; optab[OP_MODI] = op_modi; optab[OP_NEGI] = op_negi;
  optab[OP_ADDF] = op_addf; optab[OP_SUBF] = op_subf; optab[OP_MULF] = op_mulf;
  optab[OP_DIVF] = op_divf; optab[OP_NEGF] = op_negf;
  optab[OP_POWF] = op_powf; optab[OP_POWI] = op_powi;
  optab[OP_CAT] = op_cat;
  optab[OP_EQI] = op_eqi; optab[OP_NEI] = op_nei; optab[OP_LTI] = op_lti;
  optab[OP_GTI] = op_gti; optab[OP_LEI] = op_lei; optab[OP_GEI] = op_gei;
  optab[OP_EQF] = op_eqf; optab[OP_NEF] = op_nef; optab[OP_LTF] = op_ltf;
  optab[OP_GTF] = op_gtf; optab[OP_LEF] = op_lef; optab[OP_GEF] = op_gef;
  optab[OP_CMPS] = op_cmps;
  optab[OP_AND] = op_and; optab[OP_OR] = op_or; optab[OP_XOR] = op_xor;
  optab[OP_EQV] = op_eqv; optab[OP_IMP] = op_imp; optab[OP_NOT] = op_not;
  optab[OP_PRINTI] = op_printi; optab[OP_PRINTF] = op_printf; optab[OP_PRINTS] = op_prints;
  optab[OP_PRCOMMA] = op_prcomma; optab[OP_PRNL] = op_prnl;
  optab[OP_JMP] = op_jmp; optab[OP_JZ] = op_jz; optab[OP_JNZ] = op_jnz;
  optab[OP_FORI] = op_fori; optab[OP_FORF] = op_forf;
  optab[OP_NEXTI] = op_nexti; optab[OP_NEXTF] = op_nextf; optab[OP_EXITFOR] = op_exitfor;
  optab[OP_GOSUB] = op_gosub; optab[OP_RETURN] = op_return;
  optab[OP_READI] = op_readi; optab[OP_READF] = op_readf; optab[OP_READS] = op_reads;
  optab[OP_RESTORE] = op_restore;
  dptr = 0;
  return 0;
}

// cells for n slots: the old block when it is big enough, else a new one holding the old
// block's first `have` cells (the rest zeroed)
int *cells(int *old, int have, int n) {
  int *p;
  if (n <= have && old) return old;
  p = malloc((n + 1) * 4);
  if (old) { memcpy(p, old, have * 4); free(old); }
  return p;
}

int vm_alloc() {
  ivar = cells(ivar, nalloc[T_INT], nslot[T_INT]);
  fvar = (float *)cells((int *)fvar, nalloc[T_SNG], nslot[T_SNG]);
  svar = (char **)cells((int *)svar, nalloc[T_STR], nslot[T_STR]);
  if (nslot[T_INT] > nalloc[T_INT]) nalloc[T_INT] = nslot[T_INT];
  if (nslot[T_SNG] > nalloc[T_SNG]) nalloc[T_SNG] = nslot[T_SNG];
  if (nslot[T_STR] > nalloc[T_STR]) nalloc[T_STR] = nslot[T_STR];
  return 0;
}

int vm_run(int start) {
  int op;
  pc = start;
  running = 1;
  while (running) {
    op = code[pc]; pc++;
    optab[op]();
  }
  return 0;
}

void vm_free() {
  int i;
  if (svar) for (i = 0; i < nalloc[T_STR]; i++) str_free(svar[i]);
  sp = 0;                        // Task 4: strings an error left on the stack are not freed yet
  if (ivar) free(ivar);
  if (fvar) free(fvar);
  if (svar) free(svar);
  ivar = 0; fvar = 0; svar = 0;
  for (i = 0; i < 4; i++) nalloc[i] = 0;
  free(vs); free(cs);
}
