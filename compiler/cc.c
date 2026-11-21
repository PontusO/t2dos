// cc.c - a tiny single-pass C compiler emitting Thumb-2 assembly for Cortex-M33
// (RP2350). Written in the subset of C it compiles, so it can build itself.
//
// Build from the repo root: make (gcc builds compiler/cc from compiler/*.c; cc
// builds everything else, itself included). The files and what they share: cc.h.
// Use:  cc [-o out] [-S|-c] [-O] [-mshort-data] [-I dir] [-D name[=v]] [-L dir] [-l name] file.c|file.s|file.o|file.a...
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

#include "cc.h"

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

// ------------------------------------------------------------------ peephole optimizer (-O)
// peep() rewrites a function's lines before flush() writes them: fixed patterns of the code
// above become shorter ones. A rewrite that leaves a different value in r0, r1 or the flags
// first checks with dead()/fdead() that nothing reads the old one again, following branches.
// Offsets folded into loads and stores stay multiples of the access size (as wants that).
int *labline;            // labline[n]: the line of label .Ln in the function being optimized
int *labref;             // labref[n]: branches to .Ln (counted each round)
int pchg;                // a rewrite happened in this round
int oppw;                // opinfo(): the written register is only written, not also read
int oprd; int opmask; int opkind; int opn;   // opinfo(): register written (-1 none), registers read
                                             // (bit n: rn), kind (0 plain, 1 call, 2 return, 3 it
                                             // with opn instructions), 4 unknown
