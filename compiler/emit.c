// emit.c - the assembly output: the function's line list, labels and branches, and the
// buffers written when a function ends (.Linit too). See cc.h for what the files share.
#include "cc.h"

// ------------------------------------------------------------------ assembly output
// The function being compiled is a list of lines, written out when it ends (global
// initializer code goes to .Linit instead): instructions as text, labels .L<n>, and
// branches b.w / b<cond>.w whose label is filled in once the target is known. Each
// instruction is spelled so that as picks the encoding cc has always used: .w where a
// 16-bit form exists but the 32-bit one is meant, addw/subw for the T4 add/sub, and
// mov<cond> inside an IT block. Label numbers are unique in the file.
char *lpool; int lppos;                          // text of the lines, 0-terminated
int *lkind; int *ltext; int *llab; int *lcond; int *lnext; int nlines;
                                                 // per line: L_*, text, label (a branch's target, or the
                                                 // number of a label), branch cond (-1: b.w), fixup chain
int nlabels;                                     // .L numbers used so far
int optim;                                       // -O: peep() rewrites the lines before flush()
char *ccname; char *ccbuf;                       // condition names, two letters each
char *obuf; int opos;                            // the output file's buffer
char *ibuf; int ipos; int tobuf;                 // .Linit's code; tobuf: flush() appends to it
char *numbuf;
struct Sym **gsym; int *goff; int nglob;         // global variables: their data offsets, for the labels

int oflush() { if (opos && write(ofd, obuf, opos) != opos) { printf("cannot write %s\n", outname); quit(); } opos = 0; return 0; }
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
  if (optim) peep();
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
