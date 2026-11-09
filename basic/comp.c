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

// labels: name (capitals), scope (0 is the main program), code position, DATA index
int *lb_name;
int *lb_scope;
int *lb_pos;
int *lb_data;
int nlb;
int caplb;
// references to labels (GOTO, GOSUB, IF c THEN label, RESTORE), patched when all are known
int *fx_name;
int *fx_scope;
int *fx_pos;
int *fx_line;
int *fx_kind;                    // 0: patch code[pos] with the label's position, 1: its DATA index
int nfx;
int capfx;
int cur_scope;                   // 0 in the main program, p + 1 inside procedure p

// procedures (procs[] and nproc live in vm.c): parameters of procedure p are pa_*[pfirst ..
// pfirst + np): type and 1 for an array parameter
int *pa_type;
int *pa_arr;
int npa;
int cappa;
int capproc;
int cur_proc;                    // the procedure being compiled, -1 in the main program
int cur_ncell;                   // its frame cells after the arguments so far
int proc_skip;                   // the jump over its body
int scanning;                    // the pass before compiling: errors are not reported
int scan_fail;

// DATA items
char **d_item;
int *d_quoted;
int ndata;
int capdata;

// open blocks: kind, line of the opening statement, and per kind (see the statements)
enum { B_IF = 1, B_FOR, B_WHILE, B_DO, B_SELECT };
struct Blk { int kind; int line; int a; int b; int c; int d; int e; };
struct Blk *blk;
int nblk;

int e_not();
void statement();
int elem_ref();
void call_args(int p, int paren);
int proc_type(int p, int suf);
void end_proc(int isfn);

int *grow(int *p, int n, int cap) {
  int *q;
  q = malloc(cap * 4);
  if (p) { memcpy(q, p, n * 4); free(p); }
  return q;
}

// ---------------------------------------------------------------- tokens and errors
int next() { if (tp < ntok - 1) tp++; return TK; }
int accept(int tk) { if (TK == tk) { next(); return 1; } return 0; }
int cerr(char *msg) {
  if (scanning) { scan_fail = 1; return 0; }
  return err_line(toks[tp].line, msg);
}

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

// a symbol of this name, suffix and kind in scope; suf -1: the one declared with AS
int sym_in(char *uname, int suf, int kind, int scope) {
  int i;
  for (i = symhash[sym_hash(uname)]; i >= 0; i = syms[i].link)
    if (syms[i].kind == kind && syms[i].scope == scope && !strcmp(syms[i].uname, uname) &&
        ((suf < 0 && (syms[i].flags & F_AS)) || syms[i].suf == suf)) return i;
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
  n->suf = suf; n->kind = kind; n->type = type; n->slot = 0; n->val = 0; n->scope = cur_scope;
  n->cls = C_GLOBAL; n->flags = 0; n->dims = 0;
  h = sym_hash(uname);
  n->link = symhash[h]; symhash[h] = nsym;
  nsym++;
  return nsym - 1;
}

// a variable of this name with any suffix in scope?
int var_named(char *u, int scope) {
  return sym_in(u, '%', S_VAR, scope) >= 0 || sym_in(u, '&', S_VAR, scope) >= 0 ||
         sym_in(u, '!', S_VAR, scope) >= 0 || sym_in(u, '#', S_VAR, scope) >= 0 ||
         sym_in(u, '$', S_VAR, scope) >= 0;
}

// constants: the procedure's own, then the main program's
int const_find(char *u) {
  int s;
  s = sym_in(u, 0, S_CONST, cur_scope);
  if (s < 0 && cur_scope) s = sym_in(u, 0, S_CONST, 0);
  return s;
}

int proc_find(char *u) {
  int i;
  for (i = 0; i < nproc; i++) if (!strcmp(procs[i].uname, u)) return i;
  return -1;
}

// The variable (kind S_VAR) or array (S_ARR) a name written with suffix suf (0: none) means
// here, or -1: one declared with AS answers to the bare name; then the name with its suffix
// (from DEFtype if none) in the current scope; inside a procedure then a SHARED one
int lookup(char *u, int suf, int kind) {
  int s;
  if (!suf) {
    s = sym_in(u, -1, kind, cur_scope);
    if (s >= 0) return s;
    if (cur_scope) {
      s = sym_in(u, -1, kind, 0);
      if (s >= 0 && (syms[s].flags & F_SHARED)) return s;
    }
    suf = defsuf[u[0] - 'A'];
  }
  s = sym_in(u, suf, kind, cur_scope);
  if (s >= 0) return s;
  if (cur_scope) {
    s = sym_in(u, suf, kind, 0);
    if (s >= 0 && (syms[s].flags & F_SHARED)) return s;
  }
  return -1;
}

// a new variable or array in the current scope: a slot of the main program, or a cell of
// the procedure's frame
int var_new(char *u, int suf, int kind, int type) {
  int s; int k;
  s = sym_add(u, suf, kind, type);
  if (cur_scope) {
    syms[s].cls = C_LOCAL;
    syms[s].slot = 2 * procs[cur_proc].np + cur_ncell + 1;
    cur_ncell++;
  }
  else {
    k = type;
    if (kind == S_ARR) k = 0;
    syms[s].slot = nslot[k]; nslot[k]++;
  }
  return s;
}

// The constant or variable the TK_ID at tp names (consumed); a variable is created on first
// use. Constants answer to their name with any suffix.
int name_ref() {
  char u[48]; int suf; int s;
  upname(u, (char *)toks[tp].val); suf = toks[tp].suf;
  next();
  s = const_find(u);
  if (s >= 0) return s;
  s = lookup(u, suf, S_VAR);
  if (s >= 0) return s;
  if (!suf) suf = defsuf[u[0] - 'A'];
  return var_new(u, suf, S_VAR, suf_type(suf));
}

void emit_load(int s) {
  int t; int c;
  t = syms[s].type; c = syms[s].cls;
  if (syms[s].kind == S_CONST) {
    if (t == T_INT) emit(OP_PUSHI); else if (t == T_SNG) emit(OP_PUSHF); else emit(OP_PUSHS);
    emit_arg(syms[s].val);
    return;
  }
  if (c == C_GLOBAL) emit(OP_LDI + t - T_INT);           // OP_LDI, OP_LDF, OP_LDS
  else if (c == C_LOCAL) { if (t == T_STR) emit(OP_LLDS); else emit(OP_LLD); }
  else { if (t == T_STR) emit(OP_PLDS); else emit(OP_PLD); }
  emit_arg(syms[s].slot);
}

