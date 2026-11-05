// cc.c - a tiny single-pass C compiler emitting Thumb-2 code for Cortex-M33
// (RP2350). Written in the subset of C it compiles, so it can build itself.
//
// Build on a PC as a cross compiler:   gcc -O2 -I . -o compiler/cc compiler/cc.c   (from the repo root)
// Use:                                 cc -o prog.bin prog.c
//
// Generated code model:
//   r7 = frame pointer, r8 = host syscall entry, r9 = data/bss base.
//   Expression results in r0.  All args passed on the stack, left to right
//   (first arg deepest), caller pops.  Position independent, no relocations.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

// ------------------------------------------------------------------ tokens
enum { Num = 128, Str, Id,
  Break, Case, Char, Continue, Default, Else, Enum, For, If, Int, Return, Sizeof, Struct, Switch, Typedef, Void, While,
  Long, Short, Signed, Unsigned,
  Assign, AddA, SubA, MulA, DivA, ModA, AndA, OrA, XorA, ShlA, ShrA, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr,
  Add, Sub, Mul, Div, Mod, Inc, Dec, Dot, Arrow, Brak };

// symbol classes
enum { CNum = 1, CFun, CSys, CGlo, CLoc };
// base types; struct i is TSTRUCT+i; each pointer level adds PTR
// FN marks a function type; bits 22.. index fnret[] (its return type). A function
// designator has 0 pointer levels, a function pointer 1.
enum { TCHAR = 0, TINT = 1, TUCHAR = 2, TSHORT = 3, TUSHORT = 4, TUINT = 5, TSTRUCT = 6, PTR = 256, ARR = 1048576, FN = 2097152 };

#include "sys.h"

struct Sym { char *name; int len; int hash; int tk; int class; int type; int val;
             int hclass; int htype; int hval; int fix; int stag; };
struct Mem { struct Sym *id; int type; int off; int next; int cnt; };   // cnt: array member length, else -1

// ------------------------------------------------------------------ state
char *p;                 // source position
int tk, ival, line;
struct Sym *id;          // current identifier
struct Sym *syms; int nsyms;
struct Mem *mems; int nmems;
int *stsize; int *stfirst; int nstructs;
int *starr; int *stcnt;  // array rows (int[4] in int m[3][4]) are struct-like types: element type, length; -1 for structs
int *dims; int ndims;    // set by parse_dims()

char *code; int cpos;    // code segment
char *data; int dpos;    // data segment (strings + globals)
int lastload;            // bytes of the load instruction just emitted (for lvalues)

int ty;                  // type of last parsed expression
int loc;                 // current local frame depth
int maxloc;
int ret_fix, brk_fix, cont_fix, cont_tgt;  // fixup chains / targets
int nparams;
int fnrty;               // return type of the function being compiled

int *fnret; int nfn;     // function types: return type of each
int *afix_at; struct Sym **afix_sym; int naddr;   // function addresses to patch at the end
struct Sym *dname; int darr;                       // set by declarator(): name, array size, -1 none, -2 for []
int numty;                                         // type of the last number token: TINT or TUINT
int slen;                                          // bytes of the last string literal, with its 0
int ikind; int ibase;                              // initializer target: 0 = data offset, 1 = frame offset
int init_open; int init_head; int init_tail;       // code chunks for global initializers, run before main
char *sv_p; int sv_tk; int sv_ival; struct Sym *sv_id; int sv_line; int sv_dpos;   // lexer state for lookahead

// include stack
char **fstack_p; int *fstack_line; int fdepth;
char *srcname;
char *incdir;

int preprocess(); int patch_b(int at, int target); int expr(int lev); int stmt(); int next();
int declarator(int t); int die(char *msg);

// ------------------------------------------------------------------ helpers
int die(char *msg) { printf("%s:%d: error: %s near token %d\n", srcname, line, msg, tk); exit(1); return 0; }

char *readfile(char *name) {
  int fd; char *buf; int n; int total; char *alt;
  fd = open(name, 0, 0);
  if (fd < 0 && incdir) {                   // -I dir
    alt = malloc(strlen(incdir) + strlen(name) + 2); strcpy(alt, incdir); strcpy(alt + strlen(alt), "/"); strcpy(alt + strlen(alt), name);
    fd = open(alt, 0, 0);
  }
  if (fd < 0) {                             // includes also resolve against /lib
    alt = malloc(strlen(name) + 6); strcpy(alt, "/lib/"); strcpy(alt + 5, name);
    fd = open(alt, 0, 0);
  }
  if (fd < 0) { printf("cannot open %s\n", name); exit(1); }
  buf = malloc(1048576);
  total = 0;
  while ((n = read(fd, buf + total, 65536)) > 0) total = total + n;
  buf[total] = 0;
  close(fd);
  return buf;
}

// ------------------------------------------------------------------ emitter
int emit16(int x) { code[cpos] = x; code[cpos + 1] = x >> 8; cpos = cpos + 2; lastload = 0; return 0; }
int emit32(int hi, int lo) { emit16(hi); emit16(lo); return 0; }
int put16(int at, int x) { code[at] = x; code[at + 1] = x >> 8; return 0; }
int get16(int at) { return (code[at] & 255) | ((code[at + 1] & 255) << 8); }

int push() { return emit16(46081); }          // push {r0}      B401
int pop1() { return emit16(48130); }          // pop  {r1}      BC02

// movw/movt rd, #imm32
int imm(int rd, int v) {
  int lo; int hi;
  lo = v & 65535; hi = (v >> 16) & 65535;
  emit32(62016 | ((lo >> 11) & 1) << 10 | (lo >> 12), ((lo >> 8) & 7) << 12 | rd << 8 | (lo & 255));
  if (hi) emit32(62144 | ((hi >> 11) & 1) << 10 | (hi >> 12), ((hi >> 8) & 7) << 12 | rd << 8 | (hi & 255));
  return 0;
}
// add.w/sub.w rd, rn, #imm12 (T4 encodings)
int addi12(int rd, int rn, int v) { return emit32(61952 | ((v >> 11) & 1) << 10 | rn, ((v >> 8) & 7) << 12 | rd << 8 | (v & 255)); }
int subi12(int rd, int rn, int v) { return emit32(62112 | ((v >> 11) & 1) << 10 | rn, ((v >> 8) & 7) << 12 | rd << 8 | (v & 255)); }

// r0 = address of local/param at signed frame offset off
int lea_loc(int off) {
  if (off < 0) { off = -off; if (off < 4096) subi12(0, 7, off); else { imm(1, off); emit16(6776); } } // subs r0,r7,r1
  else { if (off < 4096) addi12(0, 7, off); else { imm(1, off); emit16(6328); } }                     // adds r0,r7,r1  18B8
  return 0;
}
// r0 = r9 + off
int lea_glo(int off) {
  if (off < 4096) addi12(0, 9, off); else { imm(1, off); emit32(60169, 1); }   // add.w r0,r9,r1  EB09 0001
  return 0;
}
// function types
int fntype(int ret) {
  int k;
  k = 0; while (k < nfn) { if (fnret[k] == ret) return FN | k << 22; k = k + 1; }
  if (nfn >= 512) die("too many function types");
  fnret[nfn] = ret; nfn = nfn + 1;
  return FN | k << 22;
}
int fnlevels(int t) { return (t >> 8) & 4095; }
int isfn(int t) { return (t & FN) && fnlevels(t) <= 1; }          // designator or pointer: callable
int isfndes(int t) { return (t & FN) && fnlevels(t) == 0; }       // designator: its value is its address
int fnret_of(int t) { return fnret[(t >> 22) & 511]; }

