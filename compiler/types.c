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
