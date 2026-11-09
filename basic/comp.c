// comp.c - tokens to bytecode (basic.h describes the opcodes). One pass over the token
// list: every statement is compiled by the function stmt_tab[] holds for its first token,
// expressions by expr() with static types, so everything QBasic checks before the run
// (types, constants, syntax) is reported here, with the line, before anything runs.
#include "basic.h"

#define TK (toks[tp].tk)

int *code;
int ncode;
int capcode;
int tp;
void (*stmt_tab[NTOK])();
int (*fn_tab[NTOK])();
struct Sym *syms;
int nsym;
int capsym;
int symhash[256];
int nslot[4];
char defsuf[26];                 // DEFtype: the suffix a name without one gets, by first letter
int const_mode;                  // compiling a CONST expression: no variables
int depth;                       // expression nesting, against runaway recursion
int *lm_pos;                     // line map: code position, source line
int *lm_line;
int nlm;
int caplm;

int *grow(int *p, int n, int cap) {
  int *q;
  q = malloc(cap * 4);
  if (p) { memcpy(q, p, n * 4); free(p); }
  return q;
}

// ---------------------------------------------------------------- tokens and errors
int next() { if (tp < ntok - 1) tp++; return TK; }
int accept(int tk) { if (TK == tk) { next(); return 1; } return 0; }
int cerr(char *msg) { return err_line(toks[tp].line, msg); }

int bad_token() {
  if (TK == TK_ERR) return cerr((char *)toks[tp].val);
  if (TK == K_UNSUP) return cerr("Advanced feature unavailable");
  return cerr("Syntax error");
}

int need(int tk) { if (TK == tk) { next(); return 1; } bad_token(); return 0; }
int at_end() { return TK == TK_EOL || TK == TK_COLON || TK == TK_EOF || TK == K_ELSE; }

// ---------------------------------------------------------------- code and line map
int emit(int op) {
  if (ncode >= capcode) { capcode = capcode * 2 + 1024; code = grow(code, ncode, capcode); }
  code[ncode] = op; ncode++;
  return ncode - 1;
}

int emit_arg(int v) { return emit(v); }

void line_mark(int line) {
  if (nlm > 0 && lm_pos[nlm - 1] == ncode) { lm_line[nlm - 1] = line; return; }
  if (nlm > 0 && lm_line[nlm - 1] == line) return;
  if (nlm >= caplm) {
    caplm = caplm * 2 + 256;
    lm_pos = grow(lm_pos, nlm, caplm); lm_line = grow(lm_line, nlm, caplm);
  }
  lm_pos[nlm] = ncode; lm_line[nlm] = line; nlm++;
}

int line_of(int pos) {
  int i;
  if (nlm == 0) return 0;
  i = nlm - 1;
  while (i > 0 && lm_pos[i] > pos) i--;
  return lm_line[i];
}

// drop code from pos on (a CONST's expression once it has been evaluated)
void code_rewind(int pos) {
  ncode = pos;
  while (nlm > 0 && lm_pos[nlm - 1] > pos) nlm--;
}

// ---------------------------------------------------------------- symbols
int suf_type(int suf) {
  if (suf == '%' || suf == '&') return T_INT;
  if (suf == '$') return T_STR;
  return T_SNG;
}

void upname(char *dst, char *src) {
  int c;
  while (*src) { c = *src; if (c >= 'a' && c <= 'z') c = c - 32; *dst = c; dst++; src++; }
  *dst = 0;
}

int sym_hash(char *u) {
  int h;
  h = 0;
  while (*u) { h = h * 31 + *u; u++; }
  return h & 255;
}

int sym_find(char *uname, int suf, int kind) {
  int i;
  for (i = symhash[sym_hash(uname)]; i >= 0; i = syms[i].link)
    if (syms[i].kind == kind && syms[i].suf == suf && !strcmp(syms[i].uname, uname)) return i;
  return -1;
}