// load r0 = *r0 according to type
int load(int t) {
  if (isfndes(t)) return 0;                                  // a function is its own address
  if (t == TCHAR) { emit32(63888, 0); lastload = 4; }          // ldrsb.w r0,[r0]  F990 0000
  else if (t == TINT || t == TUINT || t >= PTR) { emit16(26624); lastload = 2; } // ldr r0,[r0]  6800
  else if (t == TUCHAR) { emit16(30720); lastload = 2; }       // ldrb r0,[r0]     7800
  else if (t == TSHORT) { emit32(63920, 0); lastload = 4; }    // ldrsh.w r0,[r0]  F9B0 0000
  else if (t == TUSHORT) { emit16(34816); lastload = 2; }      // ldrh r0,[r0]     8800
  // struct: keep address
  return 0;
}
// store *r1 = r0
int store(int t) {                       // strb r0,[r1] 7008 / strh r0,[r1] 8008 / str r0,[r1] 6008
  if (t == TCHAR || t == TUCHAR) emit16(28680); else if (t == TSHORT || t == TUSHORT) emit16(32776); else emit16(24584);
  return 0;
}
// r0 = *r1 according to type (for ++ and --)
int load_r1(int t) {
  if (t == TCHAR) emit32(63889, 0);                    // ldrsb.w r0,[r1]  F991 0000
  else if (t == TUCHAR) emit16(30728);                 // ldrb r0,[r1]     7808
  else if (t == TSHORT) emit32(63921, 0);              // ldrsh.w r0,[r1]  F9B1 0000
  else if (t == TUSHORT) emit16(34824);                // ldrh r0,[r1]     8808
  else emit16(26632);                                  // ldr r0,[r1]      6808
  return 0;
}
// *r1 = r2 according to type: strb 700A / strh 800A / str 600A
int store_r2(int t) {
  if (t == TCHAR || t == TUCHAR) emit16(28682); else if (t == TSHORT || t == TUSHORT) emit16(32778); else emit16(24586);
  return 0;
}
// convert r0 to type t (casts, return values): sign or zero extend the small types
int conv(int t) {
  if (t == TCHAR) emit16(45632);                       // sxtb r0,r0  B240
  else if (t == TUCHAR) emit16(45760);                 // uxtb r0,r0  B2C0
  else if (t == TSHORT) emit16(45568);                 // sxth r0,r0  B200
  else if (t == TUSHORT) emit16(45696);                // uxth r0,r0  B280
  return 0;
}
// usual arithmetic conversions: small types promote to int; unsigned int wins
int arith(int a, int b) { if (a == TUINT || b == TUINT) return TUINT; return TINT; }
int isuns(int a, int b) { return a < PTR && b < PTR && arith(a, b) == TUINT; }

// branches, all 32-bit so they can be patched. returns offset of instruction.
int b_w(int target) {   // unconditional b.w to target (target<0: fixup later)
  int at; at = cpos; emit32(61440, 36864); if (target >= 0) patch_b(at, target); return at;
}
int bl_w() { int at; at = cpos; emit32(61440, 53248); return at; }
int bc_w(int cond) { int at; at = cpos; emit32(61440 | cond << 6, 32768); return at; }

int patch_b(int at, int target) {
  int off; int s; int j1; int j2; int h1; int h2; int i1; int i2;
  off = target - (at + 4);
  h1 = get16(at); h2 = get16(at + 2);
  if ((h2 & 36864) == 36864 || (h2 & 53248) == 53248) {      // T4 b.w / bl (24-bit)
    s = (off >> 24) & 1; i1 = (off >> 23) & 1; i2 = (off >> 22) & 1;
    j1 = (i1 ^ 1) ^ s; j2 = (i2 ^ 1) ^ s;
    put16(at, 61440 | s << 10 | ((off >> 12) & 1023));
    put16(at + 2, (h2 & 53248) | j1 << 13 | j2 << 11 | ((off >> 1) & 2047));
  } else {                                                    // T3 conditional (20-bit)
    s = (off >> 20) & 1; j2 = (off >> 19) & 1; j1 = (off >> 18) & 1;
    put16(at, (h1 & 64448) | s << 10 | ((off >> 12) & 63));
    put16(at + 2, 32768 | j1 << 13 | j2 << 11 | ((off >> 1) & 2047));
  }
  return 0;
}
// fixup chains: an unpatched branch slot holds the previous chain entry (or -1)
int chain_add(int head, int at) { put16(at + 2, head & 65535); put16(at, (head >> 16) & 65535); return at; }
int chain_patch(int head, int target, int kind) {   // kind: 36864 = b.w, 53248 = bl
  int nxt;
  while (head >= 0) { nxt = (get16(head) << 16) | get16(head + 2); put16(head + 2, kind); patch_b(head, target); head = nxt; }
  return 0;
}
// movw+movt, always both halves (fixed size, patchable)
int imm2(int rd, int v) {
  int lo; int hi;
  lo = v & 65535; hi = (v >> 16) & 65535;
  emit32(62016 | ((lo >> 11) & 1) << 10 | (lo >> 12), ((lo >> 8) & 7) << 12 | rd << 8 | (lo & 255));
  emit32(62144 | ((hi >> 11) & 1) << 10 | (hi >> 12), ((hi >> 8) & 7) << 12 | rd << 8 | (hi & 255));
  return 0;
}

// compare r1 ? r0 -> r0 = 0/1 using IT block
int setcc(int cond) {
  emit16(17025);                                              // cmp r1,r0  4281
  emit16(48896 | cond << 4 | ((cond & 1) ^ 1) << 3 | 4);      // ite cond
  emit16(8193); emit16(8192);                                 // mov r0,#1 ; mov r0,#0
  return 0;
}

// ------------------------------------------------------------------ lexer
int lookup(char *s, int len, int hash) {
  int i;
  i = 0;
  while (i < nsyms) {
    if (syms[i].hash == hash && syms[i].len == len && !memcmp(syms[i].name, s, len)) return i;
    i = i + 1;
  }
  return -1;
}

