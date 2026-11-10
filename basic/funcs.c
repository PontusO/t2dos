// funcs.c - the built-in functions and RANDOMIZE: their compilers (argument types and counts
// are checked at load time) and the one VM op, OP_FN, that runs them.
//
// A function is added in funcs_init() with def(token, argument types, fewest arguments,
// result type, handler): argument type letters are f (float), i (integer), s (string) and n
// (a number kept as it is, int or float; the handler finds which in fn_n). Result type 0
// means the type of the n argument. The compiler pushes the arguments in order and emits
// OP_FN, the token, and fn_n for the handler (the argument count, or for an n function the
// argument's type). A handler takes its arguments from the stack, frees the strings among
// them (the ownership rule in basic.h) and leaves the result in their place. Functions
// with an unusual form (RND, INSTR, STRING$, the conversions) have their own compiler.
//
// RND is a 24-bit linear congruential generator, as QBasic's: the same on the PC and on the
// board. RANDOMIZE with no argument seeds from the clock instead of asking for a number.
#include "basic.h"

#define TK (toks[tp].tk)

char *f_types[NTOK];
int f_lo[NTOK];
int f_res[NTOK];
void (*fnop[NTOK])();
int fn_n;                        // the running function's count or type (see above)
int fn_t;                        // compile time: the type of the last n argument

int rnd_state;
float rnd_last;

// ---------------------------------------------------------------- compiling
void def(int k, char *types, int lo, int res, void (*h)()) {
  f_types[k] = types; f_lo[k] = lo; f_res[k] = res; fnop[k] = h;
  fn_tab[k] = 0;
}

int count_err() { cerr("Argument-count mismatch"); return -1; }

// ')' ends the arguments; another ',' is one too many
int fn_close() {
  if (TK == TK_COMMA) return count_err();
  return need(TK_RPAR);
}

// ( args ) by the type letters; returns the count, or -1 after an error
int fn_args(char *types, int lo) {
  int n; int max; int c; int t;
  max = strlen(types);
  if (!need(TK_LPAR)) return -1;
  n = 0;
  while (1) {
    if (n >= max || (TK == TK_RPAR && n == 0)) return count_err();
    c = types[n];
    if (c == 's') expr_to(T_STR);
    else if (c == 'n') {
      t = expr();
      if (t == T_STR) cerr("Type mismatch");
      fn_t = t;
    }
    else if (c == 'f') expr_to(T_SNG);
    else expr_to(T_INT);
    n++;
    if (TK == TK_COMMA) { next(); continue; }
    break;
  }
  if (n < lo) return count_err();
  if (!need(TK_RPAR)) return -1;
  return n;
}

void emit_fn(int k, int v) { emit(OP_FN); emit_arg(k); emit_arg(v); }

int fn_compile() {
  int k; int n; int v;
  k = TK; next();
  fn_t = T_INT;
  n = fn_args(f_types[k], f_lo[k]);
  if (n < 0) return f_res[k];
  v = n;
  if (f_types[k][0] == 'n') v = fn_t;
  if (fnop[k] && !((k == K_INT || k == K_FIX) && fn_t == T_INT)) emit_fn(k, v);
  if (f_res[k]) return f_res[k];
  return fn_t;
}

int fn_timer() { next(); emit_fn(K_TIMER, 0); return T_SNG; }

// RND, RND(x)
int fn_rnd() {
  int n;
  next(); n = 0;
  if (TK == TK_LPAR) { next(); expr_to(T_SNG); n = 1; if (!fn_close()) return T_SNG; }
  emit_fn(K_RND, n);
  return T_SNG;
}

// INSTR([start,] string, string)
int fn_instr() {
  int t; int n;
  next();
  if (!need(TK_LPAR)) return T_INT;
  t = expr();
  n = 2;
  if (t == T_STR) {
    if (TK != TK_COMMA) { count_err(); return T_INT; }
    next();
    expr_to(T_STR);
  }
  else {
    if (t == T_SNG) emit(OP_F2I);
    if (TK != TK_COMMA) { count_err(); return T_INT; }
    next();
    expr_to(T_STR);
    if (TK != TK_COMMA) { count_err(); return T_INT; }
    next();
    expr_to(T_STR);
    n = 3;
  }
  if (!fn_close()) return T_INT;
  emit_fn(K_INSTR, n);
  return T_INT;
}

// STRING$(n, code) or STRING$(n, string): the string's first character
int fn_strings() {
  int t;
  next();
  if (!need(TK_LPAR)) return T_STR;
  expr_to(T_INT);
  if (TK != TK_COMMA) { count_err(); return T_STR; }
  next();
  t = expr();
  if (t == T_STR) emit_fn(K_ASC, 1);
  else if (t == T_SNG) emit(OP_F2I);
  if (!fn_close()) return T_STR;
  emit_fn(K_STRINGS, 2);
  return T_STR;
}