int sym_add(char *uname, int suf, int kind, int type) {
  struct Sym *n; int h;
  if (nsym >= capsym) {
    capsym = capsym * 2 + 64;
    n = malloc(capsym * sizeof(struct Sym));
    if (syms) { memcpy(n, syms, nsym * sizeof(struct Sym)); free(syms); }
    syms = n;
  }
  n = &syms[nsym];
  n->uname = malloc(strlen(uname) + 1); strcpy(n->uname, uname);
  n->suf = suf; n->kind = kind; n->type = type; n->slot = 0; n->val = 0; n->scope = 0;
  h = sym_hash(uname);
  n->link = symhash[h]; symhash[h] = nsym;
  nsym++;
  return nsym - 1;
}

// a variable of this name with any suffix?
int var_named(char *u) {
  return sym_find(u, '%', S_VAR) >= 0 || sym_find(u, '&', S_VAR) >= 0 || sym_find(u, '!', S_VAR) >= 0 ||
         sym_find(u, '#', S_VAR) >= 0 || sym_find(u, '$', S_VAR) >= 0;
}

// The constant or variable the TK_ID at tp names (consumed); a variable is created on first
// use, in the next free slot of its type. Constants answer to their name with any suffix.
int name_ref() {
  char u[48]; int suf; int s; int t;
  upname(u, (char *)toks[tp].val); suf = toks[tp].suf;
  next();
  s = sym_find(u, 0, S_CONST);
  if (s >= 0) return s;
  if (!suf) suf = defsuf[u[0] - 'A'];
  s = sym_find(u, suf, S_VAR);
  if (s < 0) {
    t = suf_type(suf);
    s = sym_add(u, suf, S_VAR, t);
    syms[s].slot = nslot[t]; nslot[t]++;
  }
  return s;
}

void emit_load(int s) {
  int t;
  t = syms[s].type;
  if (syms[s].kind == S_CONST) {
    if (t == T_INT) emit(OP_PUSHI); else if (t == T_SNG) emit(OP_PUSHF); else emit(OP_PUSHS);
    emit_arg(syms[s].val);
    return;
  }
  emit(OP_LDI + t - T_INT); emit_arg(syms[s].slot);      // OP_LDI, OP_LDF, OP_LDS
}

void emit_store(int s) { emit(OP_STI + syms[s].type - T_INT); emit_arg(syms[s].slot); }
void emit_ref(int s) { emit(OP_REFI + syms[s].type - T_INT); emit_arg(syms[s].slot); }

// ---------------------------------------------------------------- expressions
// Precedence, high to low: ^; unary -; * /; \; MOD; + -; comparisons; NOT; AND; OR; XOR;
// EQV; IMP. Each level returns its static type and emits conversions where types meet.
int mismatch() { cerr("Type mismatch"); return T_INT; }

int to_float(int t) {
  if (t == T_INT) emit(OP_I2F);
  else if (t == T_STR) mismatch();
  return T_SNG;
}

int to_int(int t) {
  if (t == T_SNG) emit(OP_F2I);
  else if (t == T_STR) mismatch();
  return T_INT;
}

int convert(int t, int want) {
  if (want == T_STR || t == T_STR) { if (t != want) mismatch(); return want; }
  if (want == T_INT) return to_int(t);
  return to_float(t);
}

int expr_to(int type) { return convert(expr(), type); }

// both numeric: int op if both are ints, else float op (converting the int side)
int arith(int l, int r, int opi, int opf) {
  if (l == T_INT && r == T_INT) { emit(opi); return T_INT; }
  if (l == T_INT) emit(OP_I2F2);
  if (r == T_INT) emit(OP_I2F);
  emit(opf);
  return T_SNG;
}

// negate what was compiled from start on: a literal in place, else an op
int negate(int start, int t) {
  if (t == T_STR) return mismatch();
  if (ncode == start + 2 && code[start] == OP_PUSHI) { code[start + 1] = 0 - code[start + 1]; return t; }
  if (ncode == start + 2 && code[start] == OP_PUSHF) { code[start + 1] = code[start + 1] ^ 0x80000000; return t; }
  if (t == T_INT) emit(OP_NEGI); else emit(OP_NEGF);
  return t;
}

