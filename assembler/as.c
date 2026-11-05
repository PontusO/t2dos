// as - GNU-syntax (unified) Thumb-2 assembler for the Cortex-M33, writes T2DOS objects
//   as [-o out.o] file.s       (default output: file.o)
// Errors print "file:line: error: message" and exit 1 without writing the output.
//
// One pass parses the source. Instructions whose encoding is fixed are encoded at once
// into the section buffer; branches, alignment and data that name symbols become items
// (struct Item) that sit between the buffer bytes. The second pass lays the items out,
// grows narrow branches that do not reach (like GNU as, never shrinking), then writes
// the bytes. Encodings and the narrow/wide choice follow arm-none-eabi-as
// (-mcpu=cortex-m33 -mthumb): tools/check_asm.py compares the two byte for byte.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "t2obj.c"

// ------------------------------------------------------------------ state
struct Sym { char *name; int len; int sect; int pos; int item; int val; int flags; int next; int line; int out; };
enum { F_DEF = 1, F_GLOBAL = 2, F_EQU = 4, F_EXTERN = 8, F_ANON = 16 };
struct Item { int kind; int pos; int size; int line; int sym; int sym2; int val; int addr; int flags; };
enum { K_BR = 1, K_ALIGN = 2, K_DATA = 3 };
enum { B_WIDE = 16, B_NARROW = 32, B_BL = 64, B_INIT = 128 };   // branch item flags | cond
enum { HBITS = 4096 };

char *srcname; char *src; int line; char *p;
struct Sym *syms; int nsyms; int csyms; int *hbuck;
char *sb[3]; int sl[3]; int sc[3]; int maxal[3];           // section 1 text, 2 data
struct Item *items[3]; int nitems[3]; int citems[3]; int sextra[3];
int cur;
char ebuf[300]; char nbuf[160];

// expression result: value = sym(es1) - sym(es2) + ec (es1/es2 = -1 when absent)
int es1; int es2; int ec;

// the instruction being assembled
int setf; int cond; int wid; int init;
int itleft; int itpos; int itcond[4];
enum { O_REG = 1, O_IMM, O_MEM, O_LIST, O_EXPR };
int okind[6]; int oreg[6]; int oidx[6]; int ohas[6]; int osym[6]; int osym2[6]; int oval[6]; char *otext[6]; int nops;

int fail(char *msg) { printf("%s:%d: error: %s\n", srcname, line, msg); exit(1); return 0; }
int failname(char *fmt, char *name, int len) {
  if (len > 150) len = 150;
  memcpy(nbuf, name, len); nbuf[len] = 0;
  sprintf(ebuf, fmt, nbuf);
  return fail(ebuf);
}

// ------------------------------------------------------------------ buffers
char *grow(char *buf, int used, int *cap, int need) {
  char *nb; int c;
  if (used + need <= *cap) return buf;
  c = *cap * 2; if (c < used + need) c = used + need + 4096;
  nb = malloc(c);
  if (!nb) fail("out of memory");
  memset(nb, 0, c);
  if (used) memcpy(nb, buf, used);
  if (buf) free(buf);
  *cap = c;
  return nb;
}
int emit8(int b) { sb[cur] = grow(sb[cur], sl[cur], &sc[cur], 1); sb[cur][sl[cur]] = b & 255; sl[cur] = sl[cur] + 1; return 0; }
int emit16(int h) { emit8(h); emit8(h >> 8); return 0; }
int emit32(int h1, int h2) { emit16(h1); emit16(h2); return 0; }

struct Item *newitem(int kind, int size) {
  struct Item *it; struct Item *ni; int c;
  if (nitems[cur] == citems[cur]) {
    c = citems[cur] * 2 + 64;
    ni = malloc(c * sizeof(struct Item));
    if (!ni) fail("out of memory");
    memset((char *)ni, 0, c * sizeof(struct Item));
    if (nitems[cur]) memcpy((char *)ni, (char *)items[cur], nitems[cur] * sizeof(struct Item));
    if (items[cur]) free((char *)items[cur]);
    items[cur] = ni; citems[cur] = c;
  }
  it = items[cur] + nitems[cur];
  nitems[cur] = nitems[cur] + 1;
  it->kind = kind; it->pos = sl[cur]; it->size = size; it->line = line;
  it->sym = -1; it->sym2 = -1; it->val = 0; it->addr = 0; it->flags = 0;
  return it;
}

// ------------------------------------------------------------------ symbols
int hashname(char *s, int n) { int h; int i; h = 0; for (i = 0; i < n; i++) h = h * 31 + s[i]; return h & (HBITS - 1); }

int newsym(char *s, int n) {
  struct Sym *ns; struct Sym *y; int c;
  if (nsyms == csyms) {
    c = csyms * 2 + 256;
    ns = malloc(c * sizeof(struct Sym));
    if (!ns) fail("out of memory");
    memset((char *)ns, 0, c * sizeof(struct Sym));
    if (nsyms) memcpy((char *)ns, (char *)syms, nsyms * sizeof(struct Sym));
    if (syms) free((char *)syms);
    syms = ns; csyms = c;
  }
  y = syms + nsyms;
  y->name = s; y->len = n; y->sect = SEC_UNDEF; y->pos = 0; y->item = 0; y->val = 0;
  y->flags = 0; y->next = -1; y->line = line; y->out = -1;
  nsyms = nsyms + 1;
  return nsyms - 1;
}

int lookup(char *s, int n) {
  int h; int i; struct Sym *y;
  h = hashname(s, n);
  i = hbuck[h] - 1;
  while (i >= 0) {
    y = syms + i;
    if (y->len == n && memcmp(y->name, s, n) == 0) return i;
    i = y->next;
  }
  i = newsym(s, n);
  syms[i].next = hbuck[h] - 1;
  hbuck[h] = i + 1;
  return i;
}

// a symbol for the current location (".")
int here() {
  int i; struct Sym *y;
  i = newsym("", 0);
  y = syms + i;
  y->flags = F_DEF | F_ANON; y->sect = cur; y->pos = sl[cur]; y->item = nitems[cur];
  return i;
}

int deflabel(char *s, int n) {
  int i; struct Sym *y;
  i = lookup(s, n); y = syms + i;
  if (y->flags & F_DEF) failname("symbol '%s' already defined", s, n);
  y->flags = y->flags | F_DEF; y->sect = cur; y->pos = sl[cur]; y->item = nitems[cur]; y->val = 0; y->line = line;
  return i;
}

// address of a defined symbol once the items are laid out
int symaddr(int i) {
  struct Sym *y; int k; int base;
  y = syms + i;
  if (y->sect == SEC_ABS) return y->val;
  k = y->item;
  if (k < nitems[y->sect]) base = items[y->sect][k].addr - items[y->sect][k].pos;
  else base = sextra[y->sect];
  return y->pos + base + y->val;
}

