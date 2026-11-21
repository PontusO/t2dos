// opt.c - the -O peephole: live/dead line analysis and the rewrite rules, run over
// the finished line list of a function. See cc.h for what the files share.
#include "cc.h"

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
