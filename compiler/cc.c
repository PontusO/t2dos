// cc.c - a tiny single-pass C compiler emitting Thumb-2 assembly for Cortex-M33
// (RP2350). Written in the subset of C it compiles, so it can build itself.
//
// Build on a PC as a cross compiler:   gcc -O2 -I . -o compiler/cc compiler/cc.c   (from the repo root)
// Use:  cc [-o out] [-S|-c] [-mshort-data] [-I dir] [-D name[=v]] [-L dir] [-l name] file.c|file.s|file.o|file.a...
//   cc compiles each .c to assembly itself, then runs as on it and ld on all the objects
//   (/bin/as.bin and /bin/ld.bin on the board; on the PC build/host/as and build/host/ld,
//   or the ones in $T2_TOOLS). -S stops at the .s, -c at the .o; else the output is a .bin.
//
// Generated code model:
//   r7 = frame pointer, r8 = host syscall entry, r9 = data/bss base.
//   Expression results in r0.  All args passed on the stack, left to right
//   (first arg deepest), caller pops.  Position independent: data is addressed
//   from r9 (NAME+offset for a global of this file, .Ldata+offset for string
//   literals), function addresses PC relative (fnaddr).

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>

// ------------------------------------------------------------------ tokens
enum { Num = 128, Str, Id,
  Break, Case, Char, Continue, Default, Else, Enum, For, If, Int, Return, Sizeof, Struct, Switch, Typedef, Void, While,
  Long, Short, Signed, Unsigned, Float, Double,
  Assign, AddA, SubA, MulA, DivA, ModA, AndA, OrA, XorA, ShlA, ShrA, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr,
  Add, Sub, Mul, Div, Mod, Inc, Dec, Dot, Arrow, Brak,
  Extern };   // keywords added later go after the operators, whose order expr() relies on

// symbol classes
enum { CNum = 1, CFun, CSys, CGlo, CLoc };   // 6 = global array, 8 = typedef
// 7 = extern variable, 9 = extern array: no storage in this file, ld finds the symbol
// base types; struct i is TSTRUCT+i; each pointer level adds PTR
// FN marks a function type; bits 22.. index fnret[] (its return type). A function
// designator has 0 pointer levels, a function pointer 1.
enum { TCHAR = 0, TINT = 1, TUCHAR = 2, TSHORT = 3, TUSHORT = 4, TUINT = 5, TFLOAT = 6, TSTRUCT = 7, PTR = 256, ARR = 1048576, FN = 2097152 };

#include "sys.h"

// condition codes (it, b<cond>)
enum { CC_EQ = 0, CC_NE = 1, CC_HS = 2, CC_LO = 3, CC_MI = 4, CC_HI = 8, CC_LS = 9, CC_GE = 10, CC_LT = 11, CC_GT = 12, CC_LE = 13 };

struct Sym { char *name; int len; int hash; int tk; int class; int type; int val;
             int hclass; int htype; int hval; int stag;
             int mdef; char *mbody; char **margs; int mnargs; int mvar;
             int *ptypes; int nptypes; };   // parameter types of a function, for argument conversion   // macro: defined, body, params (-1: object-like), ...
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

char *data; int dpos;    // data segment (strings + globals)
int lastload;            // lines of the load instruction just emitted (for lvalues), 0 if none

int ty;                  // type of last parsed expression
int loc;                 // current local frame depth
int maxloc;
int ret_fix, brk_fix, cont_fix, cont_tgt;  // fixup chains (branch lines) / targets (labels)
int nbrk, ncont;                           // enclosing loops and switches / loops
int nparams;
int fnrty;               // return type of the function being compiled

int *fnret; int nfn;     // function types: return type of each
int **fnparams; int *fnnp; // and parameter types (fnnp -1: not known), for argument conversion
struct Sym *dname; int darr;                       // set by declarator(): name, array size, -1 none, -2 for []
int pb; int pb_tk; int pb_ival; struct Sym *pb_id; int pb_numty;   // one token pushed back by next()
int sv_pb; int sv_pb_tk; int sv_pb_ival; struct Sym *sv_pb_id; int sv_pb_numty; int sv_numty; int sv_slen;
int numty;                                         // type of the last number token: TINT or TUINT
int slen;                                          // bytes of the last string literal, with its 0
int ikind; int ibase; struct Sym *isym;           // initializer target: 0 = data offset (of global isym), 1 = frame offset
int init_open;                                     // global initializer code is being compiled (into .Linit)
char *sv_p; int sv_tk; int sv_ival; struct Sym *sv_id; int sv_line; int sv_dpos;   // lexer state for lookahead

// include stack
// input stack: included files and macro expansions (fstack_mac: the macro, 0 for a file)
char **fstack_p; int *fstack_line; struct Sym **fstack_mac; int fdepth;
char **sv_fp; int *sv_fl; struct Sym **sv_fm; int sv_fdepth;
char *mbuf;              // macro expansion is built here
int ifdepth;             // #if levels whose taken branch is being compiled
int pp_if;               // evaluating an #if: unknown identifiers are 0
char *srcname;
char *incdir;
int ofd; char *outname;  // the output file, removed again when compiling fails
char **srcbuf; int nsrcbuf;   // the source buffers readfile() read, freed when the file is compiled
char **tmpf; int ntmp;   // the driver's temporary files, removed when cc ends (also on an error)

int preprocess(); int lex(); int expand(struct Sym *m); int expanding(struct Sym *m); int intern(char *s, int n); int idstart(int c); int idchar(int c);
int patch_b(int at, int target); int expr(int lev); int stmt(); int next();
int declarator(int t); int die(char *msg); int expr_comma(); int itcc(int cond); int param_types(int *pl);

// ------------------------------------------------------------------ helpers
int cleanup() { int i; i = 0; while (i < ntmp) { unlink(tmpf[i]); i = i + 1; } ntmp = 0; return 0; }
int quit() { if (ofd >= 0) { close(ofd); unlink(outname); } cleanup(); exit(1); return 0; }
int die(char *msg) { printf("%s:%d: error: %s near token %d\n", srcname, line, msg, tk); quit(); return 0; }

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
  if (fd < 0) { printf("cannot open %s\n", name); quit(); }
  buf = malloc(1048576);
  if (nsrcbuf < 256) { srcbuf[nsrcbuf] = buf; nsrcbuf = nsrcbuf + 1; }
  total = 0;
  while ((n = read(fd, buf + total, 65536)) > 0) total = total + n;
  buf[total] = 0;
  close(fd);
  return buf;
}

// ------------------------------------------------------------------ assembly output
// The function being compiled is a list of lines, written out when it ends (global
// initializer code goes to .Linit instead): instructions as text, labels .L<n>, and
// branches b.w / b<cond>.w whose label is filled in once the target is known. Each
// instruction is spelled so that as picks the encoding cc has always used: .w where a
// 16-bit form exists but the 32-bit one is meant, addw/subw for the T4 add/sub, and
// mov<cond> inside an IT block. Label numbers are unique in the file.
enum { L_INS = 1, L_LAB, L_BR, L_DEL };
char *lpool; int lppos;                          // text of the lines, 0-terminated
int *lkind; int *ltext; int *llab; int *lcond; int *lnext; int nlines;
                                                 // per line: L_*, text, label (a branch's target, or the
                                                 // number of a label), branch cond (-1: b.w), fixup chain
int nlabels;                                     // .L numbers used so far
char *ccname; char *ccbuf;                       // condition names, two letters each
char *obuf; int opos;                            // the output file's buffer
char *ibuf; int ipos; int tobuf;                 // .Linit's code; tobuf: flush() appends to it
char *numbuf;
struct Sym **gsym; int *goff; int nglob;         // global variables: their data offsets, for the labels

int oflush() { if (opos) write(ofd, obuf, opos); opos = 0; return 0; }
int oput(char *s) {
  if (tobuf) {
    while (*s) { ibuf[ipos] = *s; ipos = ipos + 1; s = s + 1; }
    if (ipos > 258000) die("initializer code too big");
    return 0;
  }
  while (*s) { obuf[opos] = *s; opos = opos + 1; s = s + 1; }
  if (opos > 60000) oflush();
  return 0;
}
char *dec(int v) {                               // decimal text of v, valid until the next call
  char *q; int neg;
  q = numbuf + 15; *q = 0; neg = v < 0; if (neg) v = -v;
  while (1) { q = q - 1; *q = '0' + v % 10; v = v / 10; if (!v) break; }
  if (neg) { q = q - 1; *q = '-'; }
  return q;
}
char *ccn(int c) { ccbuf[0] = ccname[2 * c]; ccbuf[1] = ccname[2 * c + 1]; ccbuf[2] = 0; return ccbuf; }
int oname(struct Sym *s) {                       // a symbol's name (built past the end of the line text)
  int k;
  k = 0; while (k < s->len) { lpool[lppos + k] = s->name[k]; k = k + 1; }
  lpool[lppos + k] = 0;
  return oput(lpool + lppos);
}

// a new line; every line but a label ends lvalue()'s chance to drop a load
int lnew(int kind) {
  if (nlines >= 16384 || lppos > 258000) die("function too long");
  lkind[nlines] = kind; ltext[nlines] = lppos; llab[nlines] = -1; lcond[nlines] = -1; lnext[nlines] = -1;
  nlines = nlines + 1;
  if (kind != L_LAB) lastload = 0;
  return nlines - 1;
}
int ls(char *s) { while (*s) { lpool[lppos] = *s; lppos = lppos + 1; s = s + 1; } return 0; }
int lsym(struct Sym *s) { int k; k = 0; while (k < s->len) { lpool[lppos] = s->name[k]; lppos = lppos + 1; k = k + 1; } return 0; }
int lend() { lpool[lppos] = 0; lppos = lppos + 1; return 0; }
int ins(char *s) { lnew(L_INS); ls(s); lend(); return 0; }
int ins_ri(char *mn, int rd, int v) {            // mn rd, #v
  lnew(L_INS); ls(mn); ls("\tr"); ls(dec(rd)); ls(", #"); ls(dec(v)); lend();
  return 0;
}
int ins_rri(char *mn, int rd, int rn, int v) {   // mn rd, rn, #v
  lnew(L_INS); ls(mn); ls("\tr"); ls(dec(rd)); ls(", r"); ls(dec(rn)); ls(", #"); ls(dec(v)); lend();
  return 0;
}
int addsp(int n) { lnew(L_INS); ls("add\tsp, #"); ls(dec(4 * n)); lend(); return 0; }   // pop n words

// write the lines out (to the file, or to .Linit's code) and start a new list
int flush() {
  int i; int k;
  i = 0;
  while (i < nlines) {
    k = lkind[i];
    if (k == L_INS) { oput("\t"); oput(lpool + ltext[i]); oput("\n"); }
    else if (k == L_LAB) { oput(".L"); oput(dec(llab[i])); oput(":\n"); }
    else if (k == L_BR) {
      if (llab[i] < 0) die("internal error: branch without a target");
      oput("\tb"); if (lcond[i] >= 0) oput(ccn(lcond[i])); oput(".w\t.L"); oput(dec(llab[i])); oput("\n");
    }
    i = i + 1;
  }
  nlines = 0; lppos = 0;
  return 0;
}

int push() { return ins("push\t{r0}"); }
int pop1() { return ins("pop\t{r1}"); }

// movw/movt rd, #imm32 (no movt when the high half is 0)
int imm(int rd, int v) {
  int lo; int hi;
  lo = v & 65535; hi = (v >> 16) & 65535;
  ins_ri("movw", rd, lo);
  if (hi) ins_ri("movt", rd, hi);
  return 0;
}
// movw+movt, always both halves
int imm2(int rd, int v) {
  ins_ri("movw", rd, v & 65535);
  ins_ri("movt", rd, (v >> 16) & 65535);
  return 0;
}
// addw/subw rd, rn, #imm12 (T4 encodings)
int addi12(int rd, int rn, int v) { return ins_rri("addw", rd, rn, v); }
int subi12(int rd, int rn, int v) { return ins_rri("subw", rd, rn, v); }

