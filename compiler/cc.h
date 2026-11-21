// cc.h - the T2DOS C compiler: what the compiler's own files share.
//
// Files: cc.c (driver: arguments, running as and ld, main), state.c (globals, reading
// files, quit/die), emit.c (the assembly output), opt.c (peephole -O), fp.c (floats in
// s registers), regs.c (locals in registers: promote/sprel/frameless), licm.c (loop-
// invariant code motion), lex.c (lexer, preprocessor, float literals), types.c
// (declarators, sizeof, constant expressions), expr.c (expressions), stmt.c
// (initializers, statements, top level).
//
// Written in the C subset cc compiles: no static (names are globally unique), and each
// file includes this header exactly once (cc has no include guards). Globals are defined
// in the file that owns them and extern here; every function has its prototype here.

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
enum { L_INS = 1, L_LAB, L_BR, L_DEL };         // assembly line kinds (emit.c owns the lists)

struct Sym { char *name; int len; int hash; int tk; int class; int type; int val;
             int hclass; int htype; int hval; int stag;
             int mdef; char *mbody; char **margs; int mnargs; int mvar;
             int *ptypes; int nptypes; };   // parameter types of a function, for argument conversion   // macro: defined, body, params (-1: object-like), ...
struct Mem { struct Sym *id; int type; int off; int next; int cnt; };   // cnt: array member length, else -1

// ------------------------------------------------------------------ globals (defined in the file that owns them)
extern char *p;
extern int tk, ival, line;
extern struct Sym *id;
extern struct Sym *syms;
extern int nsyms;
extern int *hhead;
extern int *hnext;
extern struct Mem *mems;
extern int nmems;
extern int *stsize;
extern int *stfirst;
extern int nstructs;
extern int *starr;
extern int *stcnt;
extern int *dims;
extern int ndims;
extern char *data;
extern int dpos;
extern int lastload;
extern int ty;
extern int loc;
extern int maxloc;
extern int ret_fix, brk_fix, cont_fix, cont_tgt;
extern int nbrk, ncont;
extern int nparams;
extern int fnrty;
extern int *fnret;
extern int nfn;
extern int **fnparams;
extern int *fnnp;
extern struct Sym *dname;
extern int darr;
extern int pb;
extern int pb_tk;
extern int pb_ival;
extern struct Sym *pb_id;
extern int pb_numty;
extern int sv_pb;
extern int sv_pb_tk;
extern int sv_pb_ival;
extern struct Sym *sv_pb_id;
extern int sv_pb_numty;
extern int sv_numty;
extern int sv_slen;
extern int numty;
extern int slen;
extern int ikind;
extern int ibase;
extern struct Sym *isym;
extern int init_open;
extern char *sv_p;
extern int sv_tk;
extern int sv_ival;
extern struct Sym *sv_id;
extern int sv_line;
extern int sv_dpos;
extern char **fstack_p;
extern int *fstack_line;
extern struct Sym **fstack_mac;
extern int fdepth;
extern char **sv_fp;
extern int *sv_fl;
extern struct Sym **sv_fm;
extern int sv_fdepth;
extern char *mbuf;
extern int ifdepth;
extern int pp_if;
extern char *srcname;
extern char *incdir;
extern int ofd;
extern char *outname;
extern char **srcbuf;
extern int nsrcbuf;
extern char **tmpf;
extern int ntmp;
extern char *lpool;
extern int lppos;
extern int *lkind;
extern int *ltext;
extern int *llab;
extern int *lcond;
extern int *lnext;
extern int nlines;
extern int nlabels;
extern int optim;
extern char *ccname;
extern char *ccbuf;
extern char *obuf;
extern int opos;
extern char *ibuf;
extern int ipos;
extern int tobuf;
extern char *numbuf;
extern struct Sym **gsym;
extern int *goff;
extern int nglob;
extern int dbase;
extern int dlong;
extern int *labline;
extern int *labref;
extern int pchg;
extern int oppw;
extern int oprd;
extern int opmask;
extern int opkind;
extern int opn;
extern char *opmn;
extern int *dvis;
extern int dstamp;
extern int swr;
extern int prepro;
extern int *slotk;
extern int *slotw;
extern int *pdep;
extern int npro;
extern int nfpro;
extern int *slotf;
extern int pvpush;
extern int pvpop;
extern int phold;
extern int rfoff;
extern int rfplain;
extern int spholdcvt;
extern int wstart[128], wend[128], nwin;
extern int spconv;
extern int has_calls;
extern char *licm_save;
extern int licm_savelen;
extern char licm_buf[600];
extern int outl[64];
extern int *livo;
extern struct Sym **tstack;
extern int tdepth;
extern int *bn;
extern int bnl;
extern char **dnames;
extern char **dvals;
extern int ndefs;
extern char *toolsdir;
extern char *tmppre;
extern int ntmpn;