char *opmn;              // opinfo(): the mnemonic, copied
int nx(int i) { i = i + 1; while (i < nlines && lkind[i] == L_DEL) i = i + 1; return i; }
int isins(int i, char *s) { return i < nlines && lkind[i] == L_INS && !strcmp(lpool + ltext[i], s); }
int preins(int i, char *s) { return i < nlines && lkind[i] == L_INS && !strncmp(lpool + ltext[i], s, strlen(s)); }
// line i is s and then a decimal number and nothing more: the number, else -1
int pnum(int i, char *s) {
  char *t; int v;
  if (!preins(i, s)) return -1;
  t = lpool + ltext[i] + strlen(s);
  if (*t < '0' || *t > '9') return -1;
  v = 0;
  while (*t >= '0' && *t <= '9') { if (v > 100000) return -1; v = v * 10 + *t - '0'; t = t + 1; }
  if (*t) return -1;
  return v;
}
int pset(int i) { if (lppos > 258000) die("function too long"); lkind[i] = L_INS; ltext[i] = lppos; pchg = 1; return 0; }
int pdel(int i) { lkind[i] = L_DEL; pchg = 1; return 0; }
int labat(int l) {
  int t;
  if (l < 0 || l >= 65536) return -1;
  t = labline[l];
  if (t < 0 || t >= nlines || lkind[t] != L_LAB || llab[t] != l) return -1;
  return t;
}
int regword(char *t, int n) {               // t[0..n) as a register number, -1 if it is not one
  if (n == 2 && t[0] == 'r' && t[1] >= '0' && t[1] <= '9') return t[1] - '0';
  if (n == 3 && t[0] == 'r' && t[1] == '1' && t[2] >= '0' && t[2] <= '5') return 10 + t[2] - '0';
  if (n == 2 && t[0] == 's' && t[1] == 'p') return 13;
  if (n == 2 && t[0] == 'l' && t[1] == 'r') return 14;
  if (n == 2 && t[0] == 'p' && t[1] == 'c') return 15;
  return -1;
}
int mnis(char *s) { return !strcmp(opmn, s); }
// the registers an instruction line writes and reads (conservative: a register the first
// operand also reads counts as read, an unknown form as kind 4)
int opinfo(int i) {
  char *t; char *w; int n; int r; int first; int nops; int purew; int b;
  t = lpool + ltext[i];
  n = 0; while (t[n] && t[n] != 9) { opmn[n] = t[n]; n = n + 1; } opmn[n] = 0;
  oprd = -1; opmask = 0; opkind = 0; opn = 0; oppw = 0;
  if (t[n] == 9) t = t + n + 1; else t = t + n;
  if (opmn[0] == 'i' && opmn[1] == 't') { opkind = 3; opn = n - 1; return 0; }
  if (mnis("bl")) { opkind = 1; return 0; }
  if (mnis("fnaddr")) { oprd = regword(t, 2); if (t[2] != ',') oprd = -1; if (oprd < 0) opkind = 4; return 0; }
  first = 1; nops = 1; b = 0;
  while (*t) {
    if (*t == '#') { while (*t && *t != ',' && *t != ']') t = t + 1; }
    else if (*t == ',') { if (!b) nops = nops + 1; t = t + 1; }
    else if (*t == '[' || *t == '{') { b = 1; t = t + 1; }
    else if (*t == ']' || *t == '}') { b = 0; t = t + 1; }
    else if ((*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z')) {
      w = t; while ((*t >= 'a' && *t <= 'z') || (*t >= 'A' && *t <= 'Z') || (*t >= '0' && *t <= '9') || *t == '_') t = t + 1;
      r = regword(w, t - w);
      if (r >= 0) {
        if (first && !b && oprd < 0) oprd = r; else opmask = opmask | (1 << r);
        if (b && mnis("pop")) { if (r == 15) opkind = 2; else if (oprd < 0) oprd = r; else opmask = opmask | (1 << r); }
        else if (b) opmask = opmask | (1 << r);
      }
      first = 0;
    }
    else t = t + 1;
  }
  if (mnis("pop")) { opmask = 0; return 0; }               // pop {r1} writes r1; {r7, pc} returns
  if (mnis("push")) { oprd = -1; return 0; }
  if (mnis("blx")) {                                        // blx r8: a syscall, reads r0-r2
    if (oprd >= 0) opmask = opmask | (1 << oprd);
    oprd = -1; opkind = 1; if (opmask & 256) opmask = opmask | 7; return 0;
  }
  if (mnis("str") || mnis("strb") || mnis("strh") || mnis("cmp") || mnis("cmn") || mnis("tst")) {
    if (oprd >= 0) opmask = opmask | (1 << oprd);
    oprd = -1; return 0;
  }
  purew = nops >= 3 || mnis("mov") || mnis("movs") || mnis("movw") || mnis("movw.16") || mnis("mvn") || mnis("mvns")
    || mnis("ldr") || mnis("ldrb") || mnis("ldrh") || mnis("ldrsb.w") || mnis("ldrsh.w") || mnis("vmov")
    || mnis("sxtb") || mnis("uxtb") || mnis("sxth") || mnis("uxth")
    || (strlen(opmn) == 5 && !strncmp(opmn, "mov", 3));    // mov<cond> in an it block
  if (mnis("ldr") || mnis("ldrb") || mnis("ldrh") || mnis("ldrsb.w") || mnis("ldrsh.w")) purew = 1;
  if (oprd >= 0 && !purew) opmask = opmask | (1 << oprd);
  oppw = purew;
  if (mnis("vpush") || mnis("vpop")) oprd = 13;   // FPU lines use no core register but vmov's,
  return 0;                                         // vldr/vstr's base and these (sp)
}
// 1 if the value reg holds at line i is never read again (on every path). A label already
// reached in this query (dvis[line] == dstamp) is being or has been followed: nothing new there.
int *dvis; int dstamp;
int dead(int reg, int i, int depth) {
  int t; int k; int j; int a; int c;
  if (!depth) dstamp = dstamp + 1;          // one stamp per query: recursion (and calllive's
  if (depth > 40) return 0;                 // depth-1 asks) shares the parent's visit marks
  while (i < nlines) {
    k = lkind[i];
    if (k == L_LAB) { if (dvis[i] == dstamp) return 1; dvis[i] = dstamp; i = i + 1; }
    else if (k == L_BR) {
      t = labat(llab[i]); if (t < 0) return 0;
      if (depth == 1) return 0;             // (a depth-1 query is one line deep: licm_ok's
      if (lcond[i] < 0) i = t;              // chain test. What a branch reaches is past it)
      else { if (!dead(reg, t, depth + 1)) return 0; i = i + 1; }
    }
    else if (k == L_INS) {
      opinfo(i);
      if (opkind == 4) return 0;
      if (opkind == 3) {                   // it block: conditional writes do not end the value
        j = i; a = 0; c = opn;
        while (c > 0) {
          j = nx(j); if (j >= nlines || lkind[j] != L_INS) return 0;
          opinfo(j); if (opkind != 0 || (opmask & (1 << reg))) return 0;
          if (oprd == reg) a = a + 1;
          c = c - 1;
        }
        if (a == 2 && opn == 2) return 1;   // ite: written on both paths
        i = nx(j);
      }
      else {
        if (opmask & (1 << reg)) return 0;
        if (opkind == 2) return reg != 0;   // return: r0 is the result
        if (opkind == 1) return reg <= 3 || reg == 12;
        if (oprd == reg) return 1;
        i = i + 1;
      }
    }
    else i = i + 1;
  }
  return 0;
}
// 1 if the flags at line i are never read again
int fdead(int i, int depth) {
  int t; int k;
  while (i < nlines) {
    k = lkind[i];
    if (k == L_BR) {
      if (lcond[i] >= 0) return 0;
      t = labat(llab[i]); if (t < 0 || depth > 6) return 0;
      i = t; depth = depth + 1;
    }
    else if (k == L_INS) {
      opinfo(i);
      if (opkind == 3 || opkind == 4) return 0;
      if (opkind == 1 || opkind == 2 || mnis("vmrs")) return 1;
      if (mnis("cmp") || mnis("cmn") || mnis("tst") || mnis("adds") || mnis("subs") || mnis("movs") || mnis("lsls")
        || mnis("lsrs") || mnis("asrs") || mnis("muls") || mnis("ands") || mnis("orrs") || mnis("eors") || mnis("bics")
        || mnis("mvns") || mnis("rsbs")) return 1;
      if (mnis("adc") || mnis("sbc") || mnis("adcs") || mnis("sbcs")) return 0;
      i = i + 1;
    }
    else i = i + 1;
  }
  return 0;
}
// the load mnemonic of "LD\tr0, [r0]" on line i (its access size in *sz), else 0
char *ldform(int i, int *sz) {
  if (isins(i, "ldr\tr0, [r0]")) { *sz = 4; return "ldr"; }
  if (isins(i, "ldrb\tr0, [r0]")) { *sz = 1; return "ldrb"; }
  if (isins(i, "ldrsb.w\tr0, [r0]")) { *sz = 1; return "ldrsb.w"; }
  if (isins(i, "ldrh\tr0, [r0]")) { *sz = 2; return "ldrh"; }
  if (isins(i, "ldrsh.w\tr0, [r0]")) { *sz = 2; return "ldrsh.w"; }
  return 0;
}
char *stform(int i, int *sz) {
  if (isins(i, "str\tr0, [r1]")) { *sz = 4; return "str"; }
  if (isins(i, "strb\tr0, [r1]")) { *sz = 1; return "strb"; }
  if (isins(i, "strh\tr0, [r1]")) { *sz = 2; return "strh"; }
  return 0;
}
// a frame offset (negative: below r7) that a load or store of sz bytes can take
int offok(int off, int sz) {
  if (off % sz) return 0;
  if (off < 0) return -off <= 256 - sz;
  return off <= 4096 - sz;
}
// r7 frame offset of "subw/addw r0, r7, #N" on line i, or 1 (odd: never a valid offset) if not
int froff(int i) {
  int v;
  v = pnum(i, "subw\tr0, r7, #"); if (v >= 0) return -v;
  v = pnum(i, "addw\tr0, r7, #"); if (v >= 0) return v;
  return 1;
}
int lsoff(char *mn, char *rd, int off) {   // the text "mn\trd, [r7, #off]" (for pset)
  ls(mn); ls("\t"); ls(rd); ls(", [r7, #"); ls(dec(off)); ls("]"); lend(); return 0;
}
// a line inside push {r0} ... pop {r1} that only computes r0: 1 if it does not touch r1 (or
// r2), 2 if it uses r1 as a scratch register for a data address (renamed to r2), 3 for r2
// as that scratch, 0 if it does something else
int pxline(int i) {
  int sz;
  if (lkind[i] != L_INS) return 0;
  if (preins(i, "movw\tr0, #") || preins(i, "movt\tr0, #") || preins(i, "movw.16\tr0, #") || preins(i, "subw\tr0, r7, #")
    || preins(i, "addw\tr0, r7, #") || preins(i, "addw\tr0, r9, #") || preins(i, "fnaddr\tr0, ") || ldform(i, &sz)) return 1;
  if (preins(i, "ldr\tr0, [r7, #") || preins(i, "ldrb\tr0, [r7, #") || preins(i, "ldrsb.w\tr0, [r7, #")
    || preins(i, "ldrh\tr0, [r7, #") || preins(i, "ldrsh.w\tr0, [r7, #")) return 1;
  if (preins(i, "movw\tr1, #") || preins(i, "movt\tr1, #") || preins(i, "movw.16\tr1, #") || isins(i, "add.w\tr0, r9, r1")) return 2;
  if (preins(i, "movw\tr2, #") || preins(i, "movt\tr2, #") || preins(i, "movw.16\tr2, #") || isins(i, "add.w\tr0, r9, r2")) return 3;
  return 0;
}
// a line pxline() calls 2: the same with r2 for r1
int to_r2(int i) {
  char *t; char *u;
  t = lpool + ltext[i];
  pset(i);
  if (!strcmp(t, "add.w\tr0, r9, r1")) { ls("add.w\tr0, r9, r2"); lend(); return 0; }
  u = strchr(t, 9);                         // "movw\tr1, #..." and the like: r1 follows the tab
  while (t < u + 1) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  ls("r2"); t = t + 2; ls(t); lend();
  return 0;
}
int peep_push(int a) {                      // push {r0} X pop {r1} -> mov r1, r0 X
  int j; int k; int r1; int r2; int w1; int w2;
  r1 = 0; r2 = 0; w1 = 0; w2 = 0; j = nx(a);
  while (j < nlines && !isins(j, "pop\t{r1}")) {
    k = pxline(j); if (!k) return peep_push2(a);
    if (k == 2) { r1 = 1; if (isins(j, "add.w\tr0, r9, r1")) { if (!w1) return peep_push2(a); } else w1 = 1; }
    if (k == 3) { r2 = 1; if (isins(j, "add.w\tr0, r9, r2")) { if (!w2) return peep_push2(a); } else w2 = 1; }
    j = nx(j);
  }
  if (j >= nlines || (r1 && r2)) return peep_push2(a);
  if (r1) { k = nx(a); while (k < j) { if (pxline(k) == 2) to_r2(k); k = nx(k); } }
  pset(a); ls("mov\tr1, r0"); lend();
  pdel(j);
  return 1;
}
int redest(int i, char *rd) {              // line i "mn\tr0, ..." with rd for r0
  char *t; char *u;
  t = lpool + ltext[i]; u = strchr(t, 9);
  pset(i);
  while (t <= u) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  ls(rd); ls(t + 2); lend();
  return 0;
}
int ren12(int i) {                          // line i with every register r1 spelled r2
  char *t; int prev;
  t = lpool + ltext[i]; pset(i); prev = 0;
  while (*t) {
    if (*t == '#') { while (*t && *t != ',' && *t != ']') { lpool[lppos] = *t; lppos = lppos + 1; prev = *t; t = t + 1; } }
    else if (t[0] == 'r' && t[1] == '1' && !idchar(t[2]) && !idchar(prev)) { ls("r2"); t = t + 2; prev = '2'; }
    else { lpool[lppos] = *t; lppos = lppos + 1; prev = *t; t = t + 1; }
  }
  lend();
  return 0;
}
// push {r0} X pop {r1} where X uses r1 itself: X's r1 becomes r2 (X must not use r2, r2 must be
// free afterwards, and X must write r1 before it reads it)
int peep_push2(int a) {
  int j; int w1; int n;
  j = nx(a); w1 = 0; n = 0;
  while (j < nlines && !isins(j, "pop\t{r1}")) {
    if (lkind[j] != L_INS) return 0;
    opinfo(j);
    if (opkind != 0 || mnis("push") || mnis("pop")) return 0;
    if (oprd == 13 || (opmask & 8192) || oprd == 2 || (opmask & 4)) return 0;
    if ((opmask & 2) && !w1) return 0;
    if (oprd == 1) w1 = 1;
    n = n + 1; if (n > 40) return 0;
    j = nx(j);
  }
  if (j >= nlines || !dead(2, nx(j), 0)) return 0;
  n = nx(a);
  while (n < j) { opinfo(n); if (oprd == 1 || (opmask & 2)) ren12(n); n = nx(n); }
  pset(a); ls("mov\tr1, r0"); lend();
  pdel(j);
  return 1;
}
// X r0, ... ; mov r1, r0 -> X r1, ... when r0 is not read afterwards (X writes r0 and reads only
// its other operands)
int peep_tor1(int a) {
  int b; int sz; int v; char *rd;
  b = nx(a);
  if (isins(b, "mov\tr1, r0")) rd = "r1"; else if (isins(b, "mov\tr2, r0")) rd = "r2"; else return 0;
  v = pnum(nx(b), "movw\tr0, #");          // leave mov r1, r0 ; movw r0, #N ; cmp r1, r0 to peep_mov
  if (v >= 0 && v <= 255 && rd[1] == '1' && (isins(nx(nx(b)), "cmp\tr1, r0") || isins(nx(nx(b)), "adds\tr0, r1, r0")
    || isins(nx(nx(b)), "subs\tr0, r1, r0"))) return 0;
  if (!(ldform(a, &sz) || preins(a, "ldr\tr0, [") || preins(a, "ldrb\tr0, [") || preins(a, "ldrh\tr0, [")
    || preins(a, "ldrsb.w\tr0, [") || preins(a, "ldrsh.w\tr0, [") || preins(a, "movw\tr0, #") || preins(a, "subw\tr0, ")
    || preins(a, "addw\tr0, ") || preins(a, "add.w\tr0, r") || preins(a, "adds\tr0, r") || preins(a, "subs\tr0, r"))) return 0;
  if (!dead(0, nx(b), 0)) return 0;
  redest(a, rd); pdel(b);
  return 1;
}
// the size of "mn\trX, [r1]" on line i (a load or store through r1), else 0
int viar1(int i) {
  char *t; int n;
  t = lpool + ltext[i]; n = strlen(t);
  if (lkind[i] != L_INS || n < 7 || strcmp(t + n - 6, ", [r1]")) return 0;
  if (preins(i, "ldr\t") || preins(i, "str\t")) return 4;
  if (preins(i, "ldrh\t") || preins(i, "ldrsh.w\t") || preins(i, "strh\t")) return 2;
  if (preins(i, "ldrb\t") || preins(i, "ldrsb.w\t") || preins(i, "strb\t")) return 1;
  return 0;
}
// subw/addw r1, r7, #N, then loads and stores through [r1] -> [r7, #off]
int peep_r1fr(int a) {
  int off; int v; int j; int n; int cnt; int sz; char *t; char *u;
  v = pnum(a, "subw\tr1, r7, #"); if (v >= 0) off = -v; else { off = pnum(a, "addw\tr1, r7, #"); if (off < 0) return 0; }
  j = nx(a); n = 0; cnt = 0;
  while (j < nlines && lkind[j] == L_INS && n < 8) {
    opinfo(j); if (opkind != 0) break;
    sz = viar1(j);
    if (sz) {
      if (!offok(off, sz)) return 0;
      if (preins(j, "str\tr1") || preins(j, "strb\tr1") || preins(j, "strh\tr1")) return 0;
      cnt = cnt + 1;
      if (oprd == 1) { j = nx(j); break; }  // a load into r1 ends the address
    }
    else if (opmask & 2) return 0;
    else if (oprd == 1) break;
    j = nx(j); n = n + 1;
  }
  if (!cnt || !dead(1, j, 0)) return 0;
  n = nx(a);
  while (n < j) {
    if (viar1(n)) {
      t = lpool + ltext[n]; pset(n);
      u = t + strlen(t) - 6;                 // ", [r1]"
      while (t < u) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
      ls(", [r7, #"); ls(dec(off)); ls("]"); lend();
    }
    n = nx(n);
  }
  pdel(a);
  return 1;
}
// adds r0, rA, r0 ; LD r0, [r0] -> LD r0, [rA, r0]
int peep_idx(int a) {
  int b; int sz; char *mn; char *ra;
  if (isins(a, "adds\tr0, r1, r0")) ra = "r1"; else if (isins(a, "adds\tr0, r2, r0")) ra = "r2"; else return 0;
  b = nx(a); mn = ldform(b, &sz); if (!mn || !fdead(nx(b), 0)) return 0;
  pset(b); ls(mn); ls("\tr0, ["); ls(ra); ls(", r0]"); lend(); pdel(a);
  return 1;
}
// str r0, [r7, #off] ; ldr rX, [r7, #off] -> the store ; mov rX, r0 (or nothing for r0)
int peep_reload(int a) {
  int b; char *t; char *u; int n;
  t = lpool + ltext[a];
  if (strncmp(t, "str\tr0, [r7, #", 14)) return 0;
  b = nx(a); if (b >= nlines || lkind[b] != L_INS) return 0;
  u = lpool + ltext[b];
  if (strncmp(u, "ldr\tr", 5) || u[6] != ',' || u[5] < '0' || u[5] > '2' || strcmp(u + 7, t + 7)) return 0;
  n = u[5] - '0';
  if (n == 0) { pdel(b); return 1; }
  pset(b); ls("mov\tr"); ls(dec(n)); ls(", r0"); lend();
  return 1;
}
int peep_mov(int a) {                       // mov r1, rS ; movw r0, #N ; cmp with r1 (adds/subs: S = 0)
  int b; int c; int v; int e; int sr; char *t;
  t = lpool + ltext[a]; sr = regword(t + 8, strlen(t + 8));
  if (sr < 0 || sr > 12) return 0;
  b = nx(a); c = nx(b);
  v = pnum(b, "movw\tr0, #"); if (v < 0 || v > 255) return 0;
  e = nx(c);
  if (isins(c, "cmp\tr1, r0") && dead(0, e, 0) && dead(1, e, 0)) {
    pset(c); ls("cmp\t"); ls(t + 8); ls(", #"); ls(dec(v)); lend(); pdel(a); pdel(b); return 1;
  }
  if (sr) return 0;
  if ((isins(c, "adds\tr0, r1, r0") || isins(c, "subs\tr0, r1, r0")) && dead(1, e, 0)) {
    if (isins(c, "adds\tr0, r1, r0")) { pset(c); ls("adds\tr0, #"); } else { pset(c); ls("subs\tr0, #"); }
    ls(dec(v)); lend(); pdel(a); pdel(b); return 1;
  }
  return 0;
}
int peep_store(int a) {                     // subw r0, r7, #N ; mov r1, r0 ; X ; str r0, [r1] -> X ; str r0, [r7, #-N]
  int off; int m; int j; int sz; int n; char *mn;
  off = froff(a); if (off == 1) return 0;
  m = nx(a); if (!isins(m, "mov\tr1, r0")) return 0;
  j = nx(m); n = 0;
  if (!dead(0, j, 0)) return 0;             // X does not read the address
  while (j < nlines && lkind[j] == L_INS && !stform(j, &sz)) {
    opinfo(j); if (opkind != 0 || oprd == 1 || (opmask & 2) || oprd == 13) return 0;
    n = n + 1; if (n > 12) return 0;
    j = nx(j);
  }
  mn = stform(j, &sz); if (!mn || !offok(off, sz) || !dead(1, nx(j), 0)) return 0;
  pset(j); lsoff(mn, "r0", off);
  pdel(a); pdel(m);
  return 1;
}
int peep_ite(int a) {                       // ite C ; movC r0, #1 ; mov!C r0, #0 ; cmp r0, #0 ; beq/bne -> b!C/bC
  int b; int c; int d; int e; int cc; int k; char *t;
  t = lpool + ltext[a];
  if (strncmp(t, "ite\t", 4) || strlen(t) != 6) return 0;
  cc = -1; k = 0;
  while (k < 14) { if (ccname[2 * k] == t[4] && ccname[2 * k + 1] == t[5]) cc = k; k = k + 1; }
  if (cc < 0) return 0;
  b = nx(a); c = nx(b); d = nx(c); e = nx(d);
  if (!preins(b, "mov") || !preins(c, "mov") || !isins(d, "cmp\tr0, #0")) return 0;
  t = lpool + ltext[b]; if (strcmp(t + 5, "\tr0, #1") || t[3] != ccname[2 * cc] || t[4] != ccname[2 * cc + 1]) return 0;
  t = lpool + ltext[c]; if (strcmp(t + 5, "\tr0, #0") || t[3] != ccname[2 * (cc ^ 1)] || t[4] != ccname[2 * (cc ^ 1) + 1]) return 0;
  if (e >= nlines || lkind[e] != L_BR || (lcond[e] != CC_EQ && lcond[e] != CC_NE)) return 0;
  k = labat(llab[e]); if (k < 0) return 0;
  if (!dead(0, nx(e), 0) || !dead(0, k, 0) || !fdead(nx(e), 0) || !fdead(k, 0)) return 0;
  if (lcond[e] == CC_EQ) lcond[e] = cc ^ 1; else lcond[e] = cc;
  pdel(a); pdel(b); pdel(c); pdel(d);
  return 1;
}
int peep_br(int a) {
  int j; int t; int u; int n;
  j = nx(a);
  if (lcond[a] < 0) {
    n = j; while (n < nlines && lkind[n] == L_LAB) { if (llab[n] == llab[a]) { pdel(a); return 1; } n = nx(n); }
    n = 0;                                  // unreachable lines up to the next label
    while (j < nlines && lkind[j] != L_LAB) { pdel(j); j = nx(j); n = 1; }
    if (n) return 1;
  }
  else if (j < nlines && lkind[j] == L_BR && lcond[j] < 0) {   // bC L1 ; b L2 ; L1: -> b!C L2 ; L1:
    n = nx(j);
    if (n < nlines && lkind[n] == L_LAB && llab[n] == llab[a]) { lcond[a] = lcond[a] ^ 1; llab[a] = llab[j]; pdel(j); return 1; }
  }
  t = labat(llab[a]); if (t < 0) return 0;  // a branch to a b.w goes straight to its target
  u = t; while (u < nlines && lkind[u] == L_LAB) u = nx(u);
  if (u < nlines && lkind[u] == L_BR && lcond[u] < 0 && llab[u] != llab[a] && labat(llab[u]) >= 0) { llab[a] = llab[u]; pchg = 1; return 1; }
  if (lcond[a] >= 0 && u < nlines && lkind[u] == L_BR && lcond[u] == lcond[a] && llab[u] != llab[a] && labat(llab[u]) >= 0) {
    llab[a] = llab[u]; pchg = 1; return 1;  // the flags are the same there: it branches on too
  }
  return 0;
}
int peep_ld(int a) {                        // address then load -> one load
  int b; int off; int sz; char *mn;
  b = nx(a); mn = ldform(b, &sz); if (!mn) return 0;
  off = froff(a);
  if (off != 1 && offok(off, sz)) { pset(b); lsoff(mn, "r0", off); pdel(a); return 1; }
  if (isins(a, "add.w\tr0, r9, r1") || isins(a, "add.w\tr0, r9, r2")) {
    pset(b); ls(mn); ls("\tr0, [r9, r"); if (isins(a, "add.w\tr0, r9, r1")) ls("1]"); else ls("2]"); lend(); pdel(a); return 1;
  }
  return 0;
}
// ------------------------------------------------------------------ floats in s registers (-O)
// Float code moves each operand r0/r1 -> s0/s1 and the result s0 -> r0. These rules let FPU
// instructions read and write s registers directly: a float pushed around the right operand
// waits in a free s register, "vmov sD, rX" whose rX came from "vmov rX, sS" (through mov
// copies) uses sS, and an instruction that only computes a value for sD writes sD itself.
// s0-s15 are scratch: a value in them never lives across a label, a branch or a call, in the
// plain code and in these rules' output, so sdead() can stop there. s16-s31 hold promoted
// float locals (see promote()).
int swr;                 // sscan(): the s register line i writes, -1 none
int prepro;              // 1 in the peep() before promote(): no vldr/vstr on [r7, ...] yet
int pv(int i) { i = i - 1; while (i >= 0 && lkind[i] == L_DEL) i = i - 1; return i; }
// t as an s register number ("s0".."s31" not followed by an identifier character), else -1
int sat(char *t) {
  int r;
  if (t[0] != 's' || t[1] < '0' || t[1] > '9') return -1;
  r = t[1] - '0';
  if (t[2] >= '0' && t[2] <= '9') { r = r * 10 + t[2] - '0'; if (idchar(t[3])) return -1; }
  else if (idchar(t[2])) return -1;
  if (r > 31) return -1;
  return r;
}
int swrites(char *t) {                      // the FPU mnemonic at t writes its first operand
  return t[0] == 'v' && strncmp(t, "vcmp", 4) && strncmp(t, "vstr", 4) && strncmp(t, "vmrs", 4)
    && strncmp(t, "vpush", 5) && strncmp(t, "vpop", 4);
}
// 1 if line i reads s register k; swr is the s register it writes (-1 none)
int sscan(int i, int k) {
  char *t; int first; int r; int prev; int wr; int rd;
  swr = -1; rd = 0;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (t[0] != 'v') return 0;
  wr = swrites(t);
  while (*t && *t != 9) t = t + 1;
  first = 1; prev = 9;
  while (*t) {
    r = -1; if (!idchar(prev)) r = sat(t);
    if (r >= 0) {
      if (first && wr) swr = r; else if (r == k) rd = 1;
      first = 0; prev = '0'; t = t + 2; if (r >= 10) t = t + 1;
    }
    else { if (*t == ',') first = 0; prev = *t; t = t + 1; }
  }
  return rd;
}
// 1 if line i reads or writes s register k
int smen(int i, int k) { if (sscan(i, k)) return 1; return swr == k; }
// 1 if the value scratch register sk (k < 16) holds at line i is never read
int sdead(int k, int i) {
  if (k >= 16) return 0;
  while (i < nlines) {
    if (lkind[i] == L_LAB || lkind[i] == L_BR) return 1;
    if (lkind[i] == L_INS) {
      opinfo(i);
      if (opkind == 1 || opkind == 2) return 1;
      if (opkind == 4) return 0;            // an it line (3) is fine: no FPU instruction in its block
      if (sscan(i, k)) return 0;
      if (swr == k) return 1;
    }
    i = i + 1;
  }
  return 1;
}
// line i with s register from spelled sto: mode 1 only where it is written, 2 only where read
int sren(int i, int from, int to, int mode) {
  char *t; int first; int r; int prev; int wr; int w;
  t = lpool + ltext[i]; wr = swrites(t);
  pset(i);
  while (*t && *t != 9) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  first = 1; prev = 9;
  while (*t) {
    r = -1; if (!idchar(prev)) r = sat(t);
    if (r >= 0) {
      w = first && wr;
      if (r == from && ((mode == 1 && w) || (mode == 2 && !w))) { ls("s"); ls(dec(to)); }
      else { lpool[lppos] = 's'; lpool[lppos + 1] = t[1]; lppos = lppos + 2; if (r >= 10) { lpool[lppos] = t[2]; lppos = lppos + 1; } }
      first = 0; prev = '0'; t = t + 2; if (r >= 10) t = t + 1;
    }
    else { if (*t == ',') first = 0; lpool[lppos] = *t; lppos = lppos + 1; prev = *t; t = t + 1; }
  }
  lend();
  return 0;
}
// "[rB]" or "[rB, #off]" at t, an operand vldr/vstr take: 1, else 0 (prepro: not rB = r7)
int vaddr(char *t) {
  char *c; int b; int v; int neg;
  if (*t != '[') return 0;
  c = t + 1; while (*c && *c != ',' && *c != ']') c = c + 1;
  b = regword(t + 1, c - t - 1);
  if (b < 0 || b == 15 || (prepro && b == 7)) return 0;
  if (*c == ']') return !c[1];
  if (strncmp(c, ", #", 3)) return 0;
  c = c + 3; neg = 0; if (*c == '-') { neg = 1; c = c + 1; }
  v = 0;
  while (*c >= '0' && *c <= '9') { if (v > 100000) return 0; v = v * 10 + *c - '0'; c = c + 1; }
  if (strcmp(c, "]") || v % 4 || v > 1020) return 0;
  return 1;
}
// the float with bits hi << 16 as the imm8 of vmov.f32 sd, #imm, else -1
int fimm8(int hi) {
  int b;
  if (hi < 0 || hi > 65535 || (hi & 7)) return -1;
  b = (hi >> 13) & 1;
  if (((hi >> 14) & 1) == b) return -1;
  if (((hi >> 9) & 31) != (b ? 31 : 0)) return -1;
  return (hi >> 15) << 7 | b << 6 | ((hi >> 3) & 63);
}
int lspad(int v, int w) {                  // v in w digits, leading zeros
  char *t; t = dec(v);
  while (w > strlen(t)) { ls("0"); w = w - 1; }
  ls(t);
  return 0;
}
int lsfimm(int v) {                        // vmov.f32's imm8 as a decimal ("-2.0", "0.1328125")
  int q; int f;
  q = (16 + (v & 15)) << (((v >> 4) + 4) & 7);   // the value times 128
  f = (q & 127) * 78125;                   // its fraction in 7 decimals
  if (v & 128) ls("-");
  ls(dec(q >> 7)); ls(".");
  if (f % 1000000 == 0) ls(dec(f / 1000000));
  else if (f % 10000 == 0) lspad(f / 10000, 3);
  else lspad(f, 7);
  return 0;
}
// vmov sD, rX (or vmov.f32 sD, sS) where the value is already in an s register
int peep_fmov(int j) {
  char *t; char *u; char *c; int d; int s; int x; int y; int i; int k; int m; int n; int sw; int wm; int cp; int v;
  t = lpool + ltext[j];
  if (!strncmp(t, "vmov.f32\ts", 10)) {
    d = sat(t + 9); c = strchr(t, ','); if (d < 0 || !c) return 0;
    s = sat(c + 2); if (s < 0) return 0;
    i = j;
  }
  else {
    d = sat(t + 5); c = strchr(t, ','); if (d < 0 || !c || c[1] != ' ') return 0;
    x = regword(c + 2, strlen(c + 2)); if (x < 0 || x > 12) return 0;
    i = pv(j); n = 0; s = -1; wm = 0; cp = -1;
    while (s < 0) {                         // the line that gave rX its value, through movs
      if (i < 0 || lkind[i] != L_INS || n > 24) break;
      opinfo(i);
      if (opkind != 0) break;
      if (oprd == x) {
        u = lpool + ltext[i]; c = strchr(u, ',');
        if (!c) break;
        if (!strncmp(u, "vmov\tr", 6)) { s = sat(c + 2); if (s < 0) break; }
        else if (!strncmp(u, "mov\tr", 5)) {
          y = regword(c + 2, strlen(c + 2));
          if (y < 0 || y > 12 || y == 7 || y == 8 || y == 9) break;
          x = y;
          if (!(wm & (1 << y))) cp = y;     // rY still holds it at line j
        }
        else if (n == 0 && !strncmp(u, "ldr\tr", 5) && vaddr(c + 2) && dead(x, nx(j), 0)) {
          pset(j); ls("vldr\ts"); ls(dec(d)); ls(", "); ls(c + 2); lend(); pdel(i);
          return 1;
        }
        else if (n == 0 && !strncmp(u, "movt\tr", 6) && c[2] == '#' && fimm8(atoi(c + 3)) >= 0) {
          k = pv(i); v = fimm8(atoi(c + 3));  // movw rX, #0 ; movt rX, #hi: a constant vmov.f32 takes
          if (k < 0 || !preins(k, "movw\t") || strncmp(lpool + ltext[k] + 5, u + 5, c - u - 5)
            || strcmp(lpool + ltext[k] + (c - u), ", #0") || !dead(x, nx(j), 0)) break;
          pset(j); ls("vmov.f32\ts"); ls(dec(d)); ls(", #"); lsfimm(v); lend(); pdel(i); pdel(k);
          return 1;
        }
        else break;
      }
      if (oprd >= 0) wm = wm | (1 << oprd);
      if (s < 0) { i = pv(i); n = n + 1; }
    }
    if (s < 0) {                            // vmov sD, rY for a copy rX of rY
      if (cp < 0) return 0;
      pset(j); ls("vmov\ts"); ls(dec(d)); ls(", r"); ls(dec(cp)); lend();
      return 1;
    }
    m = nx(i);
    while (m < j) { sscan(m, -1); if (swr == s) return 0; m = nx(m); }
  }
  if (s == d) { pdel(j); return 1; }
  if (s < 16 && sdead(s, nx(j))) {          // the line that computed sS computes sD
    sw = 1; m = i;
    while (m < j && sw) { if (m != i && smen(m, d)) sw = 0; m = nx(m); }
    k = pv(i); n = 0;
    while (sw) {
      if (k < 0 || lkind[k] != L_INS || n > 24) { sw = 0; break; }
      opinfo(k); if (opkind != 0) { sw = 0; break; }
      sscan(k, -1);
      if (swr == s) break;
      if (smen(k, d)) sw = 0;
      k = pv(k); n = n + 1;
    }
    if (sw) {
      m = nx(k);
      while (m < j) { if (sscan(m, s)) sren(m, s, d, 2); m = nx(m); }
      sren(k, s, d, 1); pdel(j);
      return 1;
    }
  }
  if (d < 16) {                             // later reads of sD read sS
    m = nx(j); n = 0; sw = 0; k = 0;
    while (m < nlines && lkind[m] != L_LAB && lkind[m] != L_BR) {
      if (lkind[m] == L_INS) {
        opinfo(m);
        if (opkind == 1 || opkind == 2) break;
        if (opkind == 4) return 0;
        if (sscan(m, d)) { if (sw) return 0; k = 1; }
        if (swr == d) { m = m + 1; break; }    // its own read is renamed too
        if (swr == s) sw = 1;
        n = n + 1; if (n > 200) return 0;
      }
      m = m + 1;
    }
    if (k) {
      n = nx(j);
      while (n < m) { if (lkind[n] == L_INS && sscan(n, d)) sren(n, d, s, 2); n = nx(n); }
    }
    pdel(j);
    return 1;
  }
  if (i == j) return 0;
  pset(j); ls("vmov.f32\ts"); ls(dec(d)); ls(", s"); ls(dec(s)); lend();
  return 1;
}
// vmov rX, sS ; str rX, [rB, #off] -> vstr sS, [rB, #off] (rX not read afterwards)
int peep_vstr(int i) {
  char *t; char *u; char *c; int x; int s; int b;
  t = lpool + ltext[i]; c = strchr(t, ',');
  if (!c) return 0;
  x = regword(t + 5, c - t - 5); s = sat(c + 2);
  if (x < 0 || x > 12 || s < 0) return 0;
  b = nx(i); if (b >= nlines || lkind[b] != L_INS) return 0;
  u = lpool + ltext[b];
  if (strncmp(u, "str\tr", 5)) return 0;
  c = strchr(u, ','); if (!c || regword(u + 4, c - u - 4) != x || !vaddr(c + 2)) return 0;
  opinfo(b); if (opmask & (1 << x)) return 0;   // str r1, [r1]
  if (!dead(x, nx(b), 0)) return 0;
  pset(b); ls("vstr\ts"); ls(dec(s)); ls(", "); ls(c + 2); lend(); pdel(i);
  return 1;
}
// a line whose result nobody reads: mov/vmov into r0-r3, FPU writes to s0-s15
int peep_dce(int i) {
  char *t;
  t = lpool + ltext[i];
  opinfo(i);
  if (opkind != 0) return 0;
  if (oprd >= 0 && oprd <= 3 && (mnis("mov") || mnis("vmov")) && dead(oprd, nx(i), 0)) { pdel(i); return 1; }
  if (t[0] != 'v') return 0;
  sscan(i, -1);
  if (swr >= 0 && swr < 16 && sdead(swr, nx(i))) { pdel(i); return 1; }
  return 0;
}
// vmov r0, sX ; push {r0} ; Y ; pop {r1} -> vmov r0, sX ; vmov sT, r0 ; Y ; vmov r1, sT
// when Y uses FPU registers itself (else peep_push does better): no call, label or stack use
int peep_fpush(int a) {
  int j; int dep; int n; int k; int t; int w; int used;
  j = pv(a); if (j < 0 || !preins(j, "vmov\tr0, s")) return 0;
  j = nx(a); dep = 0; n = 0; w = 0; used = 0;
  while (j < nlines && !(dep == 0 && isins(j, "pop\t{r1}"))) {
    if (lkind[j] != L_INS) return 0;
    n = n + 1; if (n > 80) return 0;
    if (isins(j, "push\t{r0}")) dep = dep + 1;
    else if (isins(j, "pop\t{r1}")) dep = dep - 1;
    else {
      opinfo(j);
      if (opkind != 0 || oprd == 13 || (opmask & 8192) || mnis("push") || mnis("pop")) return 0;
      sscan(j, -1); if (swr >= 0) w = 1;
      k = 2; while (k < 16) { if (smen(j, k)) used = used | (1 << k); k = k + 1; }
    }
    j = nx(j);
  }
  if (j >= nlines || !w) return 0;
  t = 2; while (t < 16 && ((used & (1 << t)) || !sdead(t, a))) t = t + 1;
  if (t >= 16) return 0;
  pset(a); ls("vmov\ts"); ls(dec(t)); ls(", r0"); lend();
  pset(j); ls("vmov\tr1, s"); ls(dec(t)); lend();
  return 1;
}
int peep_fp(int i) {
  if ((preins(i, "vmov\ts") || preins(i, "vmov.f32\ts")) && peep_fmov(i)) return 1;
  if (preins(i, "vmov\tr") && peep_vstr(i)) return 1;
  return peep_dce(i);
}

// ------------------------------------------------------------------ locals in registers (-O)
// After peep(), promote() moves up to 5 word-sized locals and parameters into r4-r6, r10 and
// r11: the ones only ever accessed as "ldr/str rX, [r7, #off]" (never their address, never as
// part of an array or struct), most used first, a use inside a loop counting 8 times per loop
// level. The prologue saves those registers with r7 and lr ("push {r4, r5, r7, lr}"), so the
// parameters move up by 4 bytes per register; a promoted parameter is loaded once, into one of
// the 5 empty lines parse_global() leaves after the prologue. peep() then runs again, and
// peep_hsrc()/peep_tohi() use the registers directly instead of copies in r0-r3. Float words
// go to s16-s31 instead (up to 16, "vmov rX, sN" and "vmov sN, rX"), saved by a vpush after the
// push (pvpush, an empty line) and restored by a vpop before the return (pvpop).
int *slotk;              // slotk[off / 4 + 256]: 1 when the frame word at off is part of an array or struct
int *slotw;              // promote(): use weight of each frame word; -1 not promotable, -2 - k in register k
int *pdep;               // promote(): loop depth of each line
int npro;                // registers promote() gave out in the function being compiled
int nfpro;               // s registers it gave out (s16 up)
int *slotf;              // slotf[off / 4 + 256]: 1 when the frame word at off is a float local or parameter
int pvpush; int pvpop;   // the empty lines for promote()'s vpush and vpop
int phold;               // the first of the empty lines after the prologue, -1 if none
int rfoff; int rfplain;  // r7ref(): the frame offset; 1 for a plain ldr/str of r0-r3

int slotix(int off) { off = (off & -4) / 4 + 256; if (off < 0 || off >= 512) return -1; return off; }
int markbad(int from, int to) {             // frame bytes [from, to) belong to an array or struct
  int k;
  while (from < to) { k = slotix(from); if (k >= 0) slotk[k] = 1; from = from + 4; }
  return 0;
}
char *regnm(int k) {
  if (k == 0) return "r4";
  if (k == 1) return "r5";
  if (k == 2) return "r6";
  if (k == 3) return "r10";
  return "r11";
}
int ispro(int r) { return r == 4 || r == 5 || r == 6 || r == 10 || r == 11; }
// line i's use of r7: 0 none (or the prologue and epilogue), 1 "..r7, #off.." (rfoff, rfplain),
// 2 a use promote() does not know
int fl_pc_end(int i);                     // defined below frameless(): line i pops pc
int r7ref(int i) {
  char *t; char *u; int v; int neg;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i]; u = t;
  while (*u) {
    if (u[0] == 'r' && u[1] == '7' && !idchar(u[2]) && (u == t || !idchar(*(u - 1)))) break;
    u = u + 1;
  }
  if (!*u) return 0;
  if (!strcmp(t, "push\t{r7, lr}") || !strcmp(t, "mov\tr7, sp") || !strcmp(t, "mov\tsp, r7") || !strcmp(t, "pop\t{r7, pc}")) return 0;
  if (!strncmp(t, "push\t{", 6) && inregs(t + 6, "r7")) return 0;   // promote's grown entry
  if (fl_pc_end(i)) return 0;                         // push {r4, r7, lr} and exit pop too
  if (strncmp(u, "r7, #", 5)) return 2;
  u = u + 5; neg = 0;
  if (*u == '-') { neg = 1; u = u + 1; }
  if (*u < '0' || *u > '9') return 2;
  v = 0;
  while (*u >= '0' && *u <= '9') { if (v > 100000) return 2; v = v * 10 + *u - '0'; u = u + 1; }
  if (neg) v = -v;
  if (!strncmp(t, "subw\t", 5)) v = -v;      // subw rX, r7, #N: the address r7 - N
  rfoff = v; rfplain = 0;
  if (*u == ']' && !u[1] && (!strncmp(t, "ldr\tr", 5) || !strncmp(t, "str\tr", 5)) && t[5] >= '0' && t[5] <= '3'
    && u > t + 6 && !strncmp(t + 6, ", [r7, #", 8)) rfplain = 1;   // (a plain word read or
                                                            // write of a frame slot)
  return 1;
}
int r7shift(int i, int off) {               // line i with its r7 offset (rfoff, positive) now off
  char *t;
  t = lpool + ltext[i]; pset(i);
  while (strncmp(t, "r7, #", 5)) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  ls("r7, #"); ls(dec(off)); t = t + 5;
  while (*t >= '0' && *t <= '9') t = t + 1;
  ls(t); lend();
  return 0;
}
// sprel(): -O, after promote() and before frameless(). Every frame slot is a constant
// offset from sp once the reservation is in, so a function whose r7 uses are all plain
// slot reads and whose body moves sp only in tracked ways rewrites them to [sp, #N] and
// lets frameless() drop the frame pointer - even with parameters and locals (which is
// what it used to bail on). Spec: docs/superpowers/specs/2026-11-21-cc-sp-relative-design.md.
// All-or-nothing: the screen either passes and every slot line converts, or nothing moves.
// The offset comes from each line's own literal, not rfoff: promote shifted those lines by
// sh for its grown save list, and frameless shrinks that list again (a hold load at its
// promoted [r7, #N] means the slot at N - sh, which is what the sp-relative form must be).
int spholdcvt;                           // 1 when the phold arm converted a
                                           // hold load: frameless must excuse those
                                           // lines (they read [sp, #N] by then)