// r0 = address of local/param at signed frame offset off
int lea_loc(int off) {
  if (off < 0) { off = -off; if (off < 4096) subi12(0, 7, off); else { imm(1, off); ins("subs\tr0, r7, r1"); } }
  else { if (off < 4096) addi12(0, 7, off); else { imm(1, off); ins("adds\tr0, r7, r1"); } }
  return 0;
}
// the data reference "NAME+n" (s a global of this file, n bytes into it) or ".Ldata+n" (s 0: a string literal)
int ldref(struct Sym *s, int n) {
  if (s) { lsym(s); if (n) { ls("+"); ls(dec(n)); } }
  else { ls(".Ldata+"); ls(dec(n)); }
  return 0;
}
// Where this file's data lands in the linked program, set by the driver before compile():
// dbase is the data offset ld will give it (the data of the objects before it, each aligned
// to 4, as ld lays them out), and dlong is 1 when that is unknown (-S and -c without
// -mshort-data: the object may be linked after any amount of data).
int dbase; int dlong;
// r0 = r9 + off (the data offset in this file): s + n, s a global of this file at off - n, or
// .Ldata + off when s is 0. The form is the smallest that fits the linked offset dbase + off;
// ld still checks it.
int lea_data(struct Sym *s, int n, int off) {
  if (!dlong) off = dbase + off;
  if (!dlong && off < 4096) { lnew(L_INS); ls("addw\tr0, r9, #"); ldref(s, n); lend(); return 0; }
  if (!dlong && off < 65536) { lnew(L_INS); ls("movw.16\tr1, #"); ldref(s, n); lend(); }
  else {
    lnew(L_INS); ls("movw\tr1, #:lower16:"); ldref(s, n); lend();
    lnew(L_INS); ls("movt\tr1, #:upper16:"); ldref(s, n); lend();
  }
  ins("add.w\tr0, r9, r1");
  return 0;
}
int lea_glo(int off) { return lea_data(0, off, off); }          // anonymous data (string literals)
int lea_var(struct Sym *s) { return lea_data(s, 0, s->val); }    // a global variable of this file
// r0 = r9 + the data offset of extern s, which only ld knows: movw/movt of the symbol
int lea_ext(struct Sym *s) {
  lnew(L_INS); ls("movw\tr1, #:lower16:"); lsym(s); lend();
  lnew(L_INS); ls("movt\tr1, #:upper16:"); lsym(s); lend();
  ins("add.w\tr0, r9, r1");
  return 0;
}
// function types
int fntype_p(int ret, int *pl, int np) {
  int k; int i; int same;
  k = 0;
  while (k < nfn) {
    if (fnret[k] == ret && fnnp[k] == np) {
      same = 1; i = 0; while (i < np) { if (fnparams[k][i] != pl[i]) same = 0; i = i + 1; }
      if (same) return FN | k << 22;
    }
    k = k + 1;
  }
  if (nfn >= 512) die("too many function types");
  fnret[nfn] = ret; fnnp[nfn] = np; fnparams[nfn] = 0;
  if (np > 0) { fnparams[nfn] = malloc(np * 4); i = 0; while (i < np) { fnparams[nfn][i] = pl[i]; i = i + 1; } }
  nfn = nfn + 1;
  return FN | k << 22;
}
int fntype(int ret) { return fntype_p(ret, 0, -1); }
int fnlevels(int t) { return (t >> 8) & 4095; }
int isfn(int t) { return (t & FN) && fnlevels(t) <= 1; }          // designator or pointer: callable
int isfndes(int t) { return (t & FN) && fnlevels(t) == 0; }       // designator: its value is its address
int fnret_of(int t) { return fnret[(t >> 22) & 511]; }
int fnnp_of(int t) { return fnnp[(t >> 22) & 511]; }
int fnparam_of(int t, int i) { return fnparams[(t >> 22) & 511][i]; }

// load r0 = *r0 according to type
int load(int t) {
  lastload = 0;                                              // set again below only when a load is emitted
  if (isfndes(t)) return 0;                                  // a function is its own address
  if (t == TCHAR) { ins("ldrsb.w\tr0, [r0]"); lastload = 1; }
  else if (t == TINT || t == TUINT || t == TFLOAT || t >= PTR) { ins("ldr\tr0, [r0]"); lastload = 1; }
  else if (t == TUCHAR) { ins("ldrb\tr0, [r0]"); lastload = 1; }
  else if (t == TSHORT) { ins("ldrsh.w\tr0, [r0]"); lastload = 1; }
  else if (t == TUSHORT) { ins("ldrh\tr0, [r0]"); lastload = 1; }
  // struct: keep address
  return 0;
}
// store *r1 = r0
int store(int t) {
  if (t == TCHAR || t == TUCHAR) ins("strb\tr0, [r1]"); else if (t == TSHORT || t == TUSHORT) ins("strh\tr0, [r1]"); else ins("str\tr0, [r1]");
  return 0;
}
// r0 = *r1 according to type (for ++ and --)
int load_r1(int t) {
  if (t == TCHAR) ins("ldrsb.w\tr0, [r1]");
  else if (t == TUCHAR) ins("ldrb\tr0, [r1]");
  else if (t == TSHORT) ins("ldrsh.w\tr0, [r1]");
  else if (t == TUSHORT) ins("ldrh\tr0, [r1]");
  else ins("ldr\tr0, [r1]");
  return 0;
}
// *r1 = r2 according to type
int store_r2(int t) {
  if (t == TCHAR || t == TUCHAR) ins("strb\tr2, [r1]"); else if (t == TSHORT || t == TUSHORT) ins("strh\tr2, [r1]"); else ins("str\tr2, [r1]");
  return 0;
}
// convert r0 to type t (casts, return values): sign or zero extend the small types
int conv(int t) {
  if (t == TCHAR) ins("sxtb\tr0, r0");
  else if (t == TUCHAR) ins("uxtb\tr0, r0");
  else if (t == TSHORT) ins("sxth\tr0, r0");
  else if (t == TUSHORT) ins("uxth\tr0, r0");
  return 0;
}
// usual arithmetic conversions: small types promote to int; unsigned int wins
int arith(int a, int b) { if (a == TFLOAT || b == TFLOAT) return TFLOAT; if (a == TUINT || b == TUINT) return TUINT; return TINT; }
int isuns(int a, int b) { return a < PTR && b < PTR && arith(a, b) == TUINT; }
int isflt(int t) { return t == TFLOAT; }
// r0 of type from as type to, where one of them is float (vcvt rounds toward zero)
int cvt(int from, int to) {
  if (from == to || from >= PTR || to >= PTR) return 0;
  if (isflt(to) && !isflt(from)) {
    ins("vmov\ts0, r0");
    if (from == TUINT) ins("vcvt.f32.u32\ts0, s0"); else ins("vcvt.f32.s32\ts0, s0");
    ins("vmov\tr0, s0");
  }
  else if (isflt(from) && !isflt(to)) {
    ins("vmov\ts0, r0");
    if (to == TUINT) ins("vcvt.u32.f32\ts0, s0"); else ins("vcvt.s32.f32\ts0, s0");
    ins("vmov\tr0, s0");
    conv(to);
  }
  return 0;
}
// assignment, initializer, argument: convert the value in r0 (type ty) for a t
int assignconv(int t) { if (isflt(ty) || isflt(t)) cvt(ty, t); return 0; }
// cast and return: also truncate to small integer types
int castto(int t) { if (isflt(ty) || isflt(t)) cvt(ty, t); else conv(t); return 0; }
// left (type t, in r1) and right (type ty, in r0) as floats in s0 and s1
int fpair(int t) {
  ins("vmov\ts1, r0");
  if (!isflt(ty)) { if (ty == TUINT) ins("vcvt.f32.u32\ts1, s1"); else ins("vcvt.f32.s32\ts1, s1"); }
  ins("vmov\ts0, r1");
  if (!isflt(t)) { if (t == TUINT) ins("vcvt.f32.u32\ts0, s0"); else ins("vcvt.f32.s32\ts0, s0"); }
  return 0;
}
// set flags for "is r0 true": cmp r0,#0, or for a float lsls r1,r0,#1 (+0 and -0 are false)
// flags for "is r0 true", with r0 left 0 if it is false (a float -0.0 becomes 0): for && and ||
int truth() { if (isflt(ty)) { test0(); itcc(CC_NE); } ins("cmp\tr0, #0"); return 0; }
int test0() { if (isflt(ty)) ins("lsls\tr1, r0, #1"); else ins("cmp\tr0, #0"); return 0; }

// branches, all b.w / b<cond>.w. A branch is a line; its target label is set when known.
int here() { int k; k = lnew(L_LAB); llab[k] = nlabels; nlabels = nlabels + 1; return llab[k]; }   // a label here
int b_w(int target) { int at; at = lnew(L_BR); llab[at] = target; return at; }   // target: a label, -1 for later
int bc_w(int cond) { int at; at = lnew(L_BR); lcond[at] = cond; return at; }
int patch_b(int at, int label) { llab[at] = label; return 0; }
int patch_here(int at) { return patch_b(at, here()); }
// fixup chains of branches (break, return) through lnext
int chain_add(int head, int at) { lnext[at] = head; return at; }
int chain_here(int head) {
  int nxt; int lab;
  if (head < 0) return 0;
  lab = here();
  while (head >= 0) { nxt = lnext[head]; llab[head] = lab; head = nxt; }
  return 0;
}

// r0 = cond ? 1 : 0, flags already set: ite cond ; mov<cond> r0,#1 ; mov<!cond> r0,#0
int itcc(int cond) {
  lnew(L_INS); ls("ite\t"); ls(ccn(cond)); lend();
  lnew(L_INS); ls("mov"); ls(ccn(cond)); ls("\tr0, #1"); lend();
  lnew(L_INS); ls("mov"); ls(ccn(cond ^ 1)); ls("\tr0, #0"); lend();
  return 0;
}
// compare r1 ? r0 -> r0 = 0/1
int setcc(int cond) { ins("cmp\tr1, r0"); return itcc(cond); }

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