int next() {
  char *pp; int hash; int i; int c;
  while ((tk = *p)) {
    p = p + 1;
    if (tk == '\n') { line = line + 1; }
    else if (tk == '#') { preprocess(); }
    else if ((tk >= 'a' && tk <= 'z') || (tk >= 'A' && tk <= 'Z') || tk == '_') {
      pp = p - 1; hash = tk;
      while ((*p >= 'a' && *p <= 'z') || (*p >= 'A' && *p <= 'Z') || *p == '_' || (*p >= '0' && *p <= '9')) { hash = hash * 147 + *p; p = p + 1; }
      hash = (hash & 1073741823) | (p - pp) << 6;
      i = lookup(pp, p - pp, hash);
      if (i < 0) {
        if (nsyms >= 8192) die("too many symbols");
        i = nsyms; nsyms = nsyms + 1;
        syms[i].name = pp; syms[i].len = p - pp; syms[i].hash = hash; syms[i].tk = Id;
        syms[i].class = 0; syms[i].fix = -1;
      }
      id = syms + i; tk = id->tk;
      return 0;
    }
    else if (tk >= '0' && tk <= '9') {
      if ((ival = tk - '0')) { while (*p >= '0' && *p <= '9') { ival = ival * 10 + *p - '0'; p = p + 1; } }
      else if (*p == 'x' || *p == 'X') {
        p = p + 1;
        while ((tk = *p) && ((tk >= '0' && tk <= '9') || (tk >= 'a' && tk <= 'f') || (tk >= 'A' && tk <= 'F'))) {
          ival = ival * 16 + (tk & 15) + (tk >= 'A' ? 9 : 0); p = p + 1;
        }
      }
      else { while (*p >= '0' && *p <= '7') { ival = ival * 8 + *p - '0'; p = p + 1; } }
      numty = TINT;                          // 10u 10L 10UL; 0x80000000 and up do not fit an int
      while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') { if (*p == 'u' || *p == 'U') numty = TUINT; p = p + 1; }
      if (ival < 0) numty = TUINT;
      tk = Num; return 0;
    }
    else if (tk == '/') {
      if (*p == '/') { while (*p && *p != '\n') p = p + 1; }
      else if (*p == '*') { p = p + 1; while (*p && !(*p == '*' && p[1] == '/')) { if (*p == '\n') line = line + 1; p = p + 1; } if (*p) p = p + 2; }
      else if (*p == '=') { p = p + 1; tk = DivA; return 0; }
      else { tk = Div; return 0; }
    }
    else if (tk == '"' || tk == '\'') {
      pp = data + dpos;
      while (*p && *p != tk) {
        if ((c = *p) == '\\') {
          p = p + 1; c = *p;
          // numeric values only: this file must compile with a cc that lacks these escapes
          if (c == 'n') c = 10; else if (c == 't') c = 9; else if (c == 'r') c = 13;
          else if (c == 'b') c = 8; else if (c == 'a') c = 7; else if (c == 'f') c = 12; else if (c == 'v') c = 11;
          else if (c == 'e') c = 27;
          else if (c >= '0' && c <= '7') {     // \0 \33 \033: up to 3 octal digits
            c = c - '0'; i = 1;
            while (i < 3 && p[1] >= '0' && p[1] <= '7') { p = p + 1; c = c * 8 + *p - '0'; i = i + 1; }
          }
          else if (c == 'x') {                 // \x1b: hex digits
            c = 0;
            while ((p[1] >= '0' && p[1] <= '9') || (p[1] >= 'a' && p[1] <= 'f') || (p[1] >= 'A' && p[1] <= 'F')) {
              p = p + 1; c = c * 16 + (*p & 15) + (*p >= 'A' ? 9 : 0);
            }
          }
        }
        p = p + 1;
        if (tk == '"') { data[dpos] = c; dpos = dpos + 1; } else ival = c;
      }
      p = p + 1;
      if (tk == '"') { data[dpos] = 0; dpos = dpos + 1; ival = pp - data; slen = dpos - ival; tk = Str; } else { tk = Num; numty = TINT; }
      return 0;
    }
    else if (tk == '=') { if (*p == '=') { p = p + 1; tk = Eq; } else tk = Assign; return 0; }
    else if (tk == '+') { if (*p == '+') { p = p + 1; tk = Inc; } else if (*p == '=') { p = p + 1; tk = AddA; } else tk = Add; return 0; }
    else if (tk == '-') { if (*p == '-') { p = p + 1; tk = Dec; } else if (*p == '>') { p = p + 1; tk = Arrow; } else if (*p == '=') { p = p + 1; tk = SubA; } else tk = Sub; return 0; }
    else if (tk == '!') { if (*p == '=') { p = p + 1; tk = Ne; } return 0; }
    else if (tk == '<') { if (*p == '=') { p = p + 1; tk = Le; } else if (*p == '<') { p = p + 1; if (*p == '=') { p = p + 1; tk = ShlA; } else tk = Shl; } else tk = Lt; return 0; }
    else if (tk == '>') { if (*p == '=') { p = p + 1; tk = Ge; } else if (*p == '>') { p = p + 1; if (*p == '=') { p = p + 1; tk = ShrA; } else tk = Shr; } else tk = Gt; return 0; }
    else if (tk == '|') { if (*p == '|') { p = p + 1; tk = Lor; } else if (*p == '=') { p = p + 1; tk = OrA; } else tk = Or; return 0; }
    else if (tk == '&') { if (*p == '&') { p = p + 1; tk = Lan; } else if (*p == '=') { p = p + 1; tk = AndA; } else tk = And; return 0; }
    else if (tk == '^') { if (*p == '=') { p = p + 1; tk = XorA; } else tk = Xor; return 0; }
    else if (tk == '%') { if (*p == '=') { p = p + 1; tk = ModA; } else tk = Mod; return 0; }
    else if (tk == '*') { if (*p == '=') { p = p + 1; tk = MulA; } else tk = Mul; return 0; }
    else if (tk == '[') { tk = Brak; return 0; }
    else if (tk == '?') { tk = Cond; return 0; }
    else if (tk == '.') { tk = Dot; return 0; }
    else if (tk == '~' || tk == ';' || tk == '{' || tk == '}' || tk == '(' || tk == ')' || tk == ']' || tk == ',' || tk == ':') return 0;
  }
  // end of this file: pop include stack
  if (fdepth > 0) { fdepth = fdepth - 1; p = fstack_p[fdepth]; line = fstack_line[fdepth]; return next(); }
  return 0;
}

int preprocess() {
  char *name; char *q; int n; int sv; struct Sym *d;
  while (*p == ' ' || *p == '\t') p = p + 1;
  if (!memcmp(p, "include", 7)) {
    p = p + 7;
    while (*p == ' ' || *p == '\t') p = p + 1;
    if (*p == '"') {
      p = p + 1; name = p;
      while (*p && *p != '"') p = p + 1;
      n = p - name; q = malloc(n + 1); memcpy(q, name, n); q[n] = 0;
      if (*p) p = p + 1;
      while (*p && *p != '\n') p = p + 1;
      if (fdepth >= 8) die("include nesting too deep");
      fstack_p[fdepth] = p; fstack_line[fdepth] = line; fdepth = fdepth + 1;
      p = readfile(q); line = 1;
      return 0;
    }
    // <system header>: ignored, builtins cover it
  }
  else if (!memcmp(p, "define", 6)) {
    p = p + 6;
    sv = line;
    next();                                   // identifier
    if (tk != Id) die("bad #define");
    d = id;
    while (*p == ' ' || *p == '\t') p = p + 1;
    if (*p >= '0' && *p <= '9') { next(); d->class = CNum; d->type = numty; d->val = ival; }
    line = sv;
  }
  while (*p && *p != '\n') p = p + 1;
  return 0;
}

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
  if (t == TINT || t == TUINT) return 4;
  return stsize[t - TSTRUCT];
}

