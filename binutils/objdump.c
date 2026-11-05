// objdump - dump T2DOS objects, programs and archives, with a Thumb-2 and FPU disassembler
//   objdump FILE...      (objects, .bin programs and archives; several files: one dump each)
// Output, in this order:
//   FILE: T2DOS object, text N bytes, data N bytes, N symbols, N relocations
//     (a program: "FILE: T2DOS program, code N bytes, data N bytes, entry 0xNNNN"; an archive:
//     "FILE: T2DOS archive, N members", then each member's dump headed FILE(MEMBER):)
//   SYMBOLS: (as nm) and RELOCATIONS: ("SECT OFFSET TYPE SYMBOL+ADDEND"), objects only
//   DISASSEMBLY: of the text (code), one instruction per line in GNU objdump's format and text
//     (arm-none-eabi-objdump -D -b binary -m arm -M force-thumb), without its comments; a branch
//     to a text symbol of an object is followed by <SYMBOL+0xOFF>
//   DATA: the data as hex dump lines
// The disassembler decodes the instructions build/host/as assembles (assembler/as.c), with the IT
// state tracked like GNU's. Everything else, and every encoding that as would not give back from
// the text printed, is printed as .short 0xNNNN (a 32-bit encoding as two .short lines).
// tools/check_asm.py --objdump-corpus compares the text with GNU's and reassembles it.
// Errors: "objdump: cannot read FILE", "objdump: FILE: not a T2DOS object", exit 1.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "t2obj.c"

// ------------------------------------------------------------------ names
char *rname(int r) {
  switch (r) {
  case 0: return "r0"; case 1: return "r1"; case 2: return "r2"; case 3: return "r3";
  case 4: return "r4"; case 5: return "r5"; case 6: return "r6"; case 7: return "r7";
  case 8: return "r8"; case 9: return "r9"; case 10: return "sl"; case 11: return "fp";
  case 12: return "ip"; case 13: return "sp"; case 14: return "lr";
  }
  return "pc";
}

char cbuf[3];
char *cname(int c) {
  char *t;
  t = "eqnecsccmiplvsvchilsgeltgtleal";
  if (c < 0 || c > 14) return "";
  cbuf[0] = t[2 * c]; cbuf[1] = t[2 * c + 1]; cbuf[2] = 0;
  return cbuf;
}

// ------------------------------------------------------------------ the instruction being decoded
char mn[48]; char ops[200];
int addr;          // offset of the instruction
int target;        // branch target, when hastgt
int hastgt;
int icond;         // condition from the IT block, -1 outside one
int ilast;         // the instruction is the last of its IT block
int itstate;       // GNU's IT state: firstcond << 4 | mask, 0 outside a block

int cat(char *d, char *s) { strcpy(d + strlen(d), s); return 0; }
char *itc() { if (icond >= 0) return cname(icond); return ""; }

// 16-bit forms: "%C" (s outside an IT block, the condition inside) and "%c" (the condition)
int m16s(char *base) { strcpy(mn, base); if (icond >= 0) cat(mn, cname(icond)); else cat(mn, "s"); return 1; }
int m16(char *base) { strcpy(mn, base); cat(mn, itc()); return 1; }
// 32-bit forms: base, s, condition, .w
int m32(char *base, int s, int w) {
  strcpy(mn, base);
  if (s) cat(mn, "s");
  cat(mn, itc());
  if (w) cat(mn, ".w");
  return 1;
}

int op2(int a, int b) { sprintf(ops, "%s, %s", rname(a), rname(b)); return 1; }
int op3(int a, int b, int c) { sprintf(ops, "%s, %s, %s", rname(a), rname(b), rname(c)); return 1; }
int opimm(int a, unsigned v) { sprintf(ops, "%s, #%u", rname(a), v); return 1; }
int op2imm(int a, int b, unsigned v) { sprintf(ops, "%s, %s, #%u", rname(a), rname(b), v); return 1; }

// "{r4, r5, lr}"
int reglist(char *d, int m) {
  int i; int first;
  strcpy(d, "{"); first = 1;
  for (i = 0; i < 16; i++) {
    if (!(m & (1 << i))) continue;
    if (!first) cat(d, ", ");
    cat(d, rname(i)); first = 0;
  }
  cat(d, "}");
  return 0;
}
int nbits(int m) { int n; n = 0; while (m) { n = n + (m & 1); m = (m >> 1) & 0x7fffffff; } return n; }

// [rn, #v] with the offset left out when it is 0 and zero is "plain" (32-bit forms)
int memop(int rt, int rn, int v, int plain) {
  if (v == 0 && plain) sprintf(ops, "%s, [%s]", rname(rt), rname(rn));
  else sprintf(ops, "%s, [%s, #%d]", rname(rt), rname(rn), v);
  return 1;
}