int wstart[128], wend[128], nwin;          // the argv-window spans sprel()'s prescan skipped
                                           // (3d): fl_frame excuses every line inside one,
                                           // so the scan's window test IS the walk's span
                                           // instead of a second text match that can drift.
                                           // nwin resets at sprel entry; a full table
                                           // records nothing and the scan bails on the
                                           // window's mov r4, sp (the frame is kept).
                                           // 128: the census found sh's run_cmd at 65.
int spconv;                                // 1 when sprel() made this function's slots:
int sprel_ok() { return spconv; }          // frameless() may allow >1 exit. Sticky per
                                           // function: reset at the next prologue below.
// line i's own slot offset as written (subw negated), -1 if the line is not a plain
// memory read or write of [r7, #off] (lea subw/addw, bare-register uses, vstr: no).
// A line sprel already rewrote ([sp, #off], ldr/str/vldr only) answers its offset too:
// fl_frame asks this to excuse the opinfo flag every [sp, #..] operand carries.
// cc.c has no strstr; scan like r7ref does. Only a literal ", [r7, #" (or ", " plus a
// width letter before it: str.w r0, [r7, #-8], what promote's r7shift() emits) counts:
// the lea subw/addw forms end in ", r7, #N]" with no bracket and answer -1 here.
int split(int i) {
  char *t; char *u; int v; int neg; int o;
  if (lkind[i] != L_INS) return -2;           // a label/branch/deleted line is never a slot
  t = lpool + ltext[i];                       // read: -2, so callers tell it from -1 ("a real
                                              // instruction that is not one"), which the
                                              // sprewrite walk relies on to skip screened
                                              // non-slot lines without spinning on them
  if (!strncmp(t, "subw", 4) || !strncmp(t, "addw", 4) || !strncmp(t, "vstr", 4)) return -1;
  u = t;                                        // find ", [r7, #" or (rewritten) ", [sp, #":
  o = 2;                                        // the wide forms put a letter after the comma:
  while (*u) {                                  // str.w r0, [r7, #-8] (what r7shift() emits
    if (*u == ',' && (!strncmp(u + o, "[r7, #", 6) || !strncmp(u + o, "[sp, #", 6))) break;
    if (*u == ',' && u[2] == 'w' && (!strncmp(u + 3, "[r7, #", 6) || !strncmp(u + 3, "[sp, #", 6))) { o = 3; break; }
    u = u + 1;
  }
  if (!*u) return -1;
  u = u + o + 6; neg = 0;                       // the offset right after "[r7, #" / "[sp, #"
  if (*u == '-') { neg = 1; u = u + 1; }
  v = 0;
  while (*u >= '0' && *u <= '9') { if (v > 100000) return -1; v = v * 10 + *u - '0'; u = u + 1; }
  return neg ? -v : v;
}
// line i is cc's own single-register core push/pop spill ({rX}, no r7, not a pop of pc:
// those are the exit and the entry push's business). Task 3's depth walk uses this; A1
// bails on every push/pop line instead.
int spspill(char *t, int is_push) {
  char *u;
  if (is_push) { if (strncmp(t, "push\t{r", 7)) return 0; u = t + 7; }
  else { if (strncmp(t, "pop\t{r", 6)) return 0; u = t + 6; }
  return u[0] >= '0' && u[0] <= '9' && u[1] == '}' && u[2] == 0 && strcmp(u, "7}");
}
// the line's text with its r7 slot offset rewritten to sp-relative `new` (split's forms)
int sprewrite(int i, int new) {
  char *t;
  t = lpool + ltext[i]; pset(i);              // pchg = 1: frameless' scan must see the line
  while (strncmp(t, "r7, #", 5)) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  ls("sp, #"); ls(dec(new)); t = t + 5;       // past "r7, #": the offset may be negative, so
  if (*t == '-') t = t + 1;                   // skip sign and digits, not digits only (a bare
  while (*t >= '0' && *t <= '9') t = t + 1;   // loop re-reads the '-' as a stop and duplicates
  ls(t); lend();                              // the number's tail on [r7, #-8])
  return 0;
}
// the uniform mapping is NOT uniform in -4: the caller's dropped r7 word shifts
// the BAND (incoming params, k > 0) one word down, but our own slots (k < 0) keep
// the offset they had framed - the reservation hangs off sp, which moved with the
// push list. Mixing them up parks every local 4 low; the top of the array then
// stores over the saved lr (bigprom was that witness).
int spmap(int k, int sz, int d) {
  return k + sz + d - (k < 0 ? 0 : 4);
}
// the CONVERTED lea shape: "addw rD, sp, #N" (digits, nothing after). fl_frame's
// sp-move test flags it: opinfo reads the SOURCE operand sp (oprd == 13) although
// the line only READS sp and writes rD - with the frame gone that is harmless,
// so the arm excuses the shape.
int leasp(int i) {
  char *t; char *u;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (strncmp(t, "addw\t", 5)) return 0;
  u = t;
  while (*u && strncmp(u, ", sp, #", 7)) u = u + 1;
  if (!*u) return 0;
  u = u + 7;
  if (*u < '0' || *u > '9') return 0;
  while (*u >= '0' && *u <= '9') u = u + 1;
  return !*u;
}
// the shape of a convertible lea, no window test: "subw rD, r7, #N" or
// "addw rD, r7, #N" (digits after the #, nothing after them). Acceptance already
// ran the window (leaoff) before spconv is set, so fl_r7 can excuse on shape.
int leaform(int i) {
  char *t; char *u;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (strncmp(t, "subw\t", 5) && strncmp(t, "addw\t", 5)) return 0;
  u = t;
  while (*u && strncmp(u, ", r7, #", 7)) u = u + 1;
  if (!*u) return 0;
  u = u + 7;
  if (*u < '0' || *u > '9') return 0;
  while (*u >= '0' && *u <= '9') u = u + 1;
  return !*u;
}
// the lea form: "subw rD, r7, #N" (the address of word k = -N) or "addw rD, r7, #N"
// (k = +N). A lea line computes a WORD ADDRESS, and a word's address never moves:
// pushes grow DOWNWARD from sp, so everything inside the reservation - and the
// caller's band above r7 - sits at a fixed address until the exit drains. So the
// line maps exactly like a literal read of the same word: o = spmap(k, sz, d) at
// the line's own live depth d. 1 with *o the offset when the line is one and o
// lands in the window; 0 for the other -1 forms (vstr, bare-register r7) and for
// an out-of-window lea - all three keep the frame.
int leaoff(int i, int sz, int d, int *o) {
  char *t; char *u; int v; int sgn;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (!strncmp(t, "subw\t", 5)) sgn = -1;
  else if (!strncmp(t, "addw\t", 5)) sgn = 1;
  else return 0;
  u = t;
  while (*u && strncmp(u, ", r7, #", 7)) u = u + 1;   // base operand exactly r7
  if (!*u) return 0;
  u = u + 7;                                          // past ", r7, #"
  if (*u < '0' || *u > '9') return 0;
  v = 0;
  while (*u >= '0' && *u <= '9') { if (v > 100000) return 0; v = v * 10 + *u - '0'; u = u + 1; }
  if (*u) return 0;                                   // trailing text: not our form
  *o = spmap(sgn * v, sz, d);
  return *o >= 0 && *o <= 1020;
}
// the rewrite: "addw rD, sp, #o" - the same word's address, measured from sp.
// rD is the source text between the mnemonic's tab and the base operand; o is
// non-negative (leaoff's window), so one mnemonic covers both source forms.
int leawrite(int i, int o) {
  char *t; char *u; char *r;
  t = lpool + ltext[i]; r = t + 5;                    // past "subw\t" / "addw\t"
  u = t;
  while (strncmp(u, ", r7, #", 7)) u = u + 1;
  pset(i); ls("addw\t");
  while (r != u) { lpool[lppos] = *r; lppos = lppos + 1; r = r + 1; }
  ls(", sp, #"); ls(dec(o)); lend();
  return 0;
}
// the words reglist() grows the entry push with beyond its one word (r7's): every promoted
// word saves itself - a core register among promote's npro (fl_list only ever keeps r4-r6:
// r10 and r11 spell into the list but never encode in a narrow push, so at most 3) plus
// nfpro s registers. sprel() uses this to tell an un-promoted parameter read at its
// incoming offset (8..7+4n) from one promote shifted out of it (r7shift: rfoff + sh).
int spshift() { int k; if (!npro && !nfpro) return 0; k = npro < 3 ? npro : 3; return 4 * (k + nfpro); }
int inregs(char *t, char *nm);              // defined below frameless(): "r7" among the
                                           // registers of a push/pop list, whole words only