int primary() {
  int t; int s; char u[48];
  t = TK;
  if (t == TK_INT) { emit(OP_PUSHI); emit_arg(toks[tp].val); next(); return T_INT; }
  if (t == TK_SNG) { emit(OP_PUSHF); emit_arg(toks[tp].val); next(); return T_SNG; }
  if (t == TK_STR) {
    if (toks[tp].val) { emit(OP_PUSHS); emit_arg(toks[tp].val); }
    else { emit(OP_PUSHI); emit_arg(0); }               // "" is the null str
    next();
    return T_STR;
  }
  if (t == TK_LPAR) { next(); t = expr(); need(TK_RPAR); return t; }
  if (t == TK_ID) {
    // Task 5: a name followed by ( is an array element or a FUNCTION call
    if (toks[tp + 1].tk == TK_LPAR) { bad_token(); return T_INT; }
    if (const_mode) {
      upname(u, (char *)toks[tp].val);
      if (sym_find(u, 0, S_CONST) < 0) { cerr("Invalid constant"); return T_INT; }
    }
    s = name_ref();
    emit_load(s);
    return syms[s].type;
  }
  if (fn_tab[t]) return fn_tab[t]();                      // built-in functions (Task 4 on)
  bad_token();
  return T_INT;
}

int pow_operand() {
  int start; int t;
  if (TK == TK_MINUS) { next(); start = ncode; t = pow_operand(); return negate(start, t); }
  if (TK == TK_PLUS) { next(); return pow_operand(); }
  return primary();
}

int e_pow() {
  int l; int r;
  l = primary();
  while (TK == TK_POW) {
    to_float(l); next();
    r = pow_operand();
    if (r == T_INT) emit(OP_POWI); else { to_float(r); emit(OP_POWF); }
    l = T_SNG;
  }
  return l;
}

int e_neg() {
  int start; int t;
  if (TK == TK_MINUS || TK == TK_PLUS) {
    t = TK; next();
    depth++;
    if (depth > 100) { cerr("Expression too complex"); return T_INT; }
    start = ncode;
    if (t == TK_MINUS) t = negate(start, e_neg());
    else { t = e_neg(); if (t == T_STR) mismatch(); }
    depth--;
    return t;
  }
  return e_pow();
}

int e_mul() {
  int l; int r; int t;
  l = e_neg();
  while (TK == TK_MUL || TK == TK_DIV) {
    t = TK; next();
    if (t == TK_DIV) { to_float(l); to_float(e_neg()); emit(OP_DIVF); l = T_SNG; }
    else {
      r = e_neg();
      if (l == T_STR || r == T_STR) l = mismatch(); else l = arith(l, r, OP_MULI, OP_MULF);
    }
  }
  return l;
}

int e_idiv() {
  int l;
  l = e_mul();
  while (TK == TK_IDIV) { to_int(l); next(); to_int(e_mul()); emit(OP_IDIV); l = T_INT; }
  return l;
}

int e_mod() {
  int l;
  l = e_idiv();
  while (TK == K_MOD) { to_int(l); next(); to_int(e_idiv()); emit(OP_MODI); l = T_INT; }
  return l;
}

int e_add() {
  int l; int r; int t;
  l = e_mod();
  while (TK == TK_PLUS || TK == TK_MINUS) {
    t = TK; next();
    r = e_mod();
    if (l == T_STR || r == T_STR) {
      if (l != r || t == TK_MINUS) l = mismatch(); else emit(OP_CAT);
    }
    else if (t == TK_PLUS) l = arith(l, r, OP_ADDI, OP_ADDF);
    else l = arith(l, r, OP_SUBI, OP_SUBF);
  }
  return l;
}

int e_cmp() {
  int l; int r; int t;
  l = e_add();
  while (TK >= TK_EQ && TK <= TK_GE) {
    t = TK; next();
    r = e_add();
    if (l == T_STR || r == T_STR) {
      if (l != r) mismatch();
      else { emit(OP_CMPS); emit_arg(t); }
    }
    else if (l == T_INT && r == T_INT) emit(OP_EQI + t - TK_EQ);
    else {
      if (l == T_INT) emit(OP_I2F2);
      if (r == T_INT) emit(OP_I2F);
      emit(OP_EQF + t - TK_EQ);
    }
    l = T_INT;
  }
  return l;
}