// parse a base type (optionally with struct definition) and pointer stars
int is_type() { return tk == Int || tk == Char || tk == Void || tk == Struct || tk == Long || tk == Short || tk == Signed || tk == Unsigned || (tk == Id && id->class == 8); }

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
  if (tk == Num) darr = ival; else if (tk == Id && id->class == CNum) darr = id->val; else die("array size expected");
  next(); if (tk != ']') die("] expected"); next();
  return 0;
}
// Declarator after a base type: stars, then a name (or none, for casts and
// prototypes) and optional [N][M]..., or ( *name[N] ) followed by ( params ) for
// a function pointer or by [M]... for a pointer to an array.
// Returns the type (of an element, for arrays); sets dname (0 if no name) and
// darr (outermost length, -1 if not an array, -2 for []).
int declarator(int t) {
  int ps; int inner;
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
    skip_params();
    return fntype(t) + ps * PTR;
  }
  if (tk == Id) { dname = id; next(); }
  if (tk == Brak) { parse_dims(); t = rows_of(t, 1); darr = dims[0]; }
  return t;
}

// ------------------------------------------------------------------ expressions
// Emit a user function call: args already pushed, n args.
int call_fn(struct Sym *f, int n) {
  int at;
  at = bl_w();
  if (f->class == CFun && f->val >= 0) patch_b(at, f->val);
  else { f->fix = chain_add(f->fix, at); if (f->class == 0) { f->class = CFun; f->val = -1; f->type = TINT; } }
  if (n) emit16(45056 | n);                     // add sp,#4n  B000|n
  return 0;
}
// r0 = address of function f (Thumb bit set), PC relative: movw/movt r0,#off ; add r0,pc.
// The offset is patched at the end, when every function has an address.
int fn_addr(struct Sym *f) {
  if (f->class == CSys) die("cannot take the address of a builtin");
  if (f->class == 0) { f->class = CFun; f->val = -1; f->type = TINT; }
  if (naddr >= 4096) die("too many function addresses");
  afix_at[naddr] = cpos; afix_sym[naddr] = f; naddr = naddr + 1;
  imm2(0, 0);
  emit16(17528);                                // add r0,pc        4478
  return 0;
}
// Call through a function address in r0: keep it on the stack under the args,
// then ldr r1,[sp,#4n] ; blx r1 ; pop args and the address.
int call_ind(int t) {
  int n;
  push(); next(); n = 0;
  while (tk != ')') { expr(Assign); push(); n = n + 1; if (tk == ',') next(); }
  next();
  if (n > 126) die("too many arguments");
  emit16(39168 | n);                            // ldr r1,[sp,#4n]  9900|n
  emit16(18312);                                // blx r1           4788
  emit16(45056 | (n + 1));                      // add sp,#4(n+1)
  ty = fnret_of(t);
  return 0;
}
// Emit a host syscall: r0 = num, r1 = nargs, r2 = argv (argv[n-1] is arg 0)
int call_sys(int num, int n) {
  emit16(18026);                                // mov r2,sp        466A
  emit16(46096);                                // push {r4}        B410
  emit16(18028);                                // mov r4,sp        466C
  emit16(18027);                                // mov r3,sp        466B
  emit32(61475, 775);                           // bic.w r3,r3,#7   F023 0307
  emit16(18077);                                // mov sp,r3        469D
  emit16(8448 | n);                             // movs r1,#n
  emit16(8192 | num);                           // movs r0,#num
  emit16(18368);                                // blx r8           47C0
  emit16(18085);                                // mov sp,r4        46A5
  emit16(48144);                                // pop {r4}         BC10
  if (n) emit16(45056 | n);                     // add sp,#4n
  return 0;
}

// scale r0 by n (for pointer arithmetic)
int scale(int n) {
  if (n == 1) return 0;
  if (n == 4) return emit16(128);               // lsls r0,r0,#2   0080
  imm(2, n); return emit16(17232);              // muls r0,r2      4350
}

// Roll back the last load so r0 holds an address, for lvalue use.
int lvalue() {
  if (lastload) { cpos = cpos - lastload; lastload = 0; }
  else if (!(ty >= TSTRUCT && ty < PTR)) die("lvalue expected");
  return 0;
}
// scale r1 by n
int scale1(int n) {
  if (n == 1) return 0;
  if (n == 4) return emit16(137);               // lsls r1,r1,#2   0089
  imm(2, n); return emit16(17233);              // muls r1,r2      4351
}

// r0 = r1 op r0 where r1 has type t and r0 has type ty; sets ty to the result type
int binop(int op, int t) {
  int u;
  u = isuns(t, ty);
  if (op == Or)  { emit16(17160); ty = arith(t, ty); }             // orrs r0,r1
  else if (op == Xor) { emit16(16456); ty = arith(t, ty); }        // eors r0,r1
  else if (op == And) { emit16(16392); ty = arith(t, ty); }        // ands r0,r1
  else if (op == Shl) { emit32(64001, 61440); ty = arith(t, t); }  // lsl.w r0,r1,r0
  else if (op == Shr) {                                            // the left operand decides
    if (t == TUINT) emit32(64033, 61440); else emit32(64065, 61440); // lsr.w / asr.w r0,r1,r0
    ty = arith(t, t);
  }
  else if (op == Add) {
    if (t >= PTR && ty < PTR) { scale(tysize(t - PTR)); ty = t; }
    else if (ty >= PTR && t < PTR) scale1(tysize(ty - PTR));
    else if (t < PTR && ty < PTR) ty = arith(t, ty);
    emit16(6152);                                                  // adds r0,r1,r0
  }
  else if (op == Sub) {
    if (t >= PTR && ty >= PTR) { emit16(6664); imm(2, tysize(t - PTR)); emit32(64400, 61682); ty = TINT; }  // subs ; sdiv r0,r0,r2
    else { if (t >= PTR) scale(tysize(t - PTR)); emit16(6664); ty = t < PTR ? arith(t, ty) : t; }      // subs r0,r1,r0
  }
  else if (op == Mul) { emit16(17224); ty = arith(t, ty); }        // muls r0,r1
  else if (op == Div) { if (u) emit32(64433, 61680); else emit32(64401, 61680); ty = arith(t, ty); }   // udiv / sdiv r0,r1,r0
  else if (op == Mod) {                                            // udiv / sdiv r2,r1,r0 ; mls r0,r2,r0,r1
    if (u) emit32(64433, 62192); else emit32(64401, 62192);
    emit32(64258, 4112); ty = arith(t, ty);
  }
  return 0;
}