void emit_store(int s) {
  int t; int c;
  t = syms[s].type; c = syms[s].cls;
  if (c == C_GLOBAL) emit(OP_STI + t - T_INT);
  else if (c == C_LOCAL) { if (t == T_STR) emit(OP_LSTS); else emit(OP_LST); }
  else { if (t == T_STR) emit(OP_PSTS); else emit(OP_PST); }
  emit_arg(syms[s].slot);
}

// push the address of a variable's cell (an array's: the cell holding the array)
void emit_ref(int s) {
  int c;
  c = syms[s].cls;
  if (c == C_GLOBAL) { if (syms[s].kind == S_ARR) emit(OP_REFA); else emit(OP_REFI + syms[s].type - T_INT); }
  else if (c == C_LOCAL) emit(OP_LREF);
  else emit(OP_LLD);
  emit_arg(syms[s].slot);
}

// a hidden variable (SELECT CASE's value): no name, so no lookup finds it
int hidden_var(int type) { return var_new("", 0, S_VAR, type); }

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
  if (t == K_NOT) {                                       // 1 + NOT 0: NOT as an operand
    next();
    depth++;
    if (depth > 100) { cerr("Expression too complex"); return T_INT; }
    t = e_not(); to_int(t); emit(OP_NOT);
    depth--;
    return T_INT;
  }
  if (t == TK_ID) {
    upname(u, (char *)toks[tp].val);
    if (const_mode && const_find(u) < 0) { cerr("Invalid constant"); return T_INT; }
    s = proc_find(u);
    if (s >= 0 && procs[s].isfn) {                        // a FUNCTION call
      t = proc_type(s, toks[tp].suf);
      next();
      call_args(s, 1);
      return t;
    }
    if (toks[tp + 1].tk == TK_LPAR) {                     // an array element
      if (s >= 0) { cerr("Array not defined"); return T_INT; }
      t = elem_ref();
      if (t == T_STR) emit(OP_LDAS); else emit(OP_LDA);
      return t;
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
  if (TK == TK_MINUS || TK == TK_PLUS) {
    t = TK; next();
    depth++;
    if (depth > 100) { cerr("Expression too complex"); return T_INT; }
    start = ncode;
    if (t == TK_MINUS) t = negate(start, pow_operand()); else t = pow_operand();
    depth--;
    return t;
  }
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

// compare the two values on the stack (types l, r) with TK_EQ .. TK_GE; leaves -1 or 0
void emit_cmp(int l, int r, int t) {
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
}

int e_cmp() {
  int l; int r; int t;
  l = e_add();
  while (TK >= TK_EQ && TK <= TK_GE) {
    t = TK; next();
    r = e_add();
    emit_cmp(l, r, t);
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
  int s; int t; char u[48];
  if (TK != TK_ID) { bad_token(); return; }
  upname(u, (char *)toks[tp].val);
  s = proc_find(u);
  if (s >= 0 && procs[s].isfn) {                          // a FUNCTION's result, in its body
    if (toks[tp + 1].tk == TK_LPAR) { cerr("Array not defined"); return; }
    if (s != cur_proc) { cerr("Duplicate definition"); return; }
    t = proc_type(s, toks[tp].suf);
    next();
    if (!need(TK_EQ)) return;
    expr_to(t);
    if (t == T_STR) emit(OP_LSTS); else emit(OP_LST);
    emit_arg(2 * procs[s].np + 1);
    return;
  }
  if (toks[tp + 1].tk == TK_LPAR) {                       // an array element
    if (s >= 0) { cerr("Array not defined"); return; }
    t = elem_ref();
    if (!need(TK_EQ)) return;
    expr_to(t);
    if (t == T_STR) emit(OP_STAS); else emit(OP_STA);
    return;
  }
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
  if (TK != TK_ID) { bad_token(); return T_NONE; }
  if (toks[tp + 1].tk == TK_LPAR) return elem_ref();
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
    if (sym_in(u, 0, S_CONST, cur_scope) >= 0 || var_named(u, cur_scope)) { cerr("Duplicate definition"); return; }
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
    syms[s].val = vs[sp]; vt[sp] = 0; sp--;
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

// ---------------------------------------------------------------- jumps, blocks, labels
// A jump whose target is not known yet: its argument holds the previous such jump of the same
// chain (-1 ends it); jump() returns the new head, patch_chain() sets every target.
int jump(int op, int head) {
  emit(op);
  return emit_arg(head);
}

void patch_chain(int head, int target) {
  int nx;
  while (head >= 0) { nx = code[head]; code[head] = target; head = nx; }
}

// a condition: leaves -1 or 0 on the stack (a number is true when it is not 0)
void cond() {
  int t;
  t = expr();
  if (t == T_STR) mismatch();
  else if (t == T_SNG) { emit(OP_PUSHF); emit_arg(0); emit(OP_NEF); }
}

struct Blk *blk_push(int kind) {
  struct Blk *b;
  if (nblk >= 256) { cerr("Out of memory"); return 0; }
  b = &blk[nblk]; nblk++;
  b->kind = kind; b->line = toks[tp].line;
  b->a = -1; b->b = -1; b->c = 0; b->d = 0; b->e = 0;
  return b;
}

char *unclosed(int kind) {
  if (kind == B_IF) return "Block IF without END IF";
  if (kind == B_FOR) return "FOR without NEXT";
  if (kind == B_WHILE) return "WHILE without WEND";
  if (kind == B_DO) return "DO without LOOP";
  return "SELECT without END SELECT";
}

// the innermost open block must be of this kind, else the error: msg (the closing word without
// its opener) when no block is open, else the open block is the one that was never closed
int blk_expect(int kind, char *msg) {
  if (nblk > 0 && blk[nblk - 1].kind == kind) return 1;
  if (nblk == 0) cerr(msg); else err_line(blk[nblk - 1].line, unclosed(blk[nblk - 1].kind));
  return 0;
}

char *dupstr(char *s) {
  char *d;
  d = malloc(strlen(s) + 1); strcpy(d, s);
  return d;
}

int find_label(char *u, int scope) {
  int i;
  for (i = 0; i < nlb; i++)
    if (lb_scope[i] == scope && !strcmp((char *)lb_name[i], u)) return i;
  return -1;
}

// name: at the start of a line (the label and its ':' are consumed)
void define_label() {
  char u[48];
  upname(u, (char *)toks[tp].val);
  if (find_label(u, cur_scope) >= 0) { cerr("Duplicate label"); return; }
  if (nlb >= caplb) {
    caplb = caplb * 2 + 32;
    lb_name = grow(lb_name, nlb, caplb); lb_scope = grow(lb_scope, nlb, caplb);
    lb_pos = grow(lb_pos, nlb, caplb); lb_data = grow(lb_data, nlb, caplb);
  }
  lb_name[nlb] = (int)dupstr(u); lb_scope[nlb] = cur_scope; lb_pos[nlb] = ncode; lb_data[nlb] = ndata;
  nlb++;
  next(); next();
}

// the label named by the token at tp (consumed): the argument at code[pos] is patched later
void label_ref(int pos, int kind) {
  char u[48];
  upname(u, (char *)toks[tp].val);
  if (nfx >= capfx) {
    capfx = capfx * 2 + 32;
    fx_name = grow(fx_name, nfx, capfx); fx_scope = grow(fx_scope, nfx, capfx);
    fx_pos = grow(fx_pos, nfx, capfx); fx_line = grow(fx_line, nfx, capfx);
    fx_kind = grow(fx_kind, nfx, capfx);
  }
  fx_name[nfx] = (int)dupstr(u); fx_scope[nfx] = cur_scope; fx_pos[nfx] = pos;
  fx_line[nfx] = toks[tp].line; fx_kind[nfx] = kind;
  nfx++;
  next();
}

// op label (GOTO, GOSUB, a jump from IF)
void goto_label(int op) {
  if (TK == TK_INT) { cerr("Advanced feature unavailable"); return; }   // a line number
  if (TK != TK_ID || toks[tp].suf) { bad_token(); return; }
  emit(op);
  label_ref(emit_arg(-1), 0);
}

void resolve_labels() {
  int i; int l;
  for (i = 0; i < nfx; i++) {
    l = find_label((char *)fx_name[i], fx_scope[i]);
    if (l < 0 && fx_kind[i]) l = find_label((char *)fx_name[i], 0);   // RESTORE: main's DATA
    if (l < 0) { err_line(fx_line[i], "Label not defined"); return; }
    if (fx_kind[i]) code[fx_pos[i]] = lb_data[l]; else code[fx_pos[i]] = lb_pos[l];
  }
}

void st_goto() { next(); goto_label(OP_JMP); }
void st_gosub() { next(); goto_label(OP_GOSUB); }
void st_return() { next(); if (!at_end()) { bad_token(); return; } emit(OP_RETURN); }
void st_stop() { next(); if (!at_end()) { bad_token(); return; } emit(OP_END); }

// ---------------------------------------------------------------- IF
// The statements after THEN on one line (up to ELSE or the end of the line), or a label.
void if_part() {
  int k; int p; char u[48];
  k = toks[tp + 1].tk;
  if (TK == TK_ID && (k == TK_EOL || k == TK_EOF || k == K_ELSE)) {
    upname(u, (char *)toks[tp].val);
    p = proc_find(u);
    if (p >= 0 && !procs[p].isfn && procs[p].np == 0) { next(); call_args(p, 0); return; }
    goto_label(OP_JMP);
    return;
  }
  while (1) {
    if (TK == TK_EOL || TK == TK_EOF || TK == K_ELSE) return;
    statement();
    if (TK == TK_COLON) next(); else return;
  }
}

// IF c THEN stmts [ELSE stmts] | IF c THEN label | IF c GOTO label | block IF c THEN (a
// block: a jump over the THEN part; ELSEIF and ELSE chain further jumps to END IF)
void st_if() {
  struct Blk *b; int skip; int endc;
  next();
  cond();
  if (TK != K_GOTO && !need(K_THEN)) return;
  if (TK == TK_EOL || TK == TK_EOF) {
    b = blk_push(B_IF);
    if (b) b->a = jump(OP_JZ, -1);
    return;
  }
  skip = jump(OP_JZ, -1);
  if_part();
  if (TK == K_ELSE) {
    next();
    endc = jump(OP_JMP, -1);
    patch_chain(skip, ncode);
    if_part();
    patch_chain(endc, ncode);
  }
  else patch_chain(skip, ncode);
}

// block IF: a = the jump to the next ELSEIF/ELSE/END IF (-1: none), b = jumps to END IF,
// c = 1 after ELSE
void st_elseif() {
  struct Blk *b;
  next();
  if (!blk_expect(B_IF, "ELSE without IF")) return;
  b = &blk[nblk - 1];
  if (b->c) { cerr("ELSE without IF"); return; }
  b->b = jump(OP_JMP, b->b);
  patch_chain(b->a, ncode);
  cond();
  if (!need(K_THEN)) return;
  b->a = jump(OP_JZ, -1);
}

void st_else() {
  struct Blk *b;
  next();
  if (!blk_expect(B_IF, "ELSE without IF")) return;
  b = &blk[nblk - 1];
  if (b->c) { cerr("ELSE without IF"); return; }
  b->b = jump(OP_JMP, b->b);
  patch_chain(b->a, ncode);
  b->a = -1; b->c = 1;
}

void end_if() {
  struct Blk *b;
  if (!blk_expect(B_IF, "END IF without block IF")) return;
  b = &blk[nblk - 1];
  patch_chain(b->a, ncode);
  patch_chain(b->b, ncode);
  nblk--;
}

// ---------------------------------------------------------------- loops
// FOR v = a TO b [STEP s]: v = a, then the FOR op (pops step, limit and v's address; skips to
// the end or pushes the frame). Block: a = the FOR op's skip argument, b = EXIT FOR jumps,
// c = the variable's symbol.
void st_for() {
  struct Blk *b; int s; int t;
  next();
  if (TK != TK_ID || toks[tp + 1].tk == TK_LPAR) { bad_token(); return; }
  s = name_ref();
  if (syms[s].kind == S_CONST) { cerr("Duplicate definition"); return; }
  t = syms[s].type;
  if (t == T_STR) { mismatch(); return; }
  if (!need(TK_EQ)) return;
  expr_to(t);
  emit_store(s);
  emit_ref(s);
  if (!need(K_TO)) return;
  expr_to(t);
  if (accept(K_STEP)) expr_to(t);
  else { emit(OP_PUSHI); emit_arg(1); if (t == T_SNG) emit(OP_I2F); }
  emit(OP_FORI + t - T_INT);
  b = blk_push(B_FOR);
  if (!b) return;
  b->a = emit_arg(-1);
  b->c = s;
}

void st_next() {
  struct Blk *b; int s; char u[48];
  next();
  while (1) {
    if (!blk_expect(B_FOR, "NEXT without FOR")) return;
    b = &blk[nblk - 1];
    if (TK == TK_ID) {                                    // only looked up, never created
      upname(u, (char *)toks[tp].val);
      s = lookup(u, toks[tp].suf, S_VAR);
      next();
      if (s != b->c) { err_line(toks[tp - 1].line, "NEXT without FOR"); return; }
    }
    emit_ref(b->c);
    emit(OP_NEXTI + syms[b->c].type - T_INT);
    code[b->a] = ncode;
    patch_chain(b->b, ncode);
    nblk--;
    if (!accept(TK_COMMA)) return;
    if (TK != TK_ID) { bad_token(); return; }
  }
}

// WHILE c ... WEND. Block: a = the top, b = the jump out
void st_while() {
  struct Blk *b;
  next();
  b = blk_push(B_WHILE);
  if (!b) return;
  b->a = ncode;
  cond();
  b->b = jump(OP_JZ, -1);
}

void st_wend() {
  struct Blk *b;
  next();
  if (!blk_expect(B_WHILE, "WEND without WHILE")) return;
  b = &blk[nblk - 1];
  emit(OP_JMP); emit_arg(b->a);
  patch_chain(b->b, ncode);
  nblk--;
}

// DO [WHILE|UNTIL c] ... LOOP [WHILE|UNTIL c]. Block: a = the top, b = jumps out (the test
// at DO, EXIT DO), c = 1 when DO has a test
void st_do() {
  struct Blk *b; int k;
  next();
  b = blk_push(B_DO);
  if (!b) return;
  b->a = ncode;
  if (TK == K_WHILE || TK == K_UNTIL) {
    k = TK; next();
    cond();
    if (k == K_WHILE) b->b = jump(OP_JZ, b->b); else b->b = jump(OP_JNZ, b->b);
    b->c = 1;
  }
}

void st_loop() {
  struct Blk *b; int k;
  next();
  if (!blk_expect(B_DO, "LOOP without DO")) return;
  b = &blk[nblk - 1];
  if (TK == K_WHILE || TK == K_UNTIL) {
    if (b->c) { cerr("Syntax error"); return; }
    k = TK; next();
    cond();
    if (k == K_WHILE) emit(OP_JNZ); else emit(OP_JZ);
    emit_arg(b->a);
  }
  else { emit(OP_JMP); emit_arg(b->a); }
  patch_chain(b->b, ncode);
  nblk--;
}

// EXIT FOR | EXIT DO | EXIT SUB | EXIT FUNCTION
void st_exit() {
  struct Blk *b; int k; int i;
  next();
  if (TK == K_SUB || TK == K_FUNCTION) {
    k = TK == K_FUNCTION;
    if (cur_proc < 0 || procs[cur_proc].isfn != k) {
      if (k) cerr("EXIT FUNCTION not within FUNCTION"); else cerr("EXIT SUB not within SUB");
      return;
    }
    next();
    emit(OP_RET); emit_arg(cur_proc);
    return;
  }
  if (TK == K_FOR) k = B_FOR;
  else if (TK == K_DO) k = B_DO;
  else { bad_token(); return; }
  i = nblk - 1;
  while (i >= 0 && blk[i].kind != k) i--;
  if (i < 0) {
    if (k == B_FOR) cerr("EXIT FOR not within FOR...NEXT"); else cerr("EXIT DO not within DO...LOOP");
    return;
  }
  next();
  b = &blk[i];
  if (k == B_FOR) { emit_ref(b->c); emit(OP_EXITFOR); }
  b->b = jump(OP_JMP, b->b);
}

// ---------------------------------------------------------------- SELECT CASE
// The value is kept in a hidden variable (a local inside a SUB/FUNCTION, so a recursive
// call made by a CASE test has its own). Block: a = its symbol, b = jumps to END SELECT, c = the jump to the next CASE (-1: none),
// d = 0 before the first CASE, 1 after a CASE, 2 after CASE ELSE, e = the value's type.
void select_load(struct Blk *b) { emit_load(b->a); }

void st_select() {
  struct Blk *b; int t;
  next();
  if (!need(K_CASE)) return;
  t = expr();
  b = blk_push(B_SELECT);
  if (!b) return;
  b->e = t; b->c = -1;
  b->a = hidden_var(t);
  emit_store(b->a);
}

void st_case() {
  struct Blk *b; int match; int t; int t2; int op;
  next();
  if (!blk_expect(B_SELECT, "CASE without SELECT")) return;
  b = &blk[nblk - 1];
  if (b->d == 2) { cerr("Syntax error"); return; }
  if (b->d == 1) {                                        // the end of the previous CASE's body
    b->b = jump(OP_JMP, b->b);
    patch_chain(b->c, ncode);
    b->c = -1;
  }
  if (accept(K_ELSE)) { b->d = 2; return; }
  b->d = 1;
  match = -1;
  while (1) {
    if (accept(K_IS)) {
      op = TK;
      if (op < TK_EQ || op > TK_GE) { bad_token(); return; }
      next();
      select_load(b);
      t = expr();
      emit_cmp(b->e, t, op);
    }
    else {
      select_load(b);
      t = expr();
      if (accept(K_TO)) {
        emit_cmp(b->e, t, TK_GE);
        select_load(b);
        t2 = expr();
        emit_cmp(b->e, t2, TK_LE);
        emit(OP_AND);
      }
      else emit_cmp(b->e, t, TK_EQ);
    }
    match = jump(OP_JNZ, match);
    if (!accept(TK_COMMA)) break;
  }
  b->c = jump(OP_JMP, -1);
  patch_chain(match, ncode);
}

void end_select() {
  struct Blk *b;
  if (!blk_expect(B_SELECT, "END SELECT without SELECT")) return;
  b = &blk[nblk - 1];
  patch_chain(b->c, ncode);
  patch_chain(b->b, ncode);
  nblk--;
}

// ---------------------------------------------------------------- DATA, READ, RESTORE
void data_add(char *p, int n, int quoted) {
  if (ndata >= capdata) {
    capdata = capdata * 2 + 64;
    d_item = (char **)grow((int *)d_item, ndata, capdata);
    d_quoted = grow(d_quoted, ndata, capdata);
  }
  d_item[ndata] = str_from(p, n); d_quoted[ndata] = quoted;
  ndata++;
}

// DATA a, "b, c", d: the lexer kept the text; items are split at commas, a quoted item runs
// to its closing quote, unquoted ones lose the blanks around them
void st_data() {
  char *s; int n; int i; int j; int e;
  s = (char *)toks[tp].val; n = str_len(s);
  i = 0;
  while (1) {
    while (i < n && (s[i] == ' ' || s[i] == '\t')) i++;
    if (i < n && s[i] == '"') {
      j = i + 1;
      while (j < n && s[j] != '"') j++;
      data_add(s + i + 1, j - i - 1, 1);
      i = j;
      if (i < n) i++;
      while (i < n && s[i] != ',') i++;
    }
    else {
      j = i;
      while (j < n && s[j] != ',') j++;
      e = j;
      while (e > i && (s[e - 1] == ' ' || s[e - 1] == '\t')) e--;
      data_add(s + i, e - i, 0);
      i = j;
    }
    if (i < n && s[i] == ',') { i++; continue; }
    break;
  }
  next();
}

// READ v [, v]: each value is pushed by the READ op and stored like an assignment
void st_read() {
  int s; int t;
  next();
  while (1) {
    if (TK != TK_ID) { bad_token(); return; }
    if (toks[tp + 1].tk == TK_LPAR) {                     // an array element
      t = elem_ref();
      emit(OP_READI + t - T_INT);
      if (t == T_STR) emit(OP_STAS); else emit(OP_STA);
    }
    else {
      s = name_ref();
      if (syms[s].kind == S_CONST) { cerr("Duplicate definition"); return; }
      emit(OP_READI + syms[s].type - T_INT);
      emit_store(s);
    }
    if (!accept(TK_COMMA)) return;
  }
}

void st_restore() {
  next();
  emit(OP_RESTORE);
  if (TK == TK_INT) { cerr("Advanced feature unavailable"); return; }
  if (TK == TK_ID && !toks[tp].suf) label_ref(emit_arg(-1), 1);
  else emit_arg(0);
}

// END, END IF, END SELECT, END SUB, END FUNCTION
void st_end() {
  next();
  if (TK == K_IF) { next(); end_if(); return; }
  if (TK == K_SELECT) { next(); end_select(); return; }
  if (TK == K_SUB) { end_proc(0); return; }
  if (TK == K_FUNCTION) { end_proc(1); return; }
  if (!at_end()) { bad_token(); return; }
  emit(OP_END);
}

// ---------------------------------------------------------------- arrays
// The array a name means here, or -1: like lookup, but in the current scope only (DIM) or
// with the SHARED ones too (REDIM)
int arr_find(char *u, int suf, int shared_too) {
  int s;
  if (shared_too) return lookup(u, suf, S_ARR);
  if (!suf) {
    s = sym_in(u, -1, S_ARR, cur_scope);
    if (s >= 0) return s;
    suf = defsuf[u[0] - 'A'];
  }
  return sym_in(u, suf, S_ARR, cur_scope);
}

// name(subscripts) at tp: pushes the element's address; returns the element type. An array
// not known yet is created, used without DIM: 0 to 10 in every dimension, made when the
// element is first reached.
int elem_ref() {
  char u[48]; int suf; int s; int nd; int it;
  upname(u, (char *)toks[tp].val); suf = toks[tp].suf;
  next();
  s = lookup(u, suf, S_ARR);
  if (s < 0) {
    if (!suf) suf = defsuf[u[0] - 'A'];
    s = var_new(u, suf, S_ARR, suf_type(suf));
    syms[s].flags = syms[s].flags | F_IMPL;
  }
  emit_ref(s);
  if (!need(TK_LPAR)) return syms[s].type;
  nd = 0;
  while (1) {
    expr_to(T_INT);
    nd++;
    if (!accept(TK_COMMA)) break;
  }
  if (!need(TK_RPAR)) return syms[s].type;
  if (!syms[s].dims) syms[s].dims = nd;
  if (syms[s].dims != nd) { cerr("Wrong number of dimensions"); return syms[s].type; }
  it = 0;
  if (syms[s].cls != C_PARAM && !(syms[s].flags & F_DIMMED)) it = syms[s].type;
  emit(OP_ELEM); emit_arg(nd); emit_arg(it);
  return syms[s].type;
}

int arr_named() {
  char u[48]; int s;
  if (TK != TK_ID) { bad_token(); return -1; }
  upname(u, (char *)toks[tp].val);
  s = lookup(u, toks[tp].suf, S_ARR);
  if (s < 0) { cerr("Array not defined"); return -1; }
  next();
  emit_ref(s);
  return s;
}

// AS type: the type's suffix character, or 0 after an error
int parse_as() {
  int t; int c;
  t = TK; c = 0;
  if (t == K_INTEGER) c = '%'; else if (t == K_LONG) c = '&'; else if (t == K_SINGLE) c = '!';
  else if (t == K_DOUBLE) c = '#'; else if (t == K_STRING) c = '$';
  if (t == TK_ID) { cerr("Advanced feature unavailable"); return 0; }   // a TYPE
  if (!c) { bad_token(); return 0; }
  next();
  if (c == '$' && TK == TK_MUL) { cerr("Advanced feature unavailable"); return 0; }   // STRING * n
  return c;
}

int dim_array(char *u, int suf, int as, int nd, int mask, int redim, int shared) {
  int s; int k;
  k = suf;
  if (as) k = as;
  s = arr_find(u, k, redim);
  if (s >= 0) {
    if (!redim || (syms[s].flags & F_IMPL)) { cerr("Array already dimensioned"); return 0; }
    if (syms[s].dims && syms[s].dims != nd) { cerr("Wrong number of dimensions"); return 0; }
  }
  else {
    if (!k) k = defsuf[u[0] - 'A'];
    s = var_new(u, k, S_ARR, suf_type(k));
    if (as) syms[s].flags = syms[s].flags | F_AS;
    if (shared) syms[s].flags = syms[s].flags | F_SHARED;
  }
  syms[s].dims = nd;
  syms[s].flags = syms[s].flags | F_DIMMED;
  emit_ref(s);
  emit(OP_DIM); emit_arg(nd); emit_arg(syms[s].type); emit_arg(redim); emit_arg(mask);
  return 1;
}

int dim_var(char *u, int suf, int as, int shared) {
  int s; int k;
  k = suf;
  if (!k) k = defsuf[u[0] - 'A'];
  if (as) k = as;
  if (const_find(u) >= 0 || (as && var_named(u, cur_scope)) || sym_in(u, k, S_VAR, cur_scope) >= 0 ||
      sym_in(u, -1, S_VAR, cur_scope) >= 0) {
    cerr("Duplicate definition");
    return 0;
  }
  s = var_new(u, k, S_VAR, suf_type(k));
  if (as) syms[s].flags = syms[s].flags | F_AS;
  if (shared) syms[s].flags = syms[s].flags | F_SHARED;
  return 1;
}

// DIM [SHARED] name[(bounds)] [AS type] [, ...] and REDIM (arrays only). Bounds are `hi` (0
// to hi) or `lo TO hi`; an array gets them when the statement runs, REDIM freeing the old
// one. A variable is only declared (its type, SHARED).
void st_dim() {
  int redim; int shared; char u[48]; int suf; int as; int nd; int mask;
  redim = TK == K_REDIM;
  next();
  if (TK == K_SHARED && cur_scope) { bad_token(); return; }
  shared = accept(K_SHARED);
  while (1) {
    if (TK != TK_ID) { bad_token(); return; }
    upname(u, (char *)toks[tp].val); suf = toks[tp].suf;
    next();
    nd = 0; mask = 0;
    if (TK == TK_LPAR) {
      next();
      while (1) {
        expr_to(T_INT);
        if (accept(K_TO)) { if (nd < 31) mask = mask | (1 << nd); expr_to(T_INT); }
        nd++;
        if (!accept(TK_COMMA)) break;
      }
      if (!need(TK_RPAR)) return;
      if (nd > ARR_DIMS) { cerr("Too many dimensions"); return; }
    }
    else if (redim) { bad_token(); return; }
    as = 0;
    if (accept(K_AS)) {
      as = parse_as();
      if (!as) return;
      if (suf && suf_type(suf) != suf_type(as)) { cerr("Duplicate definition"); return; }
    }
    if (nd) { if (!dim_array(u, suf, as, nd, mask, redim, shared)) return; }
    else if (!dim_var(u, suf, as, shared)) return;
    if (!accept(TK_COMMA)) return;
  }
}

// ERASE a [, b]: frees the arrays (a later use needs a REDIM, or is out of range)
void st_erase() {
  next();
  while (1) {
    if (arr_named() < 0) return;
    emit(OP_ERASE);
    if (!accept(TK_COMMA)) return;
  }
}

// ---------------------------------------------------------------- SUB and FUNCTION
int proc_add(char *u, int suf, int isfn) {
  struct Proc *n; int k;
  if (nproc >= capproc) {
    capproc = capproc * 2 + 16;
    n = malloc(capproc * sizeof(struct Proc));
    if (procs) { memcpy(n, procs, nproc * sizeof(struct Proc)); free(procs); }
    procs = n;
  }
  n = &procs[nproc];
  n->uname = dupstr(u); n->suf = suf; n->isfn = isfn; n->type = T_NONE;
  if (isfn) { k = suf; if (!k) k = defsuf[u[0] - 'A']; n->type = suf_type(k); }
  n->np = 0; n->pfirst = npa; n->entry = 0; n->ncell = 0; n->tags = 0; n->tok = 0; n->line = 0;
  nproc++;
  return nproc - 1;
}

void param_add(int p, int type, int arr) {
  if (npa >= cappa) {
    cappa = cappa * 2 + 32;
    pa_type = grow(pa_type, npa, cappa); pa_arr = grow(pa_arr, npa, cappa);
  }
  pa_type[npa] = type; pa_arr[npa] = arr; npa++;
  procs[p].np++;
}

// a FUNCTION's name written with suffix suf: its type (another type's suffix is an error)
int proc_type(int p, int suf) {
  if (suf && suf_type(suf) != procs[p].type) cerr("Duplicate definition");
  return procs[p].type;
}

char *proc_unclosed(int p) {
  if (procs[p].isfn) return "FUNCTION without END FUNCTION";
  return "SUB without END SUB";
}

// SUB name [(params)] | FUNCTION name[suffix] [(params)], the keyword at tp; a parameter is
// name[suffix][()] [AS type]. Scanning: adds the procedure and its parameter types to
// procs[] (p is not used) and returns its index; compiling: makes the parameters symbols of
// procedure p. -1 after an error.
int header(int p) {
  int isfn; char u[48]; int suf; int n; int psuf; int arr; int as; int s; int kind;
  isfn = TK == K_FUNCTION;
  next();
  if (TK != TK_ID) { bad_token(); return -1; }
  upname(u, (char *)toks[tp].val); suf = toks[tp].suf;
  if (suf && !isfn) { bad_token(); return -1; }
  if (scanning) {
    if (proc_find(u) >= 0) return -1;       // a second definition: reported when compiled
    p = proc_add(u, suf, isfn);
  }
  next();
  n = 0;
  if (accept(TK_LPAR) && !accept(TK_RPAR)) {
    while (1) {
      if (TK != TK_ID) { bad_token(); return -1; }
      upname(u, (char *)toks[tp].val); psuf = toks[tp].suf;
      next();
      arr = 0;
      if (TK == TK_LPAR) { next(); if (!need(TK_RPAR)) return -1; arr = 1; }
      as = 0;
      if (accept(K_AS)) {
        as = parse_as();
        if (!as) return -1;
        if (psuf && suf_type(psuf) != suf_type(as)) { cerr("Duplicate definition"); return -1; }
        psuf = as;
      }
      if (!psuf) psuf = defsuf[u[0] - 'A'];
      kind = S_VAR;
      if (arr) kind = S_ARR;
      if (scanning) param_add(p, suf_type(psuf), arr);
      else {
        if ((!arr && var_named(u, cur_scope)) || sym_in(u, psuf, kind, cur_scope) >= 0) {
          cerr("Duplicate definition");
          return -1;
        }
        s = sym_add(u, psuf, kind, suf_type(psuf));
        syms[s].cls = C_PARAM; syms[s].slot = 2 * n + 1;
        if (as) syms[s].flags = syms[s].flags | F_AS;
      }
      n++;
      if (!accept(TK_COMMA)) break;
    }
    if (!need(TK_RPAR)) return -1;
  }
  if (!at_end()) { bad_token(); return -1; }
  return p;
}

// The pass before compiling: every SUB and FUNCTION header (DEFtype lines are followed, so
// parameters without a suffix get the type they will have), errors left for the compile
void proc_scan() {
  char save[26]; int i; int n0; int a0; int kw; int p;
  for (i = 0; i < 26; i++) save[i] = defsuf[i];
  scanning = 1;
  tp = 0;
  while (TK != TK_EOF) {
    if (TK >= K_DEFINT && TK <= K_DEFSTR) { scan_fail = 0; st_deftype(); }
    else if (TK == K_SUB || TK == K_FUNCTION) {
      kw = tp; n0 = nproc; a0 = npa; scan_fail = 0;
      p = header(-1);
      if (p < 0 || scan_fail) {
        while (nproc > n0) { nproc--; free(procs[nproc].uname); }
        npa = a0;
      }
      else { procs[p].tok = kw; procs[p].line = toks[kw].line; }
    }
    while (TK != TK_EOL && TK != TK_EOF) next();
    if (TK == TK_EOL) next();
  }
  for (i = 0; i < 26; i++) defsuf[i] = save[i];
  scanning = 0;
  tp = 0;
}

// SUB / FUNCTION: the body is compiled in the procedure's own scope (variables, labels) and
// jumped over by the code around it
void st_sub() {
  char u[48]; int p; int kw;
  kw = tp;
  if (cur_proc >= 0) { err_line(procs[cur_proc].line, proc_unclosed(cur_proc)); return; }
  if (nblk > 0) { err_line(blk[nblk - 1].line, unclosed(blk[nblk - 1].kind)); return; }
  p = -1;
  if (toks[tp + 1].tk == TK_ID) { upname(u, (char *)toks[tp + 1].val); p = proc_find(u); }
  if (p < 0) { header(-1); if (!nerr) cerr("Syntax error"); return; }   // the scan failed
  if (procs[p].tok != kw) { cerr("Duplicate definition"); return; }
  proc_skip = jump(OP_JMP, -1);
  procs[p].entry = ncode;
  cur_proc = p; cur_scope = p + 1;
  cur_ncell = procs[p].isfn;                              // a FUNCTION's result cell
  header(p);
}

// END SUB / END FUNCTION (the keyword at tp): the return, and the frame's cell tags
void end_proc(int isfn) {
  struct Proc *pr; int i; int o;
  if (cur_proc < 0 || procs[cur_proc].isfn != isfn) {
    if (isfn) cerr("END FUNCTION without FUNCTION"); else cerr("END SUB without SUB");
    return;
  }
  next();
  if (nblk > 0) { err_line(blk[nblk - 1].line, unclosed(blk[nblk - 1].kind)); return; }
  pr = &procs[cur_proc];
  emit(OP_RET); emit_arg(cur_proc);
  pr->ncell = cur_ncell;
  pr->tags = malloc(cur_ncell + 1);
  if (pr->isfn && pr->type == T_STR) pr->tags[0] = 1;
  for (i = 0; i < nsym; i++)
    if (syms[i].scope == cur_scope && syms[i].cls == C_LOCAL) {
      o = syms[i].slot - 2 * pr->np - 1;
      if (syms[i].kind == S_ARR) pr->tags[o] = 2;
      else if (syms[i].type == T_STR) pr->tags[o] = 1;
    }
  patch_chain(proc_skip, ncode);
  cur_proc = -1; cur_scope = 0;
}

// the argument at tp is a variable or an array element alone (passed by reference)?
int ref_arg(int paren) {
  char u[48]; int j; int d; int k;
  if (TK != TK_ID) return 0;
  upname(u, (char *)toks[tp].val);
  if (const_find(u) >= 0 || proc_find(u) >= 0) return 0;
  j = tp + 1;
  if (toks[j].tk == TK_LPAR) {
    d = 0;
    while (1) {
      k = toks[j].tk;
      if (k == TK_LPAR) d++;
      else if (k == TK_RPAR) { d--; if (d == 0) break; }
      else if (k == TK_EOL || k == TK_EOF) return 0;
      j++;
    }
    j++;
  }
  k = toks[j].tk;
  if (k == TK_COMMA) return 1;
  if (paren) return k == TK_RPAR;
  return k == TK_EOL || k == TK_COLON || k == TK_EOF || k == K_ELSE;
}

// argument i of a call to p: two cells (see basic.h); 0 after an error
int call_arg(int p, int i, int paren) {
  int pt; int pa; int s; int t; char u[48];
  pt = pa_type[procs[p].pfirst + i]; pa = pa_arr[procs[p].pfirst + i];
  if (TK == TK_ID && toks[tp + 1].tk == TK_LPAR && toks[tp + 2].tk == TK_RPAR) {   // a whole array
    upname(u, (char *)toks[tp].val);
    s = lookup(u, toks[tp].suf, S_ARR);
    if (s < 0) { cerr("Array not defined"); return 0; }
    if (!pa || syms[s].type != pt) { cerr("Parameter type mismatch"); return 0; }
    next(); next(); next();
    emit_ref(s);
    emit(OP_PUSHI); emit_arg(0);
    return 1;
  }
  if (pa) { cerr("Parameter type mismatch"); return 0; }
  if (ref_arg(paren)) {
    t = ref_var();
    if (t != pt) { cerr("Parameter type mismatch"); return 0; }
    emit(OP_PUSHI); emit_arg(0);
    return 1;
  }
  expr_to(pt);
  emit(OP_BYVAL);
  return 1;
}

// the arguments of a call to p, in parentheses (paren) or to the end of the statement, and
// the call
void call_args(int p, int paren) {
  int n;
  n = 0;
  if (paren) { if (!accept(TK_LPAR) || accept(TK_RPAR)) paren = -1; }
  else if (at_end()) paren = -1;
  if (paren >= 0) {
    while (1) {
      if (n >= procs[p].np) { cerr("Argument-count mismatch"); return; }
      if (!call_arg(p, n, paren)) return;
      n++;
      if (!accept(TK_COMMA)) break;
    }
    if (paren && !need(TK_RPAR)) return;
  }
  if (n != procs[p].np) { cerr("Argument-count mismatch"); return; }
  emit(OP_CALL); emit_arg(p);
}

// CALL name [(args)]
void st_call() {
  char u[48]; int p;
  next();
  if (TK != TK_ID) { bad_token(); return; }
  upname(u, (char *)toks[tp].val);
  p = proc_find(u);
  if (p < 0 || procs[p].isfn) { cerr("Subprogram not defined"); return; }
  next();
  call_args(p, 1);
}

// DECLARE lines are not needed (procedures are found before compiling): skipped
void st_declare() { while (TK != TK_EOL && TK != TK_COLON && TK != TK_EOF) next(); }

// QBasic statements outside this BASIC that start with one of its words
void st_unsup() { cerr("Advanced feature unavailable"); }

// LINE (x1, y1)-(x2, y2) is graphics; LINE INPUT comes with Task 6
void st_line() {
  if (toks[tp + 1].tk != K_INPUT) { st_unsup(); return; }
  bad_token();
}

void statement() {
  int t; int p; char u[48];
  line_mark(toks[tp].line);
  t = TK;
  // a label: a name and ':' first on a line
  if (t == TK_ID && toks[tp + 1].tk == TK_COLON && (tp == 0 || toks[tp - 1].tk == TK_EOL)) {
    define_label();
    if (TK == TK_EOL || TK == TK_EOF || TK == TK_COLON) return;
    t = TK;
  }
  if (t == TK_INT) { cerr("Advanced feature unavailable"); return; }   // a line number
  if (nblk > 0 && blk[nblk - 1].kind == B_SELECT && blk[nblk - 1].d == 0 && t != K_CASE &&
      !(t == K_END && toks[tp + 1].tk == K_SELECT)) {
    cerr("Statements illegal between SELECT CASE and CASE");
    return;
  }
  if (t == TK_ID) {                                       // `name args` calls a SUB
    upname(u, (char *)toks[tp].val);
    p = proc_find(u);
    if (p >= 0 && !procs[p].isfn) { next(); call_args(p, 0); return; }
    st_assign();
    return;
  }
  if (stmt_tab[t]) { stmt_tab[t](); return; }
  bad_token();
}

int compile() {
  int i; int n;
  // the longest line bounds what one statement pushes on the value stack (see basic.h)
  n = 0; vmargin = 0;
  for (i = 0; i < ntok; i++) {
    n++;
    if (toks[i].tk == TK_EOL) { if (n > vmargin) vmargin = n; n = 0; }
  }
  if (n > vmargin) vmargin = n;
  vmargin = 2 * vmargin + 64;
  proc_scan();
  tp = 0; depth = 0; const_mode = 0;
  while (TK != TK_EOF) {
    if (TK == TK_EOL || TK == TK_COLON) { next(); continue; }
    statement();
    if (TK == TK_EOL || TK == TK_COLON) next();
    else if (TK != TK_EOF) bad_token();
  }
  emit(OP_END);
  if (!nerr && nblk > 0) err_line(blk[nblk - 1].line, unclosed(blk[nblk - 1].kind));
  if (!nerr && cur_proc >= 0) err_line(procs[cur_proc].line, proc_unclosed(cur_proc));
  if (!nerr) resolve_labels();
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
  stmt_tab[K_TIMER] = st_unsup;         // TIMER ON / TIMER OFF (TIMER the function is an expression)
  stmt_tab[K_IF] = st_if;
  stmt_tab[K_ELSEIF] = st_elseif;
  stmt_tab[K_ELSE] = st_else;
  stmt_tab[K_FOR] = st_for;
  stmt_tab[K_NEXT] = st_next;
  stmt_tab[K_WHILE] = st_while;
  stmt_tab[K_WEND] = st_wend;
  stmt_tab[K_DO] = st_do;
  stmt_tab[K_LOOP] = st_loop;
  stmt_tab[K_SELECT] = st_select;
  stmt_tab[K_CASE] = st_case;
  stmt_tab[K_EXIT] = st_exit;
  stmt_tab[K_GOTO] = st_goto;
  stmt_tab[K_GOSUB] = st_gosub;
  stmt_tab[K_RETURN] = st_return;
  stmt_tab[K_STOP] = st_stop;
  stmt_tab[K_SYSTEM] = st_stop;
  stmt_tab[K_DATA] = st_data;
  stmt_tab[K_READ] = st_read;
  stmt_tab[K_RESTORE] = st_restore;
  stmt_tab[K_DIM] = st_dim;
  stmt_tab[K_REDIM] = st_dim;
  stmt_tab[K_ERASE] = st_erase;
  stmt_tab[K_SUB] = st_sub;
  stmt_tab[K_FUNCTION] = st_sub;
  stmt_tab[K_CALL] = st_call;
  stmt_tab[K_DECLARE] = st_declare;
  blk = malloc(256 * sizeof(struct Blk)); nblk = 0;
  cur_scope = 0; cur_proc = -1; scanning = 0;
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
  for (i = 0; i < nlb; i++) free((char *)lb_name[i]);
  for (i = 0; i < nfx; i++) free((char *)fx_name[i]);
  for (i = 0; i < ndata; i++) str_free(d_item[i]);
  if (lb_name) { free(lb_name); free(lb_scope); free(lb_pos); free(lb_data); }
  if (fx_name) { free(fx_name); free(fx_scope); free(fx_pos); free(fx_line); free(fx_kind); }
  if (d_item) { free(d_item); free(d_quoted); }
  if (blk) free(blk);
  if (pa_type) { free(pa_type); free(pa_arr); }
  pa_type = 0; pa_arr = 0; npa = 0; cappa = 0; capproc = 0;
  lb_name = 0; nlb = 0; caplb = 0; fx_name = 0; nfx = 0; capfx = 0;
  d_item = 0; d_quoted = 0; ndata = 0; capdata = 0; blk = 0; nblk = 0;
  if (lm_pos) { free(lm_pos); free(lm_line); }
  syms = 0; nsym = 0; capsym = 0; code = 0; ncode = 0; capcode = 0;
  lm_pos = 0; lm_line = 0; nlm = 0; caplm = 0;
}