int e_not() {
  int t;
  if (TK == K_NOT) {
    next();
    depth++;
    if (depth > 100) { cerr("Expression too complex"); return T_INT; }
    t = e_not(); to_int(t); emit(OP_NOT);
    depth--;
    return T_INT;
  }
  return e_cmp();
}

// a level of the bitwise operators: sub (op sub)*, operands as integers
int logic_level(int tk, int op, int (*sub)()) {
  int t;
  t = sub();
  while (TK == tk) { to_int(t); next(); to_int(sub()); emit(op); t = T_INT; }
  return t;
}

int e_and() { return logic_level(K_AND, OP_AND, e_not); }
int e_or() { return logic_level(K_OR, OP_OR, e_and); }
int e_xor() { return logic_level(K_XOR, OP_XOR, e_or); }
int e_eqv() { return logic_level(K_EQV, OP_EQV, e_xor); }

int expr() {
  int t;
  depth++;
  if (depth > 100) { cerr("Expression too complex"); return T_INT; }
  t = logic_level(K_IMP, OP_IMP, e_eqv);
  depth--;
  return t;
}

// ---------------------------------------------------------------- statements
// PRINT [items]: items separated by ';' (nothing between), ',' (next 14-column zone) or
// nothing; no newline after a final ';' or ','. `?` is PRINT.
void st_print() {
  int sep; int t;
  next();
  sep = 0;
  while (!at_end()) {
    if (TK == TK_SEMI) { next(); sep = 1; continue; }
    if (TK == TK_COMMA) { next(); emit(OP_PRCOMMA); sep = 1; continue; }
    t = expr();
    emit(OP_PRINTI + t - T_INT);                         // OP_PRINTI, OP_PRINTF, OP_PRINTS
    sep = 0;
  }
  if (!sep) emit(OP_PRNL);
}

// name = expression (also after LET)
void st_assign() {
  int s;
  if (TK != TK_ID) { bad_token(); return; }
  if (toks[tp + 1].tk == TK_LPAR) { next(); bad_token(); return; }   // Task 5: arrays
  s = name_ref();
  if (syms[s].kind == S_CONST) { cerr("Duplicate definition"); return; }
  if (!need(TK_EQ)) return;
  expr_to(syms[s].type);
  emit_store(s);
}

void st_let() { next(); st_assign(); }

// a variable's address for SWAP; returns its type
int ref_var() {
  int s;
  if (TK != TK_ID || toks[tp + 1].tk == TK_LPAR) { bad_token(); return T_NONE; }
  s = name_ref();
  if (syms[s].kind == S_CONST) { cerr("Duplicate definition"); return T_NONE; }
  emit_ref(s);
  return syms[s].type;
}

void st_swap() {
  int a; int b;
  next();
  a = ref_var();
  if (!need(TK_COMMA)) return;
  b = ref_var();
  if (a != b) { mismatch(); return; }
  emit(OP_SWAP);
}

// CONST name = expr [, name = expr]: evaluated now (the VM runs the expression), so a
// constant is a literal wherever it is used. Its type is the suffix's, else the value's.
void st_const() {
  char u[48]; int suf; int start; int t; int s;
  next();
  while (1) {
    if (TK != TK_ID) { bad_token(); return; }
    upname(u, (char *)toks[tp].val); suf = toks[tp].suf;
    if (sym_find(u, 0, S_CONST) >= 0 || var_named(u)) { cerr("Duplicate definition"); return; }
    next();
    if (!need(TK_EQ)) return;
    start = ncode;
    const_mode = 1; t = expr(); const_mode = 0;
    if (suf) t = convert(t, suf_type(suf));
    emit(OP_HALT);
    if (nerr) return;
    vm_run(start);
    code_rewind(start);
    if (nerr) return;
    s = sym_add(u, 0, S_CONST, t);
    syms[s].val = vs[sp]; sp--;
    if (!accept(TK_COMMA)) return;
  }
}