int expr(int lev) {
  int t; int n; int at; int at2; int m; int sz; int tmp; struct Sym *d;

  if (!tk) die("unexpected eof in expression");
  else if (tk == Num) { imm(0, ival); ty = numty; next(); }
  else if (tk == Str) { lea_glo(ival); next(); ty = TCHAR + PTR; }
  else if (tk == Sizeof) {
    next(); if (tk != '(') die("( expected after sizeof"); next();
    t = declarator(parse_type());
    if (tk != ')') die(") expected"); next();
    n = tysize(t); if (darr >= 0) n = n * darr;
    imm(0, n); ty = TINT;
  }
  else if (tk == Id) {
    d = id; next();
    if (tk == '(' && (d->class == CSys || d->class == CFun || d->class == 0)) {   // direct call
      next(); n = 0;
      while (tk != ')') { expr(Assign); push(); n = n + 1; if (tk == ',') next(); }
      next();
      if (d->class == CSys) call_sys(d->val, n);
      else if (d->class == CFun || d->class == 0) call_fn(d, n);
      else die("bad function call");
      ty = d->type;
    }
    else if (d->class == CNum) { imm(0, d->val); ty = d->type; }
    else if (d->class == 6) { lea_glo(d->val); ty = d->type; }   // global array: address
    else if (d->class == CFun || d->class == CSys) { fn_addr(d); ty = fntype(d->type); }   // function as a value
    else {
      if (d->class == CLoc) lea_loc(d->val);
      else if (d->class == CGlo) lea_glo(d->val);
      else die("undefined variable");
      ty = d->type;
      load(ty);
    }
  }
  else if (tk == '(') {
    next();
    if (is_type()) {                          // cast
      t = declarator(parse_type());
      if (tk != ')') die(") expected in cast"); next();
      expr(Inc); conv(t); ty = t;
    } else { expr(Assign); if (tk != ')') die(") expected"); next(); }
  }
  else if (tk == Mul) {                         // dereference
    next(); expr(Inc);
    if (isfndes(ty)) {}                         // *f is f
    else {
      if (ty < PTR) die("bad dereference");
      ty = ty - PTR; load(ty); decay();
    }
  }
  else if (tk == And) {                         // address-of
    next(); expr(Inc);
    if (isfndes(ty)) ty = ty + PTR;             // &f: the address is already in r0
    else { lvalue(); ty = ty + PTR; }
  }
  else if (tk == '!') { next(); expr(Inc); emit16(10240); emit16(48908); emit16(8193); emit16(8192); ty = TINT; } // cmp r0,#0; ite eq; mov 1; mov 0
  else if (tk == '~') { next(); expr(Inc); emit16(17344); ty = arith(ty, ty); }   // mvns r0,r0  43C0
  else if (tk == Add) { next(); expr(Inc); ty = arith(ty, ty); }
  else if (tk == Sub) {
    next();
    if (tk == Num) { imm(0, -ival); ty = numty; next(); } else { expr(Inc); emit16(16960); ty = arith(ty, ty); }   // negs r0,r0  4240
  }
  else if (tk == Inc || tk == Dec) {            // prefix
    t = tk; next(); expr(Inc); lvalue();
    sz = ty >= PTR ? tysize(ty - PTR) : 1;
    emit16(17921);                              // mov r1,r0        4601
    load_r1(ty);
    if (t == Dec) sz = -sz;
    if (sz >= 0 && sz < 4096) addi12(0, 0, sz); else if (sz < 0 && -sz < 4096) subi12(0, 0, -sz); else { imm(2, sz); emit16(6272); } // adds r0,r0,r2 1880
    store(ty);
  }
  else die("bad expression");

  // binary operators, precedence climbing
  while (tk >= lev || (tk == '(' && isfn(ty))) {   // a call binds tightest, like [ and ->
    t = ty;
    if (tk == '(') call_ind(t);
    else if (tk == Assign) {
      next(); lvalue(); push(); expr(Assign); pop1(); store(t); ty = t;
    }
    else if (tk == Cond) {
      next(); emit16(10240); at = bc_w(0);      // cmp r0,#0 ; beq
      expr(Assign);
      if (tk != ':') die(": expected"); next();
      at2 = b_w(-1); patch_b(at, cpos);
      expr(Cond); patch_b(at2, cpos);
    }
    else if (tk == Lor) { next(); emit16(10240); at = bc_w(1); expr(Lan); patch_b(at, cpos); ty = TINT; }
    else if (tk == Lan) { next(); emit16(10240); at = bc_w(0); expr(Or); patch_b(at, cpos); ty = TINT; }
    else if (tk >= AddA && tk <= ShrA) {        // compound assignment
      n = tk; next(); lvalue(); push(); load(t); push(); expr(Assign); pop1();
      if (n == AddA) n = Add; else if (n == SubA) n = Sub; else if (n == MulA) n = Mul; else if (n == DivA) n = Div; else if (n == ModA) n = Mod;
      else if (n == AndA) n = And; else if (n == OrA) n = Or; else if (n == XorA) n = Xor; else if (n == ShlA) n = Shl; else n = Shr;
      binop(n, t);                              // r0 = old op rhs
      pop1(); store(t); ty = t;
    }
    else if (tk == Or)  { next(); push(); expr(Xor); pop1(); binop(Or, t); }
    else if (tk == Xor) { next(); push(); expr(And); pop1(); binop(Xor, t); }
    else if (tk == And) { next(); push(); expr(Eq);  pop1(); binop(And, t); }
    else if (tk == Eq)  { next(); push(); expr(Lt);  pop1(); setcc(0);  ty = TINT; }
    else if (tk == Ne)  { next(); push(); expr(Lt);  pop1(); setcc(1);  ty = TINT; }
    else if (tk == Lt)  { next(); push(); expr(Shl); pop1(); setcc(isuns(t, ty) ? 3 : 11); ty = TINT; }   // lo / lt
    else if (tk == Gt)  { next(); push(); expr(Shl); pop1(); setcc(isuns(t, ty) ? 8 : 12); ty = TINT; }   // hi / gt
    else if (tk == Le)  { next(); push(); expr(Shl); pop1(); setcc(isuns(t, ty) ? 9 : 13); ty = TINT; }   // ls / le
    else if (tk == Ge)  { next(); push(); expr(Shl); pop1(); setcc(isuns(t, ty) ? 2 : 10); ty = TINT; }   // hs / ge
    else if (tk == Shl) { next(); push(); expr(Add); pop1(); binop(Shl, t); }
    else if (tk == Shr) { next(); push(); expr(Add); pop1(); binop(Shr, t); }
    else if (tk == Add) { next(); push(); expr(Mul); pop1(); binop(Add, t); }
    else if (tk == Sub) { next(); push(); expr(Mul); pop1(); binop(Sub, t); }
    else if (tk == Mul) { next(); push(); expr(Inc); pop1(); binop(Mul, t); }
    else if (tk == Div) { next(); push(); expr(Inc); pop1(); binop(Div, t); }
    else if (tk == Mod) { next(); push(); expr(Inc); pop1(); binop(Mod, t); }
    else if (tk == Inc || tk == Dec) {          // postfix
      lvalue();
      sz = ty >= PTR ? tysize(ty - PTR) : 1;
      if (tk == Dec) sz = -sz;
      emit16(17921);                            // mov r1,r0
      load_r1(ty);
      if (sz >= 0 && sz < 4096) addi12(2, 0, sz); else if (sz < 0 && -sz < 4096) subi12(2, 0, -sz); else { imm(2, sz); emit16(6274); } // adds r2,r0,r2 1882
      store_r2(ty);
      next();
    }
    else if (tk == Dot || tk == Arrow) {
      if (tk == Arrow) { if (ty < PTR) die("-> on non-pointer"); ty = ty - PTR; }
      next(); if (tk != Id) die("member expected");
      m = find_mem(ty); next();
      if (mems[m].off) addi12(0, 0, mems[m].off);
      ty = mems[m].type;
      if (ty & ARR) ty = ty & ~ARR; else load(ty);
    }
    else if (tk == Brak) {
      next(); push(); expr(Assign); pop1();
      if (tk != ']') die("] expected"); next();
      if (t < PTR) die("indexing a non-pointer");
      scale(tysize(t - PTR));
      emit16(6152);                             // adds r0,r1,r0
      ty = t - PTR; load(ty); decay();
    }
    else die("bad operator");
  }
  return 0;
}