// LBOUND(array [, dimension]) / UBOUND: the array cell's address and the dimension (1 if
// not given)
int fn_bound() {
  int k;
  k = TK; next();
  if (!need(TK_LPAR)) return T_INT;
  if (arr_named() < 0) return T_INT;
  if (accept(TK_COMMA)) expr_to(T_INT);
  else { emit(OP_PUSHI); emit_arg(1); }
  if (!fn_close()) return T_INT;
  emit_fn(k, 0);
  return T_INT;
}

// RANDOMIZE [seed]
void st_randomize() {
  int n;
  next(); n = 0;
  if (!at_end()) { expr_to(T_SNG); n = 1; }
  emit_fn(K_RANDOMIZE, n);
}

// ---------------------------------------------------------------- running
void op_fn() {
  int k;
  k = code[pc]; fn_n = code[pc + 1]; pc = pc + 2;
  fnop[k]();
}

int bad_call() { return vm_err("Illegal function call"); }

// numbers
void h_abs() {
  int v; unsigned u;
  if (fn_n == T_INT) { v = vs[sp]; if (v < 0) { u = 0; u = u - v; vs[sp] = u; } }
  else fs[sp] = fabs(fs[sp]);
}

void h_sgn() {
  int v; float f;
  if (fn_n == T_INT) { v = vs[sp]; vs[sp] = (v > 0) - (v < 0); }
  else { f = fs[sp]; vs[sp] = (f > 0) - (f < 0); }
}

void h_int() { fs[sp] = floor(fs[sp]); }

void h_fix() {
  float f;
  f = fs[sp];
  if (f < 0) fs[sp] = ceil(f); else fs[sp] = floor(f);
}

void h_sqr() {
  float f;
  f = fs[sp];
  if (f < 0) { bad_call(); return; }
  fs[sp] = sqrt(f);
}

void h_sin() { fs[sp] = sin(fs[sp]); }
void h_cos() { fs[sp] = cos(fs[sp]); }
void h_tan() { fs[sp] = tan(fs[sp]); fok(fs[sp]); }
void h_atn() { fs[sp] = atan(fs[sp]); }
void h_exp() { fs[sp] = exp(fs[sp]); fok(fs[sp]); }

void h_log() {
  float f;
  f = fs[sp];
  if (f <= 0) { bad_call(); return; }
  fs[sp] = log(f);
}

// random numbers
void rnd_seed(int b) { rnd_state = (b & 0xFFFFFF) ^ (((b >> 24) & 255) << 8); }

void rnd_step() {
  unsigned u;
  u = rnd_state;
  u = u * 16598013 + 12820163;
  rnd_state = u & 0xFFFFFF;
  rnd_last = rnd_state;
  rnd_last = rnd_last / 16777216;
}

// RND, RND(1) the next number, RND(0) the last one again, RND(-x) reseeds from x first
void h_rnd() {
  float x; int step;
  step = 1;
  if (fn_n == 1) {
    x = fs[sp]; sp--;
    if (x < 0) rnd_seed(fbits(x));
    else if (x == 0) step = 0;
  }
  if (step) rnd_step();
  sp++; fs[sp] = rnd_last;
}

void h_randomize() {
  if (fn_n == 1) { rnd_seed(fbits(fs[sp])); sp--; }
  else rnd_seed(millis());
}

// TIMER: seconds since the start, from millis() as unsigned (it passes 2^31 after 24.8
// days); whole seconds and milliseconds are split in integers so the float is s + ms / 1000
void h_timer() {
  unsigned u; int sec; int ms; float f; float g;
  u = millis();
  sec = u / 1000; ms = u % 1000;
  f = ms; f = f / 1000; g = sec; f = f + g;
  sp++; fs[sp] = f;
}

// arrays (the layout is in basic.h): dimension d's lower bound is a[1 + 2d], its length a[2 + 2d]
void bound(int up) {
  int d; int *a;
  d = vs[sp]; sp--;
  a = *(int **)vs[sp];
  if (!a || d < 1 || d > a[1]) { vm_err("Subscript out of range"); return; }
  vs[sp] = a[1 + 2 * d];
  if (up) vs[sp] = vs[sp] + a[2 + 2 * d] - 1;
}

void h_lbound() { bound(0); }
void h_ubound() { bound(1); }

// strings
void h_len() {
  char *s;
  s = (char *)vs[sp];
  vt[sp] = 0;
  vs[sp] = str_len(s);
  str_free(s);
}