int branch(int t) {
  target = t; hastgt = 1;
  sprintf(ops, "0x%x", t);
  return 1;
}

// ------------------------------------------------------------------ Thumb-2 modified immediates
unsigned expandimm(int m) {
  unsigned v; int r;
  v = m & 255;
  switch ((m >> 8) & 15) {
  case 0: return v;
  case 1: return v | v << 16;
  case 2: return v << 8 | v << 24;
  case 3: return v | v << 8 | v << 16 | v << 24;
  }
  r = (m >> 7) & 31;
  v = (m & 127) | 128;
  return (v >> r) | (v << (32 - r));
}

// the encoding as chooses for a value (as.c modimm), or -1
int modimm(unsigned v) {
  unsigned t; int r;
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

// ------------------------------------------------------------------ 16-bit instructions
// 1 when decoded into mn and ops, 0 when it is to be printed as .short
int dec16(int h) {
  int op; int rd; int rn; int rm; int v; int fc; int mask; int i; int n; char *nm;
  rd = h & 7; rn = (h >> 3) & 7; rm = (h >> 6) & 7;
  if (h < 0x1800) {                                     // lsls/lsrs/asrs rd, rm, #imm
    op = h >> 11; v = (h >> 6) & 31;
    if (op == 0 && v == 0) {                            // movs rd, rm (as: mov inside a block is 0x4600)
      if (icond >= 0) return 0;
      m16s("mov"); return op2(rd, rn);
    }
    if (op == 0) m16s("lsl"); else if (op == 1) m16s("lsr"); else m16s("asr");
    if (v == 0) v = 32;
    sprintf(ops, "%s, %s, #%d", rname(rd), rname(rn), v);
    return 1;
  }
  if (h < 0x2000) {
    op = (h >> 9) & 3;
    m16s((op & 1) ? "sub" : "add");
    if (op < 2) return op3(rd, rn, rm);
    if (rd == rn) return 0;                             // as picks the 8-bit immediate form
    sprintf(ops, "%s, %s, #%d", rname(rd), rname(rn), rm);
    return 1;
  }
  if (h < 0x4000) {                                     // movs cmp adds subs rd, #imm8
    op = (h >> 11) & 3; rd = (h >> 8) & 7;
    if (op == 0) m16s("mov");
    if (op == 1) m16("cmp");
    if (op == 2) m16s("add");
    if (op == 3) m16s("sub");
    return opimm(rd, h & 255);
  }
  if (h < 0x4400) {                                     // data processing, two low registers
    op = (h >> 6) & 15;
    if (op == 5 || op == 6) return 0;                   // adc sbc
    nm = "and";
    if (op == 1) nm = "eor";
    if (op == 2) nm = "lsl";
    if (op == 3) nm = "lsr";
    if (op == 4) nm = "asr";
    if (op == 7) nm = "ror";
    if (op == 9) nm = "neg";
    if (op == 12) nm = "orr";
    if (op == 13) nm = "mul";
    if (op == 14) nm = "bic";
    if (op == 15) nm = "mvn";
    if (op == 8) m16("tst");
    else if (op == 10) m16("cmp");
    else if (op == 11) m16("cmn");
    else m16s(nm);
    return op2(rd, rn);
  }
  if (h < 0x4700) {                                     // add cmp mov with high registers
    op = (h >> 8) & 3;
    rd = (h & 7) | ((h >> 4) & 8); rm = (h >> 3) & 15;
    if (op == 0) {
      if (icond >= 0 && rd < 8 && rm < 8) return 0;    // as: adds form inside a block
      m16("add"); return op2(rd, rm);
    }
    if (op == 1) {
      if (rd < 8 && rm < 8) return 0;                  // as: the low-register cmp
      m16("cmp"); return op2(rd, rm);
    }
    if (h == 0x46C0) return 0;                          // GNU: nop (mov r8, r8)
    m16("mov"); return op2(rd, rm);
  }
  if (h < 0x4800) {                                     // bx blx
    if (h & 7) return 0;
    if (icond >= 0 && !ilast) return 0;
    if (h & 0x80) m16("blx"); else m16("bx");
    strcpy(ops, rname((h >> 3) & 15));
    return 1;
  }
  if (h < 0x5000) {                                     // ldr rt, [pc, #imm]
    m16("ldr");
    sprintf(ops, "%s, [pc, #%d]", rname((h >> 8) & 7), (h & 255) * 4);
    return 1;
  }
  if (h < 0x6000) {                                     // register offset
    op = (h >> 9) & 7;
    nm = "str";
    if (op == 1) nm = "strh";
    if (op == 2) nm = "strb";
    if (op == 3) nm = "ldrsb";
    if (op == 4) nm = "ldr";
    if (op == 5) nm = "ldrh";
    if (op == 6) nm = "ldrb";
    if (op == 7) nm = "ldrsh";
    m16(nm);
    sprintf(ops, "%s, [%s, %s]", rname(rd), rname(rn), rname(rm));
    return 1;
  }
  if (h < 0x9000) {                                     // immediate offset
    v = (h >> 6) & 31;
    op = (h >> 11) & 1;
    if (h < 0x7000) { m16(op ? "ldr" : "str"); v = v * 4; }
    else if (h < 0x8000) m16(op ? "ldrb" : "strb");
    else { m16(op ? "ldrh" : "strh"); v = v * 2; }
    return memop(rd, rn, v, 0);
  }
  if (h < 0xA000) {                                     // [sp, #imm]
    m16((h & 0x800) ? "ldr" : "str");
    return memop((h >> 8) & 7, 13, (h & 255) * 4, 0);
  }
  if (h < 0xA800) return 0;                             // adr
  if (h < 0xB000) {
    m16("add");
    sprintf(ops, "%s, sp, #%d", rname((h >> 8) & 7), (h & 255) * 4);
    return 1;
  }
  if (h < 0xB100) {                                     // add/sub sp, #imm
    m16((h & 0x80) ? "sub" : "add");
    sprintf(ops, "sp, #%d", (h & 127) * 4);
    return 1;
  }
  if (h >= 0xB200 && h < 0xB300) {                      // sxth sxtb uxth uxtb
    op = (h >> 6) & 3;
    nm = "sxth";
    if (op == 1) nm = "sxtb";
    if (op == 2) nm = "uxth";
    if (op == 3) nm = "uxtb";
    m16(nm);
    return op2(rd, rn);
  }
  if ((h & 0xFE00) == 0xB400 || (h & 0xFE00) == 0xBC00) {   // push pop
    v = h & 255;
    if (h & 0x100) v = v | ((h & 0x800) ? 0x8000 : 0x4000);
    if (v == 0) return 0;
    m16((h & 0x800) ? "pop" : "push");
    reglist(ops, v);
    return 1;
  }
  if ((h & 0xFF00) == 0xBF00) {
    if ((h & 15) == 0) {                                // hints: only nop
      if (h != 0xBF00) return 0;
      m16("nop"); ops[0] = 0;
      return 1;
    }
    fc = (h >> 4) & 15; mask = h & 15;                  // IT
    if (icond >= 0 || fc >= 14) return 0;               // as: no IT inside a block, no "it al"
    n = 4;
    while (!(mask & (1 << (4 - n)))) n--;
    strcpy(mn, "it");
    for (i = 1; i < n; i++) cat(mn, (((mask >> (4 - i)) & 1) == (fc & 1)) ? "t" : "e");
    strcpy(ops, cname(fc));
    return 1;
  }
  if (h >= 0xD000 && h < 0xDE00) {                      // b<cond>.n
    if (icond >= 0) return 0;                           // as: b inside a block is the 0xE000 form
    strcpy(mn, "b"); cat(mn, cname((h >> 8) & 15)); cat(mn, ".n");
    v = h & 255; if (v & 128) v = v - 256;
    return branch(addr + 4 + v * 2);
  }
  if (h >= 0xE000 && h < 0xE800) {                      // b.n
    if (icond >= 0 && !ilast) return 0;
    strcpy(mn, "b"); cat(mn, itc()); cat(mn, ".n");
    v = h & 0x7FF; if (v & 0x400) v = v - 0x800;
    return branch(addr + 4 + v * 2);
  }
  return 0;
}

// ------------------------------------------------------------------ 32-bit instructions
char *ldstname(int s, int sz, int l) {
  if (!l) { if (sz == 0) return "strb"; if (sz == 1) return "strh"; return "str"; }
  if (s) { if (sz == 0) return "ldrsb"; return "ldrsh"; }
  if (sz == 0) return "ldrb"; if (sz == 1) return "ldrh"; return "ldr";
}

int dec_ldst(int h1, int h2) {
  int s; int sz; int l; int rn; int rt; int v; int puw; int scale; char *nm;
  s = (h1 >> 8) & 1; sz = (h1 >> 5) & 3; l = (h1 >> 4) & 1; rn = h1 & 15; rt = (h2 >> 12) & 15;
  if (sz == 3 || (s && (!l || sz == 2))) return 0;
  if (l && rt == 15 && (sz == 0 || (sz == 1 && !s))) return 0;   // pld pli pldw
  nm = ldstname(s, sz, l);
  scale = 1 << sz;                                      // as: offsets are multiples of the size
  if (rn == 15) {                                       // literal: imm12 with U in bit 7
    if (!l) return 0;
    v = h2 & 0xFFF;
    if (v % scale) return 0;
    if (!(h1 & 0x80)) {
      if (v == 0 || (v & 0xF00) == 0xE00) return 0;   // GNU: ldrt and the like
      v = -v;
    }
    m32(nm, 0, 1);
    if (v == 0) sprintf(ops, "%s, [pc]", rname(rt));
    else sprintf(ops, "%s, [pc, #%d]", rname(rt), v);
    return 1;
  }
  if (h1 & 0x80) {
    if ((h2 & 0xFFF) % scale) return 0;
    m32(nm, 0, 1); return memop(rt, rn, h2 & 0xFFF, 1);
  }
  if (h2 & 0x800) {
    puw = (h2 >> 8) & 7; v = h2 & 255;
    if (puw == 4) {                                     // [rn, #-imm8]
      if (v == 0 || v % scale) return 0;
      m32(nm, 0, 1); return memop(rt, rn, -v, 1);
    }
    if (puw == 5 && !l && sz == 2 && rn == 13 && v == 4 && rt != 13 && rt != 15) {   // push.w {rt}
      m32(nm, 0, 1);
      sprintf(ops, "%s, [sp, #-4]!", rname(rt));
      return 1;
    }
    if (puw == 3 && l && !s && sz == 2 && rn == 13 && v == 4 && rt != 13) {          // pop.w {rt}
      m32(nm, 0, 1);
      sprintf(ops, "%s, [sp], #4", rname(rt));
      return 1;
    }
    return 0;
  }
  if (h2 & 0xFF0) return 0;                             // register offset, no shift
  m32(nm, 0, 1);
  sprintf(ops, "%s, [%s, %s]", rname(rt), rname(rn), rname(h2 & 15));
  return 1;
}

// data processing with a modified immediate
int dec_dpimm(int h1, int h2) {
  int op; int s; int rn; int rd; int m; unsigned v;
  op = (h1 >> 5) & 15; s = (h1 >> 4) & 1; rn = h1 & 15; rd = (h2 >> 8) & 15;
  m = ((h1 >> 10) & 1) << 11 | ((h2 >> 12) & 7) << 8 | (h2 & 255);
  v = expandimm(m);
  if (modimm(v) != m) return 0;                         // not the encoding as gives this value
  if (rd == 15 && s && (op == 0 || op == 8 || op == 13)) {
    if (op == 0) m32("tst", 0, 1);
    if (op == 8) m32("cmn", 0, 1);
    if (op == 13) m32("cmp", 0, 1);
    return opimm(rn, v);
  }
  if (op == 2 && rn == 15) { m32("mov", s, 1); return opimm(rd, v); }
  if (op == 3 && rn == 15) { m32("mvn", s, 1); return opimm(rd, v); }
  if (op == 0) m32("and", s, 1);
  else if (op == 1) m32("bic", s, 1);
  else if (op == 2) m32("orr", s, 1);
  else if (op == 3) {                                   // as writes orr rd, rn, #~v when ~v has no encoding
    if (modimm(~v) >= 0) return 0;
    m32("orn", s, 0);
  }
  else if (op == 4) { if (rd == 15 && s) return 0; m32("eor", s, 1); }
  else if (op == 8) m32("add", s, 1);
  else if (op == 13) m32("sub", s, 1);
  else if (op == 14) { if (v != 0) return 0; m32("rsb", s, 0); }   // neg.w
  else return 0;
  return op2imm(rd, rn, v);
}

// data processing with a register
int dec_dpreg(int h1, int h2) {
  int op; int s; int rn; int rd; int rm; int amt; int type; char *nm;
  if (h2 & 0x8000) return 0;
  op = (h1 >> 5) & 15; s = (h1 >> 4) & 1; rn = h1 & 15; rd = (h2 >> 8) & 15; rm = h2 & 15;
  amt = ((h2 >> 12) & 7) << 2 | ((h2 >> 6) & 3); type = (h2 >> 4) & 3;
  if (op == 2 && rn == 15) {                            // mov, and the shifts by an immediate
    m32("mov", s, 1);
    op2(rd, rm);
    if (amt == 0 && type == 0) return 1;
    if (amt == 0 && type == 3) return 0;                // rrx
    if (amt == 0) amt = 32;
    nm = ", lsl #";
    if (type == 1) nm = ", lsr #";
    if (type == 2) nm = ", asr #";
    if (type == 3) nm = ", ror #";
    cat(ops, nm);
    sprintf(ops + strlen(ops), "%d", amt);
    return 1;
  }
  if (amt || type) return 0;
  if (op == 3 && rn == 15) { m32("mvn", s, 1); return op2(rd, rm); }
  if (rd == 15 && s && (op == 0 || op == 8 || op == 13)) {
    if (op == 0) m32("tst", 0, 1);
    if (op == 8) m32("cmn", 0, 1);
    if (op == 13) m32("cmp", 0, 1);
    return op2(rn, rm);
  }
  if (op == 0) m32("and", s, 1);
  else if (op == 1) m32("bic", s, 1);
  else if (op == 2) m32("orr", s, 1);
  else if (op == 4) { if (rd == 15 && s) return 0; m32("eor", s, 1); }
  else if (op == 8) m32("add", s, 1);
  else if (op == 13) m32("sub", s, 1);
  else return 0;
  return op3(rd, rn, rm);
}

int dec_branch(int h1, int h2) {
  int sb; int j1; int j2; int v; int c;
  sb = (h1 >> 10) & 1; j1 = (h2 >> 13) & 1; j2 = (h2 >> 11) & 1;
  if (h2 & 0x1000) {                                    // b.w, bl
    v = (h1 & 0x3FF) << 12 | (h2 & 0x7FF) << 1;
    if (!(j1 ^ sb)) v = v | 1 << 23;
    if (!(j2 ^ sb)) v = v | 1 << 22;
    if (sb) v = v - (1 << 24);
    if (icond >= 0 && !ilast) return 0;
    if (h2 & 0x4000) m32("bl", 0, 0);
    else { strcpy(mn, "b"); cat(mn, itc()); cat(mn, ".w"); }
    return branch(addr + 4 + v);
  }
  if (h2 & 0x4000) return 0;                            // blx
  c = (h1 >> 6) & 15;
  if (c >= 14) {                                        // miscellaneous: only nop.w
    if (h1 != 0xF3AF || h2 != 0x8000) return 0;
    m32("nop", 0, 1); ops[0] = 0;
    return 1;
  }
  if (icond >= 0) return 0;                             // as: b<cond>.w inside a block is b.w
  v = (h1 & 0x3F) << 12 | (h2 & 0x7FF) << 1 | j1 << 18 | j2 << 19;
  if (sb) v = v - (1 << 20);
  strcpy(mn, "b"); cat(mn, cname(c)); cat(mn, ".w");
  return branch(addr + 4 + v);
}

// single-precision FPU
int sreg(int v, int bit) { return v << 1 | bit; }
int dec_vfp(int h1, int h2) {
  int d; int n; int m; int a; int b; int rn; int v; int first; int cnt; char *nm;
  if (icond >= 0) return 0;                             // as: no FPU instructions in an IT block
  d = sreg((h2 >> 12) & 15, (h1 >> 6) & 1);
  n = sreg(h1 & 15, (h2 >> 7) & 1);
  m = sreg(h2 & 15, (h2 >> 5) & 1);
  a = h1 & ~0x4F; b = h2 & ~0xF0AF;
  nm = 0;
  if (a == 0xEE30 && b == 0x0A00) nm = "vadd.f32";
  if (a == 0xEE30 && b == 0x0A40) nm = "vsub.f32";
  if (a == 0xEE20 && b == 0x0A00) nm = "vmul.f32";
  if (a == 0xEE80 && b == 0x0A00) nm = "vdiv.f32";
  if (nm) {
    strcpy(mn, nm);
    sprintf(ops, "s%d, s%d, s%d", d, n, m);
    return 1;
  }
  a = h1 & ~0x40; b = h2 & ~0xF02F;
  if (a == 0xEEB1 && b == 0x0A40) nm = "vneg.f32";
  if (a == 0xEEB1 && b == 0x0AC0) nm = "vsqrt.f32";
  if (a == 0xEEB4 && b == 0x0A40) nm = "vcmp.f32";
  if (a == 0xEEB8 && b == 0x0AC0) nm = "vcvt.f32.s32";
  if (a == 0xEEB8 && b == 0x0A40) nm = "vcvt.f32.u32";
  if (a == 0xEEBD && b == 0x0AC0) nm = "vcvt.s32.f32";
  if (a == 0xEEBC && b == 0x0AC0) nm = "vcvt.u32.f32";
  if (nm) {
    strcpy(mn, nm);
    sprintf(ops, "s%d, s%d", d, m);
    return 1;
  }
  if (a == 0xEEB5 && (h2 & ~0xF000) == 0x0A40) {
    strcpy(mn, "vcmp.f32");
    sprintf(ops, "s%d, #0.0", d);
    return 1;
  }
  if ((h1 & 0xFFE0) == 0xEE00 && (h2 & 0x0F7F) == 0x0A10) {     // vmov sn, rt / rt, sn
    v = (h2 >> 12) & 15;
    if (v == 13 || v == 15) return 0;
    strcpy(mn, "vmov");
    if (h1 & 0x10) sprintf(ops, "%s, s%d", rname(v), n); else sprintf(ops, "s%d, %s", n, rname(v));
    return 1;
  }
  if (h1 == 0xEEF1 && h2 == 0xFA10) { strcpy(mn, "vmrs"); strcpy(ops, "APSR_nzcv, fpscr"); return 1; }
  if ((h1 & 0xFF20) == 0xED00 && (h2 & 0x0F00) == 0x0A00) {     // vldr vstr
    rn = h1 & 15; v = (h2 & 255) * 4;
    if (!(h1 & 0x10) && rn == 15) return 0;
    strcpy(mn, (h1 & 0x10) ? "vldr" : "vstr");
    if (v == 0 && (h1 & 0x80)) sprintf(ops, "s%d, [%s]", d, rname(rn));
    else if (h1 & 0x80) sprintf(ops, "s%d, [%s, #%d]", d, rname(rn), v);
    else sprintf(ops, "s%d, [%s, #-%d]", d, rname(rn), v);
    return 1;
  }
  if (((h1 & 0xFFBF) == 0xED2D || (h1 & 0xFFBF) == 0xECBD) && (h2 & 0x0F00) == 0x0A00) {   // vpush vpop
    first = d; cnt = h2 & 255;
    if (cnt == 0 || first + cnt > 32) return 0;
    strcpy(mn, (h1 & 0x100) ? "vpush" : "vpop");
    if (cnt == 1) sprintf(ops, "{s%d}", first);
    else sprintf(ops, "{s%d-s%d}", first, first + cnt - 1);
    return 1;
  }
  return 0;
}

int dec32(int h1, int h2) {
  int v;
  if ((h1 & 0xF800) == 0xF000 && (h2 & 0x8000)) return dec_branch(h1, h2);
  if ((h1 & 0xFA00) == 0xF000) return dec_dpimm(h1, h2);
  if ((h1 & 0xFA00) == 0xF200) {                        // addw subw movw movt
    v = ((h1 >> 10) & 1) << 11 | ((h2 >> 12) & 7) << 8 | (h2 & 255);
    switch ((h1 >> 4) & 31) {
    case 0: m32("addw", 0, 0); return op2imm((h2 >> 8) & 15, h1 & 15, v);
    case 10: m32("subw", 0, 0); return op2imm((h2 >> 8) & 15, h1 & 15, v);
    case 4: m32("movw", 0, 0); return opimm((h2 >> 8) & 15, (h1 & 15) << 12 | v);
    case 12: m32("movt", 0, 0); return opimm((h2 >> 8) & 15, (h1 & 15) << 12 | v);
    }
    return 0;
  }
  if ((h1 & 0xFE00) == 0xEA00) return dec_dpreg(h1, h2);
  if ((h1 & 0xFE00) == 0xF800) return dec_ldst(h1, h2);
  if ((h1 & 0xFF80) == 0xFA00 && (h2 & 0xF0F0) == 0xF000) {    // lsl lsr asr ror rd, rn, rm
    v = (h1 >> 5) & 3;
    if (v == 0) m32("lsl", (h1 >> 4) & 1, 1);
    if (v == 1) m32("lsr", (h1 >> 4) & 1, 1);
    if (v == 2) m32("asr", (h1 >> 4) & 1, 1);
    if (v == 3) m32("ror", (h1 >> 4) & 1, 1);
    return op3((h2 >> 8) & 15, h1 & 15, h2 & 15);
  }
  if ((h2 & 0xF0F0) == 0xF080 && (h1 == 0xFA0F || h1 == 0xFA1F || h1 == 0xFA4F || h1 == 0xFA5F)) {
    if (h1 == 0xFA0F) m32("sxth", 0, 1);
    if (h1 == 0xFA1F) m32("uxth", 0, 1);
    if (h1 == 0xFA4F) m32("sxtb", 0, 1);
    if (h1 == 0xFA5F) m32("uxtb", 0, 1);
    return op2((h2 >> 8) & 15, h2 & 15);
  }
  if ((h1 & 0xFFF0) == 0xFB00) {                        // mul mls
    if ((h2 & 0xF0F0) == 0xF000) { m32("mul", 0, 1); return op3((h2 >> 8) & 15, h1 & 15, h2 & 15); }
    if ((h2 & 0xF0) == 0x10) {
      m32("mls", 0, 0);
      sprintf(ops, "%s, %s, %s, %s", rname((h2 >> 8) & 15), rname(h1 & 15), rname(h2 & 15), rname((h2 >> 12) & 15));
      return 1;
    }
    return 0;
  }
  if (((h1 & 0xFFF0) == 0xFB90 || (h1 & 0xFFF0) == 0xFBB0) && (h2 & 0xF0F0) == 0xF0F0) {
    m32((h1 & 0x20) ? "udiv" : "sdiv", 0, 0);
    return op3((h2 >> 8) & 15, h1 & 15, h2 & 15);
  }
  if (h1 == 0xE92D && !(h2 & 0xA000) && nbits(h2) >= 2) {      // push.w
    m32("stmdb", 0, 0);
    strcpy(ops, "sp!, "); reglist(ops + 5, h2);
    return 1;
  }
  if (h1 == 0xE8BD && !(h2 & 0x2000) && (h2 & 0xC000) != 0xC000 && nbits(h2) >= 2) {   // pop.w
    m32("ldmia", 0, 1);
    strcpy(ops, "sp!, "); reglist(ops + 5, h2);
    return 1;
  }
  if ((h1 & 0xFC00) == 0xEC00) return dec_vfp(h1, h2);
  return 0;
}

// ------------------------------------------------------------------ the disassembly
struct t2o *dobj;  // the object being dumped, 0 for a program

int get16(char *b, int at) { return (b[at] & 255) | (b[at + 1] & 255) << 8; }

// the text symbol for a branch target: the nearest at or below it, a named one before a
// section symbol, a global before a local
int symfor(int t) {
  int i; int best; struct t2sym *s; struct t2sym *b; int better;
  best = -1;
  if (!dobj) return -1;
  for (i = 0; i < dobj->h.nsyms; i++) {
    s = dobj->syms + i;
    if (s->sect != SEC_TEXT || s->value > t) continue;
    if (best < 0) { best = i; continue; }
    b = dobj->syms + best;
    better = 0;
    if (s->value > b->value) better = 1;
    else if (s->value == b->value) {
      if (strcmp(t2o_name(dobj, best), ".text") == 0 && strcmp(t2o_name(dobj, i), ".text") != 0) better = 1;
      else if (!(b->flags & SYM_GLOBAL) && (s->flags & SYM_GLOBAL)) better = 1;
    }
    if (better) best = i;
  }
  return best;
}

// 1 if a relocation patches the instruction at text offset a (its target is not known)
int relocated(int a) {
  int i;
  if (!dobj) return 0;
  for (i = 0; i < dobj->h.nrels; i++) if (dobj->rels[i].sect == SEC_TEXT && dobj->rels[i].off == a) return 1;
  return 0;
}

int hexshort(int a, int h) {
  printf("%8x:\t%04x      \t.short\t0x%04x\n", a, h, h);
  return 0;
}

int disasm(char *c, int n) {
  int pc; int h1; int h2; int ok; int wide; int next; int s;
  itstate = 0; pc = 0;
  while (pc < n) {
    if (pc + 1 >= n) { printf("%8x:\t%02x        \t.byte\t0x%02x\n", pc, c[pc] & 255, c[pc] & 255); pc++; continue; }
    h1 = get16(c, pc);
    wide = (h1 >> 11) >= 0x1D;
    if (wide && pc + 3 >= n) wide = 0;
    h2 = wide ? get16(c, pc + 2) : 0;
    // the IT state for this instruction and the one after it, as in GNU's print_insn
    icond = -1; ilast = 0; next = 0;
    if (itstate) {
      icond = (itstate >> 4) & 15;
      ilast = (itstate & 15) == 8;
      if (!ilast) next = (itstate & 0xE0) | ((itstate & 15) << 1);
    }
    if (!wide && (h1 & 0xFF00) == 0xBF00 && (h1 & 15)) next = h1 & 255;
    addr = pc; hastgt = 0; mn[0] = 0; ops[0] = 0;
    if (icond >= 14) ok = 0;                            // as: no "it al"
    else if (wide) ok = dec32(h1, h2);
    else ok = dec16(h1);
    itstate = next;
    if (!ok) {
      hexshort(pc, h1);
      if (wide) hexshort(pc + 2, h2);
    } else {
      if (wide) printf("%8x:\t%04x %04x \t%s", pc, h1, h2, mn);
      else printf("%8x:\t%04x      \t%s", pc, h1, mn);
      if (ops[0]) printf("\t%s", ops);
      if (hastgt && !relocated(pc)) {
        s = symfor(target);
        if (s >= 0 && target - dobj->syms[s].value) printf(" <%s+0x%x>", t2o_name(dobj, s), target - dobj->syms[s].value);
        else if (s >= 0) printf(" <%s>", t2o_name(dobj, s));
      }
      printf("\n");
    }
    pc = pc + (wide ? 4 : 2);
  }
  return 0;
}

int hexdump(char *d, int n) {
  int i; int j;
  for (i = 0; i < n; i = i + 16) {
    printf("%08x ", i);
    for (j = i; j < n && j < i + 16; j++) printf(" %02x", d[j] & 255);
    printf("\n");
  }
  return 0;
}

// ------------------------------------------------------------------ objects, programs, archives
int symbols(struct t2o *o) {
  int *ord; int n; int i; int j; int k; char c; struct t2sym *s;
  n = o->h.nsyms;
  ord = malloc((n + 1) * sizeof(int));
  for (i = 0; i < n; i++) {                       // stable insertion sort by name, as nm
    j = i;
    while (j > 0 && strcmp(t2o_name(o, ord[j - 1]), t2o_name(o, i)) > 0) { ord[j] = ord[j - 1]; j--; }
    ord[j] = i;
  }
  for (k = 0; k < n; k++) {
    s = o->syms + ord[k];
    c = '?';
    if (s->sect == SEC_TEXT) c = 'T';
    if (s->sect == SEC_DATA) c = 'D';
    if (s->sect == SEC_ABS) c = 'A';
    if (s->sect == SEC_UNDEF) c = 'U';
    if (c != 'U' && c != '?' && !(s->flags & SYM_GLOBAL)) c = c + 32;
    if (s->sect == SEC_UNDEF) printf("         %c %s\n", c, t2o_name(o, ord[k]));
    else printf("%08x %c %s\n", s->value, c, t2o_name(o, ord[k]));
  }
  free(ord);
  return 0;
}

char *relname(int t) {
  switch (t) {
  case R_BL: return "R_BL"; case R_BW: return "R_BW"; case R_BCOND: return "R_BCOND";
  case R_DATA_ADDW: return "R_DATA_ADDW"; case R_DATA_MOVW: return "R_DATA_MOVW";
  case R_DATA_MOVT: return "R_DATA_MOVT"; case R_DATA_MOVW16: return "R_DATA_MOVW16";
  case R_FNADDR: return "R_FNADDR"; case R_WORD: return "R_WORD";
  }
  return 0;
}

int dump_obj(char *name, struct t2o *o) {
  int i; struct t2rel *r; char *t;
  printf("%s: T2DOS object, text %d bytes, data %d bytes, %d symbols, %d relocations\n",
         name, o->h.text_size, o->h.data_size, o->h.nsyms, o->h.nrels);
  printf("SYMBOLS:\n");
  symbols(o);
  printf("RELOCATIONS:\n");
  for (i = 0; i < o->h.nrels; i++) {
    r = o->rels + i;
    printf("%s %08x ", r->sect == SEC_TEXT ? "text" : "data", r->off);
    t = relname(r->type);
    if (t) printf("%s ", t); else printf("%d ", r->type);
    printf("%s%s%d\n", t2o_name(o, r->sym), r->addend < 0 ? "" : "+", r->addend);
  }
  printf("DISASSEMBLY:\n");
  dobj = o;
  disasm(o->text, o->h.text_size);
  printf("DATA:\n");
  hexdump(o->data, o->h.data_size);
  return 0;
}

int notobj(char *name) { printf("objdump: %s: not a T2DOS object\n", name); return 1; }

int dump_bin(char *path, char *buf, int size) {
  int csize; int dsize; int entry;
  if (size < 20) return notobj(path);
  csize = get16(buf, 4) | get16(buf, 6) << 16;
  dsize = get16(buf, 8) | get16(buf, 10) << 16;
  entry = get16(buf, 16) | get16(buf, 18) << 16;
  if (csize < 0 || dsize < 0 || csize > size - 20 || dsize > size - 20 - csize) return notobj(path);
  printf("%s: T2DOS program, code %d bytes, data %d bytes, entry 0x%04x\n", path, csize, dsize, entry);
  printf("DISASSEMBLY:\n");
  dobj = 0;
  disasm(buf + 20, csize);
  printf("DATA:\n");
  hexdump(buf + 20 + csize, dsize);
  return 0;
}

int dump_archive(char *path, char *buf, int size) {
  struct t2ar a; struct t2o o; int i; int rc; char *name; char *full;
  if (t2ar_read(buf, size, &a) < 0) { printf("objdump: %s: not a T2DOS archive\n", path); return 1; }
  printf("%s: T2DOS archive, %d members\n", path, a.h.nmembers);
  rc = 0;
  for (i = 0; i < a.h.nmembers; i++) {
    name = a.str + a.members[i].name;
    full = malloc(strlen(path) + strlen(name) + 3);
    strcpy(full, path); cat(full, "("); cat(full, name); cat(full, ")");
    if (t2ar_member(&a, i, &o) < 0) rc = notobj(full);
    else dump_obj(full, &o);
    free(full);
  }
  return rc;
}

int dump_file(char *path) {
  char *buf; int size; struct t2o o; int rc;
  buf = t2_readfile(path, &size);
  if (!buf) { printf("objdump: cannot read %s\n", path); return 1; }
  if (size >= 4 && memcmp(buf, "T2AR", 4) == 0) rc = dump_archive(path, buf, size);
  else if (size >= 4 && memcmp(buf, "T2CC", 4) == 0) rc = dump_bin(path, buf, size);
  else if (t2o_read(buf, size, &o) < 0) rc = notobj(path);
  else rc = dump_obj(path, &o);
  free(buf);
  return rc;
}

int main(int argc, char **argv) {
  int i; int rc;
  if (argc < 2) { printf("usage: objdump FILE...\n"); return 1; }
  rc = 0;
  for (i = 1; i < argc; i++) if (dump_file(argv[i])) rc = 1;
  return rc;
}