// ------------------------------------------------------------------ initializers
// Globals: constants go straight into the data image. Anything else (strings for
// pointers, addresses, function pointers, expressions) becomes code in a chunk
// that runs before main: chunks are chained with b.w, see main(). Locals: code at
// the declaration; arrays and structs are zeroed first. Nested aggregates need
// their own braces; a char array also takes a string.
int save_lex() { sv_p = p; sv_tk = tk; sv_ival = ival; sv_id = id; sv_line = line; sv_dpos = dpos; return 0; }
int restore_lex() {                    // lexing strings wrote to data: clear what lookahead used
  if (dpos > sv_dpos) memset(data + sv_dpos, 0, dpos - sv_dpos);
  p = sv_p; tk = sv_tk; ival = sv_ival; id = sv_id; line = sv_line; dpos = sv_dpos;
  return 0;
}
int init_addr(int off) { if (ikind) lea_loc(ibase + off); else lea_glo(ibase + off); return 0; }
int init_code() {                      // make sure global initializer code goes into a chunk
  if (ikind || init_open) return 0;
  if (init_tail >= 0) patch_b(init_tail, cpos); else init_head = cpos;
  init_open = 1;
  return 0;
}
int init_close() { if (init_open) { init_tail = b_w(-1); init_open = 0; } return 0; }
int poke(int off, int v, int t) {
  data[off] = v;
  if (tysize(t) >= 2) data[off + 1] = v >> 8;
  if (tysize(t) == 4) { data[off + 2] = v >> 16; data[off + 3] = v >> 24; }
  return 0;
}
// [-] number | enum or #define name, followed by , } or ;  ->  1, value in ival. Else 0, nothing consumed.
int const_init() {
  int neg; int v;
  save_lex(); neg = 0;
  if (tk == Sub) { next(); neg = 1; }
  if (tk == Num) v = ival; else if (tk == Id && id->class == CNum) v = id->val; else { restore_lex(); return 0; }
  next();
  if (tk != ',' && tk != '}' && tk != ';') { restore_lex(); return 0; }
  ival = neg ? -v : v;
  return 1;
}
// number of elements in the { ... } list (or string) that starts here; consumes nothing
int count_init() {
  int depth; int paren; int n; int last;
  if (tk == Str) return slen;
  if (tk != '{') die("{ expected");
  save_lex();
  depth = 0; paren = 0; n = 0; last = 0;
  while (1) {
    if (!tk) die("unexpected eof in initializer");
    if (tk == '{') { depth = depth + 1; if (depth == 1) { next(); if (tk != '}') n = 1; continue; } }
    else if (tk == '}') { depth = depth - 1; if (!depth) break; }
    else if (tk == '(') paren = paren + 1;
    else if (tk == ')') paren = paren - 1;
    else if (tk == ',' && depth == 1 && !paren) n = n + 1;
    last = tk; next();
  }
  if (last == ',') n = n - 1;          // trailing comma
  restore_lex();
  return n;
}
int init_elem(int t, int off);
int init_scalar(int t, int off) {
  if (tk == '{') {                     // int x = { 5 };
    next(); init_scalar(t, off); if (tk == ',') next();
    if (tk != '}') die("} expected"); next();
    return 0;
  }
  if (!ikind && const_init()) { poke(ibase + off, ival, t); return 0; }
  init_code(); init_addr(off); push(); expr(Assign); pop1(); store(t);
  return 0;
}
int init_array(int et, int cnt, int off) {  // returns the number of elements given
  int i; int es; int n;
  es = tysize(et);
  if (tk == Str && (et == TCHAR || et == TUCHAR)) {   // char s[N] = "..."
    n = slen;                               // with the terminating 0
    if (n - 1 > cnt) die("string longer than the array");
    if (n > cnt) n = cnt;                   // char s[3] = "abc": no terminator, as in C
    if (ikind) {                            // memcpy(dst, literal, n)
      init_addr(off); push(); lea_glo(ival); push(); imm(0, n); push(); call_sys(SYS_memcpy, 3);
    }
    else { i = 0; while (i < n) { data[ibase + off + i] = data[ival + i]; i = i + 1; } }
    next();
    return n;
  }
  if (tk != '{') die("{ expected for array initializer");
  next(); i = 0;
  while (tk != '}') {
    if (i >= cnt) die("too many initializers");
    init_elem(et, off + i * es);
    i = i + 1;
    if (tk == ',') next(); else if (tk != '}') die(", or } expected");
  }
  next();
  return i;
}
int init_struct(int t, int off) {
  int order[256]; int n; int m; int mt;
  m = stfirst[t - TSTRUCT]; n = 0;            // members are listed last first
  while (m >= 0) { if (n >= 256) die("struct too big to initialize"); order[n] = m; n = n + 1; m = mems[m].next; }
  if (tk != '{') die("{ expected for struct initializer");
  next();
  while (tk != '}') {
    if (n <= 0) die("too many initializers");
    n = n - 1; m = order[n]; mt = mems[m].type;
    if (mt & ARR) init_array((mt & ~ARR) - PTR, mems[m].cnt, off + mems[m].off);
    else init_elem(mt, off + mems[m].off);
    if (tk == ',') next(); else if (tk != '}') die(", or } expected");
  }
  next();
  return 0;
}
int init_elem(int t, int off) {
  if (isrow(t)) { init_array(rowelem(t), rowcnt(t), off); return 0; }
  if (t >= TSTRUCT && t < PTR) return init_struct(t, off);
  return init_scalar(t, off);
}
int init_zero(int n) {                       // local aggregate: memset(storage, 0, n)
  init_addr(0); push(); imm(0, 0); push(); imm(0, n); push(); call_sys(SYS_memset, 3);
  return 0;
}

// ------------------------------------------------------------------ statements
int newlocal(struct Sym *s, int t, int size) {
  s->hclass = s->class; s->htype = s->type; s->hval = s->val;
  loc = loc + ((size + 3) & -4);
  if (loc > maxloc) maxloc = loc;
  s->class = CLoc; s->type = t; s->val = -loc;
  return 0;
}