// one letter of a DEFtype range, 0 to 25, or -1 after an error
int def_letter() {
  char *n; int c;
  if (TK != TK_ID) { bad_token(); return -1; }
  n = (char *)toks[tp].val;
  if (n[1] || toks[tp].suf) { bad_token(); return -1; }
  c = n[0];
  if (c >= 'a') c = c - 32;
  next();
  return c - 'A';
}

// DEFINT/DEFLNG/DEFSNG/DEFDBL/DEFSTR a[-b] [, ...]: the type of names written without a
// suffix, from here on in the source
void st_deftype() {
  int suf; int a; int b; int i; int t;
  t = TK;
  suf = '$';
  if (t == K_DEFINT) suf = '%'; else if (t == K_DEFLNG) suf = '&';
  else if (t == K_DEFSNG) suf = '!'; else if (t == K_DEFDBL) suf = '#';
  next();
  while (1) {
    a = def_letter(); if (a < 0) return;
    b = a;
    if (accept(TK_MINUS)) { b = def_letter(); if (b < 0) return; }
    for (i = a; i <= b; i++) defsuf[i] = suf;
    if (!accept(TK_COMMA)) return;
  }
}

// END (Task 3 adds END IF / END SELECT, Task 5 END SUB / END FUNCTION)
void st_end() {
  next();
  if (!at_end()) { bad_token(); return; }
  emit(OP_END);
}

// QBasic statements outside this BASIC that start with one of its words
void st_unsup() { cerr("Advanced feature unavailable"); }

// LINE (x1, y1)-(x2, y2) is graphics; LINE INPUT comes with Task 6
void st_line() {
  if (toks[tp + 1].tk != K_INPUT) { st_unsup(); return; }
  bad_token();
}

void statement() {
  int t;
  line_mark(toks[tp].line);
  t = TK;
  // Task 3: a label (name and ':' first on a line); Task 5: `name args` calls a SUB
  if (t == TK_ID) { st_assign(); return; }
  if (stmt_tab[t]) { stmt_tab[t](); return; }
  bad_token();
}

int compile() {
  tp = 0; depth = 0; const_mode = 0;
  while (TK != TK_EOF) {
    if (TK == TK_EOL || TK == TK_COLON) { next(); continue; }
    statement();
    if (TK == TK_EOL || TK == TK_COLON) next();
    else if (TK != TK_EOF) bad_token();
  }
  emit(OP_END);
  return nerr == 0;
}

int comp_init() {
  int i;
  for (i = 0; i < 256; i++) symhash[i] = -1;
  for (i = 0; i < 26; i++) defsuf[i] = '!';
  for (i = 0; i < 4; i++) nslot[i] = 0;
  stmt_tab[K_PRINT] = st_print;
  stmt_tab[K_LET] = st_let;
  stmt_tab[K_SWAP] = st_swap;
  stmt_tab[K_CONST] = st_const;
  stmt_tab[K_DEFINT] = st_deftype;
  stmt_tab[K_DEFLNG] = st_deftype;
  stmt_tab[K_DEFSNG] = st_deftype;
  stmt_tab[K_DEFDBL] = st_deftype;
  stmt_tab[K_DEFSTR] = st_deftype;
  stmt_tab[K_END] = st_end;
  stmt_tab[K_LINE] = st_line;
  stmt_tab[K_MIDS] = st_unsup;          // the MID$ statement form
  return 0;
}

void comp_free() {
  int i;
  for (i = 0; i < nsym; i++) {
    if (syms[i].kind == S_CONST && syms[i].type == T_STR) str_free((char *)syms[i].val);
    free(syms[i].uname);
  }
  if (syms) free(syms);
  if (code) free(code);
  if (lm_pos) { free(lm_pos); free(lm_line); }
  syms = 0; nsym = 0; capsym = 0; code = 0; ncode = 0; capcode = 0;
  lm_pos = 0; lm_line = 0; nlm = 0; caplm = 0;
}