// call_sys() (cc.c's syscall trampoline, see its emits) is a fixed line sequence: it
// saves sp in r4, aligns sp for the kernel, runs the call and restores sp from r4. The
// depth walk cannot follow its movs of sp, so it recognises the whole shape line for
// line and treats it as one unit. Its net effect on sp is zero: the push {r4} pairs with
// the pop {r4}, and the mov sp,r3 / mov sp,r4 pair cancels. The `add sp, #4n` that pops
// the argv words lives OUTSIDE the unit (call_sys emits it after the pop), so the walk
// never reaches it inside: its own arm claims na for those words exactly as for a bl's
// post-call add - and if peep moved the add in front of a result save, fl_resultsave's
// pv test still sees the add. A trampoline with no arguments ends at the pop with no add
// at all. Anything that does not match line for line is not this shape: 0, and the
// normal screen bails on it (a stray push {r4} counts as a spill, its unbalanced pop
// then bails on d). The `movs rN, #imm` test is loose on purpose: only the trampoline's
// own movs can stand here, and the kernel never touches r4 across a call (its argv
// arrives by pointer in r2).
int sp_syscall_save(int i) {
  int j; char *t;
  if (lkind[i] != L_INS || strcmp(lpool + ltext[i], "mov\tr2, sp")) return 0;
  j = nx(i);
  while (j < nlines && lkind[j] == L_INS) {
    t = lpool + ltext[j];
    if (!strcmp(t, "pop\t{r4}")) return 1;        // the restore: end of the unit
    if ((!strncmp(t, "push\t{", 6) || !strncmp(t, "pop\t{", 5))   // a push/pop LIST that
      && inregs(t + (t[1] == 'u' ? 6 : 5), "r4")                 // names r4 but is NOT the
      && strcmp(t, "push\t{r4}")) return 0;       // trampoline's own save: another register
    if (strcmp(t, "push\t{r4}") && strcmp(t, "mov\tr4, sp") && strcmp(t, "mov\tr3, sp")
      && strncmp(t, "bic.w\tr3, r3, #7", 14) && strcmp(t, "mov\tsp, r3")
      && strncmp(t, "movs\tr", 6) && strcmp(t, "blx\tr8") && strcmp(t, "mov\tsp, r4")) return 0;
    j = nx(j);
  }
  return 0;
}
// 1 when line i sits inside a window span the prescan skipped (wstart/wend): the
// excuse fl_frame needs for every line but the window's first (sp_syscall_save
// matches its text only there). The table is fresh whenever sprel_ok() holds:
// sprel resets it at entry and fl_frame's two callers both run after sprel on
// the same function.
int in_window(int i) {
  int t;
  t = 0;
  while (t < nwin) { if (i >= wstart[t] && i <= wend[t]) return 1; t = t + 1; }
  return 0;
}
// line i moves sp by this much (Task 3's depth walk): a push of exactly one core
// register +4, a pop of exactly one core register -4, add sp, #N -N. A pop or push
// list with pc in it is not a spill (the exit pops frameless rewrites separately).
// 0 means the line is none of these shapes; the caller's opinfo test still catches
// every other sp mover. The list parse skips any s-register group: vpush/vpop spell
// sNN, so they never match a one-core-register shape and stay bailed by opinfo.
int spd(int i) {
  char *t; char *u; int n; int sh;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (!strncmp(t, "push\t{", 6) || !strncmp(t, "pop\t{", 5)) {
    u = t + (strncmp(t, "push", 4) ? 5 : 6);          // past the brace: "push\t{" is 6 chars,
                                                      // "pop\t{" is 5
    if (inregs(u, "pc")) return 0;                    // the exit pop: fl_pc_end's business
    sh = strncmp(t, "push", 4) ? -4 : 4;
    n = 0;
    while (*u) {                                      // count the core registers in the list:
      if (u[0] == 'r' && u[1] >= '0' && u[1] <= '9') { // a push of r7 saves no word frameless
        if (u[1] == '7') sh = 0;                      // will not save anyway; a pop of r7
        n = n + 1; u = u + 2;                         // unbalances the converted lists by one,
      }                                               // so it counts as the -4 it really is
      else if (u[0] == 's' && u[1] >= '0' && u[1] <= '9') { n = n + 1; u = u + 2; }
      else u = u + 1;
    }
    return n == 1 ? sh : 0;
  }
  if (!strncmp(t, "add\tsp, #", 9)) { n = pnum(i, "add\tsp, #"); return n < 0 ? 0 : -n; }
  return 0;
}
// a direct or indirect call (blx r8 is cc's syscall trampoline call: it takes no pushed
// args, the screen excuses it). A tail jump to a builtin ends the body.
int fl_bl(int i) {
  char *t;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  return !strncmp(t, "bl\t", 3) || (!strncmp(t, "blx\t", 4) && strcmp(t, "blx\tr8"))
    || (!strncmp(t, "b\t", 2) && strcmp(t, "b\tw"));
}
// lines that put a value where a push of r0-r3 can read it: cc builds args from these
int fl_argsetup(char *t) {
  return !strncmp(t, "mov\tr", 4) || !strncmp(t, "movw\tr", 5)   // of r0-r3 can read it: cc
    || !strncmp(t, "movt\tr", 5) || !strncmp(t, "muls\tr", 5)    // builds args from mov/movw/
    || !strncmp(t, "adds\tr", 5) || !strncmp(t, "subs\tr", 5)    // movt/mul/add/sub of r0-r3
    || !strncmp(t, "ldr\tr", 4) || !strncmp(t, "ldr\ts", 4)     // (loads included: an arg may
    || !strncmp(t, "vmov\ts", 5) || !strncmp(t, "vcvt", 4);      // be loaded on the way)
}
// line i is a call's result save: push {r0} right after the bl (cc saves r0 exactly this
// way when a call's value must live across more code, and r0 is dead at every bl) or right
// after that call's add sp, #N. The word it pushes sits below every later push until its
// own add or pop claims it: the depth walk counts it. A push feeding a bl is never one:
// that is an arg push (fl_argpush), and a save always stands at the top of its own window.
int fl_resultsave(int i) {
  int j; char *t;
  if (lkind[i] != L_INS || strncmp(lpool + ltext[i], "push\t{r0}", 10)) return 0;
  j = nx(i);                                          // a push {r0} feeding a call right
  while (j < nlines && lkind[j] == L_INS              // below, or any arg setups that lead to
    && fl_argsetup(lpool + ltext[j])                  // one, is that call's ARG push: its word
    && strncmp(lpool + ltext[j], "push\t{", 6))       // sits ON sp during the call and its add
    j = nx(j);                                        // pops it again, never read back as a
  if (j < nlines && lkind[j] == L_INS                 // saved result (this holds whether the
    && fl_bl(j))                                      // previous line is the bl or its
    return 0;                                         // post-call add)
  j = pv(i);                                          // a save stands right after the bl or
  if (j < 0 || lkind[j] != L_INS) return 0;           // its post-call add sp, #N
  t = lpool + ltext[j];
  if (!strncmp(t, "add\tsp, #", 9)) return 1;         // right after the post-call add: that
  if (strncmp(t, "bl\t", 3) && strncmp(t, "blx\tr8", 6)) return 0;   // call's result save
  j = nx(i);                                          // a str r0, [r7, #-N] right after the
  while (j < nlines && lkind[j] == L_INS              // push stores the answer in a slot
    && !strncmp(lpool + ltext[j], "str", 3))          // instead: r0 dies there and this push
    j = nx(j);                                        // feeds no call (fl_bl's walk above):
  if (j != nx(i) || split(j) < 0) return 0;           // dead code peep deletes. Either way it
  return 1;                                           // is no save: the str reads r0, not sp
}
// the str r0, [r7, #-N] that stands for a call result save fl_resultsave would otherwise
// claim at a push. Right below a bare bl it is the save when nothing sits between: the
// str's own slot read sees the result word still on sp, so the walk counts +4 HERE and
// r0 dies at the store. After an add sp, #N stand-in it is the save unless the str's
// value flows into a call (then the push below is fl_resultsave's).
int strsave(int i) {
  int j; char *t;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (strncmp(t, "str", 3)) return 0;
  j = pv(i);
  if (j < 0 || lkind[j] != L_INS) return 0;
  t = lpool + ltext[j];
  if (!strncmp(t, "add\tsp, #", 9)) {
    j = nx(i);                                        // the add stand-in: a push below that
    while (j < nlines && lkind[j] == L_INS            // leads to a call (through value setups)
      && fl_argsetup(lpool + ltext[j])                // is fl_resultsave's save; once peep's
      && strncmp(lpool + ltext[j], "push\t{", 6))     // dead-push shape leaves it feeding
      j = nx(j);                                      // nothing, the str stands for the save
    if (j < nlines && lkind[j] == L_INS               // HERE instead (its dead push below
      && !strncmp(lpool + ltext[j], "push\t{", 6)     // shifts sp right away: net zero)
      && fl_bl(nx(j)))
      return 0;
    return 1; }
  if (!strcmp(t, "pop\t{r4}"))                      // a builtin's window whose argv add peep
    return nx(pv(j)) == i;                          // moved in front of this str: the str is
                                                     // the save when it sits right below the
  if (strncmp(t, "bl\t", 3)                       // pop (its prev line is not the add).
    && strncmp(t, "blx\tr8", 6)) return 0;        // After a bare bl the str must sit right
  return nx(j) == i;                              // below it: a push {r0} in between is
                                                   // fl_resultsave's save (or an arg push)
}
// line i is a call's argument push (cc pushes each arg as a value setup into r0-r3 and a
// push {rX}): transparent to the depth walk, because a call sits ON these words and pops
// them again before the function's exits (the fib shape: push {r0}; bl; add sp, #4). The
// test walks forward over the run of value setups until it finds either the push (then
// every setup line in the run is an arg push too) or a line that cannot be a setup.
int fl_argpush(int i) {
  int j; char *t;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (fl_resultsave(i)) return 0;                   // a result save is not an arg push: the
                                                    // test runs first, so a push {r0} whose
                                                    // prev line is a bl or an add sp belongs
                                                    // to the PREVIOUS call, not this one
  if (!strncmp(t, "push\t{", 6)) {                 // the push itself: an arg push when it
    if (inregs(t + 6, "r7") || inregs(t + 6, "lr")) // saves a value, not r7 or lr. A push
      return 0;                                     // {r0} feeding the bl right below is
    return 1;                                       // that call's FIRST arg (cc emits it with
  }                                                 // no setup line)
  if (!fl_argsetup(t)) return 0;                    // a setup line: an arg push when the run
  j = nx(i);                                        // of setups it leads ends in a push
  while (j < nlines && lkind[j] == L_INS && fl_argsetup(lpool + ltext[j])) j = nx(j);
  return j < nlines && lkind[j] == L_INS && !strncmp(lpool + ltext[j], "push\t{", 6);
}
int sprel(int fa, int sz) {
  int i; int e; int j; int k; int d; int mv; int ps; int na; int isbl; int nconv; int o; char *t;
  spconv = 0;
  nwin = 0;                                         // window spans are per-function (3d)
  if (!isins(fa - 1, "mov\tr7, sp")) return 0;        // the prologue as flush() emits it
  if (pnum(fa, "movw\tr1, #") < 0 || pnum(fa + 1, "movt\tr1, #") < 0
    || !isins(fa + 2, "sub.w\tsp, sp, r1")) return 0;   // the reservation trio (sz may be 0)
  e = nlines - 1;
  while (e >= 0 && lkind[e] == L_DEL) e = e - 1;       // the last live line (the pop of pc)
  spholdcvt = 0;                              // band aliasing is a non-issue in the
                                                    // spmap model: every literal maps at
                                                    // its line's depth (band by k+sz+d-4,
                                                    // own-frame words by k+sz+d)
  i = fa + 3; nconv = 0; d = 0; ps = 0; na = 0;    // d: spill depth, words below the frame
                                                   // reservation (Task 3). The trio at
                                                   // fa..fa+2 is fl_frame's and trimframe's
                                                   // own work: always allowed
  if (!sp_syscall_save(fa + 3)) {                    // the line right after the trio sits
    opinfo(fa + 3);                                  // OUTSIDE the walk below: an unknown
    if (opmask & 8192 || oprd == 13) return 0;       // sp move there slips past both walks
  }                                                  // and leaves d wrong at entry (the first
                                                     // line of a function whose body opens
                                                     // with a syscall is its mov r2, sp)
  while (i <= e) {
    if (lkind[i] == L_INS && i != fa - 3) {
      t = lpool + ltext[i];
      if (i >= fa && i <= fa + 2) { i = nx(i); continue; }   // the reservation trio: not body
      if (phold >= 0 && i >= phold && i < phold + 21         // promote's hold loads sit right
        && r7ref(i) == 1 && split(i) >= 0) {         // after the trio and read the GROWN
        k = spmap(split(i), sz, d);                  // incoming band. frameless drops ONE
        if (k < 0 || k > 1020) return 0;             // word - r7's - from the entry list, so
        nconv = nconv + 1; spholdcvt = 1;            // a band word (split >= 0 here) lands
        i = nx(i); continue;                         // k + sz + d - 4 below sp; own-frame
      }                                              // slots keep their offset (spmap; the
                                                     // acceptance scan below proves the
                                                     // frame actually goes)
      opinfo(i); isbl = opkind == 1;                 // a bl stores the call result to a slot
      if (!strncmp(t, "push\t{", 6) && inregs(t + 6, "r7")) { i = nx(i); continue; }
                                                      // an entry push that saves r7 lists it
                                                      // as a register, not a slot (promote
                                                      // grows the list: r7ref's exact-text
                                                      // exemption missed {r4, r7, lr} and its
                                                      // answer 2 bailed every such function)
      if (fl_pc_end(i) && inregs(t + 5, "r7"))        // an exit pop that restores r7: the
        { i = nx(i); continue; }                      // line frameless' own rewrite strips
                                                      // (a bare pop {r7} mid-body still reads
                                                      // the frame: r7ref answers 2, bail)
      if (r7ref(i) == 2) return 0;                    // unknown r7 form
      k = split(i);                                   // split/r7ref parse this line only: read
      if (r7ref(i) == 1) {                            // the values right after their calls
        if (k == -1) {                                // the -1 forms: lea subw/addw convert
          if (!leaoff(i, sz, d, &o)) return 0;        // (leaoff/leawrite: a word's ADDRESS
            nconv = nconv + 1;                        // maps through the same spmap as a
            i = nx(i); continue;                      // read of the word, at this line's
          }                                           // depth, and the line moves nothing);
                                                      // vstr and bare-register r7 still bail.
                                                      // split parses the rest: a literal
                                                      // slot may sit below r7, an unpromoted
                                                      // local at [r7, #-N], and spmap maps
                                                      // it like any other word. The mapping
                                                      // is uniform with ONE split: dropping
                                                      // r7's word from the entry list shifts
                                                      // the CALLER's band down by one, so
                                                      // [r7, #k], k >= 0, reads [sp, #k+sz+d-4];
                                                      // our own words sit in the reservation
                                                      // that hangs off the moved sp, so
                                                      // [r7, #k], k < 0, reads [sp, #k+sz+d],
                                                      // at any spshift (parked registers
                                                      // shift band and frame alike, the walk
                                                      // counts the same words sprel drained).
        k = spmap(k, sz, d);                          // Task 3: the line's live spill depth
        if (k < 0 || k > 1020) return 0;              // out of ldr/str/vldr's immediate window
        nconv = nconv + 1;
      }
      else if (fl_pc_end(i)) {                        // an exit pop: every window must be
        if (na || ps || d) return 0;                  // drained above it. frameless rebuilds
                                                      // the pop from the entry list minus
                                                      // r7, so any word still live below sp
                                                      // would land in the return slot (a
                                                      // tail-call's arg window, with no
                                                      // post-call add before the pop, stops
                                                      // the conversion here).
        opinfo(i);
        if (opmask & 8192 || oprd == 13) return 0;    // the pop must not move sp otherwise
      }
      else {                                          // no r7: spills move sp, everything
        if (!strcmp(t, "mov\tr7, sp") || !strcmp(t, "mov\tsp, r7")) { i = nx(i); continue; }
                                                      // our own bookkeeping: both move sp by
                                                      // r7, which opinfo would flag; frameless
                                                      // deletes them once the function converts
        else if (fl_bl(i))                            // a call line: nothing moves - the
          { i = nx(i); continue; }                    // arg window sits on sp until the
                                                      // post-call add sp, #N claims it (the
                                                      // caller pops, not the callee), and a
                                                      // pending save stays live below (r0
                                                      // dies only at its own add or pop)
        else if (sp_syscall_save(i)) {                // a builtin's argv window: net zero on
          j = i;                                      // sp (see sp_syscall_save). Land just
          while (j < nlines                           // past the pop {r4}; the `add sp, #N`
              && strcmp(lpool + ltext[j], "pop\t{r4}")) j = nx(j);  // that pops the argv
          if (nwin < 128)                             // record the span: fl_frame excuses
            { wstart[nwin] = i; wend[nwin] = j;       // its lines for the acceptance scan
              nwin = nwin + 1; }                      // and frameless' range scan (3d)
          i = nx(j);                                  // words is next and its own arm below
          continue;                                   // claims na, as for a bl's post-call
        }                                             // add (a result save peep moved after
                                                      // the add: fl_resultsave's pv test)
        else if (strsave(i))                          // the str r0, [r7, #-N] right after a
          { ps = 1; d = d + 4; i = nx(i); continue; } // bare bl: stands for the call result
                                                      // save. The str's own slot read above
                                                      // saw the save word still on sp, so the
                                                      // depth grew at the store, not at a push
        else if (fl_resultsave(i))                    // push {r0} right after a bl, or right
          { ps = 1; d = d + 4; i = nx(i); continue; } // after the call's add sp, #N: the call
                                                      // result save. Its word sits below every
                                                      // later push until its own add or pop
                                                      // claims it (na stays: an arg window may
                                                      // already stand above)
        else if (fl_argpush(i)) {                     // an arg push: the word lands at the
          mv = spd(i);                                // push line itself. Its setups answer
          if (mv) { na = na + mv / 4; d = d + mv; }   // too, and add nothing - counting the
          i = nx(i); continue;                        // list keeps the walk independent of
        }                                             // which setup forms peep leaves
                                                      // (add.w's lea is no fl_argsetup)
        else if (!strncmp(t, "add\tsp, #", 9)) {      // the post-call cleanup: pops the
          j = pnum(i, "add\tsp, #") / 4;              // window's newest words first - the
          while (j > 0) {                             // counted arg words (each entered d
            if (na > 0) { na = na - 1; d = d - 4; }   // at its setup line), then a pending
            else if (ps)                              // save's own word, then plain spill
              { ps = 0; if (d > 0) d = d - 4; }       // words below. Args pushed without
            else if (d >= 4) d = d - 4;               // a count (register saves' windows)
            j = j - 1;                                // leave guard words for the pop arms
          }                                           // to claim - their own lines say
          i = nx(i); continue;                        // how many words they take.
        }
        else if (!strncmp(t, "pop\t{", 5) && spd(i) < 0) {  // popping spilled words or a
          j = -spd(i) / 4;                            // pending save: newest first (args
          while (j > 0) {                             // were above the save, the save
            if (na > 0) { na = na - 1; d = d - 4; }   // above plain spills). A pop that
            else if (ps)                              // feeds pc (spd 0) is fl_pc_end's
              { ps = 0; if (d > 0) d = d - 4; }       // own business, and r7 in a list
            else if (d >= 4) d = d - 4;               // only matters once the function is
            j = j - 1;                                // frameless: the pop keeps its real
          }                                           // word count, the conversion never
          i = nx(i); continue;                        // adds or drops pop words.
        }
        else mv = spd(i);                             // depth shrinks, newest first (the
                                                      // counted cases ran above)
        if (mv) { d = d + mv; i = nx(i); continue; }  // a plain spill push/pop shifts sp for
        opinfo(i);                                    // later reads. Anything else that moves
        if (opmask & 8192 || oprd == 13) return 0;    // sp - vpush, a syscall's mov sp, r3, a
        if (opkind == 4) return 0;                    // linked .s - the frameless screen bails
      }
    }
    i = nx(i);
  }
  if (!nconv) return 0;                               // no slot reads: frameless already fires
  if (d) return 0;                                    // the body must end at reservation depth
  if (phold < 0) return 0;                            // frameless strips nothing without
                                                      // promote's prologue (its gate here,
                                                      // BEFORE rewriting: the converted
                                                      // offsets bake in the dropped r7 word,
                                                      // so the frame MUST actually go)
  if (sz > 4095) return 0;                            // reserved frames convert since 3b-iii
                                                      // (frameless rewrites the exit's
                                                      // mov sp, r7 into add sp, #sz): past the
                                                      // add.w immediate the drain has no
                                                      // single form, so those keep the frame
  t = lpool + ltext[fa - 3];                          // frameless' entry test, verbatim
  if (strncmp(t, "push\t{", 6) || !inregs(t + 6, "lr")) return 0;
  spconv = 1;                                         // from here the scan judges the
                                                      // CONVERTED function; any bail below
                                                      // clears it again (line for line the
                                                      // frameless body, kept in step)
  i = nx(fa - 3);                                     // extra pops of pc are fine converted
  while (i < e) {
    if (fl_pc_end(i) && !sprel_ok()) { spconv = 0; return 0; }
    i = nx(i);
  }
  i = fa - 3;                                         // the decisive scan: fl_frame on every
  while (i <= e) {                                    // line but the entry push and the pops
    if (lkind[i] == L_INS) {                          // of pc (frameless rewrites those)
      if (i != fa - 3 && !fl_pc_end(i))
        if (fl_frame(i, fa)) { spconv = 0; return 0; }
    }
    i = nx(i);
  }
  if (fl_outside(e + 1, nlines)) { spconv = 0; return 0; }   // code after the exit pop
  i = fa + 3; d = 0; ps = 0; na = 0;                  // the rewrite walk repeats the screen's
  while (i <= e) {                                    // rules in the same order, so every
    if (lkind[i] == L_INS && i != fa - 3) {           // read lands on its OWN live depth
      if (i >= fa && i <= fa + 2) { i = nx(i); continue; }   // the reservation trio (see above)
      if (spholdcvt && r7ref(i) == 1 && split(i) >= 0)  // hold load the screen converted: the
        { sprewrite(i, spmap(split(i), sz, d));      // same line, same depth (the walk
          i = nx(i); continue; }                     // repeats the screen's rules in order)

      t = lpool + ltext[i];
      opinfo(i); isbl = opkind == 1;
      if (!strncmp(t, "push\t{", 6) && inregs(t + 6, "r7")) { i = nx(i); continue; }
      if (fl_pc_end(i) && inregs(t + 5, "r7")) { i = nx(i); continue; }   // see the screen
      if (r7ref(i) == 1 && split(i) == -1 && leaoff(i, sz, d, &o))
        { leawrite(i, o); i = nx(i); continue; }      // the lea form: the screen's predicate,
                                                      // the same depth, leaoff recomputes o
      if (r7ref(i) == 1 && split(i) != -1) {          // (r7ref answers 0 for non-instructions;
        sprewrite(i, spmap(split(i), sz, d));         // split != -1: a real slot offset, the
                                                      // sign only says whose word it is - own
                                                      // frame slots sit below r7. spmap drops
                                                      // the -4 for them: only the CALLER's
                                                      // dropped r7 word shifts the band)
      }
      else if (!strcmp(t, "mov\tr7, sp") || !strcmp(t, "mov\tsp, r7")) { }   // bookkeeping:
                                                      // nothing to track (the rewrite walk has
                                                      // no opinfo screen: frameless deletes
                                                      // both lines once the function converts)
      else if (fl_pc_end(i)) {                        // an exit pop with a window still
        if (na || ps || d) return 0;                  // open: the screen bailed on the same
      }                                               // line (r7 pops are exempt above)

      else if (strsave(i))                            // the bare-bl result save's store:
        { ps = 1; d = d + 4; i = nx(i); continue; }   // pending +4 below (the screen ran the
                                                      // same arm at this line)
      else {                                          // the screen's arms, same order:
        if (strsave(i))
          { ps = 1; d = d + 4; i = nx(i); continue; }
        if (fl_resultsave(i))
          { ps = 1; d = d + 4; i = nx(i); continue; }
        if (fl_bl(i))                                // nothing moves: the window waits
          { i = nx(i); continue; }                   // for its add sp, #N (the screen)

        if (sp_syscall_save(i))                       // the builtin's argv window: skip it
          { j = i;                                    // exactly as the screen did (net zero:
              while (j < nlines                       // d, ps and na unchanged; the argv pop
                  && strcmp(lpool + ltext[j], "pop\t{r4}")) j = nx(j);  // claims na on its own
            i = nx(j); continue; }                    // arm below
        if (fl_argpush(i)) {                         // arg push: same line-for-line rule
          mv = spd(i);                               // as the screen's arm above (the
          if (mv) { na = na + mv / 4; d = d + mv; }  // word lands at the push line)
          i = nx(i); continue;
        }
        if (!strncmp(t, "add\tsp, #", 9)) {           // the drain arm and the pop-claim
          j = pnum(i, "add\tsp, #") / 4;              // arm, identical to the screen's so
          while (j > 0) {                             // every line sees its own live depth
            if (na > 0) { na = na - 1; d = d - 4; }
            else if (ps)
              { ps = 0; if (d > 0) d = d - 4; }
            else if (d >= 4) d = d - 4;
            j = j - 1;
          }
          i = nx(i); continue;
        }
        if (!strncmp(t, "pop\t{", 5) && spd(i) < 0) {
          j = -spd(i) / 4;
          while (j > 0) {
            if (na > 0) { na = na - 1; d = d - 4; }
            else if (ps)
              { ps = 0; if (d > 0) d = d - 4; }
            else if (d >= 4) d = d - 4;
            j = j - 1;
          }
          i = nx(i); continue;
        }
        mv = spd(i);                                  // a plain spill push/pop moves sp for
        if (mv) d = d + mv;                           // the next slot read
      }
    }
    i = nx(i);
  }
  spconv = 1;
  return 0;
}
int reglist(char *mn, char *last) {         // "mn\t{r4, .., r7, last}" with the promoted registers
  int k;
  ls(mn); ls("\t{");
  k = 0; while (k < npro && k < 3) { ls(regnm(k)); ls(", "); k = k + 1; }
  ls("r7, ");
  while (k < npro) { ls(regnm(k)); ls(", "); k = k + 1; }
  ls(last); ls("}"); lend();
  return 0;
}
// -O, after promote(): a cheaper entry and exit.  The frame reservation (movw/movt r1 at line fa,
// then sub.w sp, sp, r1) of sz bytes becomes nothing or one sub sp, #sz.  With no frame, sp equals
// r7 at the exit, so mov sp, r7 goes, a return that branches to the exit pops right there, and a
// function that never reads r7 (no parameters, nothing promoted) does not set it up.
// frameless(): -O, after promote() and before trimframe(). When nothing between entry and
// exit reads r7 (calllive leaving every slot unpromoted is what makes this common), the
// frame pointer buys nothing: delete mov r7, sp / mov sp, r7 and take r7 out of the save
// lists. sp itself must then stand still between entry and exit, so any sub of sp (a
// by-value struct argument's space), any vpush/vpop (they move sp), and any instruction
// form opinfo does not know all keep the frame. promote parked a value in a register with
// a load at phold? Those lines read r7 too - but they sit inside the hold area, between
// the frame setup and the first body line, and their value dies against r7 before any
// other line reads it: a load of [r7, #off] needs no frame pointer of its own once sp
// stands still (the caller's r7 is at the same place as ours was). So the scan runs from
// after the hold area to the exit pop of pc. trimframe runs after this and copies the
// rewritten pop into the branches that jump straight to the exit.
int inregs(char *t, char *nm) {              // "r7" among the registers of a push/pop list
  int n;                                    // ("{r4, r7, lr}" and friends), whole words only
  int k; char *u;
  n = strlen(nm); u = t;
  while (*u) {
    if (!strncmp(u, nm, n)) {
      k = 0; while (k < n && u[k] == nm[k]) k = k + 1;
      if (k == n && !idchar(u[n])) return 1;
    }
    u = u + 1;
  }
  return 0;
}
int fl_pc_end(int i) {                      // line i is a pop of pc: the exit (a value pop
  char *t;                                  // ends in rX} and is not one)
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  return !strncmp(t, "pop\t{", 5) && !strcmp(t + strlen(t) - 3, "pc}");
}
int fl_r7(int i) {                          // line i needs the frame pointer: it reads r7,
  char *t;                                  // moves sp with vpush/vpop, or is a push/pop
  int k;                                    // that saves or restores r7. A value spill
  if (lkind[i] != L_INS) return 0;          // (push {r0} / pop {r1}) and a builtin's save
  t = lpool + ltext[i];                     // (push {r4}) do not mention r7. An sprel line
  if (sprel_ok() && (split(i) != -1 || leaform(i)))
    return 0;                               // (fl_frame's opinfo test runs before this
                                            // call and excuses the same lines itself; an
                                            // accepted lea is one too - the conversion
                                            // walk rewrites it to addw rD, sp, #o, which
                                            // mentions no r7 and needs no excuse.
                                            // split answers -1 ONLY for instruction
                                            // lines that are not a slot read (the -2
                                            // non-instruction answer is handled by the
                                            // lkind test above; a real offset is a
                                            // multiple of 4, negative for own-frame
                                            // slots): every one of those converts)
  if (!strncmp(t, "push\t{", 6) && inregs(t + 6, "r7"))   // a push that saves r7 keeps the
    return !sprel_ok();                     // frame (the entry line: fl_frame's range test
                                            // exempts it; once sprel converted, frameless
                                            // deletes it and rewrites the list without r7)
  k = r7ref(i);                             // FIRST: it excuses the exact entry and exit
  if (k == 0) return 0;                     // forms (push {r7, lr}, mov r7,sp, mov sp,r7,
                                            // pop {r7, pc}) as well as lines without r7.
  if (!strncmp(t, "push\t{", 6))             // A push with r7 AND registers (promote grew
    return !(!strcmp(t + 6, "r7, lr}"));    // it) is a save the frame still needs: bail.
  if (!strncmp(t, "pop\t{", 5))              // A pop of pc is an exit form (trimframe's
    return !fl_pc_end(i)                    // copied return included): fl_list strips its
      || !inregs(t + 5, "r7");              // r7 below (the grown list too: promote's
                                            // reglist rewrote it); any other list pop
                                            // restores a saved value and keeps the frame
                                            // Any [r7, #off] keeps the frame: once any
                                            // function in the call chain goes frameless it
                                            // saves one word less, so the caller's r7 and
                                            // ours no longer see the same words - an
                                            // incoming read names the wrong word
  if (!strncmp(t, "vpush", 5) || !strncmp(t, "vpop", 4)) return 1;   // they move sp
  return 1;                                 // any [r7, #off] or unknown r7 use: with our
                                            // frame pointer gone r7 holds the caller's fp,
                                            // a different base - the line would name a
                                            // different word
}
int fl_list(int i, char *t, char *mn, char *last) {   // push/pop line t without r7: the
  char *u; int first;                       // promoted registers left with npro = 0, so
  pset(i); ls(mn); ls("\t{");               // the list is whatever remains (lr or pc plus
  u = t; while (*u != '{') u = u + 1;       // any builtin's r4). lr/pc is always last:
  u = u + 1; first = 1;                     // emit it when the rest is done
  while (*u && *u != '}') {
    if (u[0] == 'r' && u[1] >= '0' && u[1] <= '9') {
      if (u[1] != '7') {                         // copy the WHOLE token: promote lists r10
        if (!first) ls(", ");                    // and r11, and a one-digit copy of r10 is
        ls("r"); lpool[lppos] = u[1]; lppos = lppos + 1;   // an r1 that as merges into the
        if (u[2] >= '0' && u[2] <= '9')          // save list - dropping r10/r11's words
          { lpool[lppos] = u[2]; lppos = lppos + 1; u = u + 1; }
        first = 0;
      }
      u = u + 2;
    }
    else u = u + 1;
  }
  if (!first) ls(", ");
  ls(last); ls("}"); lend();
  return 0;
}
// 1 if any line in [a, b) (L_DEL lines skipped) reads or writes r7 or sp beyond what
// fl_r7 excuses. frameless() calls it only for the tail past the exit pop: the buffer
// holds ONE function, and lines before the entry push are its own entry sequence (or
// already deleted), so there is nothing else to see outside fa-3..e except code after
// the return - which a linked .s can add, running in the CALLER's frame with r7/sp of
// its own accord.
int fl_outside(int a, int b) {
  int i;
  if (a < 0) a = 0; if (b > nlines) b = nlines;
  if (a >= b) return 0;
  i = a;
  while (i < b) {
    if (lkind[i] == L_INS) {
      opinfo(i);                            // read the globals before any call clobbers
      if (opmask & 8192 || oprd == 13) return 1;   // them: fl_r7 calls r7ref and friends
      opinfo(i);
      if (fl_r7(i)) return 1;
    }
    else if (lkind[i] == L_BR && lcond[i] < 0)     // an unconditional branch past the exit
      return 1;                               // reaches lines this linear scan skips
    i = nx(i);
  }
  return 0;
}
// 1 if line i forces the frame (or needs no rewrite). opinfo's globals do NOT survive
// calls (fl_pc_end, fl_r7 and friends run opinfo themselves), so every test reads them
// immediately after its own opinfo. The entry push and a pop of pc are exempted by the
// caller; the reservation trio at fa..fa+2 is trimframe's own work and always allowed.
int fl_excused(char *t) {                   // the entry/exit bookkeeping lines verbatim:
  return !strcmp(t, "mov\tr7, sp") || !strcmp(t, "mov\tsp, r7");   // the two sp moves cc
}                                           // itself emits (push/pop have their own rules)
int fl_frame(int i, int fa) {
  char *t;
  t = lpool + ltext[i];
  if (i >= fa - 3 && i <= fa + 2)           // the frame reservation trio at fa..fa+2
    return 0;                               // (trimframe's own work: always allowed)
  opinfo(i);
  if (opkind == 4) return 1;                // unknown form: it might read anything
  if (sprel_ok() && (sp_syscall_save(i) || in_window(i)))   // a builtin's argv window converts
    return 0;                               // with the frame: the walk proved its sp
                                            // moves cancel line for line, and every line
                                            // the prescan skipped is in the span table
  if (sprel_ok() && spholdcvt && phold >= 0         // a converted hold load reads [sp, #N]
    && i >= phold && i < phold + 21                 // by now: it needs no frame pointer
    && r7ref(i) == 1)                               // (the walk rewrote every such line;
    return 0;                                       // none of them reads r7 left)
  if (!strncmp(t, "push\t{", 6))             // a push with r7 (beyond the entry push the
    return inregs(t + 6, "r7");             // caller exempted) saves what the frame holds;
  if (!strncmp(t, "pop\t{", 5))              // a value spill is balanced by its own pop - the
    return !fl_pc_end(i) && !(sprel_ok()    // screen walk proved that - and once sprel
      || inregs(t + 5, "r7"));              // converted, any pop without r7 unwinds only
                                            // spilled values (frameless rewrites the exit).
                                            // A pop WITH r7 restores the frame: it stays
  if (fl_excused(t))                         // mov r7,sp and mov sp,r7 are our own book-
    return 0;                               // keeping: deleted by the rewrite below
  opinfo(i);                                // fl_r7's helpers clobbered the globals, so
  if (opmask & 8192 || oprd == 13)          // re-read them: a line that moves sp for
    return !(sprel_ok() && (split(i) >= 0   // another reason (a linked .s: add sp,
      || leasp(i)                           // r7, #4) keeps the frame. A constant add
      || !strncmp(t, "add\tsp, #", 9)       // or sub of sp is the call cleanup the depth
      || !strncmp(t, "sub\bsp, #", 9)));    // walk drained itself. A converted lea
                                            // (addw rD, sp, #o) only READS sp: opinfo
                                            // flags its source operand (oprd == 13)
                                            // but nothing moves.
  return fl_r7(i)                           // Any other r7 use
    || (!strncmp(t, "vpush", 5) && !strncmp(t, "vpop", 4));   // keeps it; vpush/vpop move
}                                           // sp by 8 and are caught here
int frameless(int fa, int sz) {
  int i; int e; char *t;
  if (phold < 0) return 0;                      // no prologue to strip
  if (sz && !sprel_ok()) return 0;              // a reservation unwinds at the exit through
                                                // mov sp, r7: unless sprel converted the body
                                                // that unwind must stay (an unconverted
                                                // function reads its slots through r7). A
                                                // converted body drains it instead: the loop
                                                // below turns every exit's mov sp, r7 into
                                                // add sp, #sz, which is what lets sz > 0
                                                // convert at all.
  if (sz > 4095) return 0;                      // add sp, #sz needs the add.w immediate form
  if (npro + nfpro && !sprel_ok()) return 0; // promote parked a value in r4-r11 or an s
                                            // register: its load at phold reads OUR r7, the
                                            // stripped save list no longer protects the
                                            // register, and vpush/vpop move sp. Such a
                                            // function keeps its frame pointer; calllive()
                                            // is what drives npro to 0 for the functions
                                            // whose values no callee-saved register survives
                                            // a call in. An sprel-converted function is the
                                            // exception: its hold loads read [sp, #N] now and
                                            // fl_list drops the parked registers with r7.
  t = lpool + ltext[fa - 3];                          // the entry sequence as emitted: push
  if (strncmp(t, "push\t{", 6) || !inregs(t + 6, "lr")) return 0;   // r7/lr (or the reglist
                                                     // promote grew it into), (vpush), mov r7,sp
  e = nlines - 1;                           // the exit: trimframe's own test - the last
  while (e >= 0 && lkind[e] == L_DEL) e = e - 1;   // live line, a pop of pc, or no rewrite
  if (e < 0 || !fl_pc_end(e)) return 0;
  i = nx(fa - 3);                           // an earlier pop of pc is trimframe's copied
  while (i < e) {                           // return: more than one exit keeps the frame,
    if (fl_pc_end(i) && !sprel_ok())        // unless sprel converted the function: then
      return 0;                             // every exit sits at reservation depth (A1: no
                                            // body sp moves) and the rewrite deleted the
                                            // mov sp, r7 that made an early pop correct -
                                            // each inline pop unwinds exactly what entry
                                            // pushed. (the copy happens after this pass)
    i = nx(i);
  }
  i = fa - 3;                               // the decisive range: from the entry push to
  while (i <= e) {                          // that exit. The entry push at fa-3 and the
    if (lkind[i] == L_INS) {                // exit pop of pc are exempt: they are rewritten
      t = lpool + ltext[i];                 // below once the scan is through
      if (i != fa - 3 && !fl_pc_end(i)) {   // pops of pc: fl_list strips them
        if (fl_frame(i, fa)) return 0;      // one test per line: opinfo's globals do not
                                            // survive the helpers, so nothing reads them
                                            // across a call here
      }
    }
    i = nx(i);
  }
  if (fl_outside(e + 1, nlines)) return 0;  // a linked .s can leave code after the exit
                                            // pop: it runs in the caller's frame and
                                            // reads the caller's r7/sp
  i = 0;                                    // fl_list rebuilds the two lists without r7:
  while (i < nlines) {                      // what remains of each list line is kept as
                                            // written (lr or pc plus any builtin's r4).
                                            // npro keeps its value on purpose: flush()'s
                                            // final peep() gates its promoted-register
                                            // merges on it, and a converted function still
                                            // has those registers. The next function resets
                                            // it (parse_global's tail), and trimframe's nor7
                                            // only decides deletion of the mov lines that
                                            // were just deleted here.
    if (lkind[i] == L_INS) {
      t = lpool + ltext[i];
      if (!strcmp(t, "mov\tr7, sp")) pdel(i);
      else if (!strcmp(t, "mov\tsp, r7")) {      // the exit's unwind: nothing to drain at
        if (!sz) pdel(i);                      // sz == 0; the reservation's drain otherwise
        else { pset(i); ls("add\tsp, #"); ls(dec(sz)); lend(); }   // (as picks add.w past the
      }                                        // 8-bit form; the gate capped sz at 4095)

      else if (i == fa - 3 && inregs(t + 6, "r7")) fl_list(i, t, "push", "lr");
      else if (fl_pc_end(i) && inregs(t + 5, "r7")) fl_list(i, t, "pop", "pc");
    }
    i = i + 1;
  }
  return 0;
}
int trimframe(int fa, int sz) {
  int i; int e; int nl; int k; int nor7; char *pop; int labs[8];
  if (pnum(fa, "movw\tr1, #") != (sz & 65535) || pnum(fa + 1, "movt\tr1, #") != ((sz >> 16) & 65535)
    || !isins(fa + 2, "sub.w\tsp, sp, r1")) return 0;
  if (sz > 0) {
    if (sz > 508) return 0;
    pset(fa); ls("sub\tsp, #"); ls(dec(sz)); lend(); pdel(fa + 1); pdel(fa + 2);
    return 0;
  }
  pdel(fa); pdel(fa + 1); pdel(fa + 2);
  nor7 = !npro && !nfpro;
  i = 0;
  while (i < nlines) {
    if (isins(i, "mov\tsp, r7")) pdel(i);
    else if (r7ref(i)) nor7 = 0;
    i = i + 1;
  }
  if (nor7) { i = 0; while (i < nlines) { if (isins(i, "mov\tr7, sp")) pdel(i); i = i + 1; } }
  e = nlines - 1;                            // the exit: labels, then the pop of pc (no vpop)
  while (e >= 0 && lkind[e] == L_DEL) e = e - 1;
  if (e < 0 || !preins(e, "pop\t{")) return 0;
  pop = lpool + ltext[e];
  k = strlen(pop);
  if (k < 3 || strcmp(pop + k - 3, "pc}")) return 0;   // only the return, never a pop of a value
  nl = 0; i = e - 1;
  while (i >= 0 && nl < 8 && (lkind[i] == L_DEL || lkind[i] == L_LAB)) {
    if (lkind[i] == L_LAB) { labs[nl] = llab[i]; nl = nl + 1; }
    i = i - 1;
  }
  i = 0;
  while (i < e) {
    if (lkind[i] == L_BR && lcond[i] < 0) {
      k = 0; while (k < nl && labs[k] != llab[i]) k = k + 1;
      if (k < nl) { pset(i); ls(pop); lend(); }
    }
    i = i + 1;
  }
  return 0;
}
// calllive(): does a value this frame slot holds ever live across a call?  A callee-saved
// register only pays for itself when it does: bl clobbers everything past r0-r3, so a slot
// whose every value dies before (or at) each call loses nothing by staying in its frame
// word.  promote() asks this while selecting, before it rewrites any line, so dropping a
// slot needs no undo.  The unit is the SLOT VALUE, not a register: a store writes one
// (born), the next store overwrites it (dies), and every load in between reads it.  A
// value born at s that some load at e reads keeps the slot when a call lies on any path
// from s to e - cl_reach answers reachability (fallthrough, forward branch targets, and a
// loop's back edge covering s: fib's parameter is read again after every recursive call),
// and the linear window in cl_between_call covers calls on all those paths at once.
// Unknown instruction forms keep the slot; an unknown r7 use makes promote() bail anyway.
// Call-free functions are exempt wholesale (has_calls): nothing dies at a call that never
// happens, and their shapes stay exactly as promote() made them today.
int has_calls;
int iscall(int i) {
  char *t;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (!strncmp(t, "bl\t", 3)) return 1;
  if (!strncmp(t, "blx\t", 4)) return strncmp(t + 4, "r8", 2);   // blx r8 is a syscall
  return 0;
}
int cl_reach(int s, int e) {                  // can control flow get from line s to line e?
  int i; int k;                               // (s <= e assumed). Forward: fallthrough and
  if (e <= s) return 1;                       // branch targets - a conditional branch also
  i = nx(s);                                  // falls through, only an unconditional one or
  while (i < nlines && i <= e) {              // a return ends the straight path. Backward:
    if (lkind[i] == L_BR) {                   // a value written in a loop body is read again
      k = labat(llab[i]);                     // next round when the loop's back edge covers
      if (k < 0) return 1;                    // s - licm's shape, read from inside outward.
      if (k <= e && k > i) return 1;          // A return on the way stops fallthrough; a
      if (k < i && k <= s) return 1;          // loop that falls out at its end never
      if (lcond[i] < 0) { i = nx(i); continue; }   // re-enters, so only heads at or above s
    }                                         // carry a value up. An unresolvable target
    else if (lkind[i] == L_INS) { opinfo(i); if (opkind == 2) return 0; }   // reaches: sure
    i = nx(i);
  }
  if (i > e) return 1;                        // fell through to e
  return 0;
}
int calllive(int slot) {
  int i; int j; char *t;
  if (!has_calls) return 1;
  i = 0;
  while (i < nlines) {                        // an unknown form might be a call: all or nothing
    if (lkind[i] == L_INS) { opinfo(i); if (opkind == 4) return 1; }
    i = i + 1;
  }
  j = 0;                                      // birth of the value currently in the slot:
  i = 0;                                      // entry until the first store writes one
  while (i < nlines) {
    if (r7ref(i) == 2) return 1;              // unknown r7 form: promote() bails anyway
    if (r7ref(i) != 1 || slotix(rfoff) != slot) { i = i + 1; continue; }
    t = lpool + ltext[i];
    if (t[0] == 's') j = i;                   // a store: this value is born here
    else if (cl_between_call(j, i)) return 1; // a load reads the value born at j, and a call
    i = i + 1;                                // lies between on some path
  }
  return 0;                                   // no value of this slot ever crosses a call
}
int cl_between_call(int s, int e) {           // any call from line s to line e on some path?
  int i;
  if (!cl_reach(s, e)) return 0;            // unreachable: nothing lives that way
  i = s + 1;                                  // reachable: the linear window covers the
  while (i < nlines && i <= e) {              // straight path and any swept loop body alike
    if (iscall(i)) return 1;
    i = i + 1;
  }
  return 0;
}
int promote() {
  int i; int k; int n; int w; int d; int best; int bw; int sh; char *t; char *rn;
  if (!slotw) { slotw = malloc(512 * 4); pdep = malloc(16400 * 4); }
  npro = 0; nfpro = 0;
  if (phold < 0) return 0;
  i = 0; while (i < 512) { if (slotk[i]) slotw[i] = -1; else slotw[i] = 0; i = i + 1; }
  i = 0; while (i <= nlines) { pdep[i] = 0; i = i + 1; }
  has_calls = 0; i = 0;
  while (i < nlines) { if (iscall(i)) has_calls = 1; i = i + 1; }
  i = 0;
  while (i < nlines) {                      // a branch back to label t makes lines t..i a loop
    if (lkind[i] == L_BR) { k = labat(llab[i]); if (k >= 0 && k < i) { pdep[k] = pdep[k] + 1; pdep[i + 1] = pdep[i + 1] - 1; } }
    i = i + 1;
  }
  i = 1; while (i < nlines) { pdep[i] = pdep[i] + pdep[i - 1]; i = i + 1; }
  i = 0;
  while (i < nlines) {
    k = r7ref(i);
    if (k == 2) return 0;
    if (k == 1) {
      n = slotix(rfoff);
      if (n >= 0 && slotw[n] >= 0) {
        if (!rfplain || (rfoff & 3) || rfoff == 0 || rfoff == 4) slotw[n] = -1;
        else {
          w = 1; d = pdep[i];
          while (d > 0 && w < 100000) { w = w * 8; d = d - 1; }
          slotw[n] = slotw[n] + w;
        }
      }
    }
    i = i + 1;
  }
  nfpro = 0;
  while (npro < 5 || nfpro < 16) {
    best = -1; bw = 0; i = 0;
    while (i < 512) {
      w = slotw[i];
      if (i >= 258 && w > 0) w = w - 1;     // a parameter costs a load in the prologue
      if (w > 0 && !calllive(i)) w = 0;      // dead at every call: no register for it
      if (slotf[i] && (nfpro >= 16 || (i - 256) * 4 > 900)) w = 0;   // vldr reaches 1020
      if (!slotf[i] && npro >= 5) w = 0;
      if (w > bw) { bw = w; best = i; }
      i = i + 1;
    }
    if (best < 0) break;
    if (slotf[best]) { slotw[best] = -20 - nfpro; nfpro = nfpro + 1; }
    else { slotw[best] = -2 - npro; npro = npro + 1; }
  }
  if (!npro && !nfpro) return 0;
  sh = 4 * (npro + nfpro);                  // the words reglist() grows the push list by:
                                            // promote shifts every un-promoted parameter's
                                            // [r7, #N] load with them (its own hold loads at
                                            // phold read the grown offsets), so this stays
                                            // promote's business; sprel() reads each line's
                                            // own literal instead and needs no shift here
  i = 0;
  while (i < nlines) {
    k = r7ref(i);
    if (k == 1) {
      n = slotix(rfoff);
      if (n >= 0 && slotw[n] <= -20) {
        t = lpool + ltext[i]; pset(i); ls("vmov\t");
        if (t[0] == 'l') { ls("r"); lpool[lppos] = t[5]; lppos = lppos + 1; ls(", s"); ls(dec(-4 - slotw[n])); }
        else { ls("s"); ls(dec(-4 - slotw[n])); ls(", r"); lpool[lppos] = t[5]; lppos = lppos + 1; }
        lend();
      }
      else if (n >= 0 && slotw[n] <= -2) {
        t = lpool + ltext[i]; rn = regnm(-2 - slotw[n]);
        pset(i); ls("mov\t");
        if (t[0] == 'l') { lpool[lppos] = 'r'; lpool[lppos + 1] = t[5]; lppos = lppos + 2; ls(", "); ls(rn); }
        else { ls(rn); ls(", r"); lpool[lppos] = t[5]; lppos = lppos + 1; }
        lend();
      }
      else if (rfoff > 4) r7shift(i, rfoff + sh);
    }
    else if (isins(i, "push\t{r7, lr}")) { pset(i); reglist("push", "lr"); }
    else if (isins(i, "pop\t{r7, pc}")) { pset(i); reglist("pop", "pc"); }
    i = i + 1;
  }
  if (nfpro) {
    pset(pvpush); ls("vpush\t{s16"); if (nfpro > 1) { ls("-s"); ls(dec(15 + nfpro)); } ls("}"); lend();
    pset(pvpop); ls("vpop\t{s16"); if (nfpro > 1) { ls("-s"); ls(dec(15 + nfpro)); } ls("}"); lend();
  }
  n = phold; i = 258;
  while (i < 512) {                         // promoted parameters: load them once
    if (slotw[i] <= -20) {
      pset(n); ls("vldr\ts"); ls(dec(-4 - slotw[i])); ls(", [r7, #"); ls(dec((i - 256) * 4 + sh)); ls("]"); lend();
      n = n + 1;
    }
    else if (slotw[i] <= -2) {
      pset(n); ls("ldr\t"); ls(regnm(-2 - slotw[i])); ls(", [r7, #"); ls(dec((i - 256) * 4 + sh)); ls("]"); lend();
      n = n + 1;
    }
    i = i + 1;
  }
  return 0;
}
// ---------------------------------------------------------------- licm(): loop-invariant code motion
// the ALU lines licm() may hoist. ldr and friends are never candidates: no alias analysis
// exists, so a store through a pointer may change what a load would return. cmp/tst and the
// branch forms stay put too: they set flags other lines read, and moving a compare across a
// loop head is not licm()'s job.
// the instructions licm() may move at all: only address and value math whose result a later
// line reads through a register. A load's value depends on memory the loop may write (no
// alias analysis: ldr is no candidate), and cmp/tst and the branch forms write flags the
// loop head owns, so they stay put too
int licm_mnem() {
  return mnis("mov") || mnis("movw") || mnis("movt")
    || mnis("movw.16")
    || mnis("add") || mnis("adds") || mnis("add.w") || mnis("addw")
    || mnis("sub") || mnis("subs") || mnis("subw")
    || mnis("mul") || mnis("muls") || mnis("lsls") || mnis("lsrs");
}
// the text of line j with every r9 spelled "rB" (a name no real register reaches: idchar('B')
// is 0), saved in licm_save; the number of r9s, or -1 if the line does not read r9 at all.
// Only such lines move: licm() rewrites every placeholder in the body's copy, so a line with no
// r9 has nothing to say here and answers -1 (a symbol reference is immovable by itself). The
// copy lives outside lpool on purpose: a candidate
// at the pool's tail would lose its own bytes to its placeholders (ls() appends), and a rejected
// line must stand exactly as it was for opinfo(), dead() and the output. licm_to() writes the
// saved text into the pool when the line really moves, and licm_ok() reads the answer right
// after this: opinfo() runs again then, because scanning resets opmn. One candidate is live at
// a time: licm_rtext fills the buffer, licm_ok reads it, licm_to writes it out.
char *licm_save; int licm_savelen;
char licm_buf[600];                        // (licm_to(): a copy of the saved text, see there)
int outl[64];                              // (licm(): labels outside the loop a body branch reaches)
int licm_rtext(int j) {
  char *t; int prev; int c; int e;
  if (!licm_save) licm_save = malloc(512);
  t = lpool + ltext[j]; prev = 0; c = 0; e = 0;
  { char *tt = t;                            // a line licm() moved carries the mark where its
    while (*tt) {                            // base stood: it reads r9 no more, and moving one
      if (!*tt && !tt[1]) {                  // again would rewrite the placeholder in its own
        opinfo(j); return -1;                // text. Refuse it here (opinfo: the scan below
      }                                      // never runs then, so opmn must be set)
      tt = tt + 1;
    }
  }
  while (*t) {                               // (the mark is two NULs, so this scan stops at it:
    if (!t[1]) break;                        // a line that carries one never gets this far)
    if (idstart(*t)) {
      if (!prev || !idchar(prev)) {
        if (t[0] == 'r' && t[1] == '9' && !idchar(t[2])) {   // the data base: a placeholder,
          licm_save[e] = 0; licm_save[e + 1] = 0;            // marked with two NULs: idchar(0)
          e = e + 2; t = t + 2; c = c + 1; prev = '9'; continue;   // is 0, so no line's own bytes
        }                                           // can spell the mark by accident
      }
      while (*t && idchar(*t)) { licm_save[e] = *t; e = e + 1; t = t + 1; }
    }
    else { licm_save[e] = *t; e = e + 1; t = t + 1; }
    prev = *(t - 1);
  }
  licm_savelen = e;                          // (the text, without its terminator)
  licm_save[e] = 0;                          // (a string: probes and later reads stop at it)
  if (c) return c;                           // it reads r9: the saved text is what moves
  opinfo(j);                                 // (scanning the copy reset opmn: licm_ok() reads
  return -1;                                 // the mnemonic, and lpool never saw a byte)
}
// the hoisted text of line j at the end of the pool (rB for every base register), its r9
// count, or -1 if licm() may not move the line. Only a line that reads r9 moves: its saved
// text becomes the body's copy with the placeholder, and the insertion point's copy with r9.
// A symbol's movw/movt pair stays put: hoisting one half alone would split the chain, and ld
// resolves the symbol from each line's own text.
int licm_ok(int j) {
  int c;
  c = licm_rtext(j);                         // (it re-reads the line into opinfo: its scan
  if (c > 0) return c;                       // reset opmn, and licm() reads the mnemonic after)
  return -1;                                 // only a line that reads the data base moves: its
}                                            // marked text is saved for licm_to(). A symbol's
                                             // movw/movt half stays put: hoisting one half alone
                                             // splits the chain, and ld reads the symbol from
                                             // each line's own text