int stmt() {
  int at; int at2; int sv_brk; int sv_cont; int sv_tgt; int body; int t; int bt; int n; int hi; struct Sym *d;
  if (tk == If) {
    next(); if (tk != '(') die("( expected"); next();
    expr(Assign); if (tk != ')') die(") expected"); next();
    emit16(10240); at = bc_w(0);                // cmp r0,#0 ; beq else
    stmt();
    if (tk == Else) { next(); at2 = b_w(-1); patch_b(at, cpos); stmt(); patch_b(at2, cpos); }
    else patch_b(at, cpos);
  }
  else if (tk == While) {
    next(); if (tk != '(') die("( expected"); next();
    sv_brk = brk_fix; sv_cont = cont_fix; sv_tgt = cont_tgt;
    brk_fix = -1; cont_fix = -1; cont_tgt = cpos;
    expr(Assign); if (tk != ')') die(") expected"); next();
    emit16(10240); at = bc_w(0);
    stmt();
    b_w(cont_tgt); patch_b(at, cpos);
    chain_patch(brk_fix, cpos, 36864);
    brk_fix = sv_brk; cont_fix = sv_cont; cont_tgt = sv_tgt;
  }
  else if (tk == For) {
    next(); if (tk != '(') die("( expected"); next();
    if (tk != ';') expr(Assign);
    if (tk != ';') die("; expected"); next();
    sv_brk = brk_fix; sv_cont = cont_fix; sv_tgt = cont_tgt;
    brk_fix = -1; cont_fix = -1;
    at = cpos;                                  // condition
    if (tk != ';') { expr(Assign); emit16(10240); at2 = bc_w(0); } else at2 = -1;
    if (tk != ';') die("; expected"); next();
    body = b_w(-1);                             // skip increment first time
    cont_tgt = cpos;                            // increment
    if (tk != ')') expr(Assign);
    if (tk != ')') die(") expected"); next();
    b_w(at);
    patch_b(body, cpos);
    stmt();
    b_w(cont_tgt);
    if (at2 >= 0) patch_b(at2, cpos);
    chain_patch(brk_fix, cpos, 36864);
    brk_fix = sv_brk; cont_fix = sv_cont; cont_tgt = sv_tgt;
  }
  else if (tk == Switch) {
    next(); if (tk != '(') die("( expected"); next();
    expr(Assign); if (tk != ')') die(") expected"); next();
    if (tk != '{') die("{ expected"); next();
    sv_brk = brk_fix; brk_fix = -1;
    loc = loc + 4; if (loc > maxloc) maxloc = loc; n = -loc;        // temp slot for the switch value
    push(); lea_loc(n); pop1(); emit16(24577);                    // str r1,[r0]
    at = b_w(-1);                                                 // -> first test (case chain head)
    at2 = -1;                                                     // default position
    body = -1;                                                    // fallthrough skip
    while (tk != '}') {
      if (tk == Case) {
        next();
        if (tk == Sub) { next(); t = -ival; } else if (tk == Num) t = ival; else if (tk == Id && id->class == CNum) t = id->val; else die("case constant expected");
        next(); if (tk != ':') die(": expected"); next();
        body = b_w(-1);                                           // previous body falls through over the test
        patch_b(at, cpos);                                        // failed previous test lands here
        lea_loc(n); emit16(26624); imm(1, t); emit16(17032);      // ldr r0,[r0] ; cmp r0,r1  4288
        at = bc_w(1);                                             // bne -> next test
        patch_b(body, cpos);
      }
      else if (tk == Default) { next(); if (tk != ':') die(": expected"); next(); at2 = cpos; }
      else { if (!tk) die("unexpected eof"); stmt(); }
    }
    next();
    if (at2 >= 0) patch_b(at, at2); else patch_b(at, cpos);
    chain_patch(brk_fix, cpos, 36864);
    brk_fix = sv_brk;
  }
  else if (tk == Break) { next(); if (tk != ';') die("; expected"); next(); brk_fix = chain_add(brk_fix, b_w(-1)); }
  else if (tk == Continue) { next(); if (tk != ';') die("; expected"); next(); b_w(cont_tgt); }
  else if (tk == Return) {
    next();
    if (tk != ';') { expr(Assign); conv(fnrty); }   // unsigned char f() { return 300; } gives 44
    if (tk != ';') die("; expected"); next();
    ret_fix = chain_add(ret_fix, b_w(-1));
  }
  else if (tk == '{') {
    next();
    while (tk != '}') { if (!tk) die("unexpected eof"); stmt(); }
    next();
  }
  else if (tk == ';') { next(); }
  else if (is_type()) {                       // local declaration
    bt = parse_type();
    while (tk != ';') {
      t = declarator(bt);
      if (!dname) die("local name expected");
      d = dname; hi = 0;
      if (tk == Assign) { next(); hi = 1; }
      if (darr == -2) { if (!hi) die("array size expected"); darr = count_init(); }
      n = tysize(t);
      if (darr >= 0) {                          // local array: slot holds pointer to the storage that follows it
        n = n * darr;
        newlocal(d, t + PTR, n + 4);
        lea_loc(d->val); addi12(1, 0, 4); emit16(24577);   // r0=slot; r1=r0+4; str r1,[r0]
        if (hi) { ikind = 1; ibase = d->val + 4; init_zero(n); init_array(t, darr, 0); }
      } else {
        newlocal(d, t, n);
        if (hi) {
          ikind = 1; ibase = d->val;
          if (t >= TSTRUCT && t < PTR) { init_zero(n); init_struct(t, 0); } else init_scalar(t, 0);
        }
      }
      if (tk == ',') next();
    }
    next();
  }
  else { expr(Assign); if (tk != ';') die("; expected"); next(); }
  return 0;
}

