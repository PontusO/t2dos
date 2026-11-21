// types.c - types: declarators, sizeof, struct rows and the constant-expression
// folder. See cc.h for what the files share.
#include "cc.h"

// ------------------------------------------------------------------ types
int newstruct() {                          // returns the index of a fresh struct-like type
  if (nstructs >= 250) die("too many struct and array row types");
  stsize[nstructs] = 0; stfirst[nstructs] = -1; starr[nstructs] = -1; stcnt[nstructs] = 0;
  nstructs = nstructs + 1;
  return nstructs - 1;
}
int isrow(int t) { return t >= TSTRUCT && t < PTR && starr[t - TSTRUCT] >= 0; }
int rowelem(int t) { return starr[t - TSTRUCT]; }
int rowcnt(int t) { return stcnt[t - TSTRUCT]; }
int mkrow(int et, int n);
// an expression of row type is the address of its first element
int decay() { if (isrow(ty)) ty = rowelem(ty) + PTR; return 0; }
int tysize(int t) {
  if (t >= PTR) return 4;
  if (t == TCHAR || t == TUCHAR) return 1;
  if (t == TSHORT || t == TUSHORT) return 2;
  if (t == TINT || t == TUINT || t == TFLOAT) return 4;
  return stsize[t - TSTRUCT];
}

// parse a base type (optionally with struct definition) and pointer stars
int is_type() { return tk == Int || tk == Char || tk == Void || tk == Struct || tk == Long || tk == Short || tk == Signed || tk == Unsigned || tk == Float || tk == Double || (tk == Id && id->class == 8); }

int parse_type() {
  int bt; int mt; int si; int off; int sz; int al;
  if (tk == Int || tk == Char || tk == Long || tk == Short || tk == Signed || tk == Unsigned) {
    off = 0; sz = 0; al = 0;               // unsigned, short, char seen
    while (tk == Int || tk == Char || tk == Long || tk == Short || tk == Signed || tk == Unsigned) {
      if (tk == Unsigned) off = 1; else if (tk == Short) sz = 1; else if (tk == Char) al = 1;
      next();
    }
    if (al) bt = off ? TUCHAR : TCHAR; else if (sz) bt = off ? TUSHORT : TSHORT; else bt = off ? TUINT : TINT;
  }
  else if (tk == Void) { bt = TCHAR; next(); }
  else if (tk == Float || tk == Double) { bt = TFLOAT; next(); }   // double is the 32-bit float too
  else if (tk == Id && id->class == 8) { bt = id->type; next(); }   // typedef name
  else if (tk == Struct) {
    next();
    if (tk == Id) {
      if (!id->stag) { newstruct(); id->stag = nstructs; }
      si = id->stag - 1; next();
    } else if (tk == '{') { si = nstructs; newstruct(); }   // anonymous
    else die("struct name expected");
    {
      if (tk == '{') {
        next(); off = 0;
        while (tk != '}') {
          bt = parse_type();
          while (tk != ';') {
            mt = declarator(bt);
            if (!dname) die("member name expected");
            al = mt; while (isrow(al)) al = rowelem(al);    // a row aligns like its element
            sz = tysize(al); al = sz > 4 ? 4 : sz; if (al == 3) al = 4;
            sz = tysize(mt);
            if (al > 1) off = (off + al - 1) & -al;
            mems[nmems].id = dname; mems[nmems].type = mt; mems[nmems].off = off; mems[nmems].next = stfirst[si];
            stfirst[si] = nmems; nmems = nmems + 1;
            mems[nmems - 1].cnt = darr;
            if (darr == -2) die("member array size expected");
            if (darr >= 0) {                    // array member: storage inline, type is "address of element"
              sz = sz * darr;
              mems[nmems - 1].type = (mt + PTR) | ARR;
            }
            off = off + sz;
            if (tk == ',') next();
          }
          next();
        }
        next();
        stsize[si] = (off + 3) & -4;
      }
    }
    bt = TSTRUCT + si;
  }
  else die("type expected");
  while (tk == Mul) { next(); bt = bt + PTR; }
  return bt;
}

// find member of struct type t for identifier id
int find_mem(int t) {
  int m;
  if (t < TSTRUCT || t >= PTR) die("not a struct");
  m = stfirst[t - TSTRUCT];
  while (m >= 0) { if (mems[m].id == id) return m; m = mems[m].next; }
  die("no such member"); return 0;
}