int idstart(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
int idchar(int c) { return idstart(c) || (c >= '0' && c <= '9'); }
// the symbol for identifier s[0..n), created if new
int intern(char *s, int n) {
  int hash; int i; int k;
  hash = s[0]; k = 1;
  while (k < n) { hash = hash * 147 + s[k]; k = k + 1; }
  hash = (hash & 1073741823) | n << 6;
  i = lookup(s, n, hash);
  if (i < 0) {
    if (nsyms >= 8192) die("too many symbols");
    i = nsyms; nsyms = nsyms + 1;
    syms[i].name = s; syms[i].len = n; syms[i].hash = hash; syms[i].tk = Id;
    syms[i].class = 0;
  }
  return i;
}
char *sdup(char *a, char *b) {          // copy of a..b, newlines turned into spaces
  char *r; int n; int i;
  n = b - a; r = malloc(n + 1);
  i = 0; while (i < n) { r[i] = a[i] == '\n' ? ' ' : a[i]; i = i + 1; }
  r[n] = 0;
  return r;
}
char *trim(char *s) {
  int n;
  while (*s == ' ' || *s == '\t') s = s + 1;
  n = strlen(s); while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n = n - 1;
  s[n] = 0;
  return s;
}
int push_src(char *np, struct Sym *m) {
  if (fdepth >= 64) die("macro or include nesting too deep");
  fstack_p[fdepth] = p; fstack_line[fdepth] = line; fstack_mac[fdepth] = m; fdepth = fdepth + 1;
  p = np;
  return 0;
}
int expanding(struct Sym *m) {         // a macro is not expanded again inside its own expansion
  int k;
  k = 0; while (k < fdepth) { if (fstack_mac[k] == m) return 1; k = k + 1; }
  return 0;
}
int param_index(struct Sym *m, char *s, int n) {
  int k;
  k = 0;
  while (k < m->mnargs) { if (strlen(m->margs[k]) == n && !memcmp(m->margs[k], s, n)) return k; k = k + 1; }
  return -1;
}
int mput(char *o, int c) { if (o - mbuf >= 65500) die("macro expansion too long"); *o = c; return 0; }
// expansion buffers: every subst() and expand_text() call has its own, freed when done
char *bput(char *b, char *o, int c) { if (o - b >= 65500) die("macro expansion too long"); *o = c; return o + 1; }
char *bcat(char *b, char *o, char *s) { while (*s) { o = bput(b, o, *s); s = s + 1; } return o; }
char *blit(char *b, char *o, char **ps) {   // copy the literal at *ps ("..." or '...')
  char *s; int q;
  s = *ps; q = *s; o = bput(b, o, *s); s = s + 1;
  while (*s && *s != q) { if (*s == '\\' && s[1]) { o = bput(b, o, *s); s = s + 1; } o = bput(b, o, *s); s = s + 1; }
  if (*s) { o = bput(b, o, *s); s = s + 1; }
  *ps = s;
  return o;
}
struct Sym *find_sym(char *s, int n) {      // like intern() but never creates one (s may be a temporary)
  int hash; int i; int k;
  hash = s[0]; k = 1;
  while (k < n) { hash = hash * 147 + s[k]; k = k + 1; }
  i = lookup(s, n, (hash & 1073741823) | n << 6);
  if (i < 0) return 0;
  return syms + i;
}
// arguments of a call of m; *pq points at its '(' and ends after the ')'.
// nl: count newlines into line (reading the source itself)
char **collect_args(struct Sym *m, char **pq, int nl) {
  char *q; char **args; int n; int depth; char *start; int c;
  q = *pq + 1;
  args = malloc(4 * (m->mnargs + 1)); n = 0; depth = 0; start = q;
  while (1) {
    c = *q;
    if (!c) die("unterminated macro call");
    if (c == '"' || c == '\'') {
      q = q + 1;
      while (*q && *q != c) { if (*q == '\\' && q[1]) q = q + 1; q = q + 1; }
      if (*q) q = q + 1;
      continue;
    }
    if (c == '\n' && nl) line = line + 1;
    if (c == '(') depth = depth + 1;
    else if (c == ')') { if (!depth) break; depth = depth - 1; }
    else if (c == ',' && !depth && !(m->mvar && n == m->mnargs - 1)) {   // the ... argument keeps its commas
      if (n >= m->mnargs) die("too many macro arguments");
      args[n] = trim(sdup(start, q)); n = n + 1; start = q + 1;
    }
    q = q + 1;
  }
  if (n >= m->mnargs && !(m->mnargs == 0 && !*trim(sdup(start, q)))) die("too many macro arguments");
  if (m->mnargs > 0) { args[n] = trim(sdup(start, q)); n = n + 1; }
  if (n == m->mnargs - 1 && m->mvar) { args[n] = ""; n = n + 1; }   // f(x) for f(x, ...)
  if (n != m->mnargs) die("wrong number of macro arguments");
  *pq = q + 1;
  return args;
}
struct Sym **tstack; int tdepth;            // macros being expanded by expand_text()
int tactive(struct Sym *m) { int k; k = 0; while (k < tdepth) { if (tstack[k] == m) return 1; k = k + 1; } return 0; }
char *subst(struct Sym *m, char **args);
// s with all macros in it expanded: what C does to an argument before substituting it
char *expand_text(char *s) {
  char *b; char *o; char *w; char *r; char *q; char **args; struct Sym *m;
  b = malloc(65536); o = b;
  while (*s) {
    if (*s == '"' || *s == '\'') o = blit(b, o, &s);
    else if (*s >= '0' && *s <= '9') { while (idchar(*s)) { o = bput(b, o, *s); s = s + 1; } }   // 0x1f, 10u
    else if (idstart(*s)) {
      w = s; while (idchar(*s)) s = s + 1;
      m = find_sym(w, s - w); r = 0;
      if (m && m->mdef && !expanding(m) && !tactive(m)) {
        if (m->mnargs < 0) r = m->mbody;
        else {
          q = s; while (*q == ' ' || *q == '\t') q = q + 1;
          if (*q == '(') { args = collect_args(m, &q, 0); s = q; r = subst(m, args); }
        }
      }
      if (r) {                               // rescan the replacement with m disabled
        if (tdepth >= 64) die("macro nesting too deep");
        tstack[tdepth] = m; tdepth = tdepth + 1;
        r = expand_text(r);
        tdepth = tdepth - 1;
        o = bcat(b, o, r); free(r);
      }
      else while (w < s) { o = bput(b, o, *w); w = w + 1; }
    }
    else { o = bput(b, o, *s); s = s + 1; }
  }
  bput(b, o, 0);
  r = sdup(b, o); free(b);
  return r;
}
// body of m with params replaced by args; #x makes a string, a ## b joins.
// Arguments are macro-expanded first, except next to # and ##, as in C.
char *subst(struct Sym *m, char **args) {
  char *b; char *o; char *s; char *a; char *w; char *t; int k; int raw;
  b = malloc(65536); o = b; s = m->mbody; raw = 0;
  while (*s) {
    if (*s == '"' || *s == '\'') { o = blit(b, o, &s); raw = 0; }   // literals are copied as they are
    else if (*s == '#' && s[1] == '#') {
      while (o > b && (*(o - 1) == ' ' || *(o - 1) == '\t')) o = o - 1;
      s = s + 2; while (*s == ' ' || *s == '\t') s = s + 1;
      raw = 1;                                // the next parameter is pasted as written
    }
    else if (*s == '#') {
      s = s + 1; while (*s == ' ' || *s == '\t') s = s + 1;
      w = s; while (idchar(*s)) s = s + 1;
      k = param_index(m, w, s - w);
      if (k < 0) die("# must be followed by a macro parameter");
      o = bput(b, o, '"');
      a = args[k];
      while (*a) {                            // runs of white space become one space, as in C
        if (*a == ' ' || *a == '\t') { o = bput(b, o, ' '); while (*a == ' ' || *a == '\t') a = a + 1; continue; }
        if (*a == '"' || *a == '\\') o = bput(b, o, '\\');
        o = bput(b, o, *a); a = a + 1;
      }
      o = bput(b, o, '"'); raw = 0;
    }
    else if (idstart(*s)) {
      w = s; while (idchar(*s)) s = s + 1;
      k = param_index(m, w, s - w);
      if (k >= 0) {
        t = s; while (*t == ' ' || *t == '\t') t = t + 1;
        if (raw || (t[0] == '#' && t[1] == '#')) o = bcat(b, o, args[k]);
        else { a = expand_text(args[k]); o = bcat(b, o, a); free(a); }
      }
      else while (w < s) { o = bput(b, o, *w); w = w + 1; }
      raw = 0;
    }
    else if (*s >= '0' && *s <= '9') { while (idchar(*s)) { o = bput(b, o, *s); s = s + 1; } raw = 0; }
    else { if (*s != ' ' && *s != '\t') raw = 0; o = bput(b, o, *s); s = s + 1; }
  }
  bput(b, o, 0);
  a = sdup(b, o); free(b);
  return a;
}
// expand macro m whose name was just read: 0 if it is function-like and not called
int expand(struct Sym *m) {
  char *q; char **args; int nl; int lv;
  if (m->mnargs < 0) { push_src(m->mbody, m); return 1; }
  // look for the '(', also past the end of finished macro expansions: F(x)(y)
  q = p; nl = 0; lv = fdepth;
  while (1) {
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') { if (*q == '\n') nl = nl + 1; q = q + 1; }
    if (*q || lv == 0 || !fstack_mac[lv - 1]) break;
    lv = lv - 1; q = fstack_p[lv]; nl = 0;
  }
  if (*q != '(') return 0;
  if (lv < fdepth) { line = fstack_line[lv]; fdepth = lv; }   // those expansions are done
  line = line + nl;
  args = collect_args(m, &q, 1);
  p = q;
  push_src(subst(m, args), m);
  return 1;
}
// ------------------------------------------------------------------ float literals
// Decimal to IEEE single, correctly rounded, in integer arithmetic only (cc.c
// itself cannot use float). The digits go into a big number of 16-bit limbs.
int *bn; int bnl;
int bn_muladd(int m, int a) {             // bn = bn * m + a, m and a small
  int i; int c;
  c = a; i = 0;
  while (i < bnl) { c = bn[i] * m + c; bn[i] = c & 65535; c = c >> 16; i = i + 1; }
  while (c) { if (bnl >= 250) die("float literal too long"); bn[bnl] = c & 65535; c = c >> 16; bnl = bnl + 1; }
  return 0;
}
int bn_div10() {                          // bn = bn / 10, returns the remainder
  int i; int r;
  r = 0; i = bnl - 1;
  while (i >= 0) { r = r * 65536 + bn[i]; bn[i] = r / 10; r = r % 10; i = i - 1; }
  while (bnl > 1 && !bn[bnl - 1]) bnl = bnl - 1;
  return r;
}
int bn_bit(int k) { if (k < 0 || k / 16 >= bnl) return 0; return (bn[k / 16] >> (k % 16)) & 1; }
int bn_top() {                            // index of the highest set bit, -1 for zero
  int i; int b;
  i = bnl - 1; while (i >= 0 && !bn[i]) i = i - 1;
  if (i < 0) return -1;
  b = 15; while (!((bn[i] >> b) & 1)) b = b - 1;
  return i * 16 + b;
}
int bn_below(int k) { int i; i = 0; while (i < k) { if (bn_bit(i)) return 1; i = i + 1; } return 0; }
int bn_field(int at, int n) { int v; int i; v = 0; i = n - 1; while (i >= 0) { v = v * 2 + bn_bit(at + i); i = i - 1; } return v; }
// bn * 2^-k (plus a nonzero tail if sticky) as float bits, rounded to nearest even
int bn_float(int k, int sticky) {
  int h; int at; int mant; int rnd; int e2; int ef;
  h = bn_top();
  if (h < 0) return 0;
  at = h - 23;                            // lowest bit that fits in the 24-bit mantissa
  if (at - k < -149) at = k - 149;        // subnormal: no bits below 2^-149
  if (at > 0) {
    mant = h - at + 1 > 0 ? bn_field(at, h - at + 1) : 0;
    rnd = bn_bit(at - 1); if (bn_below(at - 1)) sticky = 1;
  }
  else { mant = bn_field(0, h + 1) << -at; rnd = 0; }
  if (rnd && (sticky || (mant & 1))) mant = mant + 1;
  e2 = at - k;                            // value = mant * 2^e2
  if (mant == 16777216) { mant = mant >> 1; e2 = e2 + 1; }
  if (mant >= 8388608) {
    ef = e2 + 23 + 127;
    if (ef >= 255) return 2139095040;     // infinity
    return ef << 23 | (mant - 8388608);
  }
  return mant;                            // subnormal or 0
}
// the float literal at s: ival = its bits; returns the end (after any f/F/l/L)
char *fl_parse(char *s) {
  int e10; int nd; int sticky; int ex; int neg; int k; int n; int c;
  bn[0] = 0; bnl = 1; e10 = 0; nd = 0; sticky = 0;
  while (*s >= '0' && *s <= '9') {        // integer part; 200 digits decide even exact halfway cases
    c = *s - '0';
    if (nd < 200) { bn_muladd(10, c); if (nd || c) nd = nd + 1; } else { e10 = e10 + 1; if (c) sticky = 1; }
    s = s + 1;
  }
  if (*s == '.') {
    s = s + 1;
    while (*s >= '0' && *s <= '9') {
      c = *s - '0';
      if (nd < 200) { bn_muladd(10, c); e10 = e10 - 1; if (nd || c) nd = nd + 1; } else if (c) sticky = 1;
      s = s + 1;
    }
  }
  if (*s == 'e' || *s == 'E') {
    s = s + 1; neg = 0; ex = 0;
    if (*s == '+') s = s + 1; else if (*s == '-') { neg = 1; s = s + 1; }
    if (*s < '0' || *s > '9') die("bad float exponent");
    while (*s >= '0' && *s <= '9') { if (ex < 10000) ex = ex * 10 + *s - '0'; s = s + 1; }
    e10 = neg ? e10 - ex : e10 + ex;
  }
  while (*s == 'f' || *s == 'F' || *s == 'l' || *s == 'L') s = s + 1;
  if (bn_top() < 0 || e10 + nd < -50) { ival = 0; return s; }
  if (e10 + nd > 40) { ival = 2139095040; return s; }
  k = 0;
  if (e10 >= 0) { n = 0; while (n < e10) { bn_muladd(10, 0); n = n + 1; } }
  else {                                  // shift left so the quotient keeps 26+ bits, then divide
    n = -e10;
    k = 28 + (n * 3322 + 999) / 1000 - bn_top(); if (k < 0) k = 0;
    c = 0; while (c < k) { bn_muladd(2, 0); c = c + 1; }
    while (n > 0) { if (bn_div10()) sticky = 1; n = n - 1; }
  }
  ival = bn_float(k, sticky);
  return s;
}
int is_float_lit(char *s) {               // digits followed by . or an exponent (not hex)
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return 0;
  while (*s >= '0' && *s <= '9') s = s + 1;
  return *s == '.' || *s == 'e' || *s == 'E';
}
// compile time conversions, for constant initializers
int int_to_float(int v, int uns) {
  int neg; int b;
  neg = 0;
  if (!uns && v < 0) { neg = 1; v = -v; }   // -2^31 stays 0x80000000: as 16-bit limbs that is 2^31
  bn[0] = v & 65535; bn[1] = (v >> 16) & 65535; bnl = 2;
  b = bn_float(0, 0);
  return neg ? b | (1 << 31) : b;
}
int float_to_int(int b) {                 // toward zero, like vcvt
  int e; int m; int neg;
  neg = b < 0; e = (b >> 23) & 255; m = (b & 8388607) | 8388608;
  if (e < 127) return 0;
  if (e > 157) return neg ? -2147483647 - 1 : 2147483647;
  e = e - 150;
  if (e >= 0) m = m << e; else m = m >> -e;
  return neg ? -m : m;
}
int lex() {
  char *pp; int i; int c;
  while ((tk = *p)) {
    p = p + 1;
    if (tk == '\n') { line = line + 1; }
    else if (tk == '#') { preprocess(); }
    else if (idstart(tk)) {
      pp = p - 1;
      while (idchar(*p)) p = p + 1;
      i = intern(pp, p - pp);
      if (syms[i].mdef && !expanding(syms + i) && expand(syms + i)) continue;   // read the expansion
      id = syms + i; tk = id->tk;
      return 0;
    }
    else if (tk >= '0' && tk <= '9') {
      if (is_float_lit(p - 1)) { p = fl_parse(p - 1); numty = TFLOAT; tk = Num; return 0; }
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
    else if (tk == '.') {
      if (*p >= '0' && *p <= '9') { p = fl_parse(p - 1); numty = TFLOAT; tk = Num; return 0; }   // .5
      tk = Dot; return 0;
    }
    else if (tk == '~' || tk == ';' || tk == '{' || tk == '}' || tk == '(' || tk == ')' || tk == ']' || tk == ',' || tk == ':') return 0;
  }
  // end of this file or macro expansion: pop the input stack
  if (fdepth > 0) { fdepth = fdepth - 1; p = fstack_p[fdepth]; line = fstack_line[fdepth]; return lex(); }
  return 0;
}
// next token. Adjacent string literals ("a" "b", also from macros) become one:
// after a string, peek at the next token and push it back if it is not a string.
int next() {
  int s_ival; int s_slen; int i;
  if (pb) { pb = 0; tk = pb_tk; ival = pb_ival; id = pb_id; numty = pb_numty; return 0; }
  lex();
  if (tk != Str) return 0;
  while (1) {
    s_ival = ival; s_slen = slen;
    lex();
    if (tk != Str) { pb = 1; pb_tk = tk; pb_ival = ival; pb_id = id; pb_numty = numty; tk = Str; ival = s_ival; slen = s_slen; return 0; }
    if (ival != s_ival + s_slen) die("string literals not adjacent");
    i = ival; while (i < dpos) { data[i - 1] = data[i]; i = i + 1; }   // drop the first literal's 0
    dpos = dpos - 1; slen = s_slen + slen - 1; ival = s_ival;
  }
}

// rest of the line, \-newline continuations joined: a macro body
char *line_body() {
  char *o; char *r;
  while (*p == ' ' || *p == '\t') p = p + 1;
  o = mbuf;
  while (*p && *p != '\n') {
    if (*p == '\\' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n'))) {
      p = p + (p[1] == '\r' ? 3 : 2); line = line + 1; mput(o, ' '); o = o + 1;
    }
    else { mput(o, *p); o = o + 1; p = p + 1; }
  }
  mput(o, 0);
  r = sdup(mbuf, o);
  return trim(r);
}
// is the directive word w at p (and not just the start of a longer word)?
int pp_word(char *w) { int n; n = strlen(w); return !memcmp(p, w, n) && !idchar(p[n]); }
int pp_skip_ws() { while (*p == ' ' || *p == '\t') p = p + 1; return 0; }
// Skip a block whose condition is false: lines up to the #elif, #else or #endif of
// this level (only #endif if to_endif). Nested #if blocks are skipped whole.
// Returns 1 #elif, 2 #else, 3 #endif, with p after the word.
int skip_cond(int to_endif) {
  int depth;
  depth = 0;
  while (1) {
    while (*p && *p != '\n') { if (*p == '\\' && p[1] == '\n') { p = p + 1; line = line + 1; } p = p + 1; }
    if (!*p) die("#endif missing");
    p = p + 1; line = line + 1;
    pp_skip_ws();
    if (*p != '#') continue;
    p = p + 1; pp_skip_ws();
    if (pp_word("if") || pp_word("ifdef") || pp_word("ifndef")) depth = depth + 1;
    else if (pp_word("endif")) { if (!depth) { p = p + 5; return 3; } depth = depth - 1; }
    else if (!depth && !to_endif && pp_word("elif")) { p = p + 4; return 1; }
    else if (!depth && !to_endif && pp_word("else")) { p = p + 4; return 2; }
  }
}
// the value of an #if or #elif expression (the rest of the line)
int pp_eval() {
  char *t; char *b; char *o; char *w; struct Sym *m; int v; int paren;
  int s_tk; int s_ival; struct Sym *s_id; int s_numty; int s_slen; int s_pb;
  t = line_body();
  b = malloc(strlen(t) * 2 + 16); o = b;     // defined(X) and defined X become 1 or 0, before expansion
  while (*t) {
    if (t[0] == '/' && t[1] == '/') break;      // comments go
    if (t[0] == '/' && t[1] == '*') { t = t + 2; while (*t && !(t[0] == '*' && t[1] == '/')) t = t + 1; if (*t) t = t + 2; *o = ' '; o = o + 1; }
    else if (idstart(*t)) {
      w = t; while (idchar(*t)) t = t + 1;
      if (t - w == 7 && !memcmp(w, "defined", 7)) {
        while (*t == ' ' || *t == '\t') t = t + 1;
        paren = *t == '(';
        if (paren) { t = t + 1; while (*t == ' ' || *t == '\t') t = t + 1; }
        w = t; while (idchar(*t)) t = t + 1;
        if (t == w) die("defined needs a name");
        m = find_sym(w, t - w);
        *o = m && m->mdef ? '1' : '0'; o = o + 1;
        if (paren) { while (*t == ' ' || *t == '\t') t = t + 1; if (*t != ')') die(") expected after defined"); t = t + 1; }
      }
      else while (w < t) { *o = *w; o = o + 1; w = w + 1; }
    }
    else { *o = *t; o = o + 1; t = t + 1; }
  }
  *o = 0;
  t = expand_text(b); free(b);
  b = malloc(strlen(t) + 4); strcpy(b, t); strcpy(b + strlen(b), " ;"); free(t);   // ; ends it
  s_tk = tk; s_ival = ival; s_id = id; s_numty = numty; s_slen = slen; s_pb = pb;   // we are inside lex()
  push_src(b, 0); pb = 0; pp_if = 1;
  next(); v = cexpr();
  if (tk != ';') die("bad #if expression");
  pp_if = 0;
  fdepth = fdepth - 1; p = fstack_p[fdepth]; line = fstack_line[fdepth];
  tk = s_tk; ival = s_ival; id = s_id; numty = s_numty; slen = s_slen; pb = s_pb;
  return v;
}
// #define NAME body from C (-D and predefined macros)
int define_macro(char *name, char *body) {
  struct Sym *d;
  d = syms + intern(name, strlen(name));
  d->mdef = 1; d->mnargs = -1; d->mvar = 0; d->mbody = body;
  return 0;
}
int preprocess() {
  char *name; char *q; int n; struct Sym *d; char **names; int r;
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
      push_src(readfile(q), 0); line = 1;
      return 0;
    }
    // <system header>: ignored, builtins cover it
  }
  else if (!memcmp(p, "define", 6)) {
    p = p + 6;
    while (*p == ' ' || *p == '\t') p = p + 1;
    if (!idstart(*p)) die("bad #define");
    name = p; while (idchar(*p)) p = p + 1;
    d = syms + intern(name, p - name);
    d->mnargs = -1; d->mvar = 0;
    if (*p == '(') {                          // NAME( with no space: function-like
      p = p + 1; n = 0; names = malloc(32 * 4);
      while (*p == ' ' || *p == '\t') p = p + 1;
      while (*p != ')') {
        if (n >= 32) die("too many macro parameters");
        if (*p == '.' && p[1] == '.' && p[2] == '.') { names[n] = "__VA_ARGS__"; d->mvar = 1; p = p + 3; }
        else { if (!idstart(*p)) die("bad macro parameter"); q = p; while (idchar(*p)) p = p + 1; names[n] = sdup(q, p); }
        n = n + 1;
        while (*p == ' ' || *p == '\t') p = p + 1;
        if (*p == ',' && !d->mvar) { p = p + 1; while (*p == ' ' || *p == '\t') p = p + 1; }
        else if (*p != ')') die("bad macro parameter list");
      }
      p = p + 1;
      d->margs = names; d->mnargs = n;
    }
    d->mbody = line_body(); d->mdef = 1;   // the symbol keeps its class: a name can be a macro and a variable
    return 0;
  }
  else if (pp_word("ifdef") || pp_word("ifndef") || pp_word("if")) {
    if (pp_word("if")) { p = p + 2; n = pp_eval(); }
    else {
      r = pp_word("ifndef"); p = p + (r ? 6 : 5); pp_skip_ws();
      name = p; while (idchar(*p)) p = p + 1;
      if (p == name) die("#ifdef needs a name");
      d = find_sym(name, p - name);
      n = d && d->mdef; if (r) n = !n;
    }
    if (n) ifdepth = ifdepth + 1;
    else {
      while (1) {                               // find the branch to compile, if any
        r = skip_cond(0);
        if (r == 3) break;
        if (r == 2) { ifdepth = ifdepth + 1; break; }
        if (pp_eval()) { ifdepth = ifdepth + 1; break; }
      }
    }
  }
  else if (pp_word("elif") || pp_word("else")) {  // the branch before was taken: skip the rest
    if (!ifdepth) die("#elif or #else without #if");
    skip_cond(1); ifdepth = ifdepth - 1;
  }
  else if (pp_word("endif")) {
    if (!ifdepth) die("#endif without #if");
    ifdepth = ifdepth - 1;
  }
  else if (pp_word("error")) {
    p = p + 5; printf("%s:%d: #error %s\n", srcname, line, line_body()); quit();
  }
  else if (!memcmp(p, "undef", 5)) {
    p = p + 5;
    while (*p == ' ' || *p == '\t') p = p + 1;
    name = p; while (idchar(*p)) p = p + 1;
    if (p > name) {
      d = syms + intern(name, p - name);
      d->mdef = 0;
    }
  }
  while (*p && *p != '\n') {                // other directives are ignored, continuations too
    if (*p == '\\' && p[1] == '\n') { p = p + 1; line = line + 1; }
    p = p + 1;
  }
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

// ------------------------------------------------------------------ expressions
// Emit a user function call: args already pushed, n args.
int call_fn(struct Sym *f, int n) {
  if (n > 127) die("too many arguments");
  lnew(L_INS); ls("bl\t"); lsym(f); lend();    // as resolves it, or ld when f is in another file
  if (f->class == 0) { f->class = CFun; f->val = -1; f->type = TINT; }
  if (n) addsp(n);                              // pop the args
  return 0;
}
// r0 = address of function f (Thumb bit set), PC relative: fnaddr is movw/movt r0,#off ; add r0,pc
int fn_addr(struct Sym *f) {
  if (f->class == CSys) die("cannot take the address of a builtin");
  if (f->class == 0) { f->class = CFun; f->val = -1; f->type = TINT; }
  lnew(L_INS); ls("fnaddr\tr0, "); lsym(f); lend();
  return 0;
}
// Call through a function address in r0: keep it on the stack under the args,
// then ldr r1,[sp,#4n] ; blx r1 ; pop args and the address.
int call_ind(int t) {
  int n;
  push(); next(); n = 0;
  while (tk != ')') { expr(Assign); if (n < fnnp_of(t)) assignconv(fnparam_of(t, n)); push(); n = n + 1; if (tk == ',') next(); }
  next();
  if (n > 126) die("too many arguments");
  lnew(L_INS); ls("ldr\tr1, [sp, #"); ls(dec(4 * n)); ls("]"); lend();   // the address, under the n args
  ins("blx\tr1");
  addsp(n + 1);                                 // args and the address
  ty = fnret_of(t);
  return 0;
}
// Emit a host syscall: r0 = num, r1 = nargs, r2 = argv (argv[n-1] is arg 0)
int call_sys(int num, int n) {
  ins("mov\tr2, sp");                           // r2 = argv
  ins("push\t{r4}");
  ins("mov\tr4, sp");
  ins("mov\tr3, sp");
  ins("bic.w\tr3, r3, #7");                     // 8-byte aligned sp for the kernel
  ins("mov\tsp, r3");
  ins_ri("movs", 1, n);
  ins_ri("movs", 0, num);
  ins("blx\tr8");
  ins("mov\tsp, r4");
  ins("pop\t{r4}");
  if (n) addsp(n);                              // pop the args
  return 0;
}

// scale r0 by n (for pointer arithmetic)
int scale(int n) {
  if (n == 1) return 0;
  if (n == 4) return ins("lsls\tr0, r0, #2");
  imm(2, n); return ins("muls\tr0, r2");
}

// Roll back the last load so r0 holds an address, for lvalue use.
int lvalue() {
  int k;
  if (lastload) {                               // drop that many instruction lines (labels stay)
    k = nlines - 1;
    while (lastload > 0) { if (lkind[k] == L_INS) { lkind[k] = L_DEL; lastload = lastload - 1; } k = k - 1; }
  }
  else if (!(ty >= TSTRUCT && ty < PTR)) die("lvalue expected");
  return 0;
}
// scale r1 by n
int scale1(int n) {
  if (n == 1) return 0;
  if (n == 4) return ins("lsls\tr1, r1, #2");
  imm(2, n); return ins("muls\tr1, r2");
}

// r0 = r1 op r0 where r1 has type t and r0 has type ty; sets ty to the result type
// r0 = r1 op r0 for == != < > <= >= (left type t, right type ty)
int relop(int op, int t) {
  if ((isflt(t) || isflt(ty)) && t < PTR && ty < PTR) {          // vcmp; with a NaN only != is true
    fpair(t); ins("vcmp.f32\ts0, s1"); ins("vmrs\tAPSR_nzcv, fpscr");
    if (op == Eq) itcc(CC_EQ); else if (op == Ne) itcc(CC_NE); else if (op == Lt) itcc(CC_MI);
    else if (op == Gt) itcc(CC_GT); else if (op == Le) itcc(CC_LS); else itcc(CC_GE);
    return 0;
  }
  if (op == Eq) setcc(CC_EQ); else if (op == Ne) setcc(CC_NE);
  else if (op == Lt) setcc(isuns(t, ty) ? CC_LO : CC_LT); else if (op == Gt) setcc(isuns(t, ty) ? CC_HI : CC_GT);
  else if (op == Le) setcc(isuns(t, ty) ? CC_LS : CC_LE); else setcc(isuns(t, ty) ? CC_HS : CC_GE);
  return 0;
}
// s0 = r0 + 1.0 or r0 - 1.0 (float ++ and --)
int fstep(int up) {
  ins("vmov\ts0, r0"); imm(2, 1065353216); ins("vmov\ts1, r2");   // 1.0f
  if (up) ins("vadd.f32\ts0, s0, s1"); else ins("vsub.f32\ts0, s0, s1");
  return 0;
}
int binop(int op, int t) {
  int u;
  if ((isflt(t) || isflt(ty)) && t < PTR && ty < PTR) {          // float arithmetic
    fpair(t);
    if (op == Add) ins("vadd.f32\ts0, s0, s1"); else if (op == Sub) ins("vsub.f32\ts0, s0, s1");
    else if (op == Mul) ins("vmul.f32\ts0, s0, s1"); else if (op == Div) ins("vdiv.f32\ts0, s0, s1");
    else die("invalid operator for float");
    ins("vmov\tr0, s0"); ty = TFLOAT;
    return 0;
  }
  u = isuns(t, ty);
  if (op == Or)  { ins("orrs\tr0, r1"); ty = arith(t, ty); }
  else if (op == Xor) { ins("eors\tr0, r1"); ty = arith(t, ty); }
  else if (op == And) { ins("ands\tr0, r1"); ty = arith(t, ty); }
  else if (op == Shl) { ins("lsl.w\tr0, r1, r0"); ty = arith(t, t); }
  else if (op == Shr) {                                            // the left operand decides
    if (t == TUINT) ins("lsr.w\tr0, r1, r0"); else ins("asr.w\tr0, r1, r0");
    ty = arith(t, t);
  }
  else if (op == Add) {
    if (t >= PTR && ty < PTR) { scale(tysize(t - PTR)); ty = t; }
    else if (ty >= PTR && t < PTR) scale1(tysize(ty - PTR));
    else if (t < PTR && ty < PTR) ty = arith(t, ty);
    ins("adds\tr0, r1, r0");
  }
  else if (op == Sub) {
    if (t >= PTR && ty >= PTR) { ins("subs\tr0, r1, r0"); imm(2, tysize(t - PTR)); ins("sdiv\tr0, r0, r2"); ty = TINT; }   // pointer difference
    else { if (t >= PTR) scale(tysize(t - PTR)); ins("subs\tr0, r1, r0"); ty = t < PTR ? arith(t, ty) : t; }
  }
  else if (op == Mul) { ins("muls\tr0, r1"); ty = arith(t, ty); }
  else if (op == Div) { if (u) ins("udiv\tr0, r1, r0"); else ins("sdiv\tr0, r1, r0"); ty = arith(t, ty); }
  else if (op == Mod) {                                            // r1 - (r1 / r0) * r0
    if (u) ins("udiv\tr2, r1, r0"); else ins("sdiv\tr2, r1, r0");
    ins("mls\tr0, r2, r0, r1"); ty = arith(t, ty);
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
      while (tk != ')') { expr(Assign); if (n < d->nptypes) assignconv(d->ptypes[n]); push(); n = n + 1; if (tk == ',') next(); }
      next();
      if (d->class == CSys) call_sys(d->val, n);
      else if (d->class == CFun || d->class == 0) call_fn(d, n);
      else die("bad function call");
      ty = d->type;
    }
    else if (d->class == CNum) { imm(0, d->val); ty = d->type; }
    else if (d->class == 6) { lea_var(d); ty = d->type; }   // global array: address
    else if (d->class == 9) { lea_ext(d); ty = d->type; }        // extern array: address
    else if (d->class == CFun || d->class == CSys) { fn_addr(d); ty = fntype_p(d->type, d->ptypes, d->ptypes ? d->nptypes : -1); }   // function as a value
    else {
      if (d->class == CLoc) lea_loc(d->val);
      else if (d->class == CGlo) lea_var(d);
      else if (d->class == 7) lea_ext(d);
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
      expr(Inc); castto(t); ty = t;
    } else { expr_comma(); if (tk != ')') die(") expected"); next(); }
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
  else if (tk == '!') { next(); expr(Inc); test0(); itcc(CC_EQ); ty = TINT; }
  else if (tk == '~') { next(); expr(Inc); if (isflt(ty)) die("~ on a float"); ins("mvns\tr0, r0"); ty = arith(ty, ty); }
  else if (tk == Add) { next(); expr(Inc); ty = arith(ty, ty); }
  else if (tk == Sub) {
    next();
    if (tk == Num) { imm(0, numty == TFLOAT ? ival ^ (1 << 31) : -ival); ty = numty; next(); }   // a float flips its sign bit
    else {
      expr(Inc);
      if (isflt(ty)) { ins("vmov\ts0, r0"); ins("vneg.f32\ts0, s0"); ins("vmov\tr0, s0"); }
      else { ins("negs\tr0, r0"); ty = arith(ty, ty); }
    }
  }
  else if (tk == Inc || tk == Dec) {            // prefix
    t = tk; next(); expr(Inc); lvalue();
    if (isflt(ty)) { ins("mov\tr1, r0"); load_r1(ty); fstep(t == Inc); ins("vmov\tr0, s0"); store(ty); }
    else {
      sz = ty >= PTR ? tysize(ty - PTR) : 1;
      ins("mov\tr1, r0");
      load_r1(ty);
      if (t == Dec) sz = -sz;
      if (sz >= 0 && sz < 4096) addi12(0, 0, sz); else if (sz < 0 && -sz < 4096) subi12(0, 0, -sz); else { imm(2, sz); ins("adds\tr0, r0, r2"); }
      store(ty);
    }
  }
  else die("bad expression");

  // binary operators, precedence climbing
  while (tk >= lev || (tk == '(' && isfn(ty))) {   // a call binds tightest, like [ and ->
    t = ty;
    if (tk == '(') call_ind(t);
    else if (tk == Assign) {
      if (t >= TSTRUCT && t < PTR) die("struct assignment is not supported");
      next(); lvalue(); push(); expr(Assign); assignconv(t); pop1(); store(t); ty = t;
    }
    else if (tk == Cond) {
      next(); test0(); at = bc_w(CC_EQ);
      expr_comma();
      if (tk != ':') die(": expected"); next();
      t = ty; at2 = b_w(-1); patch_here(at);
      expr(Cond);
      if ((isflt(t) || isflt(ty)) && t < PTR && ty < PTR && t != ty) {   // the int branch becomes float
        if (!isflt(ty)) cvt(ty, TFLOAT);
        if (!isflt(t)) { m = b_w(-1); patch_here(at2); cvt(t, TFLOAT); at2 = m; }
        ty = TFLOAT;
      }
      patch_here(at2);
    }
    else if (tk == Lor) { next(); truth(); at = bc_w(CC_NE); expr(Lan); truth(); patch_here(at); itcc(CC_NE); ty = TINT; }
    else if (tk == Lan) { next(); truth(); at = bc_w(CC_EQ); expr(Or); truth(); patch_here(at); itcc(CC_NE); ty = TINT; }
    else if (tk >= AddA && tk <= ShrA) {        // compound assignment
      n = tk; next(); lvalue(); push(); load(t); push(); expr(Assign); pop1();
      if (n == AddA) n = Add; else if (n == SubA) n = Sub; else if (n == MulA) n = Mul; else if (n == DivA) n = Div; else if (n == ModA) n = Mod;
      else if (n == AndA) n = And; else if (n == OrA) n = Or; else if (n == XorA) n = Xor; else if (n == ShlA) n = Shl; else n = Shr;
      binop(n, t);                              // r0 = old op rhs
      assignconv(t); pop1(); store(t); ty = t;
    }
    else if (tk == Or)  { next(); push(); expr(Xor); pop1(); binop(Or, t); }
    else if (tk == Xor) { next(); push(); expr(And); pop1(); binop(Xor, t); }
    else if (tk == And) { next(); push(); expr(Eq);  pop1(); binop(And, t); }
    else if (tk >= Eq && tk <= Ge) { n = tk; next(); push(); expr(n <= Ne ? Lt : Shl); pop1(); relop(n, t); ty = TINT; }
    else if (tk == Shl) { next(); push(); expr(Add); pop1(); binop(Shl, t); }
    else if (tk == Shr) { next(); push(); expr(Add); pop1(); binop(Shr, t); }
    else if (tk == Add) { next(); push(); expr(Mul); pop1(); binop(Add, t); }
    else if (tk == Sub) { next(); push(); expr(Mul); pop1(); binop(Sub, t); }
    else if (tk == Mul) { next(); push(); expr(Inc); pop1(); binop(Mul, t); }
    else if (tk == Div) { next(); push(); expr(Inc); pop1(); binop(Div, t); }
    else if (tk == Mod) { next(); push(); expr(Inc); pop1(); binop(Mod, t); }
    else if ((tk == Inc || tk == Dec) && isflt(ty)) {   // postfix on a float: r0 keeps the old value
      lvalue(); ins("mov\tr1, r0"); load_r1(ty); fstep(tk == Inc); ins("vmov\tr2, s0"); store_r2(ty); next();
    }
    else if (tk == Inc || tk == Dec) {          // postfix
      lvalue();
      sz = ty >= PTR ? tysize(ty - PTR) : 1;
      if (tk == Dec) sz = -sz;
      ins("mov\tr1, r0");
      load_r1(ty);
      if (sz >= 0 && sz < 4096) addi12(2, 0, sz); else if (sz < 0 && -sz < 4096) subi12(2, 0, -sz); else { imm(2, sz); ins("adds\tr2, r0, r2"); }
      store_r2(ty);
      next();
    }
    else if (tk == Dot || tk == Arrow) {
      if (tk == Arrow) { if (ty < PTR) die("-> on non-pointer"); ty = ty - PTR; }
      next(); if (tk != Id) die("member expected");
      m = find_mem(ty); next();
      if (mems[m].off) addi12(0, 0, mems[m].off);
      ty = mems[m].type;
      lastload = 0;                             // a struct or array member emits no load: do not let lvalue() undo the pointer's
      if (ty & ARR) ty = ty & ~ARR; else load(ty);
    }
    else if (tk == Brak) {
      next(); push(); expr_comma(); pop1();
      if (tk != ']') die("] expected"); next();
      if (t < PTR) die("indexing a non-pointer");
      scale(tysize(t - PTR));
      ins("adds\tr0, r1, r0");
      ty = t - PTR; load(ty); decay();
    }
    else die("bad operator");
  }
  return 0;
}

// a, b, c: evaluate left to right, the value is the last one. Used where C takes a
// full expression; arguments and initializers use expr(Assign), so a comma there
// still separates them.
int expr_comma() {
  expr(Assign);
  while (tk == ',') { next(); expr(Assign); }
  return 0;
}

// ------------------------------------------------------------------ initializers
// Globals: constants go straight into the data image. Anything else (strings for
// pointers, addresses, function pointers, expressions) becomes code in .Linit, the
// file's run-before-main function (.t2init; ld's entry stub calls it). Locals: code at
// the declaration; arrays and structs are zeroed first. Nested aggregates need
// their own braces; a char array also takes a string.
int save_lex() {
  sv_p = p; sv_tk = tk; sv_ival = ival; sv_id = id; sv_line = line; sv_dpos = dpos;
  sv_pb = pb; sv_pb_tk = pb_tk; sv_pb_ival = pb_ival; sv_pb_id = pb_id; sv_pb_numty = pb_numty; sv_numty = numty; sv_slen = slen;
  sv_fdepth = fdepth; memcpy(sv_fp, fstack_p, 4 * fdepth); memcpy(sv_fl, fstack_line, 4 * fdepth); memcpy(sv_fm, fstack_mac, 4 * fdepth);
  return 0;
}
int restore_lex() {                    // lexing strings wrote to data: clear what lookahead used
  if (dpos > sv_dpos) memset(data + sv_dpos, 0, dpos - sv_dpos);
  p = sv_p; tk = sv_tk; ival = sv_ival; id = sv_id; line = sv_line; dpos = sv_dpos;
  pb = sv_pb; pb_tk = sv_pb_tk; pb_ival = sv_pb_ival; pb_id = sv_pb_id; pb_numty = sv_pb_numty; numty = sv_numty; slen = sv_slen;
  fdepth = sv_fdepth; memcpy(fstack_p, sv_fp, 4 * fdepth); memcpy(fstack_line, sv_fl, 4 * fdepth); memcpy(fstack_mac, sv_fm, 4 * fdepth);
  return 0;
}
int init_addr(int off) { if (ikind) lea_loc(ibase + off); else lea_data(isym, off, ibase + off); return 0; }
int init_code() { if (!ikind) init_open = 1; return 0; }   // global initializer code: it goes to .Linit
int init_close() { if (init_open) { tobuf = 1; flush(); tobuf = 0; init_open = 0; } return 0; }
int poke(int off, int v, int t) {
  data[off] = v;
  if (tysize(t) >= 2) data[off + 1] = v >> 8;
  if (tysize(t) == 4) { data[off + 2] = v >> 16; data[off + 3] = v >> 24; }
  return 0;
}
// [-] number | enum or #define name, followed by , } or ;  ->  1, value in ival. Else 0, nothing consumed.
int const_init(int t) {
  int neg; int v; int vt;
  save_lex(); neg = 0;
  if (tk == Sub) { next(); neg = 1; }
  if (tk == Num) { v = ival; vt = numty; } else if (tk == Id && id->class == CNum) { v = id->val; vt = TINT; } else { restore_lex(); return 0; }
  next();
  if (tk != ',' && tk != '}' && tk != ';') { restore_lex(); return 0; }
  if (neg) { if (vt == TFLOAT) v = v ^ (1 << 31); else v = -v; }
  if (isflt(t) && vt != TFLOAT) v = int_to_float(v, vt == TUINT);   // float f = 3;
  else if (!isflt(t) && t < PTR && vt == TFLOAT) v = float_to_int(v);  // int i = 2.5;
  ival = v;
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
  if (!ikind && const_init(t)) { poke(ibase + off, ival, t); return 0; }
  init_code(); init_addr(off); push(); expr(Assign); assignconv(t); pop1(); store(t);
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
    expr_comma(); if (tk != ')') die(") expected"); next();
    test0(); at = bc_w(CC_EQ);                  // false: to the else
    stmt();
    if (tk == Else) { next(); at2 = b_w(-1); patch_here(at); stmt(); patch_here(at2); }
    else patch_here(at);
  }
  else if (tk == While) {
    next(); if (tk != '(') die("( expected"); next();
    sv_brk = brk_fix; sv_cont = cont_fix; sv_tgt = cont_tgt;
    brk_fix = -1; cont_fix = -1; cont_tgt = here();
    expr_comma(); if (tk != ')') die(") expected"); next();
    test0(); at = bc_w(CC_EQ);
    nbrk = nbrk + 1; ncont = ncont + 1; stmt(); nbrk = nbrk - 1; ncont = ncont - 1;
    b_w(cont_tgt); patch_here(at);
    chain_here(brk_fix);
    brk_fix = sv_brk; cont_fix = sv_cont; cont_tgt = sv_tgt;
  }
  else if (tk == For) {
    next(); if (tk != '(') die("( expected"); next();
    if (tk != ';') expr_comma();
    if (tk != ';') die("; expected"); next();
    sv_brk = brk_fix; sv_cont = cont_fix; sv_tgt = cont_tgt;
    brk_fix = -1; cont_fix = -1;
    at = here();                                // condition
    if (tk != ';') { expr_comma(); test0(); at2 = bc_w(CC_EQ); } else at2 = -1;
    if (tk != ';') die("; expected"); next();
    body = b_w(-1);                             // skip increment first time
    cont_tgt = here();                          // increment
    if (tk != ')') expr_comma();
    if (tk != ')') die(") expected"); next();
    b_w(at);
    patch_here(body);
    nbrk = nbrk + 1; ncont = ncont + 1; stmt(); nbrk = nbrk - 1; ncont = ncont - 1;
    b_w(cont_tgt);
    if (at2 >= 0) patch_here(at2);
    chain_here(brk_fix);
    brk_fix = sv_brk; cont_fix = sv_cont; cont_tgt = sv_tgt;
  }
  else if (tk == Switch) {
    next(); if (tk != '(') die("( expected"); next();
    expr_comma(); if (tk != ')') die(") expected"); next();
    if (tk != '{') die("{ expected"); next();
    sv_brk = brk_fix; brk_fix = -1;
    loc = loc + 4; if (loc > maxloc) maxloc = loc; n = -loc;        // temp slot for the switch value
    push(); lea_loc(n); pop1(); ins("str\tr1, [r0]");
    at = b_w(-1);                                                 // -> first test (case chain head)
    at2 = -1;                                                     // default position
    body = -1;                                                    // fallthrough skip
    while (tk != '}') {
      if (tk == Case) {
        next();
        t = cexpr();
        if (tk != ':') die(": expected"); next();
        body = b_w(-1);                                           // previous body falls through over the test
        patch_here(at);                                           // failed previous test lands here
        lea_loc(n); ins("ldr\tr0, [r0]"); imm(1, t); ins("cmp\tr0, r1");
        at = bc_w(CC_NE);                                         // -> next test
        patch_here(body);
      }
      else if (tk == Default) { next(); if (tk != ':') die(": expected"); next(); at2 = here(); }
      else { if (!tk) die("unexpected eof"); nbrk = nbrk + 1; stmt(); nbrk = nbrk - 1; }
    }
    next();
    if (at2 >= 0) patch_b(at, at2); else patch_here(at);
    chain_here(brk_fix);
    brk_fix = sv_brk;
  }
  else if (tk == Break) { if (!nbrk) die("break outside a loop or switch"); next(); if (tk != ';') die("; expected"); next(); brk_fix = chain_add(brk_fix, b_w(-1)); }
  else if (tk == Continue) { if (!ncont) die("continue outside a loop"); next(); if (tk != ';') die("; expected"); next(); b_w(cont_tgt); }
  else if (tk == Return) {
    next();
    if (tk != ';') { expr_comma(); castto(fnrty); }   // unsigned char f() { return 300; } gives 44
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
        lea_loc(d->val); addi12(1, 0, 4); ins("str\tr1, [r0]");   // slot = slot + 4
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
  else { expr_comma(); if (tk != ';') die("; expected"); next(); }
  return 0;
}

// ------------------------------------------------------------------ top level
int parse_global() {
  int bt; int t; struct Sym *s; int n; int i; int frame_at; int sz; int hi; int ext;
  if (tk == Enum) {
    next(); if (tk == Id) next();
    if (tk == '{') {
      next(); i = 0;
      while (tk != '}') {
        if (tk != Id) die("enum name expected");
        s = id; next();
        if (tk == Assign) { next(); i = cexpr(); }
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
  ext = 0;
  if (tk == Extern) { ext = 1; next(); }        // a declaration: the storage is in another file
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
      if (!s->ptypes) s->ptypes = malloc(32 * 4);
      while (tk != ')') {
        bt = declarator(parse_type());
        if (darr != -1) bt = bt + PTR;          // int a[N] or a[] as a parameter is int *a
        if (dname) {
          if (dname->class == CLoc) die("duplicate param");
          dname->hclass = dname->class; dname->htype = dname->type; dname->hval = dname->val;
          dname->class = CLoc; dname->type = bt; dname->val = n;   // index, fixed up below
        }
        if (n < 32) s->ptypes[n] = bt;
        n = n + 1;
        if (tk == ',') next();
      }
      next();
      s->nptypes = n < 32 ? n : 32;
      if (tk == ';') {                          // prototype
        next();
        i = 0; while (i < nsyms) { if (syms[i].class == CLoc) { syms[i].class = syms[i].hclass; syms[i].type = syms[i].htype; syms[i].val = syms[i].hval; } i = i + 1; }
        if (s->class != CFun) { s->class = CFun; s->val = -1; }
        return 0;
      }
      if (tk != '{') die("{ expected");
      // fix param offsets: param i at r7 + 8 + 4*(n-1-i)
      i = 0; while (i < nsyms) { if (syms[i].class == CLoc) syms[i].val = 8 + 4 * (n - 1 - syms[i].val); i = i + 1; }
      s->class = CFun; s->val = 0; nparams = n; fnrty = t;   // val >= 0: defined
      oput("\t.global\t"); oname(s); oput("\n"); oname(s); oput(":\n");
      ins("push\t{r7, lr}");
      ins("mov\tr7, sp");
      frame_at = nlines; imm2(1, 0);            // movw+movt r1,#frame (filled in below)
      ins("sub.w\tsp, sp, r1");
      ret_fix = -1;
      stmt();
      chain_here(ret_fix);
      ins("mov\tsp, r7");
      ins("pop\t{r7, pc}");
      sz = (maxloc + 7) & -8;
      i = nlines; imm2(1, sz); ltext[frame_at] = ltext[i]; ltext[frame_at + 1] = ltext[i + 1]; nlines = i;
      flush();
      // restore shadowed symbols
      i = 0; while (i < nsyms) { if (syms[i].class == CLoc) { syms[i].class = syms[i].hclass; syms[i].type = syms[i].htype; syms[i].val = syms[i].hval; } i = i + 1; }
      return 0;
    }
    if (ext) {                                  // extern variable or array
      if (tk == Assign) die("extern with an initializer");
      if (s->class == 0) {
        if (darr != -1) { s->class = 9; s->type = t + PTR; } else { s->class = 7; s->type = t; }
      }
      else if (s->class != CGlo && s->class != 6 && s->class != 7 && s->class != 9) die("duplicate global");
      if (tk == ',') next();
      continue;
    }
    // global variable (declared extern before, it is now defined here: later references are normal)
    if (s->class && s->class != 7 && s->class != 9) die("duplicate global");
    if (nglob >= 8192) die("too many globals");
    hi = 0;
    if (tk == Assign) { next(); hi = 1; }
    if (darr == -2) { if (!hi) die("array size expected"); darr = count_init(); }
    n = tysize(t);
    if (darr >= 0) {
      n = n * darr;
      // global array: the symbol is the storage itself; treat name as pointer constant
      dpos = (dpos + 3) & -4;
      s->class = 6; s->type = t + PTR; s->val = dpos;     // val = address of storage
      gsym[nglob] = s; goff[nglob] = dpos; nglob = nglob + 1;
      dpos = dpos + n;
    } else {
      dpos = (dpos + 3) & -4;
      s->class = CGlo; s->type = t; s->val = dpos;
      gsym[nglob] = s; goff[nglob] = dpos; nglob = nglob + 1;
      dpos = dpos + n;
    }
    if (hi) {
      ikind = 0; ibase = s->val; isym = s;
      if (darr >= 0) init_array(t, darr, 0); else init_elem(t, 0);
      init_close();
    }
    if (tk == ',') next();
  }
  next();
  return 0;
}

// the data image: .Ldata at its start, a global label at each variable, 16 bytes a line
int write_data() {
  int i; int g; int n;
  oput("\t.data\n.Ldata:\n");
  i = 0; g = 0; n = 0;
  while (1) {
    while (g < nglob && goff[g] <= i) {
      if (n) { oput("\n"); n = 0; }
      oput("\t.global\t"); oname(gsym[g]); oput("\n"); oname(gsym[g]); oput(":\n");
      g = g + 1;
    }
    if (i >= dpos) break;
    if (n) oput(", "); else oput("\t.byte\t");
    oput(dec(data[i] & 255)); n = n + 1;
    if (n == 16) { oput("\n"); n = 0; }
    i = i + 1;
  }
  if (n) oput("\n");
  return 0;
}


// ------------------------------------------------------------------ one C file to assembly
char **dnames; char **dvals; int ndefs;          // -D NAME[=value]

// Compile src to the assembly file out. Every table is allocated here and freed at the
// end, so the driver can compile several files in one run and as and ld get the memory.
int compile(char *src, char *out) {
  int i; struct Sym *s;
  tk = 0; ival = 0; line = 0; id = 0; nsyms = 0; nmems = 0; nstructs = 0; ndims = 0; dpos = 0; lastload = 0;
  ty = 0; loc = 0; maxloc = 0; ret_fix = 0; brk_fix = 0; cont_fix = 0; cont_tgt = 0; nbrk = 0; ncont = 0; nparams = 0; fnrty = 0;
  nfn = 0; dname = 0; darr = 0; pb = 0; numty = 0; slen = 0; ikind = 0; ibase = 0; init_open = 0;
  fdepth = 0; ifdepth = 0; pp_if = 0; tdepth = 0; nsrcbuf = 0;
  srcname = src; outname = out; ofd = -1;
  syms = malloc(8192 * sizeof(struct Sym)); nsyms = 0;
  mems = malloc(2048 * sizeof(struct Mem)); nmems = 0;
  stsize = malloc(256 * 4); stfirst = malloc(256 * 4); nstructs = 0;
  starr = malloc(256 * 4); stcnt = malloc(256 * 4); dims = malloc(8 * 4);
  data = malloc(1048576); dpos = 0;
  memset(data, 0, 1048576);                     // the data image starts zeroed (malloc may reuse memory)
  fstack_p = malloc(64 * 4); fstack_line = malloc(64 * 4); fstack_mac = malloc(64 * 4); fdepth = 0;
  sv_fp = malloc(64 * 4); sv_fl = malloc(64 * 4); sv_fm = malloc(64 * 4); mbuf = malloc(65536);
  tstack = malloc(64 * 4); tdepth = 0; bn = malloc(256 * 4); bnl = 1;
  fnret = malloc(512 * 4); fnparams = malloc(512 * 4); fnnp = malloc(512 * 4); nfn = 0;
  lpool = malloc(262144); lppos = 0; nlines = 0; nlabels = 0;
  lkind = malloc(16384 * 4); ltext = malloc(16384 * 4); llab = malloc(16384 * 4); lcond = malloc(16384 * 4); lnext = malloc(16384 * 4);
  obuf = malloc(65536); opos = 0; ibuf = malloc(262144); ipos = 0; tobuf = 0;
  numbuf = malloc(16); ccbuf = malloc(4); ccname = "eqnecsccmiplvsvchilsgeltgtle";
  gsym = malloc(8192 * sizeof(struct Sym *)); goff = malloc(8192 * 4); nglob = 0;
  srcbuf = malloc(256 * sizeof(char *));
  memset(syms, 0, 8192 * sizeof(struct Sym));

  // keywords, in token enum order
  p = "break case char continue default else enum for if int return sizeof struct switch typedef void while long short signed unsigned float double";
  i = Break; while (i <= Double) { next(); id->tk = i; i = i + 1; }
  p = "extern"; next(); id->tk = Extern;
  // builtin host functions: name, syscall number (see sys.h)
  p = "exit 0 exec 1 millis 2 sleep 3 sysinfo 4 spawn 5 yield 6 jobs 7 kill 8 putchar 16 getchar 17 kbhit 18 conctl 19 redirect 20 open 32 close 33 read 34 write 35 seek 36 stat 37 unlink 38 rename 39 mkdir 40 rmdir 41 opendir 42 readdir 43 closedir 44 chdir 45 getcwd 46 mkpipe 47 malloc 64 free 65 memset 66 memcmp 67 memcpy 68 strlen 69 strcmp 70 strcpy 71 printf 72 sprintf 73 atoi 74 strncmp 75 strchr 76 net_join 80 net_status 81 net_ip 82 connect 83 send 84 recv 85 listen 86 accept 87 sock_avail 88 gpio_mode 96 gpio_write 97 gpio_read 98 analog_read 99 pwm_write 100 i2c_write 101 i2c_read 102 spi_transfer 103 delay_us 104 sin 112 cos 113 tan 114 asin 115 acos 116 atan 117 atan2 118 sinh 119 cosh 120 tanh 121 exp 122 log 123 log10 124 pow 125 sqrt 126 fabs 127 floor 128 ceil 129 fmod 130";
  next();
  while (tk == Id) {
    s = id; next(); s->class = CSys; s->val = ival; s->type = TINT;
    if (ival == SYS_malloc || ival == SYS_strcpy || ival == SYS_strchr || ival == SYS_getcwd || ival == SYS_net_ip) s->type = TCHAR + PTR;
    if (ival >= SYS_sin && ival <= SYS_fmod) {     // maths: float f(float) or float f(float, float)
      s->type = TFLOAT; s->ptypes = malloc(32 * 4); s->ptypes[0] = TFLOAT; s->ptypes[1] = TFLOAT;
      s->nptypes = 1; if (ival == SYS_atan2 || ival == SYS_pow || ival == SYS_fmod) s->nptypes = 2;
    }
    next();
  }
  // predefined macro and -D NAME[=value]
  define_macro("__T2CC__", "1");
  i = 0; while (i < ndefs) { define_macro(dnames[i], dvals[i]); i = i + 1; }

  p = readfile(srcname); line = 1;
  ofd = open(outname, 577, 420);                // O_WRONLY|O_CREAT|O_TRUNC, 0644
  if (ofd < 0) { printf("cannot write %s\n", outname); quit(); }
  oput("\t.syntax unified\n\t.thumb\n\t.text\n");
  next();
  while (tk) parse_global();
  if (ifdepth) die("#endif missing");

  // global initializer code: one run-before-main function (ld calls it from its entry stub)
  if (ipos) {
    oput(".Linit:\n\tpush\t{r4, lr}\n"); oflush();
    write(ofd, ibuf, ipos);
    oput("\tpop\t{r4, pc}\n\t.t2init\t.Linit\n");
  }
  write_data();
  oflush();
  close(ofd); ofd = -1;

  i = 0; while (i < nsyms) { if (syms[i].ptypes) free(syms[i].ptypes); i = i + 1; }
  i = 0; while (i < nfn) { if (fnparams[i]) free(fnparams[i]); i = i + 1; }
  i = 0; while (i < nsrcbuf) { free(srcbuf[i]); i = i + 1; }
  free(srcbuf); free(syms); free(mems); free(stsize); free(stfirst); free(starr); free(stcnt); free(dims); free(data);
  free(fstack_p); free(fstack_line); free(fstack_mac); free(sv_fp); free(sv_fl); free(sv_fm); free(mbuf);
  free(tstack); free(bn); free(fnret); free(fnparams); free(fnnp); free(lpool);
  free(lkind); free(ltext); free(llab); free(lcond); free(lnext); free(obuf); free(ibuf); free(numbuf); free(ccbuf);
  free(gsym); free(goff);
  return 0;
}

// ------------------------------------------------------------------ running as and ld
// tmp_id(): the number in the temporary names, make_tmpdir(), init_tools(argv0), and
// run_tool(name, argc, argv): the tool's exit code (argv[0] is its name).
#ifdef __T2CC__
int tmp_id() { return millis(); }
int make_tmpdir() { mkdir("/tmp"); return 0; }   // an error means it is there already
int init_tools(char *argv0) { return 0; }
int run_tool(char *name, int argc, char **argv) {
  char *path; int r;
  path = malloc(strlen(name) + 10); sprintf(path, "/bin/%s.bin", name);
  r = exec(path, argc, argv);
  if (r < 0) { printf("cc: cannot run %s (%d)\n", path, r); r = 1; }
  free(path);
  return r;
}
#endif
#ifndef __T2CC__
// The PC (gcc) build: as and ld from $T2_TOOLS, else build/host/ under the directory that
// holds the directory of this executable (compiler/cc -> build/host/), run by system().
// Temporary names use the process id: make -j starts several cc in the same millisecond.
#include <sys/wait.h>
char *toolsdir;
int tmp_id() { return getpid(); }
int make_tmpdir() { return 0; }
int init_tools(char *argv0) {
  char buf[4096]; ssize_t k; int j; char *sl; char *e;
  e = getenv("T2_TOOLS");
  if (e && *e) { toolsdir = e; return 0; }
  k = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (k > 0) buf[k] = 0; else { strncpy(buf, argv0, sizeof buf - 1); buf[sizeof buf - 1] = 0; }
  for (j = 0; j < 2; j++) { sl = strrchr(buf, '/'); if (sl) *sl = 0; else strcpy(buf, "."); }
  toolsdir = malloc(strlen(buf) + 16); sprintf(toolsdir, "%s/build/host", buf);
  return 0;
}
static char *shquote(char *o, const char *s) {
  *o++ = '\'';
  for (; *s; s++) { if (*s == '\'') { strcpy(o, "'\\''"); o += 4; } else *o++ = *s; }
  *o++ = '\''; *o = 0;
  return o;
}
int run_tool(char *name, int argc, char **argv) {
  size_t n; int i; int st; char *cmd; char *o; char *path;
  path = malloc(strlen(toolsdir) + strlen(name) + 2); sprintf(path, "%s/%s", toolsdir, name);
  n = 4 * strlen(path) + 8;
  for (i = 1; i < argc; i++) n += 4 * strlen(argv[i]) + 3;
  cmd = malloc(n); o = shquote(cmd, path);
  for (i = 1; i < argc; i++) { *o++ = ' '; o = shquote(o, argv[i]); }
  fflush(stdout);
  st = system(cmd);
  free(cmd);
  if (st == -1 || !WIFEXITED(st)) { printf("cc: cannot run %s\n", path); free(path); return 1; }
  free(path);
  return WEXITSTATUS(st);
}
#endif

// ------------------------------------------------------------------ the driver
char *tmppre; int ntmpn;                         // temporary files: /tmp/cc<id & 0xffff>_<n>_<base>.s/.o

int endswith(char *s, char *suf) { int n; int k; n = strlen(s); k = strlen(suf); return n > k && !strcmp(s + n - k, suf); }
// a new temporary file name for input n (from 1), named after its source src (the base name
// without the .c/.s, so ld's messages point at the source); removed again by cleanup()
char *newtmp(char *src, int n, char *ext) {
  char *t; char *b; char *q; int k;
  if (!ntmpn) make_tmpdir();
  ntmpn = 1;
  b = src; q = src; while (*q) { if (*q == '/') b = q + 1; q = q + 1; }
  k = strlen(b) - 2;
  t = malloc(strlen(tmppre) + k + 24); sprintf(t, "%s%d_", tmppre, n);
  q = t + strlen(t); memcpy(q, b, k); sprintf(q + k, ".%s", ext);
  tmpf[ntmp] = t; ntmp = ntmp + 1;
  return t;
}
// the base name of path (a .c or .s) in the current directory, with ext (".o" or ".s") for its suffix
char *outfile(char *path, char *ext) {
  char *b; char *q; char *r; int n;
  b = path; q = path; while (*q) { if (*q == '/') b = q + 1; q = q + 1; }
  n = strlen(b); r = malloc(n + 4); strcpy(r, b); strcpy(r + n - 2, ext);
  return r;
}
// as -o obj src; a failure ends cc with as's exit code
int assemble(char *src, char *obj) {
  char **av; int r;
  av = malloc(5 * sizeof(char *)); av[0] = "as"; av[1] = "-o"; av[2] = obj; av[3] = src; av[4] = 0;
  r = run_tool("as", 4, av);
  free(av);
  if (r) { cleanup(); exit(r); }
  return 0;
}
int exists(char *path) { int fd; fd = open(path, 0, 0); if (fd < 0) return 0; close(fd); return 1; }
// the data size in the header of the object obj (t2obj_hdr.data_size, bytes 12..15);
// 0 if it cannot be read (ld then reports the file)
int objdsize(char *obj) {
  int fd; int v; char *b;
  b = malloc(16); memset(b, 0, 16); v = 0;
  fd = open(obj, 0, 0);
  if (fd >= 0) {
    if (read(fd, b, 16) == 16) v = (b[12] & 255) | ((b[13] & 255) << 8) | ((b[14] & 255) << 16) | ((b[15] & 255) << 24);
    close(fd);
  }
  free(b);
  if (v < 0) v = 0;
  return v;
}
// libNAME.a in the -L directories, the current directory, /lib; 0 if not found
char *findlib(char *name, char **ldirs, int nldir) {
  char *path; int i;
  i = 0;
  while (i <= nldir + 1) {
    path = malloc(strlen(name) + 16 + (i < nldir ? strlen(ldirs[i]) : 0));
    if (i < nldir) sprintf(path, "%s/lib%s.a", ldirs[i], name);
    else if (i == nldir) sprintf(path, "lib%s.a", name);
    else sprintf(path, "/lib/lib%s.a", name);
    if (exists(path)) return path;
    free(path);
    i = i + 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  int i; int mode; int nin; char **in; int nlib; char **libs; char **ldirs; int nldir; char *a; char *dn; char *dv;
  char *out; char *s; char *o; char **av; int nav; int r; int shortdata; int dend;
  ofd = -1; outname = 0; incdir = 0; ntmp = 0; ntmpn = 0; out = 0;
  in = malloc(64 * sizeof(char *)); libs = malloc(64 * sizeof(char *)); ldirs = malloc(64 * sizeof(char *));
  dnames = malloc(64 * sizeof(char *)); dvals = malloc(64 * sizeof(char *));
  tmpf = malloc(160 * sizeof(char *)); nin = 0; nlib = 0; nldir = 0; ndefs = 0;
  mode = 0;                                     // 0: link a .bin, 1: -c, 2: -S
  shortdata = 0; dbase = 0; dlong = 0;
  i = 1;
  while (i < argc) {
    a = argv[i];
    if (nin >= 64 || nlib >= 64 || nldir >= 64 || ndefs >= 64) { printf("cc: too many arguments\n"); return 1; }
    if (a[0] == '-' && i + 1 >= argc && (!strcmp(a, "-o") || !strcmp(a, "-I") || !strcmp(a, "-D") || !strcmp(a, "-L") || !strcmp(a, "-l"))) {
      printf("cc: %s needs an argument\n", a); return 1;
    }
    if (!strcmp(a, "-o")) { out = argv[i + 1]; i = i + 2; }
    else if (!strcmp(a, "-S")) { mode = 2; i = i + 1; }
    else if (!strcmp(a, "-c")) { if (mode != 2) mode = 1; i = i + 1; }
    else if (!strcmp(a, "-mshort-data")) { shortdata = 1; i = i + 1; }
    else if (!strcmp(a, "-I")) { incdir = argv[i + 1]; i = i + 2; }
    else if (!strcmp(a, "-D")) {                // a copy: every compile() defines it again
      dn = malloc(strlen(argv[i + 1]) + 1); strcpy(dn, argv[i + 1]); dv = strchr(dn, '=');
      if (dv) { *dv = 0; dv = dv + 1; } else dv = "1";
      dnames[ndefs] = dn; dvals[ndefs] = dv; ndefs = ndefs + 1; i = i + 2;
    }
    else if (!strcmp(a, "-L")) { ldirs[nldir] = argv[i + 1]; nldir = nldir + 1; i = i + 2; }
    else if (!strcmp(a, "-l")) { libs[nlib] = argv[i + 1]; nlib = nlib + 1; i = i + 2; }
    else if (a[0] == '-' && a[1] == 'L' && a[2]) { ldirs[nldir] = a + 2; nldir = nldir + 1; i = i + 1; }
    else if (a[0] == '-' && a[1] == 'l' && a[2]) { libs[nlib] = a + 2; nlib = nlib + 1; i = i + 1; }
    else if (a[0] == '-') { printf("cc: unknown option %s\n", a); return 1; }
    else { in[nin] = a; nin = nin + 1; i = i + 1; }
  }
  if (!nin) { printf("usage: cc [-o out] [-S|-c] [-mshort-data] [-I dir] [-D name[=v]] [-L dir] [-l name] file.c|file.s|file.o|file.a...\n"); return 1; }
  i = 0;
  while (i < nin) {
    s = in[i];
    if (!endswith(s, ".c") && !endswith(s, ".s") && !endswith(s, ".o") && !endswith(s, ".a")) { printf("cc: %s: unknown file type\n", s); return 1; }
    if (mode == 2 && !endswith(s, ".c")) { printf("cc: -S needs .c inputs\n"); return 1; }
    if (mode == 1 && !endswith(s, ".c") && !endswith(s, ".s")) { printf("cc: -c needs .c or .s inputs\n"); return 1; }
    i = i + 1;
  }
  if (mode == 1 && out && nin != 1) { printf("cc: -o with -c needs exactly one input\n"); return 1; }
  if (mode == 2 && out && nin != 1) { printf("cc: -o with -S needs exactly one input\n"); return 1; }
  init_tools(argv[0]);
  if (mode && !shortdata) dlong = 1;            // -S, -c: where the data lands is unknown
  tmppre = malloc(32); sprintf(tmppre, "/tmp/cc%x_", tmp_id() & 65535);

  if (mode == 2) {                              // -S: NAME.s, or out
    i = 0;
    while (i < nin) { o = out; if (!o) o = outfile(in[i], ".s"); compile(in[i], o); i = i + 1; }
    return 0;
  }
  if (mode == 1) {                              // -c: NAME.o, or out
    i = 0;
    while (i < nin) {
      o = out; if (!o) o = outfile(in[i], ".o");
      if (endswith(in[i], ".c")) { s = newtmp(in[i], i + 1, "s"); compile(in[i], s); assemble(s, o); }
      else assemble(in[i], o);
      i = i + 1;
    }
    cleanup();
    return 0;
  }
  // link: objects in command-line order, then the -l archives
  av = malloc((nin + nlib + 4) * sizeof(char *)); av[0] = "ld"; av[1] = "-o"; av[2] = out; if (!out) av[2] = "a.bin"; nav = 3;
  i = 0;
  while (i < nlib) {                            // look for them first: no work wasted on a typo
    s = findlib(libs[i], ldirs, nldir);
    if (!s) { printf("cc: library %s not found\n", libs[i]); return 1; }
    libs[i] = s; i = i + 1;
  }
  // each file's data goes where ld puts it: after the data of the objects before it, aligned
  // to 4 when it has any; archive members come after all objects, so .a inputs move nothing
  i = 0; dend = 0;
  while (i < nin) {
    s = in[i];
    dbase = (dend + 3) & -4;
    if (endswith(s, ".c")) { o = newtmp(in[i], i + 1, "s"); compile(s, o); s = o; }
    if (endswith(s, ".s")) { o = newtmp(in[i], i + 1, "o"); assemble(s, o); s = o; }
    if (endswith(s, ".o")) { r = objdsize(s); if (r > 0) dend = dbase + r; }
    av[nav] = s; nav = nav + 1;
    i = i + 1;
  }
  i = 0; while (i < nlib) { av[nav] = libs[i]; nav = nav + 1; i = i + 1; }
  av[nav] = 0;
  r = run_tool("ld", nav, av);
  cleanup();
  return r;
}