// ------------------------------------------------------------------ top level
int parse_global() {
  int bt; int t; struct Sym *s; int n; int i; int frame_at; int sz; int hi;
  if (tk == Enum) {
    next(); if (tk == Id) next();
    if (tk == '{') {
      next(); i = 0;
      while (tk != '}') {
        if (tk != Id) die("enum name expected");
        s = id; next();
        if (tk == Assign) { next(); if (tk == Sub) { next(); i = -ival; } else i = ival; next(); }
        s->class = CNum; s->type = TINT; s->val = i; i = i + 1;
        if (tk == ',') next();
      }
      next();
    }
    if (tk != ';') die("; expected"); next();
    return 0;
  }
  if (tk == Typedef) {
    next(); bt = declarator(parse_type());
    if (!dname || darr >= 0) die("typedef name expected");
    dname->class = 8; dname->type = bt;
    if (tk != ';') die("; expected"); next();
    return 0;
  }
  bt = parse_type();
  while (tk != ';') {
    t = declarator(bt);
    if (!dname) die("global name expected");
    s = dname;
    if (tk == '(' && darr < 0) {                // function (a function pointer's params were consumed)
      if (s->class == CFun && s->val >= 0) die("duplicate function");
      s->type = t; next();
      // prototype or definition? parse params either way
      n = 0; loc = 0; maxloc = 0;
      while (tk != ')') {
        bt = declarator(parse_type());
        if (darr != -1) bt = bt + PTR;          // int a[N] or a[] as a parameter is int *a
        if (dname) {
          if (dname->class == CLoc) die("duplicate param");
          dname->hclass = dname->class; dname->htype = dname->type; dname->hval = dname->val;
          dname->class = CLoc; dname->type = bt; dname->val = n;   // index, fixed up below
        }
        n = n + 1;
        if (tk == ',') next();
      }
      next();
      if (tk == ';') {                          // prototype
        next();
        i = 0; while (i < nsyms) { if (syms[i].class == CLoc) { syms[i].class = syms[i].hclass; syms[i].type = syms[i].htype; syms[i].val = syms[i].hval; } i = i + 1; }
        if (s->class != CFun) { s->class = CFun; s->val = -1; }
        return 0;
      }
      if (tk != '{') die("{ expected");
      // fix param offsets: param i at r7 + 8 + 4*(n-1-i)
      i = 0; while (i < nsyms) { if (syms[i].class == CLoc) syms[i].val = 8 + 4 * (n - 1 - syms[i].val); i = i + 1; }
      s->class = CFun; s->val = cpos; nparams = n; fnrty = t;
      chain_patch(s->fix, cpos, 53248); s->fix = -1;
      emit16(46464);                            // push {r7,lr}      B580
      emit16(18031);                            // mov r7,sp         466F
      frame_at = cpos; imm2(1, 0);              // movw+movt r1,#frame (patched)
      emit32(60333, 3329);                      // sub.w sp,sp,r1    EBAD 0D01
      ret_fix = -1;
      stmt();
      chain_patch(ret_fix, cpos, 36864);
      emit16(18109);                            // mov sp,r7         46BD
      emit16(48512);                            // pop {r7,pc}       BD80
      sz = (maxloc + 7) & -8;
      i = cpos; cpos = frame_at; imm2(1, sz); cpos = i;
      // restore shadowed symbols
      i = 0; while (i < nsyms) { if (syms[i].class == CLoc) { syms[i].class = syms[i].hclass; syms[i].type = syms[i].htype; syms[i].val = syms[i].hval; } i = i + 1; }
      return 0;
    }
    // global variable
    if (s->class) die("duplicate global");
    hi = 0;
    if (tk == Assign) { next(); hi = 1; }
    if (darr == -2) { if (!hi) die("array size expected"); darr = count_init(); }
    n = tysize(t);
    if (darr >= 0) {
      n = n * darr;
      // global array: the symbol is the storage itself; treat name as pointer constant
      dpos = (dpos + 3) & -4;
      s->class = 6; s->type = t + PTR; s->val = dpos;     // val = address of storage
      dpos = dpos + n;
    } else {
      dpos = (dpos + 3) & -4;
      s->class = CGlo; s->type = t; s->val = dpos;
      dpos = dpos + n;
    }
    if (hi) {
      ikind = 0; ibase = s->val;
      if (darr >= 0) init_array(t, darr, 0); else init_elem(t, 0);
      init_close();
    }
    if (tk == ',') next();
  }
  next();
  return 0;
}

int main(int argc, char **argv) {
  char *out; int fd; int i; int n; int hdr; int entry; struct Sym *s; struct Sym *d;
  out = "a.bin"; srcname = 0; incdir = 0;
  i = 1;
  while (i < argc) {
    if (!strcmp(argv[i], "-o")) { out = argv[i + 1]; i = i + 2; }
    else if (!strcmp(argv[i], "-I")) { incdir = argv[i + 1]; i = i + 2; }
    else { srcname = argv[i]; i = i + 1; }
  }
  if (!srcname) { printf("usage: cc [-o out.bin] [-I incdir] file.c\n"); return 1; }

  syms = malloc(8192 * sizeof(struct Sym)); nsyms = 0;
  mems = malloc(2048 * sizeof(struct Mem)); nmems = 0;
  stsize = malloc(256 * 4); stfirst = malloc(256 * 4); nstructs = 0;
  starr = malloc(256 * 4); stcnt = malloc(256 * 4); dims = malloc(8 * 4);
  code = malloc(1048576); cpos = 0;
  data = malloc(1048576); dpos = 0;
  fstack_p = malloc(8 * 4); fstack_line = malloc(8 * 4); fdepth = 0;
  fnret = malloc(512 * 4); nfn = 0;
  init_open = 0; init_head = -1; init_tail = -1;
  afix_at = malloc(4096 * 4); afix_sym = malloc(4096 * 4); naddr = 0;
  memset(syms, 0, 8192 * sizeof(struct Sym));

  // keywords, in token enum order
  p = "break case char continue default else enum for if int return sizeof struct switch typedef void while long short signed unsigned";
  i = Break; while (i <= Unsigned) { next(); id->tk = i; i = i + 1; }
  // builtin host functions: name, syscall number (see sys.h)
  p = "exit 0 exec 1 millis 2 sleep 3 sysinfo 4 putchar 16 getchar 17 kbhit 18 conctl 19 redirect 20 open 32 close 33 read 34 write 35 seek 36 stat 37 unlink 38 rename 39 mkdir 40 rmdir 41 opendir 42 readdir 43 closedir 44 chdir 45 getcwd 46 malloc 64 free 65 memset 66 memcmp 67 memcpy 68 strlen 69 strcmp 70 strcpy 71 printf 72 sprintf 73 atoi 74 strncmp 75 strchr 76 net_join 80 net_status 81 net_ip 82 connect 83 send 84 recv 85 listen 86 accept 87 sock_avail 88 gpio_mode 96 gpio_write 97 gpio_read 98 analog_read 99 pwm_write 100 i2c_write 101 i2c_read 102 spi_transfer 103 delay_us 104";
  next();
  while (tk == Id) {
    s = id; next(); s->class = CSys; s->val = ival; s->type = TINT;
    if (ival == SYS_malloc || ival == SYS_strcpy || ival == SYS_strchr || ival == SYS_getcwd || ival == SYS_net_ip) s->type = TCHAR + PTR;
    next();
  }
  p = "main"; next(); s = id;

  p = readfile(srcname); line = 1;
  next();
  while (tk) parse_global();

  // check for unresolved functions
  i = 0; while (i < nsyms) { if (syms[i].class == CFun && syms[i].val < 0 && syms[i].fix >= 0) { printf("undefined function: %s\n", syms[i].name); exit(1); } i = i + 1; }
  if (s->class != CFun || s->val < 0) die("no main()");
  // function addresses: movw/movt r0,#off at afix_at, then add r0,pc; pc reads as afix_at + 12
  i = 0;
  while (i < naddr) {
    d = afix_sym[i];
    if (d->class != CFun || d->val < 0) {
      printf("undefined function: "); n = 0; while (n < d->len) { putchar(d->name[n]); n = n + 1; } printf("\n"); exit(1);
    }
    n = cpos; cpos = afix_at[i]; imm2(0, d->val + 1 - (afix_at[i] + 12)); cpos = n;
    i = i + 1;
  }

  fd = open(out, 577, 420);    // O_WRONLY|O_CREAT|O_TRUNC, 0644
  if (fd < 0) { printf("cannot write %s\n", out); return 1; }
  hdr = 0; write(fd, "T2CC", 4);
  // global initializer chunks: entry = push {r4,lr} ; b chunks ; (last chunk) pop {r4,lr} ; b main
  entry = s->val;
  if (init_head >= 0) {
    entry = cpos;
    emit16(46352);                            // push {r4,lr}     B510
    b_w(init_head);
    patch_b(init_tail, cpos);
    emit32(59581, 16400);                     // pop.w {r4,lr}    E8BD 4010
    b_w(s->val);
  }
  write(fd, &cpos, 4); write(fd, &dpos, 4); write(fd, &hdr, 4); write(fd, &entry, 4);
  write(fd, code, cpos); write(fd, data, dpos);
  close(fd);
  printf("%s: %d bytes code, %d bytes data, entry %d\n", out, cpos, dpos, entry);
  return 0;
}