// skip a parenthesized parameter list (function pointer declarators don't check params)
int skip_params() {
  int depth;
  if (tk != '(') die("( expected");
  depth = 0;
  while (1) {
    if (!tk) die("unexpected eof in parameter list");
    if (tk == '(') depth = depth + 1;
    else if (tk == ')') { depth = depth - 1; if (!depth) { next(); return 0; } }
    next();
  }
}
// ------------------------------------------------------------------ constant expressions
// Evaluated while parsing, for array sizes, case labels and enum values:
// numbers, enum and macro constants, sizeof(type), ( ), unary - + ~ !, binary
// operators and ?:.
int cexpr();
int cunary() {
  int v; int t;
  if (tk == Num) { if (numty == TFLOAT) die("integer constant expected"); v = ival; next(); return v; }
  if (tk == Id && id->class == CNum) { v = id->val; next(); return v; }
  if (tk == Id && pp_if) { next(); return 0; }          // #if FOO with FOO not a macro
  if (tk == '(') { next(); v = cexpr(); if (tk != ')') die(") expected"); next(); return v; }
  if (tk == Sub) { next(); return -cunary(); }
  if (tk == Add) { next(); return cunary(); }
  if (tk == '~') { next(); return ~cunary(); }
  if (tk == '!') { next(); return !cunary(); }
  if (tk == Sizeof) {
    next(); if (tk != '(') die("( expected after sizeof"); next();
    t = declarator(parse_type()); v = tysize(t); if (darr >= 0) v = v * darr;
    if (tk != ')') die(") expected"); next();
    return v;
  }
  die("constant expected"); return 0;
}
int cprec(int op) {
  if (op == Mul || op == Div || op == Mod) return 10;
  if (op == Add || op == Sub) return 9;
  if (op == Shl || op == Shr) return 8;
  if (op == Lt || op == Gt || op == Le || op == Ge) return 7;
  if (op == Eq || op == Ne) return 6;
  if (op == And) return 5;
  if (op == Xor) return 4;
  if (op == Or) return 3;
  if (op == Lan) return 2;
  if (op == Lor) return 1;
  return 0;
}
int cbin(int minp) {
  int v; int r; int op; int pr;
  v = cunary();
  while ((pr = cprec(tk)) && pr >= minp) {
    op = tk; next(); r = cbin(pr + 1);
    if ((op == Div || op == Mod) && !r) die("division by zero in a constant");
    if (op == Mul) v = v * r; else if (op == Div) v = v / r; else if (op == Mod) v = v % r;
    else if (op == Add) v = v + r; else if (op == Sub) v = v - r;
    else if (op == Shl) v = v << r; else if (op == Shr) v = v >> r;
    else if (op == Lt) v = v < r; else if (op == Gt) v = v > r; else if (op == Le) v = v <= r; else if (op == Ge) v = v >= r;
    else if (op == Eq) v = v == r; else if (op == Ne) v = v != r;
    else if (op == And) v = v & r; else if (op == Xor) v = v ^ r; else if (op == Or) v = v | r;
    else if (op == Lan) v = v && r; else v = v || r;
  }
  return v;
}
int cexpr() {
  int v; int a; int b;
  v = cbin(1);
  if (tk == Cond) {
    next(); a = cexpr(); if (tk != ':') die(": expected"); next(); b = cexpr();
    return v ? a : b;
  }
  return v;
}

// array row type: n elements of type et (shared between declarations)
int mkrow(int et, int n) {
  int i;
  i = 0; while (i < nstructs) { if (starr[i] == et && stcnt[i] == n) return TSTRUCT + i; i = i + 1; }
  i = newstruct(); starr[i] = et; stcnt[i] = n; stsize[i] = tysize(et) * n;
  return TSTRUCT + i;
}
// [N][M]...: dims[0..ndims); only the first may be [] (-2)
int parse_dims() {
  ndims = 0;
  while (tk == Brak) {
    if (ndims >= 8) die("too many array dimensions");
    array_dim(); dims[ndims] = darr; ndims = ndims + 1;
  }
  return 0;
}
// element type of dims[from - 1]: rows of dims[from..ndims), innermost first
int rows_of(int t, int from) {
  int k;
  k = ndims - 1;
  while (k >= from) { if (dims[k] < 0) die("array size expected"); t = mkrow(t, dims[k]); k = k - 1; }
  return t;
}
// [N] or [] (size from the initializer: darr = -2)
int array_dim() {
  next();
  if (tk == ']') { darr = -2; next(); return 0; }
  darr = cexpr();
  if (darr < 0) die("negative array size");
  if (tk != ']') die("] expected"); next();
  return 0;
}
// ( types ) of a function pointer into pl[]; returns how many, -1 for ( )
int param_types(int *pl) {
  struct Sym *sv_name; int sv_arr; int n; int t; int isvoid;
  if (tk != '(') die("( expected");
  sv_name = dname; sv_arr = darr;           // declarator() is used for the parameters too
  next(); n = 0;
  if (tk == ')') { next(); dname = sv_name; darr = sv_arr; return -1; }
  while (tk != ')') {
    if (!is_type()) die("parameter type expected");
    isvoid = tk == Void;
    t = declarator(parse_type());
    if (darr != -1) t = t + PTR;
    if (isvoid && t == TCHAR && tk == ')' && !n) break;   // (void)
    if (n < 16) pl[n] = t;
    n = n + 1;
    if (tk == ',') next(); else if (tk != ')') die(", or ) expected");
  }
  next();
  dname = sv_name; darr = sv_arr;
  return n < 16 ? n : 16;
}
// Declarator after a base type: stars, then a name (or none, for casts and
// prototypes) and optional [N][M]..., or ( *name[N] ) followed by ( params ) for
// a function pointer or by [M]... for a pointer to an array.
// Returns the type (of an element, for arrays); sets dname (0 if no name) and
// darr (outermost length, -1 if not an array, -2 for []).
int declarator(int t) {
  int ps; int inner; int pl[16];
  while (tk == Mul) { next(); t = t + PTR; }
  dname = 0; darr = -1;
  if (tk == '(') {
    next(); ps = 0;
    while (tk == Mul) { next(); ps = ps + 1; }
    if (!ps) die("only function and array pointers are supported: (*name)");
    if (tk == Id) { dname = id; next(); }
    if (tk == Brak) array_dim();
    if (tk != ')') die(") expected in declarator"); next();
    if (tk == Brak) {                           // int (*p)[4]: pointer to array
      inner = darr; parse_dims(); darr = inner;
      return rows_of(t, 0) + ps * PTR;
    }
    inner = param_types(pl);
    return fntype_p(t, pl, inner) + ps * PTR;
  }
  if (tk == Id) { dname = id; next(); }
  if (tk == Brak) { parse_dims(); t = rows_of(t, 1); darr = dims[0]; }
  return t;
}