void h_left() {
  char *s; int n; int l;
  n = vs[sp]; sp--; s = (char *)vs[sp];
  l = str_len(s);
  if (n < 0) { str_free(s); vs[sp] = 0; bad_call(); return; }
  if (n > l) n = l;
  vs[sp] = (int)str_from(s, n);
  str_free(s);
}

void h_right() {
  char *s; int n; int l;
  n = vs[sp]; sp--; s = (char *)vs[sp];
  l = str_len(s);
  if (n < 0) { str_free(s); vs[sp] = 0; bad_call(); return; }
  if (n > l) n = l;
  vs[sp] = (int)str_from(s + l - n, n);
  str_free(s);
}

// MID$(s, start [, length])
void h_mid() {
  char *s; int st; int n; int l;
  n = -1;
  if (fn_n == 3) { n = vs[sp]; sp--; }
  st = vs[sp]; sp--; s = (char *)vs[sp];
  l = str_len(s);
  if (st < 1 || (fn_n == 3 && n < 0)) { str_free(s); vs[sp] = 0; bad_call(); return; }
  if (st > l) n = 0;
  else if (n < 0 || n > l - st + 1) n = l - st + 1;
  vs[sp] = (int)str_from(s + st - 1, n);
  str_free(s);
}

// INSTR([start,] a, b): where b first occurs in a at or after start (from 1), else 0
void h_instr() {
  char *a; char *b; int st; int la; int lb; int i; int r;
  b = (char *)vs[sp]; a = (char *)vs[sp - 1]; vt[sp] = 0; vt[sp - 1] = 0; sp = sp - 2;
  st = 1;
  if (fn_n == 3) { st = vs[sp]; sp--; }
  la = str_len(a); lb = str_len(b);
  r = 0;
  if (st < 1) { str_free(a); str_free(b); sp++; vs[sp] = 0; bad_call(); return; }
  if (st <= la) {
    if (lb == 0) r = st;
    else {
      for (i = st - 1; i + lb <= la && !r; i++)
        if (memcmp(a + i, b, lb) == 0) r = i + 1;
    }
  }
  str_free(a); str_free(b);
  sp++; vs[sp] = r;
}

void h_ucase() {
  char *s; char *t; int i; int n; int c;
  s = (char *)vs[sp]; n = str_len(s);
  t = str_from(s, n);
  for (i = 0; i < n; i++) { c = t[i]; if (c >= 'a' && c <= 'z') t[i] = c - 32; }
  str_free(s);
  vs[sp] = (int)t;
}

void h_lcase() {
  char *s; char *t; int i; int n; int c;
  s = (char *)vs[sp]; n = str_len(s);
  t = str_from(s, n);
  for (i = 0; i < n; i++) { c = t[i]; if (c >= 'A' && c <= 'Z') t[i] = c + 32; }
  str_free(s);
  vs[sp] = (int)t;
}

void h_ltrim() {
  char *s; int i; int n;
  s = (char *)vs[sp]; n = str_len(s);
  i = 0;
  while (i < n && s[i] == ' ') i++;
  vs[sp] = (int)str_from(s + i, n - i);
  str_free(s);
}

void h_rtrim() {
  char *s; int n;
  s = (char *)vs[sp]; n = str_len(s);
  while (n > 0 && s[n - 1] == ' ') n--;
  vs[sp] = (int)str_from(s, n);
  str_free(s);
}

// STR$(n): the number as PRINT shows it, with a space in front of a positive one
void h_strs() {
  char b[32]; char *t; int n;
  if (fn_n == T_INT) n = fmt_int(b, vs[sp]); else n = fmt_sng(b, fs[sp]);
  if (b[0] == '-') t = str_from(b, n);
  else { t = str_new(n + 1); t[0] = ' '; memcpy(t + 1, b, n); }
  vs[sp] = (int)t; vt[sp] = 1;
}

// VAL(s): the number at the start of s (blanks, a sign, then a literal as in the source,
// &H and &O included); 0 if there is none
void h_val() {
  char *s; int n; int i; int neg; int j; int type; int val; float f;
  s = (char *)vs[sp]; n = str_len(s);
  vt[sp] = 0;
  i = 0;
  while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
  neg = 0;
  if (i < n && (s[i] == '-' || s[i] == '+')) { neg = s[i] == '-'; i++; }
  j = num_scan(s + i, n - i, &type, &val);
  f = 0;
  if (j > 0) {
    if (type == T_NONE) { str_free(s); vs[sp] = 0; vm_err("Overflow"); return; }
    if (type == T_INT) f = val; else f = bitsf(val);
  }
  if (neg) f = -f;
  str_free(s);
  fs[sp] = f;
}