// line j written at the pool's tail, every base register spelled `to` (the caller's ls()/
// lend() terminate it). licm_ok() saved this line's text with its r9s marked by two NULs:
int licm_to(int j, char *to, int nr) {       // that is what moves, the mark spelled `to` here.
  char *t; int e;                            // A line licm_ok kept no text for copies its own
                                             // bytes verbatim: it reads no base, so there is
  if (nr > 0 && licm_savelen) {              // nothing to replace. One byte past the saved text:
    e = licm_savelen + 1;                    // its NUL, so a mark at the very end does not
    while (e) { e = e - 1; licm_buf[e] = licm_save[e]; }   // clobber text this writes. The copy
    t = licm_buf; e = 0;                     // goes into a buffer of its own first: nothing may
    while (e < licm_savelen) {               // write licm_save between licm_ok() and here. The
      if (!t[0] && !t[1]) {                  // walk is by position: the old one ran while (e) on
        lpool[lppos] = to[0]; lpool[lppos + 1] = to[1];     // a countdown, and a mark at the very
        lppos = lppos + 2; t = t + 2; e = e + 2;           // end drove its counter negative and
      }                                      // copied bytes past the copy into the line's text
      else { lpool[lppos] = t[0]; lppos = lppos + 1; t = t + 1; e = e + 1; }
    }
    return 0;
  }
  t = lpool + ltext[j];                      // (the line's own text up to its NUL; lend() ends it)
  while (*t) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  return 0;
}
// licm(): move loop-invariant lines out of loops. A branch back to a label at or before it
// makes a loop, the same shape promote() reads. licm(hl) scans the loop forward: a known ALU
// line is invariant when every register it reads was written before the loop or by an earlier
// invariant line. That covers chains: in matmul's inner loop today the movw/movt pair for a
// global's offset and the add.w r9 that turns it into an address rebuild on every iteration,
// and all three lines move because each reads only what the previous one wrote (the symbol
// half of the pair moves when dead() says its value dies at once). The original goes away
// (delete-only, per the design note): sound because nothing in the loop writes the
// destination, no branch enters the body below the head (the back edge aside), and rB names a
// register only licm() writes. A line that reads r9 keeps it at the insertion point and
// becomes rB in the body: the data base survives calls, peep() runs again after licm_loops()
// so peep_tohi folds "add.w r0, r9, rB ; mov rP, r0" into "add.w rP, r9, rB", and promote()
// gives the hoisted base a callee-saved register. A loop that makes a call hoists nothing
// else: bl clobbers r0-r3 and r12, so a line writing one of those moves out on its own only
// when the body makes no call at all. Flags need no care: licm_mnem's lines never read them,
// so moving one keeps every flag use in place. A loop whose body holds an unknown line, an it
// block, stack traffic, or any v line is skipped whole. Inserting a line shifts the line
// numbers after it, so licm_loops() rescans from the top and hoists loops out one at a time:
// the innermost first, because the scan meets its back edge first.
// livo[n]: line n carries the text licm() moved out of a loop: its tail holds the NUL mark
// where its base register stood. Nothing may move twice, so licm() refuses a loop whose lines
// livo already marks (the body scan in pass 2 skips such a line, but one between c and hl
// would hide from it, and the guard after licm_ok() catches what is left)
int *livo;               // livo[n]: line n is invariant in the loop being hoisted
int licm(int h);
int licm_loops() {
  int i; int k; int round; int moved;
  if (!livo) { livo = malloc(16400 * 4); }
  round = 0;
  i = 0; while (i < 16400) { livo[i] = 0; i = i + 1; }   // (marks stand across flushes: cc.c
                                           // compiles itself, and a new function's lines must
                                           // not inherit one. licm() sets them as it moves)
  while (round < 512) {                    // one move per pass: line numbers shift under it
    round = round + 1;
    moved = 0;                             // (a loop that stays put must not spin the rounds:
                                           // the scan breaks only when a licm() call moves one)
    i = 0; while (i < nlines) { if (lkind[i] == L_LAB && llab[i] >= 0 && llab[i] < 65536) labline[llab[i]] = i; i = i + 1; }   // (a hoist's insert shifts every label line after it: the
                                           // scan reads labline, so it is rebuilt from the lines
                                           // each round instead of tracked through the shifts)
    i = 0;
    while (i < nlines) {                   // a branch back to label t makes a loop at line t
      if (lkind[i] == L_BR) { k = labat(llab[i]); if (k >= 0 && k < i) { if (licm(k)) { moved = 1; break; } } }
      i = i + 1;
    }
    if (!moved) break;                     // nothing moved anywhere: the work is done
  }
  return 0;
}
// licm(hl): hoist what is invariant in the loop whose head label's line is hl, and answer 1
// if anything moved (the caller rescans: the insertion shifted every line number after it).
// peep() has rewritten the branches: a conditional branch to the head sits above the label and
// an unconditional one below it. The loop runs from the first of those, "c", through the body
// and the back edge b; the lines between c and hl are part of its test, not a preheader.
int licm(int hl) {
  int i; int j; int k; int m; int n; int r; int moved; int wmask; int dmask; int nr; int b; int calls; int c; int nout; int changed; int t; int cstack; int pool_at; int sl; char *t2; char *w;
  if (lkind[hl] != L_LAB) return 0;
  c = hl;                                  // c: the loop's first line, a branch back to the head
  while (c > 0) { k = pv(c);               // (a branch may spell the head's number differently,
    if (lkind[k] != L_BR) break;           // peep() has rewritten some of them: labat says where
    if (labat(llab[k]) != hl) break;       // a branch really lands)
    c = k;
  }
  b = -1;                                  // the back branch: the last line that reaches hl again
  i = nx(hl);
  while (i < nlines) {                     // labat, not llab: peep() rewrites some branches so
    if (lkind[i] == L_BR && labat(llab[i]) == hl) b = i;   // they no longer spell the head's own
    i = nx(i);                             // number. Every back edge counts: see pass 1
  }
  if (b < 0 || b <= hl) return 0;          // no back edge, or an empty body
  dmask = 0; calls = 0; cstack = 0;
  nout = 0;                                // outl[] starts with the head's own label: a branch
  m = llab[hl];                            // from the body to it would enter below the hoisted
  if (m >= 0) {                            // line (and its flags, which the head sets). The
    outl[nout] = m; nout = nout + 1;       // walk below tests every branch's target against it
  }                                        // so a label two lines reach is no entry either
  i = c;                                   // pass 1: what the loop writes, and whether it is safe at all
  while (i < b) {
    if (lkind[i] == L_INS) {
      opinfo(i);
      if (opkind == 1) calls = 1;          // a call: r0-r3 and r12 do not survive it
      else if (opkind != 0) return 0;                   // an it block or unknown line: skip the loop
                                           // (a cmp in the body sets no flags a later line
                                           // reads: peep() leaves only dead ones. licm() moves
                                           // one line per round, and pass 2 below marks lines
                                           // in order, so what reaches the head's test stands
                                           // above every hoisted line already)
      if (mnis("pop")) {                                // a pop writes its list: those registers
        t2 = lpool + ltext[i];                          // are not candidates. A push only reads
        while (*t2) {                                   // its source and stores to the stack
          if (!*t2 && t2[1] == 0) break;                // (opinfo() leaves a pop's list in opmask
          if (idstart(*t2)) {                           // only for its pc case: read the tokens.
            w = t2;                                     // A hoisted line's NUL mark ends the
            while (idchar(*t2)) t2 = t2 + 1;            // scan: regword sees no register in it)
            while (idchar(*t2)) t2 = t2 + 1;
            r = regword(w, t2 - w);
            if (r >= 0) dmask = dmask | (1 << r);
          }
          else t2 = t2 + 1;
        }
      }
      if (lpool[ltext[i]] == 'v') return 0;                             // float code stays put
      if (oprd >= 0) dmask = dmask | (1 << oprd);   // (an unknown destination writes no register
                                                    // name opinfo() knows: str, it blocks)
    }
    i = nx(i);
  }
  // licm() inserts above the head label, so a line that reaches the body without passing the
  // head would miss the hoisted line. The walk starts with every label outside c..b that a
  // branch from hl to b reaches (the loop's own exits: cc builds loops whose exit test branches
  // back into this very body, and licm_loops() accepts the innermost back edge first, so an
  // outer loop is only walked once every loop inside it has been hoisted from or rejected).
  // It grows two ways: fall-through from a reached label, and what a line at or after a reached
  // label branches to. A label two lines reach is no entry either: nothing below it runs except
  // from inside the body.
  i = hl;
  while (i <= b) {                          // inclusive: b itself branches back to hl, and a
    if (lkind[i] == L_BR && llab[i] >= 0) { // branch at b lands on the head like any other
      m = labat(llab[i]);
      if (m < c || m > b) {                 // a target outside the loop: where an entry would be
        if (nout < 64) outl[nout] = llab[i];
        nout = nout + 1;
      }
    }
    i = nx(i);
  }
  changed = 1;
  while (changed) {                         // follow the entry labels to fixpoint
    changed = 0;
    r = 0;
    while (r < nout && r < 64) {
      m = outl[r]; r = r + 1;
      t = labat(m);
      if (t < 0) return 0;                  // no line carries the label: fail closed
      i = c;                                   // (the walk starts at the region's first line: a
      while (i < b) {                          // branch in c..hl lands on the head's test, which
        k = nx(i);                             // runs after it. A body line only falls through
        if (lkind[i] == L_INS && t == k) return 0;             // into the reached label only by
        i = k;                                 // next line is at or before t)
      }
      n = -1; i = c;                           // what a line at or after the region's first line
      while (i < nlines) {                     // branches to is reachable too: from inside c..b it
        if (lkind[i] == L_BR && llab[i] >= 0 && labat(llab[i]) == m) {   // would enter below the
          if (i > hl) return 0;                // head: its flags are set there, and a branch in
          if (n < 0) n = i;                    // c..hl lands on the head's test after the hoist
        }
        i = nx(i);
      }
      if (n >= 0) {                         // its own target joins the walk (m itself: reached)
        if (nout < 64) outl[nout] = llab[n];
        nout = nout + 1;
        changed = 1;
      }
    }
  }
  wmask = 1 << 9; moved = 0; cstack = 0;    // pass 2: mark the invariant lines, in order. The
  i = c;                                    // walk above proved no body branch reaches a label
                                            // that falls into the body, so only fall-through
                                            // enters it: r9 (the data base) and every register
                                            // the loop never writes reach the head unchanged
                                            // from outside. wmask grows with each line's dest;
                                            // a source outside it is set inside the loop, and
                                            // a chain through that one stays put
  while (i < b) {                           // from c: a branch to the head above the label reads
                                            // its flags, and they are set inside the loop
    if (lkind[i] == L_INS) {
      opinfo(i);
      if (opkind == 1) calls = 1;           // a call: r0-r3 and r12 do not survive it
      if (mnis("push")) cstack = cstack | opmask;   // a push reads its source here and hands the
      if (mnis("pop")) {                    // value to the matching pop: neither register may be
        t2 = lpool + ltext[i];              // hoisted, nor anything in between (the stack model
        while (*t2) {                       // is one push per register). A pop whose register no
          if (idstart(*t2)) {               // push here wrote reads a value from outside the
            w = t2;                         // loop: nothing above it moved by mistake, so its
            while (idchar(*t2)) t2 = t2 + 1;   // candidates may still move. dmask in pass 1
            r = regword(w, t2 - w);         // keeps such a register away from the candidates
            if (r >= 0 && (cstack & (1 << r))) cstack = cstack & ~(1 << r);   // of later rounds
          }
          else t2 = t2 + 1;
        }
      }
      // dmask plays no part here: a register the loop writes every round (the counter, r0-r2)
      // is still a constant source for a line whose own value is fixed at the head. wmask says
      // which sources are read before the loop writes them, and that is the test pass 2 needs.
      // cstack guards both ways: a candidate may not write a register whose value is on the
      // stack (its pop would read the hoist's result), nor read one (hoisting it above the
      // push would pass the push's earlier value by). The second test holds for chains too:
      // every line between the pushed line and this one is a candidate, so the stack sits
      // above all of them once they move.
      opinfo(i);
      t2 = lpool + ltext[i];                // (a line licm() moved before carries the NUL mark
      n = 0;                                // where its base stood: it stands for r9, so the
      while (*t2) {                         // walk stops: never move a line twice. Scanning
        if (!*t2 && t2[1] == 0) { n = 1; break; }   // for an "rB" token would not do: a moved
        t2 = t2 + 1; }                      // chain's movw names symbols that could spell one)
      if (n) { i = nx(i); continue; }
      n = 0; t2 = lpool + ltext[i];          // (only a line that reads the data base is worth
      while (*t2) {                          // hoisting: address math on r9 repeats every round
        if (idstart(*t2)) {                  // with the same result. A bare constant like
          w = t2;                            // movw r0, #1 moves forever for nothing: it reads
          while (idchar(*t2)) t2 = t2 + 1;   // no base, and a moved line's mark stops its
          if (t2 - w == 2 && w[0] == 'r' && w[1] == '9') n = 1;   // chain's add.w from moving
          continue;                          // again, so the halves stay where they are)
        }
        t2 = t2 + 1;
      }
      if (!n) { i = nx(i); continue; }       // (no base read: leave the line where it is)
      if (licm_mnem() && oprd >= 0 && oprd <= 12 && !(cstack & ((1 << oprd) | opmask))) {
        m = opmask; n = 0;
        while (n < 16) { if ((m & (1 << n)) && !(wmask & (1 << n))) break; n = n + 1; }   // (not
        if (n == 16 && licm_ok(i) >= 0) { livo[i] = 1; moved = moved + 1;   // dmask: a line may
          wmask = wmask | (1 << oprd);      // the line moves too: later lines may still read it
        }
        else wmask = wmask | (1 << oprd);   // its value is fixed at the head: later lines may read it
      }
    }
    i = nx(i);
  }
  if (!moved) return 0;
  i = c;                                    // (a line an earlier round moved carries the mark in
  while (i < b) {                           // its text, and pass 2 left livo standing for it:
    if (livo[i]) return 0;                  // refuse a second move before touching anything.
    i = nx(i);                              // The guard after licm_ok() catches the rest)
  }
  j = -1; i = b - 1;                        // the bottommost candidate: a chain moves as add.w,
  pool_at = lppos;                          // movt, movw, in order. The pool's tail: licm_to
                                            // appends from here, and a late licm_ok failure
                                            // rewinds it (the line's own text stands until then)
  while (i > c) { if (livo[i]) { j = i; break; } i = pv(i); }   // movt, movw, in order
  if (j < 0) return 0;
  nr = licm_ok(j);                          // > 0: the hoisted text, marked at every base
  sl = licm_savelen;                        // (a symbol's movw/movt keeps licm_save from an
                                           // earlier line: pin this line's own length now)
  if (nr <= 0) { lppos = pool_at; return 0; }   // (the candidate died at the last look: undo
                                           // anything its check appended to the pool)
  opinfo(j);                                 // (licm_rtext's scan reset opmn)
  if (calls && oprd <= 3) { lppos = pool_at; return 0; }   // bl clobbers r0-r3 and r12: its
                                           // value would not live past the call (rewind too)
  k = pv(hl);                               // an empty line before the head label, or insert one
  licm_savelen = sl;                        // (dead() above ran licm_ok() for another line:
                                           // licm_to() reads licm_save for this one)
  m = nlines - 1;                           // nothing may move twice. livo is the record of
  while (m > k) { if (livo[m]) { lppos = pool_at; return 0; }   // what moved: its shift above
    m = m - 1; }                            // runs after this point, so a marked line can only
                                           // stand below the head now. Pass 2 skips marks in the
                                           // body, but one between c and hl would hide from it
  if (k >= 0 && lkind[k] == L_DEL) {       // the line above the head was deleted: its text is
    pool_at = lppos;                        // free pool space. Write the hoist's r9 copy there:
    licm_to(j, "r9", nr); lend();     // the pool only grows, and this space would go unused
                                           // anyway (no live line points at it: a hoisted line
                                           // points at its own text, written below)
    lkind[k] = L_INS; ltext[k] = pool_at;   // (pset after lend left k at the NEXT line's text:
                                           // every round rewrote that line with this one's copy)
  }
  else {
    m = nlines; nlines = nlines + 1;        // shift the lines from hl up by one and make room
    while (m > hl) { lkind[m] = lkind[m - 1]; ltext[m] = ltext[m - 1]; llab[m] = llab[m - 1]; lcond[m] = lcond[m - 1]; m = m - 1; }
    pool_at = lppos;                        // (the new line's text starts here: lend() moves the
                                           // tail, and licm_to() writes from this point)
    licm_to(j, "r9", nr); lend();     // at the head: the mark becomes r9 again. The pool
    ltext[hl] = pool_at;                    // write comes first: a line's text must never point
                                           // at another line's text (ltext[j] still does here)
    lkind[hl] = L_INS; llab[hl] = -1; lcond[hl] = -1;
    k = hl;
  }
  i = k;                                    // livo follows the shift (labline: licm_loops()
  m = nlines - 1;                           // rebuilds it from the lines each round)
  while (m > i) { livo[m] = livo[m - 1]; m = m - 1; }
  if (i >= 0) livo[i] = 0;                  // (an insert before line 0 shifts it all)
  licm_savelen = sl;                        // (the head's licm_to() may have rewritten it: the
                                           // body's copy of this line goes in now)
  pool_at = lppos;                          // (the body's text starts here: lend() moves the
                                           // tail past it. The walk in licm_to() is by
                                           // position: its old countdown ran past the end when
                                           // a mark stood at the very tail of the line)
  licm_to(j, "rB", nr); lend();       // the body keeps the mark (a name no
  lkind[j] = L_INS; ltext[j] = pool_at;     // real register reaches) and points at its own text
  livo[j] = 1;                              // (the shift above copied livo with the lines: this
                                           // body now carries moved text, so it must say so. It
                                           // stands below k, where that scan cannot see it)
  return 1;
}
// the instructions that take r4-r6, r10, r11 in any register operand (the wide encodings)
int hiok() {
  return mnis("ldr") || mnis("ldrb") || mnis("ldrh") || mnis("ldrsb.w") || mnis("ldrsh.w") || mnis("str") || mnis("strb")
    || mnis("strh") || mnis("cmp") || mnis("mov") || mnis("movw") || mnis("addw") || mnis("subw") || mnis("add.w")
    || mnis("adds") || mnis("subs");
}
// line i with register rf (r0-r3) spelled to, the first operand left alone if skip
int renreg(int i, int rf, char *to, int skip) {
  char *t; int prev;
  t = lpool + ltext[i]; pset(i); prev = 0;
  while (*t && *t != 9) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  if (*t) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  if (skip) { lpool[lppos] = t[0]; lpool[lppos + 1] = t[1]; lppos = lppos + 2; prev = t[1]; t = t + 2; }
  while (*t) {
    if (*t == '#') { while (*t && *t != ',' && *t != ']') { lpool[lppos] = *t; lppos = lppos + 1; prev = *t; t = t + 1; } }
    else if (t[0] == 'r' && t[1] == '0' + rf && !idchar(t[2]) && !idchar(prev)) { ls(to); t = t + 2; prev = '0'; }
    else { lpool[lppos] = *t; lppos = lppos + 1; prev = *t; t = t + 1; }
  }
  lend();
  return 0;
}
// mov rA, rP ; X -> X reading rP in place of rA (rP a promoted register, rA r0-r3 not read after X)
int peep_hsrc(int a) {
  int j; int ra; int w; char *t;
  t = lpool + ltext[a];
  if (t[5] < '0' || t[5] > '3' || t[6] != ',' || t[7] != ' ') return 0;
  ra = t[5] - '0'; w = regword(t + 8, strlen(t + 8));
  if (!ispro(w)) return 0;
  j = nx(a); if (j >= nlines || lkind[j] != L_INS) return 0;
  opinfo(j);
  if (opkind != 0 || !hiok() || !(opmask & (1 << ra))) return 0;
  if (oprd == ra) { if (!oppw) return 0; renreg(j, ra, t + 8, 1); }
  else { if (!dead(ra, nx(j), 0)) return 0; renreg(j, ra, t + 8, 0); }
  pdel(a);
  return 1;
}
// X rA, ... ; mov rP, rA -> X rP, ... (rP a promoted register, rA r0-r3 not read afterwards)
int peep_tohi(int a) {
  int b; int rp; int ra; char *u; char *c;
  b = nx(a); if (b >= nlines || lkind[b] != L_INS) return 0;
  u = lpool + ltext[b];
  if (strncmp(u, "mov\tr", 5)) return 0;
  c = strchr(u, ','); if (!c || c[1] != ' ') return 0;
  rp = regword(u + 4, c - u - 4); ra = regword(c + 2, strlen(c + 2));
  if (!ispro(rp) || ra < 0 || ra > 3) return 0;
  opinfo(a);
  if (opkind != 0 || oprd != ra || !oppw || !hiok()) return 0;
  if (!dead(ra, nx(b), 0)) return 0;
  if (rp >= 10) rp = rp - 7; else rp = rp - 4;   // r4 r5 r6 r10 r11 -> 0..4
  redest(a, regnm(rp)); pdel(b);
  return 1;
}
// subw/addw r0, r7, #N ; push {r0} ; X ; pop {r1} ; ST r0, [r1] -> X ; ST r0, [r7, #off]
// (an assignment to a local whose right side uses r1 and r2). X must leave the pushed word alone:
// stack references only to words it pushed itself, a syscall's argv only over its own args.
int peep_astore(int a) {
  int off; int b; int j; int dep; int n; int k; int sz; char *mn; char *t;
  off = froff(a); b = nx(a);
  if (off == 1 || !isins(b, "push\t{r0}")) return 0;
  if (!dead(0, nx(b), 0)) return 0;          // X does not read the address
  j = nx(b); dep = 0; n = 0;
  while (j < nlines && !(dep == 0 && isins(j, "pop\t{r1}"))) {
    if (lkind[j] != L_INS) return 0;
    n = n + 1; if (n > 200) return 0;
    t = lpool + ltext[j];
    if (!strcmp(t, "push\t{r0}")) dep = dep + 1;
    else if (!strcmp(t, "pop\t{r1}")) dep = dep - 1;
    else if (!strncmp(t, "add\tsp, #", 9)) { dep = dep - pnum(j, "add\tsp, #") / 4; if (dep < 0) return 0; }
    else if (!strncmp(t, "ldr\tr1, [sp, #", 14)) { if (atoi(t + 14) >= 4 * dep) return 0; }   // a call's function address
    else if (!strcmp(t, "mov\tr2, sp")) {   // a syscall: its args are the top movs r1, #n words
      k = 0; while (k < 8 && !preins(j, "movs\tr1, #")) { j = nx(j); k = k + 1; }
      if (k >= 8 || pnum(j, "movs\tr1, #") > dep) return 0;
      while (j < nlines && !isins(j, "pop\t{r4}")) { if (lkind[j] != L_INS) return 0; j = nx(j); }
      if (j >= nlines) return 0;
    }
    else {
      opinfo(j);
      if (opkind == 4 || opkind == 2 || oprd == 13 || (opmask & 8192) || mnis("push") || mnis("pop")) return 0;
    }
    j = nx(j);
  }
  if (j >= nlines) return 0;
  k = nx(j); mn = 0;
  if (isins(k, "str\tr0, [r1]")) { mn = "str"; sz = 4; }
  if (isins(k, "strb\tr0, [r1]")) { mn = "strb"; sz = 1; }
  if (isins(k, "strh\tr0, [r1]")) { mn = "strh"; sz = 2; }
  if (!mn || !offok(off, sz) || !dead(1, nx(k), 0)) return 0;
  pset(k); lsoff(mn, "r0", off);
  pdel(a); pdel(b); pdel(j);
  return 1;
}
// add.w rA, r9, rA ; (up to 3 lines without rA) ; LD/ST rX, [rA] -> ... ; LD/ST rX, [r9, rA]
int peep_gidx(int a) {
  int j; int n; int ra; char *t; char *u; char *v;
  if (isins(a, "add.w\tr1, r9, r1")) ra = 1; else if (isins(a, "add.w\tr2, r9, r2")) ra = 2; else return 0;
  j = nx(a); n = 0;
  while (j < nlines && lkind[j] == L_INS && n < 4) {
    t = lpool + ltext[j]; u = t + strlen(t);
    if (u - t > 6 && *(u - 4) == '[' && *(u - 3) == 'r' && *(u - 2) == '0' + ra && *(u - 1) == ']'
      && (preins(j, "ldr\t") || preins(j, "ldrb\t") || preins(j, "ldrh\t") || preins(j, "ldrsb.w\t") || preins(j, "ldrsh.w\t")
        || preins(j, "str\t") || preins(j, "strb\t") || preins(j, "strh\t"))) {
      opinfo(j);
      if (oprd == ra) { if (!(oppw)) return 0; }    // ldr r1, [r1]: fine, the load replaces the address
      else if (!dead(ra, nx(j), 0)) return 0;
      v = strchr(t, 9);                     // str r1, [r1] stores the address itself
      if (preins(j, "st") && v[1] == 'r' && v[2] == '0' + ra && v[3] == ',') return 0;
      pset(j);
      while (t < u - 3) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
      ls("r9, r"); ls(dec(ra)); ls("]"); lend();
      pdel(a);
      return 1;
    }
    opinfo(j);
    if (opkind != 0 || oprd == ra || (opmask & (1 << ra))) return 0;
    j = nx(j); n = n + 1;
  }
  return 0;
}
// push {r0} ; ldr r1, [sp, #0] ; blx r1 ; add sp, #4 -> blx r0 (a call through a pointer, no args)
int peep_icall(int a) {
  int b; int c; int d;
  b = nx(a); c = nx(b); d = nx(c);
  if (!isins(b, "ldr\tr1, [sp, #0]") || !isins(c, "blx\tr1") || !isins(d, "add\tsp, #4")) return 0;
  pset(c); ls("blx\tr0"); lend();
  pdel(a); pdel(b); pdel(d);
  return 1;
}
int peep() {
  int i; int round; int k;
  if (!labline) { labline = malloc(65536 * 4); opmn = malloc(64); labref = malloc(65536 * 4); dvis = malloc(16400 * 4);
    i = 0; while (i < 65536) { labline[i] = -1; i = i + 1; }   // (a label no line carries points nowhere)
    i = 0; while (i < 16400) { dvis[i] = 0; i = i + 1; }   // stamps start at 1: no line is visited yet
  }
  i = 0; while (i < nlines) { if (lkind[i] == L_LAB && llab[i] >= 0 && llab[i] < 65536) labline[llab[i]] = i; i = i + 1; }
  pchg = 1; round = 0;
  while (pchg && round < 16) {
    pchg = 0;
    i = 0; while (i < nlines) { if (lkind[i] == L_LAB && llab[i] >= 0 && llab[i] < 65536) labref[llab[i]] = 0; i = i + 1; }
    i = 0; while (i < nlines) { if (lkind[i] == L_BR && llab[i] >= 0 && llab[i] < 65536) labref[llab[i]] = labref[llab[i]] + 1; i = i + 1; }
    i = 0; while (i < nlines) { if (lkind[i] == L_LAB && llab[i] >= 0 && llab[i] < 65536 && !labref[llab[i]]) pdel(i); i = i + 1; }
    i = 0;
    while (i < nlines) {
      k = lkind[i];
      if (k == L_BR) peep_br(i);
      else if (k == L_INS) {
        if (npro && preins(i, "mov\tr") && peep_hsrc(i)) ;
        else if (npro && peep_tohi(i)) ;
        else if (lpool[ltext[i]] == 'v') peep_fp(i);
        else if (preins(i, "mov\tr") && peep_dce(i)) ;
        else if (isins(i, "push\t{r0}")) { if (!peep_icall(i) && !peep_fpush(i)) peep_push(i); }
        else if (preins(i, "mov\tr1, r")) peep_mov(i);
        else if (preins(i, "ite\t")) peep_ite(i);
        else if (froff(i) != 1) { if (!peep_astore(i) && !peep_store(i)) { if (!peep_ld(i)) peep_tor1(i); } }
        else if (preins(i, "add.w\tr0, r9, r")) { if (!peep_ld(i)) peep_tor1(i); }
        else if (preins(i, "add.w\tr1, r9, r") || preins(i, "add.w\tr2, r9, r")) peep_gidx(i);
        else if (preins(i, "subw\tr1, r7, #") || preins(i, "addw\tr1, r7, #")) peep_r1fr(i);
        else if (isins(i, "adds\tr0, r1, r0") || isins(i, "adds\tr0, r2, r0")) { if (!peep_idx(i)) peep_tor1(i); }
        else if (preins(i, "str\tr0, [r7, #")) peep_reload(i);
        else peep_tor1(i);
      }
      i = i + 1;
    }
    round = round + 1;
  }
  return 0;
}