// ------------------------------------------------------------------ prototypes
int cleanup();
int quit();
int die(char *msg);
char *readfile(char *name);
int oflush();
int oput(char *s);
char *dec(int v);
char *ccn(int c);
int oname(struct Sym *s);
int lnew(int kind);
int ls(char *s);
int lsym(struct Sym *s);
int lend();
int ins(char *s);
int ins_ri(char *mn, int rd, int v);
int ins_rri(char *mn, int rd, int rn, int v);
int addsp(int n);
int flush();
int push();
int pop1();
int imm(int rd, int v);
int imm2(int rd, int v);
int addi12(int rd, int rn, int v);
int subi12(int rd, int rn, int v);
int lea_loc(int off);
int ldref(struct Sym *s, int n);
int lea_data(struct Sym *s, int n, int off);
int lea_glo(int off);
int lea_var(struct Sym *s);
int lea_ext(struct Sym *s);
int fntype_p(int ret, int *pl, int np);
int fntype(int ret);
int fnlevels(int t);
int isfn(int t);
int isfndes(int t);
int fnret_of(int t);
int fnnp_of(int t);
int fnparam_of(int t, int i);
int load(int t);
int store(int t);
int load_r1(int t);
int store_r2(int t);
int conv(int t);
int arith(int a, int b);
int isuns(int a, int b);
int isflt(int t);
int cvt(int from, int to);
int assignconv(int t);
int castto(int t);
int fpair(int t);
int truth();
int test0();
int here();
int b_w(int target);
int bc_w(int cond);
int patch_b(int at, int label);
int patch_here(int at);
int chain_add(int head, int at);
int chain_here(int head);
int itcc(int cond);
int setcc(int cond);
int nx(int i);
int isins(int i, char *s);
int preins(int i, char *s);
int pnum(int i, char *s);
int pset(int i);
int pdel(int i);
int labat(int l);
int regword(char *t, int n);
int mnis(char *s);
int opinfo(int i);
int dead(int reg, int i, int depth);
int fdead(int i, int depth);
char *ldform(int i, int *sz);
char *stform(int i, int *sz);
int offok(int off, int sz);
int froff(int i);
int lsoff(char *mn, char *rd, int off);
int pxline(int i);
int to_r2(int i);
int peep_push(int a);
int redest(int i, char *rd);
int ren12(int i);
int peep_push2(int a);
int peep_tor1(int a);
int viar1(int i);
int peep_r1fr(int a);
int peep_idx(int a);
int peep_reload(int a);
int peep_mov(int a);
int peep_store(int a);
int peep_ite(int a);
int peep_br(int a);
int peep_ld(int a);
int pv(int i);
int sat(char *t);
int swrites(char *t);
int sscan(int i, int k);
int smen(int i, int k);
int sdead(int k, int i);
int sren(int i, int from, int to, int mode);
int vaddr(char *t);
int fimm8(int hi);
int lspad(int v, int w);
int lsfimm(int v);
int peep_fmov(int j);
int peep_vstr(int i);
int peep_dce(int i);
int peep_fpush(int a);
int peep_fp(int i);
int slotix(int off);
int markbad(int from, int to);
char *regnm(int k);
int ispro(int r);
int r7ref(int i);
int r7shift(int i, int off);
int sprel_ok();
int split(int i);
int spspill(char *t, int is_push);
int sprewrite(int i, int new);
int spmap(int k, int sz, int d);
int leasp(int i);
int leaform(int i);
int leaoff(int i, int sz, int d, int *o);
int leawrite(int i, int o);
int spshift();
int sp_syscall_save(int i);
int in_window(int i);
int spd(int i);
int fl_bl(int i);
int fl_argsetup(char *t);
int fl_resultsave(int i);
int strsave(int i);
int fl_argpush(int i);
int sprel(int fa, int sz);
int reglist(char *mn, char *last);
int inregs(char *t, char *nm);
int fl_pc_end(int i);
int fl_r7(int i);
int fl_list(int i, char *t, char *mn, char *last);
int fl_outside(int a, int b);
int fl_excused(char *t);
int fl_frame(int i, int fa);
int frameless(int fa, int sz);
int trimframe(int fa, int sz);
int iscall(int i);
int cl_reach(int s, int e);
int calllive(int slot);
int cl_between_call(int s, int e);
int promote();
int licm_mnem();
int licm_rtext(int j);
int licm_ok(int j);
int licm_to(int j, char *to, int nr);
int licm_loops();
int licm(int hl);
int hiok();
int renreg(int i, int rf, char *to, int skip);
int peep_hsrc(int a);
int peep_tohi(int a);
int peep_astore(int a);
int peep_gidx(int a);
int peep_icall(int a);
int peep();
int lookup(char *s, int len, int hash);
int idstart(int c);
int idchar(int c);
int intern(char *s, int n);
char *sdup(char *a, char *b);
char *trim(char *s);
int push_src(char *np, struct Sym *m);
int expanding(struct Sym *m);
int param_index(struct Sym *m, char *s, int n);
int mput(char *o, int c);
char *bput(char *b, char *o, int c);
char *bcat(char *b, char *o, char *s);
char *blit(char *b, char *o, char **ps);
char **collect_args(struct Sym *m, char **pq, int nl);
int tactive(struct Sym *m);
char *expand_text(char *s);
char *subst(struct Sym *m, char **args);
int expand(struct Sym *m);
int bn_muladd(int m, int a);
int bn_div10();
int bn_bit(int k);
int bn_top();
int bn_below(int k);
int bn_field(int at, int n);
int bn_float(int k, int sticky);
char *fl_parse(char *s);
int is_float_lit(char *s);
int int_to_float(int v, int uns);
int float_to_int(int b);
int lex();
int next();
char *line_body();
int pp_word(char *w);
int pp_skip_ws();
int skip_cond(int to_endif);
int pp_eval();
int define_macro(char *name, char *body);
int preprocess();
int newstruct();
int isrow(int t);
int rowelem(int t);
int rowcnt(int t);
int decay();
int tysize(int t);
int is_type();
int parse_type();
int find_mem(int t);
int skip_params();
int cunary();
int cprec(int op);
int cbin(int minp);
int cexpr();
int mkrow(int et, int n);
int parse_dims();
int rows_of(int t, int from);
int array_dim();
int param_types(int *pl);
int declarator(int t);
int call_fn(struct Sym *f, int n);
int fn_addr(struct Sym *f);
int call_ind(int t);
int call_sys(int num, int n);
int scale(int n);
int lvalue();
int scale1(int n);
int relop(int op, int t);
int fstep(int up);
int binop(int op, int t);
int expr(int lev);
int expr_comma();
int save_lex();
int restore_lex();
int init_addr(int off);
int init_code();
int init_close();
int poke(int off, int v, int t);
int const_init(int t);
int count_init();
int init_scalar(int t, int off);
int init_array(int et, int cnt, int off);
int init_struct(int t, int off);
int init_elem(int t, int off);
int init_zero(int n);
int newlocal(struct Sym *s, int t, int size);
int stmt();
int parse_global();
int write_data();
int compile(char *src, char *out);
int tmp_id();
int make_tmpdir();
int init_tools(char *argv0);
int run_tool(char *name, int argc, char **argv);
int endswith(char *s, char *suf);
char *newtmp(char *src, int n, char *ext);
char *outfile(char *path, char *ext);
int assemble(char *src, char *obj);
int exists(char *path);
int objdsize(char *obj);
char *findlib(char *name, char **ldirs, int nldir);
int main(int argc, char **argv);
