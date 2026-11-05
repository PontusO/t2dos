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
  Long, Short, Signed, Unsigned, Float, Double,
  Assign, AddA, SubA, MulA, DivA, ModA, AndA, OrA, XorA, ShlA, ShrA, Cond, Lor, Lan, Or, Xor, And, Eq, Ne, Lt, Gt, Le, Ge, Shl, Shr,
  Add, Sub, Mul, Div, Mod, Inc, Dec, Dot, Arrow, Brak };

// symbol classes
enum { CNum = 1, CFun, CSys, CGlo, CLoc };   // 6 = global array, 8 = typedef
// base types; struct i is TSTRUCT+i; each pointer level adds PTR
// FN marks a function type; bits 22.. index fnret[] (its return type). A function
// designator has 0 pointer levels, a function pointer 1.
enum { TCHAR = 0, TINT = 1, TUCHAR = 2, TSHORT = 3, TUSHORT = 4, TUINT = 5, TFLOAT = 6, TSTRUCT = 7, PTR = 256, ARR = 1048576, FN = 2097152 };

#include "sys.h"

// ------------------------------------------------------------------ Thumb-2 encodings
// Instructions as the ARMv8-M manual writes them. A 32-bit instruction is two
// halfwords, I_X then I_X_2. Bases (marked +) get register or immediate fields
// ORed in by the helpers that use them. Plain numbers: older cc must build this.
enum {
  I_PUSH_R0 = 0xB401, I_POP_R1 = 0xBC02,                      // push {r0} / pop {r1}
  I_PUSH_R4 = 0xB410, I_POP_R4 = 0xBC10,                      // push {r4} / pop {r4}
  I_PUSH_R4_LR = 0xB510, I_PUSH_R7_LR = 0xB580, I_POP_R7_PC = 0xBD80,
  I_POP_W_R4_LR = 0xE8BD, I_POP_W_R4_LR_2 = 0x4010,           // pop.w {r4,lr}  (ldmia.w sp!)
  I_MOVW = 0xF240, I_MOVT = 0xF2C0,                           // + movw/movt rd,#imm16 (T3)
  I_ADDW = 0xF200, I_SUBW = 0xF2A0,                           // + addw/subw rd,rn,#imm12 (T4)
  I_ADDS_R0_R7_R1 = 0x18B8, I_SUBS_R0_R7_R1 = 0x1A78,
  I_ADD_W_R0_R9_R1 = 0xEB09, I_ADD_W_R0_R9_R1_2 = 0x0001,     // add.w r0,r9,r1
  I_LDR_R0_R0 = 0x6800, I_LDRB_R0_R0 = 0x7800, I_LDRH_R0_R0 = 0x8800,
  I_LDRSB_W_R0_R0 = 0xF990, I_LDRSB_W_R0_R0_2 = 0x0000,      // ldrsb.w r0,[r0]
  I_LDRSH_W_R0_R0 = 0xF9B0, I_LDRSH_W_R0_R0_2 = 0x0000,      // ldrsh.w r0,[r0]
  I_LDR_R0_R1 = 0x6808, I_LDRB_R0_R1 = 0x7808, I_LDRH_R0_R1 = 0x8808,
  I_LDRSB_W_R0_R1 = 0xF991, I_LDRSB_W_R0_R1_2 = 0x0000,      // ldrsb.w r0,[r1]
  I_LDRSH_W_R0_R1 = 0xF9B1, I_LDRSH_W_R0_R1_2 = 0x0000,      // ldrsh.w r0,[r1]
  I_STR_R0_R1 = 0x6008, I_STRB_R0_R1 = 0x7008, I_STRH_R0_R1 = 0x8008,
  I_STR_R2_R1 = 0x600A, I_STRB_R2_R1 = 0x700A, I_STRH_R2_R1 = 0x800A,
  I_STR_R1_R0 = 0x6001,                                       // str r1,[r0]
  I_SXTB_R0 = 0xB240, I_UXTB_R0 = 0xB2C0, I_SXTH_R0 = 0xB200, I_UXTH_R0 = 0xB280,
  I_B_W = 0xF000,                                             // + first half of b.w, bl and b<cond>.w
  I_B_W_2 = 0x9000, I_BL_2 = 0xD000, I_BCOND_2 = 0x8000,      // + second halves (T4 b.w, bl, T3 b<cond>.w)
  I_BCOND_KEEP = 0xFBC0,                                      // opcode and cond bits of a T3 first half
  I_CMP_R1_R0 = 0x4281, I_CMP_R0_R1 = 0x4288, I_CMP_R0_0 = 0x2800,
  I_IT = 0xBF00, I_ITE_EQ = 0xBF0C,                           // + it<cond>, firstcond << 4 | mask
  I_MOVS_R0 = 0x2000, I_MOVS_R1 = 0x2100,                     // + movs r0/r1,#imm8
  I_MOVS_R0_1 = 0x2001,                                       // movs r0,#1 (cc does not fold constants: keep it one number)
  I_ADD_SP = 0xB000,                                          // + add sp,#4*imm7
  I_LDR_R1_SP = 0x9900,                                       // + ldr r1,[sp,#4*imm8]
  I_ADD_R0_PC = 0x4478, I_BLX_R1 = 0x4788, I_BLX_R8 = 0x47C0,
  I_MOV_R1_R0 = 0x4601, I_MOV_R2_SP = 0x466A, I_MOV_R3_SP = 0x466B, I_MOV_R4_SP = 0x466C, I_MOV_R7_SP = 0x466F,
  I_MOV_SP_R3 = 0x469D, I_MOV_SP_R4 = 0x46A5, I_MOV_SP_R7 = 0x46BD,
  I_BIC_W_R3_R3_7 = 0xF023, I_BIC_W_R3_R3_7_2 = 0x0307,      // bic.w r3,r3,#7
  I_SUB_W_SP_SP_R1 = 0xEBAD, I_SUB_W_SP_SP_R1_2 = 0x0D01,    // sub.w sp,sp,r1
  I_LSLS_R0_R0_2 = 0x0080, I_LSLS_R1_R1_2 = 0x0089,           // lsls rd,rd,#2
  I_MULS_R0_R1 = 0x4348, I_MULS_R0_R2 = 0x4350, I_MULS_R1_R2 = 0x4351,
  I_ORRS_R0_R1 = 0x4308, I_EORS_R0_R1 = 0x4048, I_ANDS_R0_R1 = 0x4008,
  I_MVNS_R0 = 0x43C0, I_NEGS_R0 = 0x4240,
  I_ADDS_R0_R1_R0 = 0x1808, I_SUBS_R0_R1_R0 = 0x1A08, I_ADDS_R0_R0_R2 = 0x1880, I_ADDS_R2_R0_R2 = 0x1882,
  I_LSL_W_R0_R1_R0 = 0xFA01, I_LSL_W_R0_R1_R0_2 = 0xF000,
  I_LSR_W_R0_R1_R0 = 0xFA21, I_LSR_W_R0_R1_R0_2 = 0xF000,
  I_ASR_W_R0_R1_R0 = 0xFA41, I_ASR_W_R0_R1_R0_2 = 0xF000,
  I_SDIV_R0_R1_R0 = 0xFB91, I_SDIV_R0_R1_R0_2 = 0xF0F0,
  I_UDIV_R0_R1_R0 = 0xFBB1, I_UDIV_R0_R1_R0_2 = 0xF0F0,
  I_SDIV_R2_R1_R0 = 0xFB91, I_SDIV_R2_R1_R0_2 = 0xF2F0,
  I_UDIV_R2_R1_R0 = 0xFBB1, I_UDIV_R2_R1_R0_2 = 0xF2F0,
  I_SDIV_R0_R0_R2 = 0xFB90, I_SDIV_R0_R0_R2_2 = 0xF0F2,
  I_MLS_R0_R2_R0_R1 = 0xFB02, I_MLS_R0_R2_R0_R1_2 = 0x1010,  // mls r0,r2,r0,r1: r0 = r1 - r2 * r0
  // single precision FPU (floats live in r0 as bits; s0/s1 only for the operation)
  I_VMOV_S0_R0 = 0xEE00, I_VMOV_S0_R0_2 = 0x0A10, I_VMOV_S1_R0 = 0xEE00, I_VMOV_S1_R0_2 = 0x0A90,
  I_VMOV_S0_R1 = 0xEE00, I_VMOV_S0_R1_2 = 0x1A10, I_VMOV_S1_R2 = 0xEE00, I_VMOV_S1_R2_2 = 0x2A90,
  I_VMOV_R0_S0 = 0xEE10, I_VMOV_R0_S0_2 = 0x0A10, I_VMOV_R2_S0 = 0xEE10, I_VMOV_R2_S0_2 = 0x2A10,
  I_VADD = 0xEE30, I_VADD_2 = 0x0A20, I_VSUB = 0xEE30, I_VSUB_2 = 0x0A60,      // vadd/vsub.f32 s0,s0,s1
  I_VMUL = 0xEE20, I_VMUL_2 = 0x0A20, I_VDIV = 0xEE80, I_VDIV_2 = 0x0A20,      // vmul/vdiv.f32 s0,s0,s1
  I_VNEG = 0xEEB1, I_VNEG_2 = 0x0A40,                                          // vneg.f32 s0,s0
  I_VCMP = 0xEEB4, I_VCMP_2 = 0x0A60,                                          // vcmp.f32 s0,s1
  I_VMRS = 0xEEF1, I_VMRS_2 = 0xFA10,                                          // vmrs APSR_nzcv,fpscr
  I_VCVT_F_S_S0 = 0xEEB8, I_VCVT_F_S_S0_2 = 0x0AC0, I_VCVT_F_U_S0 = 0xEEB8, I_VCVT_F_U_S0_2 = 0x0A40,   // f32 <- s32/u32
  I_VCVT_F_S_S1 = 0xEEF8, I_VCVT_F_S_S1_2 = 0x0AE0, I_VCVT_F_U_S1 = 0xEEF8, I_VCVT_F_U_S1_2 = 0x0A60,
  I_VCVT_S_F_S0 = 0xEEBD, I_VCVT_S_F_S0_2 = 0x0AC0, I_VCVT_U_F_S0 = 0xEEBC, I_VCVT_U_F_S0_2 = 0x0AC0,   // s32/u32 <- f32, toward 0
  I_LSLS_R1_R0_1 = 0x0041                                                       // the float truth test: zero unless +-0
};
// condition codes (it, b<cond>)
enum { CC_EQ = 0, CC_NE = 1, CC_HS = 2, CC_LO = 3, CC_MI = 4, CC_HI = 8, CC_LS = 9, CC_GE = 10, CC_LT = 11, CC_GT = 12, CC_LE = 13 };

