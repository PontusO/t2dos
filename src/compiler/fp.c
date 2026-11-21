// fp.c - the -O float passes: which floats live in s registers, and the vldr/vstr
// shrink that follows from it. See cc.h for what the files share.
#include "cc.h"

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