void h_chr() {
  int c; char *s;
  c = vs[sp];
  if (c < 0 || c > 255) { bad_call(); return; }
  s = str_new(1); s[0] = c;
  vs[sp] = (int)s; vt[sp] = 1;
}

void h_asc() {
  char *s; int c;
  s = (char *)vs[sp];
  vt[sp] = 0;
  if (str_len(s) == 0) { vs[sp] = 0; bad_call(); return; }
  c = s[0] & 255;
  str_free(s);
  vs[sp] = c;
}

// n copies of the character code c
void repeat(int n, int c) {
  char *s;
  if (n < 0 || n > STR_MAX || c < 0 || c > 255) { bad_call(); return; }
  s = str_new(n);
  if (n > 0) memset(s, c, n);
  vs[sp] = (int)s; vt[sp] = 1;
}

void h_strings() { int c; c = vs[sp]; sp--; repeat(vs[sp], c); }
void h_spaces() { repeat(vs[sp], 32); }

// HEX$ / OCT$: the 32-bit pattern, no padding
void digits(int base) {
  char b[16]; unsigned u; int k; int d;
  u = vs[sp]; k = 16;
  while (1) {
    d = u % base; u = u / base;
    k--;
    if (d < 10) b[k] = '0' + d; else b[k] = 'A' + d - 10;
    if (!u) break;
  }
  vs[sp] = (int)str_from(b + k, 16 - k); vt[sp] = 1;
}

void h_hex() { digits(16); }
void h_oct() { digits(8); }

// ---------------------------------------------------------------- registration
void funcs_init() {
  int k;
  def(K_ABS, "n", 1, 0, h_abs);
  def(K_SGN, "n", 1, T_INT, h_sgn);
  def(K_INT, "n", 1, 0, h_int);
  def(K_FIX, "n", 1, 0, h_fix);
  def(K_SQR, "f", 1, T_SNG, h_sqr);
  def(K_SIN, "f", 1, T_SNG, h_sin);
  def(K_COS, "f", 1, T_SNG, h_cos);
  def(K_TAN, "f", 1, T_SNG, h_tan);
  def(K_ATN, "f", 1, T_SNG, h_atn);
  def(K_EXP, "f", 1, T_SNG, h_exp);
  def(K_LOG, "f", 1, T_SNG, h_log);
  def(K_CINT, "i", 1, T_INT, 0);
  def(K_CLNG, "i", 1, T_INT, 0);
  def(K_CSNG, "f", 1, T_SNG, 0);
  def(K_CDBL, "f", 1, T_SNG, 0);
  def(K_LEN, "s", 1, T_INT, h_len);
  def(K_LEFTS, "si", 2, T_STR, h_left);
  def(K_RIGHTS, "si", 2, T_STR, h_right);
  def(K_MIDS, "sii", 2, T_STR, h_mid);
  def(K_UCASES, "s", 1, T_STR, h_ucase);
  def(K_LCASES, "s", 1, T_STR, h_lcase);
  def(K_LTRIMS, "s", 1, T_STR, h_ltrim);
  def(K_RTRIMS, "s", 1, T_STR, h_rtrim);
  def(K_STRS, "n", 1, T_STR, h_strs);
  def(K_VAL, "s", 1, T_SNG, h_val);
  def(K_CHRS, "i", 1, T_STR, h_chr);
  def(K_ASC, "s", 1, T_INT, h_asc);
  def(K_SPACES, "i", 1, T_STR, h_spaces);
  def(K_HEXS, "i", 1, T_STR, h_hex);
  def(K_OCTS, "i", 1, T_STR, h_oct);
  for (k = K_ABS; k <= K_OCTS; k++) fn_tab[k] = fn_compile;
  // the ones with their own compiler
  fn_tab[K_TIMER] = fn_timer; fnop[K_TIMER] = h_timer;
  fn_tab[K_RND] = fn_rnd; fnop[K_RND] = h_rnd;
  fn_tab[K_INSTR] = fn_instr; fnop[K_INSTR] = h_instr;
  fn_tab[K_STRINGS] = fn_strings; fnop[K_STRINGS] = h_strings;
  fn_tab[K_LBOUND] = fn_bound; fnop[K_LBOUND] = h_lbound;
  fn_tab[K_UBOUND] = fn_bound; fnop[K_UBOUND] = h_ubound;
  fnop[K_RANDOMIZE] = h_randomize;
  stmt_tab[K_RANDOMIZE] = st_randomize;
  optab[OP_FN] = op_fn;
  rnd_state = 0x50000; rnd_last = 0;
}