struct Sym { char *name; int len; int hash; int tk; int class; int type; int val;
             int hclass; int htype; int hval; int fix; int stag;
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
int **fnparams; int *fnnp; // and parameter types (fnnp -1: not known), for argument conversion
int *afix_at; struct Sym **afix_sym; int naddr;   // function addresses to patch at the end
struct Sym *dname; int darr;                       // set by declarator(): name, array size, -1 none, -2 for []
int pb; int pb_tk; int pb_ival; struct Sym *pb_id; int pb_numty;   // one token pushed back by next()
int sv_pb; int sv_pb_tk; int sv_pb_ival; struct Sym *sv_pb_id; int sv_pb_numty; int sv_numty; int sv_slen;
int numty;                                         // type of the last number token: TINT or TUINT
int slen;                                          // bytes of the last string literal, with its 0
int ikind; int ibase;                              // initializer target: 0 = data offset, 1 = frame offset
int init_open; int init_head; int init_tail;       // code chunks for global initializers, run before main
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

int preprocess(); int lex(); int expand(struct Sym *m); int expanding(struct Sym *m); int intern(char *s, int n); int idstart(int c); int idchar(int c);
int patch_b(int at, int target); int expr(int lev); int stmt(); int next();
int declarator(int t); int die(char *msg); int expr_comma(); int itcc(int cond); int param_types(int *pl);

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

int push() { return emit16(I_PUSH_R0); }
int pop1() { return emit16(I_POP_R1); }

// movw/movt rd, #imm32
int imm(int rd, int v) {
  int lo; int hi;
  lo = v & 65535; hi = (v >> 16) & 65535;
  emit32(I_MOVW | ((lo >> 11) & 1) << 10 | (lo >> 12), ((lo >> 8) & 7) << 12 | rd << 8 | (lo & 255));
  if (hi) emit32(I_MOVT | ((hi >> 11) & 1) << 10 | (hi >> 12), ((hi >> 8) & 7) << 12 | rd << 8 | (hi & 255));
  return 0;
}
// add.w/sub.w rd, rn, #imm12 (T4 encodings)
int addi12(int rd, int rn, int v) { return emit32(I_ADDW | ((v >> 11) & 1) << 10 | rn, ((v >> 8) & 7) << 12 | rd << 8 | (v & 255)); }
int subi12(int rd, int rn, int v) { return emit32(I_SUBW | ((v >> 11) & 1) << 10 | rn, ((v >> 8) & 7) << 12 | rd << 8 | (v & 255)); }

// r0 = address of local/param at signed frame offset off
int lea_loc(int off) {
  if (off < 0) { off = -off; if (off < 4096) subi12(0, 7, off); else { imm(1, off); emit16(I_SUBS_R0_R7_R1); } }
  else { if (off < 4096) addi12(0, 7, off); else { imm(1, off); emit16(I_ADDS_R0_R7_R1); } }
  return 0;
}
// r0 = r9 + off
int lea_glo(int off) {
  if (off < 4096) addi12(0, 9, off); else { imm(1, off); emit32(I_ADD_W_R0_R9_R1, I_ADD_W_R0_R9_R1_2); }
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
  if (isfndes(t)) return 0;                                  // a function is its own address
  if (t == TCHAR) { emit32(I_LDRSB_W_R0_R0, I_LDRSB_W_R0_R0_2); lastload = 4; }
  else if (t == TINT || t == TUINT || t == TFLOAT || t >= PTR) { emit16(I_LDR_R0_R0); lastload = 2; }
  else if (t == TUCHAR) { emit16(I_LDRB_R0_R0); lastload = 2; }
  else if (t == TSHORT) { emit32(I_LDRSH_W_R0_R0, I_LDRSH_W_R0_R0_2); lastload = 4; }
  else if (t == TUSHORT) { emit16(I_LDRH_R0_R0); lastload = 2; }
  // struct: keep address
  return 0;
}
// store *r1 = r0
int store(int t) {
  if (t == TCHAR || t == TUCHAR) emit16(I_STRB_R0_R1); else if (t == TSHORT || t == TUSHORT) emit16(I_STRH_R0_R1); else emit16(I_STR_R0_R1);
  return 0;
}
// r0 = *r1 according to type (for ++ and --)
int load_r1(int t) {
  if (t == TCHAR) emit32(I_LDRSB_W_R0_R1, I_LDRSB_W_R0_R1_2);
  else if (t == TUCHAR) emit16(I_LDRB_R0_R1);
  else if (t == TSHORT) emit32(I_LDRSH_W_R0_R1, I_LDRSH_W_R0_R1_2);
  else if (t == TUSHORT) emit16(I_LDRH_R0_R1);
  else emit16(I_LDR_R0_R1);
  return 0;
}
// *r1 = r2 according to type
int store_r2(int t) {
  if (t == TCHAR || t == TUCHAR) emit16(I_STRB_R2_R1); else if (t == TSHORT || t == TUSHORT) emit16(I_STRH_R2_R1); else emit16(I_STR_R2_R1);
  return 0;
}
// convert r0 to type t (casts, return values): sign or zero extend the small types
int conv(int t) {
  if (t == TCHAR) emit16(I_SXTB_R0);
  else if (t == TUCHAR) emit16(I_UXTB_R0);
  else if (t == TSHORT) emit16(I_SXTH_R0);
  else if (t == TUSHORT) emit16(I_UXTH_R0);
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
    emit32(I_VMOV_S0_R0, I_VMOV_S0_R0_2);
    if (from == TUINT) emit32(I_VCVT_F_U_S0, I_VCVT_F_U_S0_2); else emit32(I_VCVT_F_S_S0, I_VCVT_F_S_S0_2);
    emit32(I_VMOV_R0_S0, I_VMOV_R0_S0_2);
  }
  else if (isflt(from) && !isflt(to)) {
    emit32(I_VMOV_S0_R0, I_VMOV_S0_R0_2);
    if (to == TUINT) emit32(I_VCVT_U_F_S0, I_VCVT_U_F_S0_2); else emit32(I_VCVT_S_F_S0, I_VCVT_S_F_S0_2);
    emit32(I_VMOV_R0_S0, I_VMOV_R0_S0_2);
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
  emit32(I_VMOV_S1_R0, I_VMOV_S1_R0_2);
  if (!isflt(ty)) { if (ty == TUINT) emit32(I_VCVT_F_U_S1, I_VCVT_F_U_S1_2); else emit32(I_VCVT_F_S_S1, I_VCVT_F_S_S1_2); }
  emit32(I_VMOV_S0_R1, I_VMOV_S0_R1_2);
  if (!isflt(t)) { if (t == TUINT) emit32(I_VCVT_F_U_S0, I_VCVT_F_U_S0_2); else emit32(I_VCVT_F_S_S0, I_VCVT_F_S_S0_2); }
  return 0;
}
// set flags for "is r0 true": cmp r0,#0, or for a float lsls r1,r0,#1 (+0 and -0 are false)
// flags for "is r0 true", with r0 left 0 if it is false (a float -0.0 becomes 0): for && and ||
int truth() { if (isflt(ty)) { test0(); itcc(CC_NE); } emit16(I_CMP_R0_0); return 0; }
int test0() { if (isflt(ty)) emit16(I_LSLS_R1_R0_1); else emit16(I_CMP_R0_0); return 0; }

// branches, all 32-bit so they can be patched. returns offset of instruction.
int b_w(int target) {   // unconditional b.w to target (target<0: fixup later)
  int at; at = cpos; emit32(I_B_W, I_B_W_2); if (target >= 0) patch_b(at, target); return at;
}
int bl_w() { int at; at = cpos; emit32(I_B_W, I_BL_2); return at; }
int bc_w(int cond) { int at; at = cpos; emit32(I_B_W | cond << 6, I_BCOND_2); return at; }

int patch_b(int at, int target) {
  int off; int s; int j1; int j2; int h1; int h2; int i1; int i2;
  off = target - (at + 4);
  h1 = get16(at); h2 = get16(at + 2);
  if ((h2 & I_B_W_2) == I_B_W_2 || (h2 & I_BL_2) == I_BL_2) {   // T4 b.w / bl (24-bit)
    s = (off >> 24) & 1; i1 = (off >> 23) & 1; i2 = (off >> 22) & 1;
    j1 = (i1 ^ 1) ^ s; j2 = (i2 ^ 1) ^ s;
    put16(at, I_B_W | s << 10 | ((off >> 12) & 1023));
    put16(at + 2, (h2 & I_BL_2) | j1 << 13 | j2 << 11 | ((off >> 1) & 2047));
  } else {                                                    // T3 conditional (20-bit)
    s = (off >> 20) & 1; j2 = (off >> 19) & 1; j1 = (off >> 18) & 1;
    put16(at, (h1 & I_BCOND_KEEP) | s << 10 | ((off >> 12) & 63));
    put16(at + 2, I_BCOND_2 | j1 << 13 | j2 << 11 | ((off >> 1) & 2047));
  }
  return 0;
}
// fixup chains: an unpatched branch slot holds the previous chain entry (or -1)
int chain_add(int head, int at) { put16(at + 2, head & 65535); put16(at, (head >> 16) & 65535); return at; }
int chain_patch(int head, int target, int kind) {   // kind: I_B_W_2 = b.w, I_BL_2 = bl
  int nxt;
  while (head >= 0) { nxt = (get16(head) << 16) | get16(head + 2); put16(head + 2, kind); patch_b(head, target); head = nxt; }
  return 0;
}
// movw+movt, always both halves (fixed size, patchable)
int imm2(int rd, int v) {
  int lo; int hi;
  lo = v & 65535; hi = (v >> 16) & 65535;
  emit32(I_MOVW | ((lo >> 11) & 1) << 10 | (lo >> 12), ((lo >> 8) & 7) << 12 | rd << 8 | (lo & 255));
  emit32(I_MOVT | ((hi >> 11) & 1) << 10 | (hi >> 12), ((hi >> 8) & 7) << 12 | rd << 8 | (hi & 255));
  return 0;
}

// compare r1 ? r0 -> r0 = 0/1 using IT block
int itcc(int cond) {                     // r0 = cond ? 1 : 0, flags already set
  emit16(I_IT | cond << 4 | ((cond & 1) ^ 1) << 3 | 4);       // ite cond
  emit16(I_MOVS_R0_1); emit16(I_MOVS_R0);                   // movs r0,#1 ; movs r0,#0
  return 0;
}
int setcc(int cond) {
  emit16(I_CMP_R1_R0);
  emit16(I_IT | cond << 4 | ((cond & 1) ^ 1) << 3 | 4);       // ite cond
  emit16(I_MOVS_R0_1); emit16(I_MOVS_R0);                   // movs r0,#1 ; movs r0,#0
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
    syms[i].class = 0; syms[i].fix = -1;
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
    p = p + 5; printf("%s:%d: #error %s\n", srcname, line, line_body()); exit(1);
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
  int at;
  at = bl_w();
  if (f->class == CFun && f->val >= 0) patch_b(at, f->val);
  else { f->fix = chain_add(f->fix, at); if (f->class == 0) { f->class = CFun; f->val = -1; f->type = TINT; } }
  if (n) emit16(I_ADD_SP | n);                  // pop the args
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
  emit16(I_ADD_R0_PC);
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
  emit16(I_LDR_R1_SP | n);                      // the address, under the n args
  emit16(I_BLX_R1);
  emit16(I_ADD_SP | (n + 1));                   // args and the address
  ty = fnret_of(t);
  return 0;
}
// Emit a host syscall: r0 = num, r1 = nargs, r2 = argv (argv[n-1] is arg 0)
int call_sys(int num, int n) {
  emit16(I_MOV_R2_SP);                          // r2 = argv
  emit16(I_PUSH_R4);
  emit16(I_MOV_R4_SP);
  emit16(I_MOV_R3_SP);
  emit32(I_BIC_W_R3_R3_7, I_BIC_W_R3_R3_7_2);   // 8-byte aligned sp for the kernel
  emit16(I_MOV_SP_R3);
  emit16(I_MOVS_R1 | n);
  emit16(I_MOVS_R0 | num);
  emit16(I_BLX_R8);
  emit16(I_MOV_SP_R4);
  emit16(I_POP_R4);
  if (n) emit16(I_ADD_SP | n);                  // pop the args
  return 0;
}

// scale r0 by n (for pointer arithmetic)
int scale(int n) {
  if (n == 1) return 0;
  if (n == 4) return emit16(I_LSLS_R0_R0_2);
  imm(2, n); return emit16(I_MULS_R0_R2);
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
  if (n == 4) return emit16(I_LSLS_R1_R1_2);
  imm(2, n); return emit16(I_MULS_R1_R2);
}

// r0 = r1 op r0 where r1 has type t and r0 has type ty; sets ty to the result type
// r0 = r1 op r0 for == != < > <= >= (left type t, right type ty)
int relop(int op, int t) {
  if ((isflt(t) || isflt(ty)) && t < PTR && ty < PTR) {          // vcmp; with a NaN only != is true
    fpair(t); emit32(I_VCMP, I_VCMP_2); emit32(I_VMRS, I_VMRS_2);
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
  emit32(I_VMOV_S0_R0, I_VMOV_S0_R0_2); imm(2, 1065353216); emit32(I_VMOV_S1_R2, I_VMOV_S1_R2_2);   // 1.0f
  if (up) emit32(I_VADD, I_VADD_2); else emit32(I_VSUB, I_VSUB_2);
  return 0;
}
int binop(int op, int t) {
  int u;
  if ((isflt(t) || isflt(ty)) && t < PTR && ty < PTR) {          // float arithmetic
    fpair(t);
    if (op == Add) emit32(I_VADD, I_VADD_2); else if (op == Sub) emit32(I_VSUB, I_VSUB_2);
    else if (op == Mul) emit32(I_VMUL, I_VMUL_2); else if (op == Div) emit32(I_VDIV, I_VDIV_2);
    else die("invalid operator for float");
    emit32(I_VMOV_R0_S0, I_VMOV_R0_S0_2); ty = TFLOAT;
    return 0;
  }
  u = isuns(t, ty);
  if (op == Or)  { emit16(I_ORRS_R0_R1); ty = arith(t, ty); }
  else if (op == Xor) { emit16(I_EORS_R0_R1); ty = arith(t, ty); }
  else if (op == And) { emit16(I_ANDS_R0_R1); ty = arith(t, ty); }
  else if (op == Shl) { emit32(I_LSL_W_R0_R1_R0, I_LSL_W_R0_R1_R0_2); ty = arith(t, t); }
  else if (op == Shr) {                                            // the left operand decides
    if (t == TUINT) emit32(I_LSR_W_R0_R1_R0, I_LSR_W_R0_R1_R0_2); else emit32(I_ASR_W_R0_R1_R0, I_ASR_W_R0_R1_R0_2);
    ty = arith(t, t);
  }
  else if (op == Add) {
    if (t >= PTR && ty < PTR) { scale(tysize(t - PTR)); ty = t; }
    else if (ty >= PTR && t < PTR) scale1(tysize(ty - PTR));
    else if (t < PTR && ty < PTR) ty = arith(t, ty);
    emit16(I_ADDS_R0_R1_R0);
  }
  else if (op == Sub) {
    if (t >= PTR && ty >= PTR) { emit16(I_SUBS_R0_R1_R0); imm(2, tysize(t - PTR)); emit32(I_SDIV_R0_R0_R2, I_SDIV_R0_R0_R2_2); ty = TINT; }   // pointer difference
    else { if (t >= PTR) scale(tysize(t - PTR)); emit16(I_SUBS_R0_R1_R0); ty = t < PTR ? arith(t, ty) : t; }
  }
  else if (op == Mul) { emit16(I_MULS_R0_R1); ty = arith(t, ty); }
  else if (op == Div) { if (u) emit32(I_UDIV_R0_R1_R0, I_UDIV_R0_R1_R0_2); else emit32(I_SDIV_R0_R1_R0, I_SDIV_R0_R1_R0_2); ty = arith(t, ty); }
  else if (op == Mod) {                                            // r1 - (r1 / r0) * r0
    if (u) emit32(I_UDIV_R2_R1_R0, I_UDIV_R2_R1_R0_2); else emit32(I_SDIV_R2_R1_R0, I_SDIV_R2_R1_R0_2);
    emit32(I_MLS_R0_R2_R0_R1, I_MLS_R0_R2_R0_R1_2); ty = arith(t, ty);
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
    else if (d->class == 6) { lea_glo(d->val); ty = d->type; }   // global array: address
    else if (d->class == CFun || d->class == CSys) { fn_addr(d); ty = fntype_p(d->type, d->ptypes, d->ptypes ? d->nptypes : -1); }   // function as a value
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
  else if (tk == '!') { next(); expr(Inc); test0(); emit16(I_ITE_EQ); emit16(I_MOVS_R0_1); emit16(I_MOVS_R0); ty = TINT; }
  else if (tk == '~') { next(); expr(Inc); if (isflt(ty)) die("~ on a float"); emit16(I_MVNS_R0); ty = arith(ty, ty); }
  else if (tk == Add) { next(); expr(Inc); ty = arith(ty, ty); }
  else if (tk == Sub) {
    next();
    if (tk == Num) { imm(0, numty == TFLOAT ? ival ^ (1 << 31) : -ival); ty = numty; next(); }   // a float flips its sign bit
    else {
      expr(Inc);
      if (isflt(ty)) { emit32(I_VMOV_S0_R0, I_VMOV_S0_R0_2); emit32(I_VNEG, I_VNEG_2); emit32(I_VMOV_R0_S0, I_VMOV_R0_S0_2); }
      else { emit16(I_NEGS_R0); ty = arith(ty, ty); }
    }
  }
  else if (tk == Inc || tk == Dec) {            // prefix
    t = tk; next(); expr(Inc); lvalue();
    if (isflt(ty)) { emit16(I_MOV_R1_R0); load_r1(ty); fstep(t == Inc); emit32(I_VMOV_R0_S0, I_VMOV_R0_S0_2); store(ty); }
    else {
      sz = ty >= PTR ? tysize(ty - PTR) : 1;
      emit16(I_MOV_R1_R0);
      load_r1(ty);
      if (t == Dec) sz = -sz;
      if (sz >= 0 && sz < 4096) addi12(0, 0, sz); else if (sz < 0 && -sz < 4096) subi12(0, 0, -sz); else { imm(2, sz); emit16(I_ADDS_R0_R0_R2); }
      store(ty);
    }
  }
  else die("bad expression");

  // binary operators, precedence climbing
  while (tk >= lev || (tk == '(' && isfn(ty))) {   // a call binds tightest, like [ and ->
    t = ty;
    if (tk == '(') call_ind(t);
    else if (tk == Assign) {
      next(); lvalue(); push(); expr(Assign); assignconv(t); pop1(); store(t); ty = t;
    }
    else if (tk == Cond) {
      next(); test0(); at = bc_w(CC_EQ);
      expr_comma();
      if (tk != ':') die(": expected"); next();
      t = ty; at2 = b_w(-1); patch_b(at, cpos);
      expr(Cond);
      if ((isflt(t) || isflt(ty)) && t < PTR && ty < PTR && t != ty) {   // the int branch becomes float
        if (!isflt(ty)) cvt(ty, TFLOAT);
        if (!isflt(t)) { m = b_w(-1); patch_b(at2, cpos); cvt(t, TFLOAT); at2 = m; }
        ty = TFLOAT;
      }
      patch_b(at2, cpos);
    }
    else if (tk == Lor) { next(); truth(); at = bc_w(CC_NE); expr(Lan); truth(); patch_b(at, cpos); itcc(CC_NE); ty = TINT; }
    else if (tk == Lan) { next(); truth(); at = bc_w(CC_EQ); expr(Or); truth(); patch_b(at, cpos); itcc(CC_NE); ty = TINT; }
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
      lvalue(); emit16(I_MOV_R1_R0); load_r1(ty); fstep(tk == Inc); emit32(I_VMOV_R2_S0, I_VMOV_R2_S0_2); store_r2(ty); next();
    }
    else if (tk == Inc || tk == Dec) {          // postfix
      lvalue();
      sz = ty >= PTR ? tysize(ty - PTR) : 1;
      if (tk == Dec) sz = -sz;
      emit16(I_MOV_R1_R0);
      load_r1(ty);
      if (sz >= 0 && sz < 4096) addi12(2, 0, sz); else if (sz < 0 && -sz < 4096) subi12(2, 0, -sz); else { imm(2, sz); emit16(I_ADDS_R2_R0_R2); }
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
      next(); push(); expr_comma(); pop1();
      if (tk != ']') die("] expected"); next();
      if (t < PTR) die("indexing a non-pointer");
      scale(tysize(t - PTR));
      emit16(I_ADDS_R0_R1_R0);
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
// pointers, addresses, function pointers, expressions) becomes code in a chunk
// that runs before main: chunks are chained with b.w, see main(). Locals: code at
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
    if (tk == Else) { next(); at2 = b_w(-1); patch_b(at, cpos); stmt(); patch_b(at2, cpos); }
    else patch_b(at, cpos);
  }
  else if (tk == While) {
    next(); if (tk != '(') die("( expected"); next();
    sv_brk = brk_fix; sv_cont = cont_fix; sv_tgt = cont_tgt;
    brk_fix = -1; cont_fix = -1; cont_tgt = cpos;
    expr_comma(); if (tk != ')') die(") expected"); next();
    test0(); at = bc_w(CC_EQ);
    stmt();
    b_w(cont_tgt); patch_b(at, cpos);
    chain_patch(brk_fix, cpos, I_B_W_2);
    brk_fix = sv_brk; cont_fix = sv_cont; cont_tgt = sv_tgt;
  }
  else if (tk == For) {
    next(); if (tk != '(') die("( expected"); next();
    if (tk != ';') expr_comma();
    if (tk != ';') die("; expected"); next();
    sv_brk = brk_fix; sv_cont = cont_fix; sv_tgt = cont_tgt;
    brk_fix = -1; cont_fix = -1;
    at = cpos;                                  // condition
    if (tk != ';') { expr_comma(); test0(); at2 = bc_w(CC_EQ); } else at2 = -1;
    if (tk != ';') die("; expected"); next();
    body = b_w(-1);                             // skip increment first time
    cont_tgt = cpos;                            // increment
    if (tk != ')') expr_comma();
    if (tk != ')') die(") expected"); next();
    b_w(at);
    patch_b(body, cpos);
    stmt();
    b_w(cont_tgt);
    if (at2 >= 0) patch_b(at2, cpos);
    chain_patch(brk_fix, cpos, I_B_W_2);
    brk_fix = sv_brk; cont_fix = sv_cont; cont_tgt = sv_tgt;
  }
  else if (tk == Switch) {
    next(); if (tk != '(') die("( expected"); next();
    expr_comma(); if (tk != ')') die(") expected"); next();
    if (tk != '{') die("{ expected"); next();
    sv_brk = brk_fix; brk_fix = -1;
    loc = loc + 4; if (loc > maxloc) maxloc = loc; n = -loc;        // temp slot for the switch value
    push(); lea_loc(n); pop1(); emit16(I_STR_R1_R0);
    at = b_w(-1);                                                 // -> first test (case chain head)
    at2 = -1;                                                     // default position
    body = -1;                                                    // fallthrough skip
    while (tk != '}') {
      if (tk == Case) {
        next();
        t = cexpr();
        if (tk != ':') die(": expected"); next();
        body = b_w(-1);                                           // previous body falls through over the test
        patch_b(at, cpos);                                        // failed previous test lands here
        lea_loc(n); emit16(I_LDR_R0_R0); imm(1, t); emit16(I_CMP_R0_R1);
        at = bc_w(CC_NE);                                         // -> next test
        patch_b(body, cpos);
      }
      else if (tk == Default) { next(); if (tk != ':') die(": expected"); next(); at2 = cpos; }
      else { if (!tk) die("unexpected eof"); stmt(); }
    }
    next();
    if (at2 >= 0) patch_b(at, at2); else patch_b(at, cpos);
    chain_patch(brk_fix, cpos, I_B_W_2);
    brk_fix = sv_brk;
  }
  else if (tk == Break) { next(); if (tk != ';') die("; expected"); next(); brk_fix = chain_add(brk_fix, b_w(-1)); }
  else if (tk == Continue) { next(); if (tk != ';') die("; expected"); next(); b_w(cont_tgt); }
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
        lea_loc(d->val); addi12(1, 0, 4); emit16(I_STR_R1_R0);   // slot = slot + 4
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
  int bt; int t; struct Sym *s; int n; int i; int frame_at; int sz; int hi;
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
      s->class = CFun; s->val = cpos; nparams = n; fnrty = t;
      chain_patch(s->fix, cpos, I_BL_2); s->fix = -1;
      emit16(I_PUSH_R7_LR);
      emit16(I_MOV_R7_SP);
      frame_at = cpos; imm2(1, 0);              // movw+movt r1,#frame (patched)
      emit32(I_SUB_W_SP_SP_R1, I_SUB_W_SP_SP_R1_2);
      ret_fix = -1;
      stmt();
      chain_patch(ret_fix, cpos, I_B_W_2);
      emit16(I_MOV_SP_R7);
      emit16(I_POP_R7_PC);
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
  char *out; int fd; int i; int n; int hdr; int entry; int ndefs; char *dn; char *dv; struct Sym *s; struct Sym *d;
  out = "a.bin"; srcname = 0; incdir = 0; ndefs = 0;
  i = 1;
  while (i < argc) {
    if (!strcmp(argv[i], "-o")) { out = argv[i + 1]; i = i + 2; }
    else if (!strcmp(argv[i], "-I")) { incdir = argv[i + 1]; i = i + 2; }
    else if (!strcmp(argv[i], "-D")) { ndefs = ndefs + 1; i = i + 2; }   // defined below, once syms exist
    else { srcname = argv[i]; i = i + 1; }
  }
  if (!srcname) { printf("usage: cc [-o out.bin] [-I incdir] [-D name[=value]] file.c\n"); return 1; }

  syms = malloc(8192 * sizeof(struct Sym)); nsyms = 0;
  mems = malloc(2048 * sizeof(struct Mem)); nmems = 0;
  stsize = malloc(256 * 4); stfirst = malloc(256 * 4); nstructs = 0;
  starr = malloc(256 * 4); stcnt = malloc(256 * 4); dims = malloc(8 * 4);
  code = malloc(1048576); cpos = 0;
  data = malloc(1048576); dpos = 0;
  fstack_p = malloc(64 * 4); fstack_line = malloc(64 * 4); fstack_mac = malloc(64 * 4); fdepth = 0;
  sv_fp = malloc(64 * 4); sv_fl = malloc(64 * 4); sv_fm = malloc(64 * 4); mbuf = malloc(65536);
  tstack = malloc(64 * 4); tdepth = 0; bn = malloc(256 * 4); bnl = 1;
  fnret = malloc(512 * 4); fnparams = malloc(512 * 4); fnnp = malloc(512 * 4); nfn = 0;
  init_open = 0; init_head = -1; init_tail = -1;
  afix_at = malloc(4096 * 4); afix_sym = malloc(4096 * 4); naddr = 0;
  memset(syms, 0, 8192 * sizeof(struct Sym));

  // keywords, in token enum order
  p = "break case char continue default else enum for if int return sizeof struct switch typedef void while long short signed unsigned float double";
  i = Break; while (i <= Double) { next(); id->tk = i; i = i + 1; }
  // builtin host functions: name, syscall number (see sys.h)
  p = "exit 0 exec 1 millis 2 sleep 3 sysinfo 4 putchar 16 getchar 17 kbhit 18 conctl 19 redirect 20 open 32 close 33 read 34 write 35 seek 36 stat 37 unlink 38 rename 39 mkdir 40 rmdir 41 opendir 42 readdir 43 closedir 44 chdir 45 getcwd 46 malloc 64 free 65 memset 66 memcmp 67 memcpy 68 strlen 69 strcmp 70 strcpy 71 printf 72 sprintf 73 atoi 74 strncmp 75 strchr 76 net_join 80 net_status 81 net_ip 82 connect 83 send 84 recv 85 listen 86 accept 87 sock_avail 88 gpio_mode 96 gpio_write 97 gpio_read 98 analog_read 99 pwm_write 100 i2c_write 101 i2c_read 102 spi_transfer 103 delay_us 104";
  next();
  while (tk == Id) {
    s = id; next(); s->class = CSys; s->val = ival; s->type = TINT;
    if (ival == SYS_malloc || ival == SYS_strcpy || ival == SYS_strchr || ival == SYS_getcwd || ival == SYS_net_ip) s->type = TCHAR + PTR;
    next();
  }
  p = "main"; next(); s = id;
  // predefined macro and -D NAME[=value]
  define_macro("__T2CC__", "1");
  i = 1;
  while (i < argc) {
    if (!strcmp(argv[i], "-D") && i + 1 < argc) {
      dn = argv[i + 1]; dv = strchr(dn, '=');
      if (dv) { *dv = 0; define_macro(dn, dv + 1); } else define_macro(dn, "1");
    }
    i = i + 1;
  }

  p = readfile(srcname); line = 1;
  next();
  while (tk) parse_global();
  if (ifdepth) die("#endif missing");

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
    emit16(I_PUSH_R4_LR);
    b_w(init_head);
    patch_b(init_tail, cpos);
    emit32(I_POP_W_R4_LR, I_POP_W_R4_LR_2);
    b_w(s->val);
  }
  write(fd, &cpos, 4); write(fd, &dpos, 4); write(fd, &hdr, 4); write(fd, &entry, 4);
  write(fd, code, cpos); write(fd, data, dpos);
  close(fd);
  printf("%s: %d bytes code, %d bytes data, entry %d\n", out, cpos, dpos, entry);
  return 0;
}