// ------------------------------------------------------------------ lexer
int lookup(char *s, int len, int hash) {
  int i;
  i = hhead[hash & 4095] - 1;
  while (i >= 0) {
    if (syms[i].hash == hash && syms[i].len == len && !memcmp(syms[i].name, s, len)) return i;
    i = hnext[i] - 1;
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
    hnext[i] = hhead[hash & 4095]; hhead[hash & 4095] = i + 1;
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
  if (optim) {                              // for promote(): words of arrays and structs
    if (t >= PTR && !(t & ARR) && size > 4) markbad(-loc + 4, -loc + size);   // array: its pointer word, then the storage
    else if (size != 4 || (t & ARR) || (t < PTR && t != TINT && t != TUINT && t != TFLOAT)) markbad(-loc, -loc + size);
    else if (t == TFLOAT && slotix(-loc) >= 0) slotf[slotix(-loc)] = 1;
  }
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
      if (optim) {
        if (!slotk) { slotk = malloc(512 * 4); slotf = malloc(512 * 4); }
        i = 0; while (i < 512) { slotk[i] = 0; slotf[i] = 0; i = i + 1; }
      }
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
      if (optim) { i = 0; while (i < nsyms) { if (syms[i].class == CLoc && syms[i].type == TFLOAT && slotix(syms[i].val) >= 0) slotf[slotix(syms[i].val)] = 1; i = i + 1; } }
      s->class = CFun; s->val = 0; nparams = n; fnrty = t;   // val >= 0: defined
      oput("\t.global\t"); oname(s); oput("\n"); oname(s); oput(":\n");
      ins("push\t{r7, lr}");
      if (optim) pvpush = lnew(L_DEL);         // promote()'s vpush
      ins("mov\tr7, sp");
      frame_at = nlines; imm2(1, 0);            // movw+movt r1,#frame (filled in below)
      ins("sub.w\tsp, sp, r1");
      phold = -1;
      if (optim) { phold = nlines; i = 0; while (i < 21) { lnew(L_DEL); i = i + 1; } }  // promote()'s parameter loads
      ret_fix = -1;
      stmt();
      chain_here(ret_fix);
      ins("mov\tsp, r7");
      if (optim) pvpop = lnew(L_DEL);          // promote()'s vpop
      ins("pop\t{r7, pc}");
      sz = (maxloc + 7) & -8;
      i = nlines; imm2(1, sz); ltext[frame_at] = ltext[i]; ltext[frame_at + 1] = ltext[i + 1]; nlines = i;
      if (optim) { prepro = 1; peep(); licm_loops(); prepro = 0; peep(); promote(); sprel(frame_at, sz); frameless(frame_at, sz); trimframe(frame_at, sz); }
      flush();
      npro = 0; phold = -1;
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
  hhead = malloc(4096 * 4); hnext = malloc(8192 * 4);
  i = 0; while (i < 4096) { hhead[i] = 0; i = i + 1; }
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
  p = "exit 0 exec 1 millis 2 sleep 3 sysinfo 4 spawn 5 yield 6 jobs 7 kill 8 reboot 9 putchar 16 getchar 17 kbhit 18 conctl 19 redirect 20 open 32 close 33 read 34 write 35 seek 36 stat 37 unlink 38 rename 39 mkdir 40 rmdir 41 opendir 42 readdir 43 closedir 44 chdir 45 getcwd 46 mkpipe 47 mount 48 mountat 49 malloc 64 free 65 memset 66 memcmp 67 memcpy 68 strlen 69 strcmp 70 strcpy 71 printf 72 sprintf 73 atoi 74 strncmp 75 strchr 76 net_join 80 net_status 81 net_ip 82 connect 83 send 84 recv 85 listen 86 accept 87 sock_avail 88 wifi_save 89 wifi_saved 90 ping 91 telnetd 92 gpio_mode 96 gpio_write 97 gpio_read 98 analog_read 99 pwm_write 100 i2c_write 101 i2c_read 102 spi_transfer 103 delay_us 104 sin 112 cos 113 tan 114 asin 115 acos 116 atan 117 atan2 118 sinh 119 cosh 120 tanh 121 exp 122 log 123 log10 124 pow 125 sqrt 126 fabs 127 floor 128 ceil 129 fmod 130";
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
    if (write(ofd, ibuf, ipos) != ipos) { printf("cannot write %s\n", outname); quit(); }
    oput("\tpop\t{r4, pc}\n\t.t2init\t.Linit\n");
  }
  write_data();
  oflush();
  if (close(ofd) < 0) { ofd = -1; printf("cannot write %s\n", outname); unlink(outname); cleanup(); exit(1); }
  ofd = -1;

  i = 0; while (i < nsyms) { if (syms[i].ptypes) free(syms[i].ptypes); i = i + 1; }
  i = 0; while (i < nfn) { if (fnparams[i]) free(fnparams[i]); i = i + 1; }
  i = 0; while (i < nsrcbuf) { free(srcbuf[i]); i = i + 1; }
  free(srcbuf); free(syms); free(hhead); free(hnext); free(mems); free(stsize); free(stfirst); free(starr); free(stcnt); free(dims); free(data);
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
    else if (!strcmp(a, "-O")) { optim = 1; i = i + 1; }
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
  if (!nin) { printf("usage: cc [-o out] [-S|-c] [-O] [-mshort-data] [-I dir] [-D name[=v]] [-L dir] [-l name] file.c|file.s|file.o|file.a...\n"); return 1; }
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