// ------------------------------------------------------------------ lexing
int isidstart(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_' || c == '.' || c == '$'; }
int isidch(int c) { return isidstart(c) || (c >= '0' && c <= '9'); }
int skipws() { while (*p == ' ' || *p == '\t' || *p == '\r') p++; return 0; }
int scanident() { char *q; q = p; while (isidch(*p)) p++; return p - q; }
int lower(int c) { if (c >= 'A' && c <= 'Z') return c + 32; return c; }

// blank out @ // /* */ comments and # lines, keeping newlines, strings and char literals
int strip(char *s) {
  int bol;
  bol = 1;
  while (*s) {
    if (*s == '"') {
      s++;
      while (*s && *s != '"' && *s != '\n') { if (*s == '\\' && s[1]) s++; s++; }
      if (*s == '"') s++;
      bol = 0;
    } else if (*s == '\'') {
      s++;
      if (*s == '\\' && s[1]) s = s + 2; else if (*s && *s != '\n') s++;
      if (*s == '\'') s++;
      bol = 0;
    } else if (*s == '@' || (*s == '/' && s[1] == '/') || (*s == '#' && bol)) {
      while (*s && *s != '\n') { *s = ' '; s++; }
    } else if (*s == '/' && s[1] == '*') {
      *s = ' '; s[1] = ' '; s = s + 2;
      while (*s && !(*s == '*' && s[1] == '/')) { if (*s != '\n') *s = ' '; s++; }
      if (*s) { *s = ' '; s[1] = ' '; s = s + 2; }
    } else {
      if (*s == '\n') bol = 1;
      else if (*s != ' ' && *s != '\t' && *s != '\r') bol = 0;
      s++;
    }
  }
  return 0;
}

// register name -> number, -1 if not a register
int regnum(char *s, int n) {
  int a; int b; int r;
  if (n < 2 || n > 3) return -1;
  a = lower(s[0]); b = lower(s[1]);
  if (n == 2) {
    if (a == 's' && b == 'p') return 13;
    if (a == 'l' && b == 'r') return 14;
    if (a == 'p' && b == 'c') return 15;
    if (a == 'i' && b == 'p') return 12;
    if (a == 'f' && b == 'p') return 11;
    if (a == 's' && b == 'l') return 10;
    if (a == 's' && b == 'b') return 9;
  }
  if (a != 'r' || b < '0' || b > '9') return -1;
  r = b - '0';
  if (n == 3) {
    if (r != 1 || s[2] < '0' || s[2] > '5') return -1;
    r = 10 + s[2] - '0';
  } else if (r > 9) return -1;
  return r;
}

// condition code at s (two letters), -1 if none
int condnum(char *s) {
  char *t; int i;
  t = "eqnecsccmiplvsvchilsgeltgtleal";
  for (i = 0; i < 15; i++) if (s[0] == t[2 * i] && s[1] == t[2 * i + 1]) return i;
  if (s[0] == 'h' && s[1] == 's') return 2;
  if (s[0] == 'l' && s[1] == 'o') return 3;
  return -1;
}

// ------------------------------------------------------------------ expressions
int addsym(int sign, int i) {
  if (sign > 0) { if (es1 >= 0) fail("expression too complex"); es1 = i; }
  else { if (es2 >= 0) fail("expression too complex"); es2 = i; }
  return 0;
}

int escchar() {   // p is after a backslash; returns the character, advances p
  int c; int n;
  c = *p; p++;
  if (c == 'n') return 10;
  if (c == 't') return 9;
  if (c == 'r') return 13;
  if (c == 'b') return 8;
  if (c == 'f') return 12;
  if (c == 'v') return 11;                            // GNU as knows no \a or \e: they are a and e
  if (c == 'x') {
    n = 0;
    while ((*p >= '0' && *p <= '9') || (lower(*p) >= 'a' && lower(*p) <= 'f')) {
      if (*p <= '9') n = n * 16 + *p - '0'; else n = n * 16 + lower(*p) - 'a' + 10;
      p++;
    }
    return n & 255;
  }
  if (c >= '0' && c <= '7') {
    n = c - '0';
    if (*p >= '0' && *p <= '7') { n = n * 8 + *p - '0'; p++; }
    if (*p >= '0' && *p <= '7') { n = n * 8 + *p - '0'; p++; }
    return n & 255;
  }
  return c;
}

int number() {
  unsigned v; int base; int d; int c;
  v = 0; base = 10;
  if (*p == '0' && (p[1] == 'x' || p[1] == 'X')) { base = 16; p = p + 2; }
  else if (*p == '0' && (p[1] == 'b' || p[1] == 'B')) { base = 2; p = p + 2; }
  else if (*p == '0') base = 8;
  while (1) {
    c = lower(*p);
    if (c >= '0' && c <= '9') d = c - '0';
    else if (c >= 'a' && c <= 'f') d = c - 'a' + 10;
    else break;
    if (d >= base) fail("bad number");
    v = v * base + d;
    p++;
  }
  if (isidch(*p)) fail("bad number");
  return v;
}

int esum(int sign);

int eterm(int sign) {
  char *q; int n; int i;
  skipws();
  if (*p == '-') { p++; return eterm(-sign); }
  if (*p == '+') { p++; return eterm(sign); }
  if (*p == '(') {
    p++; esum(sign); skipws();
    if (*p != ')') fail("missing )");
    p++; return 0;
  }
  if (*p >= '0' && *p <= '9') { ec = ec + sign * number(); return 0; }
  if (*p == '\'') {
    p++;
    if (*p == '\\') { p++; n = escchar(); } else { n = *p & 255; p++; }
    if (*p == '\'') p++;
    ec = ec + sign * n;
    return 0;
  }
  if (*p == '.' && !isidch(p[1])) { p++; addsym(sign, here()); return 0; }
  if (isidstart(*p)) {
    q = p; n = scanident();
    i = lookup(q, n);
    if ((syms[i].flags & F_DEF) && syms[i].sect == SEC_ABS) ec = ec + sign * syms[i].val;
    else addsym(sign, i);
    return 0;
  }
  return fail("expression expected");
}

int esum(int sign) {
  eterm(sign);
  while (1) {
    skipws();
    if (*p == '+') { p++; eterm(sign); }
    else if (*p == '-') { p++; eterm(-sign); }
    else return 0;
  }
  return 0;
}

int expr() { es1 = -1; es2 = -1; ec = 0; esum(1); return 0; }

int undefined(int i) {
  if (syms[i].flags & (F_GLOBAL | F_EXTERN))
    return failname("external symbol '%s' needs a relocation (not supported yet)", syms[i].name, syms[i].len);
  return failname("undefined symbol '%s'", syms[i].name, syms[i].len);
}

// a constant now: folds a - b of two labels at fixed places in the same section
int constexpr() {
  expr();
  if (es1 < 0 && es2 < 0) return ec;
  if (es1 >= 0 && !(syms[es1].flags & F_DEF)) undefined(es1);
  if (es2 >= 0 && !(syms[es2].flags & F_DEF)) undefined(es2);
  return fail("constant expression expected");
}

// ------------------------------------------------------------------ operands
int parseop(int i) {
  char *q; int n; int r; int r2; int k;
  skipws();
  otext[i] = p; oidx[i] = -1; ohas[i] = 0; oval[i] = 0; osym[i] = -1; osym2[i] = -1; oreg[i] = -1;
  if (*p == '#') {
    p++; expr();
    okind[i] = O_IMM; osym[i] = es1; osym2[i] = es2; oval[i] = ec;
    return 0;
  }
  if (*p == '[') {
    p++; skipws();
    q = p; n = scanident(); r = regnum(q, n);
    if (r < 0) fail("register expected");
    okind[i] = O_MEM; oreg[i] = r;
    skipws();
    if (*p == ',') {
      p++; skipws();
      if (*p == '#') {
        p++; expr(); ohas[i] = 1; osym[i] = es1; osym2[i] = es2; oval[i] = ec;
      } else {
        q = p; n = scanident(); r = regnum(q, n);
        if (r < 0) fail("register expected");
        oidx[i] = r;
      }
      skipws();
    }
    if (*p != ']') fail("missing ]");
    p++; skipws();
    if (*p == '!') fail("writeback is not supported");
    return 0;
  }
  if (*p == '{') {
    p++; k = 0;
    while (1) {
      skipws(); q = p; n = scanident(); r = regnum(q, n);
      if (r < 0) fail("register expected");
      skipws();
      r2 = r;
      if (*p == '-') {
        p++; skipws(); q = p; n = scanident(); r2 = regnum(q, n);
        if (r2 < r) fail("bad register range");
        skipws();
      }
      while (r <= r2) { k = k | (1 << r); r++; }
      if (*p == ',') { p++; continue; }
      if (*p == '}') { p++; break; }
      fail("missing }");
    }
    okind[i] = O_LIST; oreg[i] = k;
    return 0;
  }
  if (isidstart(*p)) {
    q = p; n = scanident(); r = regnum(q, n);
    skipws();
    if (r >= 0 && (*p == ',' || *p == 0)) { okind[i] = O_REG; oreg[i] = r; return 0; }
    p = q;
  }
  expr();
  okind[i] = O_EXPR; osym[i] = es1; osym2[i] = es2; oval[i] = ec;
  return 0;
}

int parseops() {
  nops = 0;
  skipws();
  if (*p == 0) return 0;
  while (1) {
    if (nops == 6) fail("too many operands");
    parseop(nops);
    nops++;
    skipws();
    if (*p != ',') return 0;
    p++;
  }
  return 0;
}

int R(int i) { if (i >= nops || okind[i] != O_REG) fail("register expected"); return oreg[i]; }
int lo(int r) { return r < 8; }
int nargs(int a, int b) { if (nops < a || nops > b) fail("wrong number of operands"); return 0; }

// the value of an operand that must be a constant (#imm, or the offset in [rn, #imm])
int opconst(int i) {
  if (osym[i] >= 0 && !(syms[osym[i]].flags & F_DEF)) undefined(osym[i]);
  if (osym2[i] >= 0 && !(syms[osym2[i]].flags & F_DEF)) undefined(osym2[i]);
  if (osym[i] >= 0 || osym2[i] >= 0) fail("constant expression expected");
  return oval[i];
}
int IMM(int i) { if (i >= nops || okind[i] != O_IMM) fail("immediate expected"); return opconst(i); }
int isimm(int i) { return i < nops && okind[i] == O_IMM; }

// ------------------------------------------------------------------ encodings
// Thumb-2 modified immediate (i:imm3:imm8 as 12 bits) or -1
int modimm(int x) {
  unsigned v; unsigned t; int r;
  v = x;
  if (v < 256) return v;
  if ((v & 0xff00ff00) == 0 && ((v >> 16) & 255) == (v & 255)) return 0x100 | (v & 255);
  if ((v & 0x00ff00ff) == 0 && ((v >> 24) & 255) == ((v >> 8) & 255)) return 0x200 | ((v >> 8) & 255);
  if (((v >> 24) & 255) == (v & 255) && ((v >> 16) & 255) == (v & 255) && ((v >> 8) & 255) == (v & 255)) return 0x300 | (v & 255);
  for (r = 8; r < 32; r++) {
    t = (v << r) | (v >> (32 - r));
    if (t >= 128 && t < 256) return (r << 7) | (t & 127);
  }
  return -1;
}

// data processing: op AND 0 BIC 1 ORR 2 ORN 3 EOR 4 ADD 8 SUB 13 RSB 14
int dpimm(int op, int s, int rn, int rd, int m) {
  return emit32(0xF000 | ((m >> 11) & 1) << 10 | op << 5 | s << 4 | rn, ((m >> 8) & 7) << 12 | rd << 8 | (m & 255));
}
int dpreg(int op, int s, int rn, int rd, int rm, int type, int amt) {
  return emit32(0xEA00 | op << 5 | s << 4 | rn, ((amt >> 2) & 7) << 12 | rd << 8 | (amt & 3) << 6 | type << 4 | rm);
}
int plain12(int sub, int rn, int rd, int v) {   // addw / subw
  return emit32((sub ? 0xF2A0 : 0xF200) | ((v >> 11) & 1) << 10 | rn, ((v >> 8) & 7) << 12 | rd << 8 | (v & 255));
}
int mov16(int top, int rd, int v) {             // movw / movt
  if (v < 0 || v > 65535) fail("immediate out of range");
  return emit32((top ? 0xF2C0 : 0xF240) | ((v >> 11) & 1) << 10 | ((v >> 12) & 15), ((v >> 8) & 7) << 12 | rd << 8 | (v & 255));
}

// a 16-bit data-processing encoding sets the flags outside an IT block and not inside one
int narrowdp() { return wid != 4 && ((setf && !init) || (!setf && init)); }
int notnarrow() { if (wid == 2) fail("no narrow encoding for these operands"); return 0; }

// mnemonics, in the order of the enum
enum { M_MOV, M_MVN, M_MOVW, M_MOVT, M_ADD, M_ADDW, M_SUB, M_SUBW, M_CMP, M_CMN, M_TST,
  M_AND, M_ORR, M_EOR, M_BIC, M_LSL, M_LSR, M_ASR, M_ROR, M_MUL, M_MLS, M_SDIV, M_UDIV, M_NEG,
  M_SXTB, M_UXTB, M_SXTH, M_UXTH, M_LDR, M_STR, M_LDRB, M_STRB, M_LDRH, M_STRH, M_LDRSB, M_LDRSH,
  M_PUSH, M_POP, M_B, M_BL, M_BX, M_BLX, M_NOP, M_COUNT };

int cans(int id) {
  return id == M_MOV || id == M_MVN || id == M_ADD || id == M_SUB || (id >= M_AND && id <= M_MUL) || id == M_NEG;
}

// mnemonic (lower case, no .w/.n) -> id; sets setf and cond. -1 if unknown
int findmn(char *mn) {
  char *t; char *nm; int id; int n; char *r;
  t = "mov mvn movw movt add addw sub subw cmp cmn tst and orr eor bic lsl lsr asr ror mul mls sdiv udiv neg "
      "sxtb uxtb sxth uxth ldr str ldrb strb ldrh strh ldrsb ldrsh push pop b bl bx blx nop ";
  id = 0;
  while (*t) {
    nm = t; n = 0;
    while (t[n] != ' ') n++;
    t = t + n + 1;
    if (strncmp(mn, nm, n) == 0) {
      r = mn + n;
      setf = 0; cond = -1;
      if (*r == 's' && cans(id)) { setf = 1; r++; }
      if (*r == 0) return id;
      if (strlen(r) == 2 && condnum(r) >= 0) { cond = condnum(r); return id; }
    }
    id++;
  }
  return -1;
}

int do_mov(int id) {
  int rd; int rm; int v; int m;
  nargs(2, 2); rd = R(0);
  if (!isimm(1)) {
    rm = R(1);
    if (id == M_MVN) {
      if (lo(rd) && lo(rm) && narrowdp()) return emit16(0x43C0 | rm << 3 | rd);
      notnarrow(); return dpreg(3, setf, 15, rd, rm, 0, 0);
    }
    if (setf) {
      if (wid != 4 && !init && lo(rd) && lo(rm)) return emit16(rm << 3 | rd);   // lsls rd, rm, #0
      notnarrow(); return dpreg(2, 1, 15, rd, rm, 0, 0);
    }
    if (wid != 4) return emit16(0x4600 | (rd & 8) << 4 | rm << 3 | (rd & 7));
    return dpreg(2, 0, 15, rd, rm, 0, 0);
  }
  v = IMM(1);
  if (id == M_MOV && lo(rd) && narrowdp()) {
    if (v >= 0 && v <= 255) return emit16(0x2000 | rd << 8 | v);
    if (setf && wid == 0) fail("immediate out of range");    // movs rd, #imm8; movs.w takes more
  }
  notnarrow();
  m = modimm(v);                                       // the instruction itself, else mov <-> mvn
  if (m >= 0) return dpimm(id == M_MOV ? 2 : 3, setf, 15, rd, m);
  m = modimm(~v);
  if (m >= 0) return dpimm(id == M_MOV ? 3 : 2, setf, 15, rd, m);
  if (id == M_MOV && !setf && wid == 0 && v >= 0 && v <= 65535) return mov16(0, rd, v);
  return fail("immediate out of range");
}

int do_addsub(int id) {
  int rd; int rn; int rm; int v; int m; int sub; int o;
  nargs(2, 3);
  sub = id == M_SUB;
  rd = R(0);
  if (nops == 3) rn = R(1); else rn = rd;
  if (isimm(nops - 1)) {
    v = IMM(nops - 1);
    if (rn == 13 && !setf && wid != 4 && (v & 3) == 0) {
      if (!sub && lo(rd) && v >= 0 && v <= 1020) return emit16(0xA800 | rd << 8 | v >> 2);
      if (rd == 13 && v >= 0 && v <= 508) return emit16((sub ? 0xB080 : 0xB000) | v >> 2);
    }
    if (lo(rd) && lo(rn) && narrowdp()) {
      if (rd == rn && v >= 0 && v <= 255) return emit16((sub ? 0x3800 : 0x3000) | rd << 8 | v);
      if (v >= 0 && v <= 7) return emit16((sub ? 0x1E00 : 0x1C00) | v << 6 | rn << 3 | rd);
    }
    notnarrow();
    m = modimm(v);
    if (m >= 0) return dpimm(sub ? 13 : 8, setf, rn, rd, m);
    m = modimm(-v);
    if (m >= 0) return dpimm(sub ? 8 : 13, setf, rn, rd, m);
    if (!setf && v >= 0 && v <= 4095) return plain12(sub, rn, rd, v);
    if (!setf && v < 0 && v >= -4095) return plain12(!sub, rn, rd, -v);
    return fail("immediate out of range");
  }
  rm = R(nops - 1);
  if (lo(rd) && lo(rn) && lo(rm) && narrowdp()) return emit16((sub ? 0x1A00 : 0x1800) | rm << 6 | rn << 3 | rd);
  if (!sub && !setf && wid != 4 && (rd == rn || rd == rm)) {
    if (rd == rn) o = rm; else o = rn;
    return emit16(0x4400 | (rd & 8) << 4 | o << 3 | (rd & 7));
  }
  notnarrow();
  return dpreg(sub ? 13 : 8, setf, rn, rd, rm, 0, 0);
}

int do_addw(int id) {
  int rd; int rn; int v;
  nargs(2, 3);
  if (wid == 2) fail("no narrow encoding for these operands");
  rd = R(0);
  if (nops == 3) rn = R(1); else rn = rd;
  v = IMM(nops - 1);
  if (v < 0 || v > 4095) fail("immediate out of range");
  return plain12(id == M_SUBW, rn, rd, v);
}

int do_cmp(int id) {
  int rn; int rm; int v; int m;
  nargs(2, 2); rn = R(0);
  if (isimm(1)) {
    v = IMM(1);
    if (id == M_CMP && lo(rn) && wid != 4 && v >= 0 && v <= 255) return emit16(0x2800 | rn << 8 | v);
    notnarrow();
    m = modimm(v);
    if (m >= 0) return dpimm(id == M_CMP ? 13 : (id == M_CMN ? 8 : 0), 1, rn, 15, m);
    m = modimm(-v);
    if (m >= 0 && id != M_TST) return dpimm(id == M_CMP ? 8 : 13, 1, rn, 15, m);
    return fail("immediate out of range");
  }
  rm = R(1);
  if (wid != 4 && lo(rn) && lo(rm)) return emit16((id == M_CMP ? 0x4280 : (id == M_CMN ? 0x42C0 : 0x4200)) | rm << 3 | rn);
  if (id == M_CMP && wid != 4) return emit16(0x4500 | (rn & 8) << 4 | rm << 3 | (rn & 7));
  notnarrow();
  return dpreg(id == M_CMP ? 13 : (id == M_CMN ? 8 : 0), 1, rn, 15, rm, 0, 0);
}

int do_logic(int id) {
  int rd; int rn; int rm; int v; int m; int op; int nop16; int o; int comm;
  nargs(2, 3);
  op = 0; nop16 = 0x4000; comm = 1;
  if (id == M_ORR) { op = 2; nop16 = 0x4300; }
  if (id == M_EOR) { op = 4; nop16 = 0x4040; }
  if (id == M_BIC) { op = 1; nop16 = 0x4380; comm = 0; }
  rd = R(0);
  if (nops == 3) rn = R(1); else rn = rd;
  if (isimm(nops - 1)) {
    v = IMM(nops - 1);
    notnarrow();
    m = modimm(v);
    if (m >= 0) return dpimm(op, setf, rn, rd, m);
    m = modimm(~v);
    if (m >= 0 && op != 4) return dpimm(op ^ 1, setf, rn, rd, m);   // and <-> bic, orr <-> orn
    return fail("immediate out of range");
  }
  rm = R(nops - 1);
  if (lo(rd) && lo(rn) && lo(rm) && narrowdp() && (rd == rn || (comm && rd == rm))) {
    if (rd == rn) o = rm; else o = rn;
    return emit16(nop16 | o << 3 | rd);
  }
  notnarrow();
  return dpreg(op, setf, rn, rd, rm, 0, 0);
}

int do_shift(int id) {
  int rd; int rn; int rm; int v; int type;
  nargs(2, 3);
  type = id - M_LSL;                   // lsl 0 lsr 1 asr 2 ror 3
  rd = R(0);
  if (isimm(nops - 1)) {
    if (nops == 3) rm = R(1); else rm = rd;
    v = IMM(nops - 1);
    if (type == 0 && (v < 0 || v > 31)) fail("shift out of range");
    if ((type == 1 || type == 2) && (v < 1 || v > 32)) fail("shift out of range");
    if (type == 3 && (v < 1 || v > 31)) fail("shift out of range");
    if (type != 3 && lo(rd) && lo(rm) && narrowdp()) return emit16(type << 11 | (v & 31) << 6 | rm << 3 | rd);
    notnarrow();
    return dpreg(2, setf, 15, rd, rm, type, v & 31);
  }
  if (nops == 3) { rn = R(1); rm = R(2); } else { rn = rd; rm = R(1); }
  if (lo(rd) && lo(rm) && rd == rn && narrowdp()) {
    v = 0x4080;
    if (type == 1) v = 0x40C0;
    if (type == 2) v = 0x4100;
    if (type == 3) v = 0x41C0;
    return emit16(v | rm << 3 | rd);
  }
  notnarrow();
  return emit32(0xFA00 | type << 5 | setf << 4 | rn, 0xF000 | rd << 8 | rm);
}

int do_mul(int id) {
  int rd; int rn; int rm; int ra;
  if (id == M_MLS) {
    nargs(4, 4); if (wid == 2) notnarrow();
    rd = R(0); rn = R(1); rm = R(2); ra = R(3);
    return emit32(0xFB00 | rn, ra << 12 | rd << 8 | 0x10 | rm);
  }
  if (id == M_SDIV || id == M_UDIV) {
    nargs(3, 3); if (wid == 2) notnarrow();
    rd = R(0); rn = R(1); rm = R(2);
    return emit32((id == M_SDIV ? 0xFB90 : 0xFBB0) | rn, 0xF0F0 | rd << 8 | rm);
  }
  nargs(2, 3);
  rd = R(0); rn = R(1);
  if (nops == 3) rm = R(2); else rm = rd;
  if (lo(rd) && lo(rn) && lo(rm) && narrowdp() && (rd == rm || rd == rn))
    return emit16(0x4340 | (rd == rm ? rn : rm) << 3 | rd);
  if (setf) fail("muls needs low registers and rd equal to one source");
  notnarrow();
  return emit32(0xFB00 | rn, 0xF000 | rd << 8 | rm);
}

int do_neg() {
  int rd; int rm;
  nargs(2, 2); rd = R(0); rm = R(1);
  if (lo(rd) && lo(rm) && narrowdp()) return emit16(0x4240 | rm << 3 | rd);
  notnarrow();
  return dpimm(14, setf, rm, rd, 0);               // rsb rd, rm, #0
}

int do_ext(int id) {
  int rd; int rm; int k; int w;
  nargs(2, 2); rd = R(0); rm = R(1);
  k = 0; w = 0;                                     // sxth
  if (id == M_SXTB) { k = 1; w = 4; }
  if (id == M_UXTH) { k = 2; w = 1; }
  if (id == M_UXTB) { k = 3; w = 5; }
  if (wid != 4 && lo(rd) && lo(rm)) return emit16(0xB200 | k << 6 | rm << 3 | rd);
  notnarrow();
  return emit32(0xFA0F | w << 4, 0xF080 | rd << 8 | rm);
}

int do_mem(int id) {
  int rt; int rn; int rm; int v; int k; int scale; int nb; int tb;
  // k: str strh strb ldrsb ldr ldrh ldrb ldrsh (the order of the 16-bit register forms)
  k = 0; scale = 4; nb = 0x6000; tb = 0xF8C0;
  if (id == M_STRH) { k = 1; scale = 2; nb = 0x8000; tb = 0xF8A0; }
  if (id == M_STRB) { k = 2; scale = 1; nb = 0x7000; tb = 0xF880; }
  if (id == M_LDRSB) { k = 3; scale = 1; nb = 0; tb = 0xF990; }
  if (id == M_LDR) { k = 4; scale = 4; nb = 0x6800; tb = 0xF8D0; }
  if (id == M_LDRH) { k = 5; scale = 2; nb = 0x8800; tb = 0xF8B0; }
  if (id == M_LDRB) { k = 6; scale = 1; nb = 0x7800; tb = 0xF890; }
  if (id == M_LDRSH) { k = 7; scale = 2; nb = 0; tb = 0xF9B0; }
  nargs(2, 2);
  rt = R(0);
  if (okind[1] != O_MEM) fail("memory operand expected");
  rn = oreg[1];
  if (rn == 15 && (k < 3 || oidx[1] >= 0)) fail("cannot use register index with PC-relative addressing");   // GNU's words
  if (oidx[1] >= 0) {
    rm = oidx[1];
    if (wid != 4 && lo(rt) && lo(rn) && lo(rm)) return emit16(0x5000 | k << 9 | rm << 6 | rn << 3 | rt);
    notnarrow();
    return emit32((tb - 0x80) | rn, rt << 12 | rm);
  }
  v = 0;
  if (ohas[1]) v = opconst(1);
  if (v % scale) fail("misaligned offset");
  if (rn == 15) {                      // literal loads: ldr T1 (narrow), else imm12 with the U bit (bit 7) for the sign
    if (wid != 4 && k == 4 && lo(rt) && v >= 0 && v <= 1020) return emit16(0x4800 | rt << 8 | v >> 2);
    notnarrow();
    if (v >= 0 && v <= 4095) return emit32(tb | 15, rt << 12 | v);
    if (v < 0 && v >= -4095) return emit32((tb - 0x80) | 15, rt << 12 | -v);
    return fail("offset out of range");
  }
  if (wid != 4) {
    if (rn == 13 && (k == 0 || k == 4) && lo(rt) && v >= 0 && v <= 1020) return emit16((k == 4 ? 0x9800 : 0x9000) | rt << 8 | v >> 2);
    if (nb && lo(rt) && lo(rn) && v >= 0 && v <= 31 * scale) return emit16(nb | (v / scale) << 6 | rn << 3 | rt);
  }
  notnarrow();
  if (v >= 0 && v <= 4095) return emit32(tb | rn, rt << 12 | v);
  if (v < 0 && v >= -255) return emit32((tb - 0x80) | rn, rt << 12 | 0xC00 | -v);
  return fail("offset out of range");
}

int do_pushpop(int id) {
  int m; int n; int r; int i;
  nargs(1, 1);
  if (okind[0] != O_LIST) fail("register list expected");
  m = oreg[0];
  if (m == 0 || (m & 0x2000)) fail("invalid register list");
  if (id == M_PUSH && (m & 0x8000)) fail("invalid register list");
  if (id == M_POP && (m & 0xC000) == 0xC000) fail("invalid register list");
  if (wid != 4) {
    if (id == M_PUSH && (m & ~0x40FF) == 0) return emit16(0xB400 | ((m >> 14) & 1) << 8 | (m & 255));
    if (id == M_POP && (m & ~0x80FF) == 0) return emit16(0xBC00 | ((m >> 15) & 1) << 8 | (m & 255));
  }
  notnarrow();
  n = 0; r = 0;
  for (i = 0; i < 16; i++) if (m & (1 << i)) { n++; r = i; }
  if (n == 1) {
    if (id == M_PUSH) return emit32(0xF84D, r << 12 | 0x0D04);   // str.w r, [sp, #-4]!
    return emit32(0xF85D, r << 12 | 0x0B04);                     // ldr.w r, [sp], #4
  }
  if (id == M_PUSH) return emit32(0xE92D, m);
  return emit32(0xE8BD, m);
}

int do_branch(int id) {
  struct Item *it; char *save;
  nargs(1, 1);
  if (id == M_BX || id == M_BLX) {
    if (wid == 4) fail("no wide encoding for bx/blx");
    return emit16((id == M_BX ? 0x4700 : 0x4780) | R(0) << 3);
  }
  if (okind[0] != O_EXPR && okind[0] != O_REG) fail("branch target expected");
  save = p; p = otext[0]; expr(); p = save;          // any name is a symbol here, even sp
  if (es2 >= 0 || es1 < 0) fail("branch target must be a label");
  if (id == M_BL) {
    if (wid == 2) fail("no narrow encoding for bl");
    it = newitem(K_BR, 4);
    it->flags = B_BL | B_WIDE | 14;
  } else {
    it = newitem(K_BR, wid == 4 ? 4 : 2);
    it->flags = (cond < 0 || init) ? 14 : cond;
    if (wid == 4) it->flags = it->flags | B_WIDE;
    if (wid == 2) it->flags = it->flags | B_NARROW;
  }
  if (init) it->flags = it->flags | B_INIT;
  it->sym = es1; it->val = ec;
  return 0;
}

int do_it(char *mn) {
  int n; int fc; int i; int mask; int c; char *save;
  if (itleft) fail("IT block inside an IT block");
  n = strlen(mn) - 1;                                  // instructions in the block
  skipws();
  save = p;
  while (isidch(*p)) p++;
  if (p - save != 2 || condnum(save) < 0 || condnum(save) == 14) fail("condition expected");
  fc = condnum(save);
  mask = 0;
  itcond[0] = fc;
  for (i = 1; i < n; i++) {
    c = mn[i + 1] == 't' ? (fc & 1) : !(fc & 1);
    mask = mask | c << (4 - i);
    itcond[i] = mn[i + 1] == 't' ? fc : fc ^ 1;
  }
  mask = mask | 1 << (4 - n);
  itleft = n; itpos = 0;
  return emit16(0xBF00 | fc << 4 | mask);
}

int isit(char *mn) {
  int i; int n;
  if (mn[0] != 'i' || mn[1] != 't') return 0;
  n = strlen(mn);
  if (n > 5) return 0;
  for (i = 2; i < n; i++) if (mn[i] != 't' && mn[i] != 'e') return 0;
  return 1;
}

int instruction(char *q, int n) {
  char mn[20]; int i; int id; int k;
  if (n > 15) failname("unknown instruction '%s'", q, n);
  for (i = 0; i < n; i++) mn[i] = lower(q[i]);
  mn[n] = 0;
  wid = 0; k = n;
  if (k > 2 && mn[k - 2] == '.') {
    if (mn[k - 1] == 'w') wid = 4;
    else if (mn[k - 1] == 'n') wid = 2;
    else failname("unknown instruction '%s'", q, n);
    mn[k - 2] = 0;
  }
  if (wid == 0 && isit(mn)) return do_it(mn);
  id = findmn(mn);
  if (id < 0) failname("unknown instruction '%s'", q, n);
  parseops();
  init = 0;
  if (itleft) {
    if (cond != itcond[itpos]) fail("wrong condition in IT block");
    init = 1; itpos++; itleft--;
    if (itleft && (id == M_B || id == M_BL || id == M_BX || id == M_BLX)) fail("a branch must be the last instruction of an IT block");
  } else if (cond >= 0 && id != M_B) fail("conditional instruction outside an IT block");
  if (id == M_MOV || id == M_MVN) return do_mov(id);
  if (id == M_MOVW || id == M_MOVT) {
    nargs(2, 2); if (wid == 2) notnarrow();
    return mov16(id == M_MOVT, R(0), IMM(1));
  }
  if (id == M_ADD || id == M_SUB) return do_addsub(id);
  if (id == M_ADDW || id == M_SUBW) return do_addw(id);
  if (id == M_CMP || id == M_CMN || id == M_TST) return do_cmp(id);
  if (id >= M_AND && id <= M_BIC) return do_logic(id);
  if (id >= M_LSL && id <= M_ROR) return do_shift(id);
  if (id >= M_MUL && id <= M_UDIV) return do_mul(id);
  if (id == M_NEG) return do_neg();
  if (id >= M_SXTB && id <= M_UXTH) return do_ext(id);
  if (id >= M_LDR && id <= M_LDRSH) return do_mem(id);
  if (id == M_PUSH || id == M_POP) return do_pushpop(id);
  if (id == M_NOP) { nargs(0, 0); if (wid == 4) return emit32(0xF3AF, 0x8000); return emit16(0xBF00); }
  return do_branch(id);
}

// ------------------------------------------------------------------ directives
int isdir(char *q, int n, char *name) { return strlen(name) == n && memcmp(q, name, n) == 0; }

// a .byte or .short value must fit: 0 .. 2^bits - 1 or -2^bits .. -1, as GNU as checks
int fits(int v, int size) {
  if (size == 1 && (v < -256 || v > 255)) fail("value out of range");
  if (size == 2 && (v < -65536 || v > 65535)) fail("value out of range");
  return v;
}

int data(int size) {
  struct Item *it;
  skipws();
  if (*p == 0) return 0;
  while (1) {
    expr();
    if (es1 < 0 && es2 < 0) {
      fits(ec, size);
      emit8(ec);
      if (size > 1) emit8(ec >> 8);
      if (size > 2) { emit8(ec >> 16); emit8(ec >> 24); }
    } else {
      it = newitem(K_DATA, size);
      it->sym = es1; it->sym2 = es2; it->val = ec;
    }
    skipws();
    if (*p != ',') return 0;
    p++;
  }
  return 0;
}

int strings(int zero) {
  while (1) {
    skipws();
    if (*p != '"') fail("string expected");
    p++;
    while (*p != '"') {
      if (*p == 0) fail("unterminated string");
      if (*p == '\\') { p++; emit8(escchar()); }
      else { emit8(*p); p++; }
    }
    p++;
    if (zero) emit8(0);
    skipws();
    if (*p != ',') return 0;
    p++;
  }
  return 0;
}

int align(int bytes) {
  struct Item *it;
  if (bytes > 32768 || (bytes & (bytes - 1))) fail("alignment must be a power of two");
  it = newitem(K_ALIGN, 0);
  it->val = bytes; it->sym2 = -1;
  skipws();
  if (*p == ',') { p++; it->sym2 = constexpr() & 255; }
  if (bytes > maxal[cur]) maxal[cur] = bytes;
  return 0;
}

int names(int flag) {
  char *q; int n; int i;
  while (1) {
    skipws();
    q = p; n = scanident();
    if (n == 0) fail("symbol name expected");
    i = lookup(q, n);
    syms[i].flags = syms[i].flags | flag;
    skipws();
    if (*p != ',') return 0;
    p++;
  }
  return 0;
}

int directive(char *q, int n) {
  int v; int f; int i; char *nm; int nl; struct Sym *y;
  if (isdir(q, n, ".syntax")) {
    skipws(); nm = p; nl = scanident();
    if (nl != 7 || memcmp(nm, "unified", 7) != 0) fail("only .syntax unified is supported");
    return 0;
  }
  if (isdir(q, n, ".thumb") || isdir(q, n, ".thumb_func")) return 0;
  if (isdir(q, n, ".text")) { cur = SEC_TEXT; return 0; }
  if (isdir(q, n, ".data")) { cur = SEC_DATA; return 0; }
  if (isdir(q, n, ".global") || isdir(q, n, ".globl")) return names(F_GLOBAL);
  if (isdir(q, n, ".extern")) return names(F_EXTERN);
  if (isdir(q, n, ".word")) return data(4);
  if (isdir(q, n, ".short") || isdir(q, n, ".hword")) return data(2);
  if (isdir(q, n, ".byte")) return data(1);
  if (isdir(q, n, ".ascii")) return strings(0);
  if (isdir(q, n, ".asciz") || isdir(q, n, ".string")) return strings(1);
  if (isdir(q, n, ".space") || isdir(q, n, ".zero")) {
    v = constexpr(); f = 0;
    if (v < 0) fail("negative size");
    skipws();
    if (*p == ',') { p++; f = constexpr(); }
    for (i = 0; i < v; i++) emit8(f);
    return 0;
  }
  if (isdir(q, n, ".align")) {
    v = constexpr();
    if (v < 0 || v > 15) fail("alignment out of range");
    return align(1 << v);
  }
  if (isdir(q, n, ".balign")) {
    v = constexpr();
    if (v < 1) fail("alignment must be a power of two");
    return align(v);
  }
  if (isdir(q, n, ".equ") || isdir(q, n, ".set")) {
    skipws(); nm = p; nl = scanident();
    if (nl == 0) fail("symbol name expected");
    skipws();
    if (*p != ',') fail("missing ,");
    p++;
    i = lookup(nm, nl);
    y = syms + i;
    if ((y->flags & F_DEF) && !(y->flags & F_EQU)) failname("symbol '%s' already defined", nm, nl);
    expr();
    y = syms + i;                      // expr may have moved the table
    if (es2 >= 0) fail("constant or label expected");
    if (es1 < 0) { y->sect = SEC_ABS; y->val = ec; }
    else {
      if (!(syms[es1].flags & F_DEF)) undefined(es1);
      y->sect = syms[es1].sect; y->pos = syms[es1].pos; y->item = syms[es1].item; y->val = syms[es1].val + ec;
    }
    y->flags = y->flags | F_DEF | F_EQU; y->line = line;
    return 0;
  }
  return failname("unknown directive '%s'", q, n);
}

int statement() {
  char *q; int n; char *t;
  while (1) {
    skipws();
    if (*p == 0) return 0;
    if (!isidstart(*p)) fail("syntax error");
    q = p; n = scanident();
    t = p; skipws();
    if (*p == ':') { p++; deflabel(q, n); continue; }
    p = t;
    if (q[0] == '.') directive(q, n); else instruction(q, n);
    skipws();
    if (*p != 0) fail("junk at end of line");
    return 0;
  }
  return 0;
}

// ------------------------------------------------------------------ second pass
int layout(int sec) {
  int k; int extra; struct Item *it;
  extra = 0;
  for (k = 0; k < nitems[sec]; k++) {
    it = items[sec] + k;
    it->addr = it->pos + extra;
    if (it->kind == K_ALIGN) it->size = (0 - it->addr) & (it->val - 1);
    extra = extra + it->size;
  }
  sextra[sec] = extra;
  return 0;
}

int reaches(struct Item *it, int off) {
  int c;
  c = it->flags & 15;
  if (it->size == 2) {
    if (c != 14 && !(it->flags & B_INIT)) return off >= -256 && off <= 254;
    return off >= -2048 && off <= 2046;
  }
  if (c != 14 && !(it->flags & B_INIT)) return off >= -1048576 && off <= 1048574;
  return off >= -16777216 && off <= 16777214;
}

int checkrefs(int sec) {
  int k; struct Item *it;
  for (k = 0; k < nitems[sec]; k++) {
    it = items[sec] + k;
    line = it->line;
    if (it->sym >= 0 && !(syms[it->sym].flags & F_DEF)) undefined(it->sym);
    if (it->sym2 >= 0 && it->kind == K_DATA && !(syms[it->sym2].flags & F_DEF)) undefined(it->sym2);
    if (it->kind == K_BR && syms[it->sym].sect != sec) fail("branch target in another section");
    if (it->kind == K_DATA && it->sym2 >= 0) {
      if (it->sym < 0 && syms[it->sym2].sect != SEC_ABS) fail("constant or label expected");
      if (it->sym >= 0 && syms[it->sym].sect != syms[it->sym2].sect) fail("difference of symbols in different sections");
    }
  }
  return 0;
}

int relax() {
  int changed; int sec; int k; int off; struct Item *it;
  changed = 1;
  while (changed) {
    changed = 0;
    layout(SEC_TEXT); layout(SEC_DATA);
    for (sec = SEC_TEXT; sec <= SEC_DATA; sec++)
      for (k = 0; k < nitems[sec]; k++) {
        it = items[sec] + k;
        if (it->kind != K_BR || it->size != 2 || (it->flags & B_NARROW)) continue;
        off = symaddr(it->sym) + it->val - (it->addr + 4);
        if (!reaches(it, off)) { it->size = 4; changed = 1; }
      }
  }
  return 0;
}

int put16(char *o, int h) { o[0] = h & 255; o[1] = (h >> 8) & 255; return 0; }

int encbranch(char *o, struct Item *it) {
  int off; int c; int s; int j1; int j2;
  line = it->line;
  off = symaddr(it->sym) + it->val - (it->addr + 4);
  if (off & 1) fail("branch target not halfword aligned");
  if (!reaches(it, off)) fail("branch out of range");
  c = it->flags & 15;
  if (it->flags & B_INIT) c = 14;
  s = off < 0 ? 1 : 0;
  if (it->size == 2) {
    if (c != 14) return put16(o, 0xD000 | c << 8 | ((off >> 1) & 255));
    return put16(o, 0xE000 | ((off >> 1) & 2047));
  }
  if (c != 14) {
    j1 = (off >> 18) & 1; j2 = (off >> 19) & 1;
    put16(o, 0xF000 | s << 10 | c << 6 | ((off >> 12) & 63));
    return put16(o + 2, 0x8000 | j1 << 13 | j2 << 11 | ((off >> 1) & 2047));
  }
  j1 = !(((off >> 23) & 1) ^ s); j2 = !(((off >> 22) & 1) ^ s);
  put16(o, 0xF000 | s << 10 | ((off >> 12) & 1023));
  return put16(o + 2, ((it->flags & B_BL) ? 0xD000 : 0x9000) | j1 << 13 | j2 << 11 | ((off >> 1) & 2047));
}

// fill n bytes at address a: zeros, or in text the GNU code fill (a zero byte to even, nop, nop.w)
int fill(char *o, int a, int n, int f, int code) {
  int i;
  if (f >= 0 || !code) { for (i = 0; i < n; i++) o[i] = f < 0 ? 0 : f; return 0; }
  i = 0;
  if ((a & 1) && n > 0) { o[0] = 0; i = 1; }
  if (n - i >= 2 && ((n - i) & 2)) { put16(o + i, 0xBF00); i = i + 2; }
  while (n - i >= 4) { put16(o + i, 0xF3AF); put16(o + i + 2, 0x8000); i = i + 4; }
  return 0;
}

int value(struct Item *it) {
  int v;
  v = it->val;
  if (it->sym >= 0) v = v + symaddr(it->sym);
  if (it->sym2 >= 0) v = v - symaddr(it->sym2);
  return v;
}

// the final bytes of a section; *size gets its length
char *finish(int sec, int *size) {
  char *o; int n; int k; int q; int at; int v; int pad; int a; struct Item *it;
  pad = 0;
  n = sl[sec] + sextra[sec];
  if (sec == SEC_TEXT && maxal[sec] >= 2) {
    a = maxal[sec] > 4 ? 4 : maxal[sec];
    pad = (0 - n) & (a - 1);
  }
  o = malloc(n + pad + 1);
  if (!o) fail("out of memory");
  memset(o, 0, n + pad + 1);
  q = 0; at = 0;
  for (k = 0; k < nitems[sec]; k++) {
    it = items[sec] + k;
    memcpy(o + at, sb[sec] + q, it->pos - q);
    at = at + it->pos - q; q = it->pos;
    if (it->kind == K_BR) encbranch(o + at, it);
    if (it->kind == K_ALIGN) fill(o + at, it->addr, it->size, it->sym2, sec == SEC_TEXT);
    if (it->kind == K_DATA) {
      line = it->line;
      v = fits(value(it), it->size);
      o[at] = v & 255;
      if (it->size > 1) o[at + 1] = (v >> 8) & 255;
      if (it->size > 2) { o[at + 2] = (v >> 16) & 255; o[at + 3] = (v >> 24) & 255; }
    }
    at = at + it->size;
  }
  memcpy(o + at, sb[sec] + q, sl[sec] - q);
  fill(o + n, n, pad, -1, 1);
  *size = n + pad;
  return o;
}

int writeobj(char *out) {
  struct t2o o; struct t2sym *ts; struct Sym *y; char *str; int ns; int strsize; int i; int fd; int at; int tsz; int dsz;
  o.text = finish(SEC_TEXT, &tsz);
  o.data = finish(SEC_DATA, &dsz);
  o.h.text_size = tsz; o.h.data_size = dsz;
  ns = 0; strsize = 1;
  for (i = 0; i < nsyms; i++) {
    y = syms + i;
    if (y->flags & F_ANON) continue;
    if (y->len >= 2 && y->name[0] == '.' && y->name[1] == 'L') continue;
    if (!(y->flags & (F_DEF | F_GLOBAL | F_EXTERN))) continue;
    y->out = ns; ns++; strsize = strsize + y->len + 1;
  }
  ts = malloc(ns * sizeof(struct t2sym) + 4);
  str = malloc(strsize + 1); memset(str, 0, strsize + 1);
  at = 1;
  for (i = 0; i < nsyms; i++) {
    y = syms + i;
    if (y->out < 0) continue;
    ts[y->out].name = at;
    memcpy(str + at, y->name, y->len); at = at + y->len + 1;
    if (y->flags & F_DEF) { ts[y->out].sect = y->sect; ts[y->out].value = symaddr(i); }
    else { ts[y->out].sect = SEC_UNDEF; ts[y->out].value = 0; }
    ts[y->out].flags = (y->flags & (F_GLOBAL | F_EXTERN)) ? SYM_GLOBAL : 0;
  }
  o.h.magic = T2OBJ_MAGIC; o.h.version = T2OBJ_VERSION;
  o.h.nsyms = ns; o.h.nrels = 0; o.h.str_size = strsize;
  o.syms = ts; o.rels = 0; o.str = str;
  fd = open(out, 577, 420);
  if (fd < 0) { printf("as: cannot write %s\n", out); exit(1); }
  if (t2o_write(fd, &o) < 0) { printf("as: write error on %s\n", out); close(fd); exit(1); }
  close(fd);
  return 0;
}

int assemble() {
  char *e; char c;
  line = 1; p = src;
  while (1) {
    e = p;
    while (*e && *e != '\n') e++;
    c = *e; *e = 0;
    statement();
    if (c == 0) break;
    p = e + 1; line++;
  }
  if (itleft) fail("IT block not finished");
  checkrefs(SEC_TEXT); checkrefs(SEC_DATA);
  relax();
  return 0;
}

int main(int argc, char **argv) {
  char *out; char *in; int i; int size; int n;
  out = 0; in = 0;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { i++; out = argv[i]; }
    else if (in == 0) in = argv[i];
    else { printf("usage: as [-o out.o] file.s\n"); return 1; }
  }
  if (!in) { printf("usage: as [-o out.o] file.s\n"); return 1; }
  if (!out) {
    n = strlen(in);
    out = malloc(n + 3);
    strcpy(out, in);
    if (n > 2 && in[n - 2] == '.' && in[n - 1] == 's') out[n - 1] = 'o'; else strcpy(out + n, ".o");
  }
  srcname = in;
  src = t2_readfile(in, &size);
  if (!src) { printf("as: cannot read %s\n", in); return 1; }
  hbuck = malloc(HBITS * sizeof(int)); memset((char *)hbuck, 0, HBITS * sizeof(int));
  for (i = 0; i < 3; i++) { sb[i] = 0; sl[i] = 0; sc[i] = 0; maxal[i] = 0; items[i] = 0; nitems[i] = 0; citems[i] = 0; sextra[i] = 0; }
  cur = SEC_TEXT; syms = 0; nsyms = 0; csyms = 0; itleft = 0;
  strip(src);
  assemble();
  line = 0;
  writeobj(out);
  return 0;
}
