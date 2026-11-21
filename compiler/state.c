// state.c - the compiler's globals, file reading and the error exits. See cc.h for
// what the files share.
#include "cc.h"

// ------------------------------------------------------------------ state
char *p;                 // source position
int tk, ival, line;
struct Sym *id;          // current identifier
struct Sym *syms; int nsyms;
int *hhead; int *hnext;  // hash chains: hhead[hash & 4095] is the newest symbol of a bucket, + 1 (0 none)
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
int declarator(int t); int die(char *msg); int expr_comma(); int itcc(int cond); int param_types(int *pl); int peep();

// ------------------------------------------------------------------ helpers
int cleanup() { int i; i = 0; while (i < ntmp) { unlink(tmpf[i]); i = i + 1; } ntmp = 0; return 0; }
int quit() { if (ofd >= 0) { close(ofd); unlink(outname); } cleanup(); exit(1); return 0; }
int die(char *msg) { printf("%s:%d: error: %s near token %d\n", srcname, line, msg, tk); quit(); return 0; }

char *readfile(char *name) {
  int fd; char *buf; int n; int total; char *alt; char *real; int cap; int want; int slot;
  struct stat_t st;
  real = name;
  fd = open(name, 0, 0);
  if (fd < 0 && incdir) {                   // -I dir
    alt = malloc(strlen(incdir) + strlen(name) + 2); strcpy(alt, incdir); strcpy(alt + strlen(alt), "/"); strcpy(alt + strlen(alt), name);
    fd = open(alt, 0, 0); real = alt;
  }
  if (fd < 0) {                             // includes also resolve against /lib
    alt = malloc(strlen(name) + 6); strcpy(alt, "/lib/"); strcpy(alt + 5, name);
    fd = open(alt, 0, 0); real = alt;
  }
  if (fd < 0) { printf("cannot open %s\n", name); quit(); }
  // the buffer fits the file (it grew 1 MB per file, sources and headers alike, which on
  // the board pushed big or multi-file compiles past the PSRAM floor /tmp allocates from)
  cap = 1048576;
#ifdef __T2CC__
  if (stat(real, &st) == 0) cap = st.size + 1;   // the builtin stat, not the PC build's glibc one
#endif
  buf = malloc(cap);
  slot = -1;
  if (nsrcbuf < 256) { srcbuf[nsrcbuf] = buf; slot = nsrcbuf; nsrcbuf = nsrcbuf + 1; }
  total = 0;
  while (1) {
    if (total + 1 >= cap) {                 // the file grew past stat's size: double
      alt = malloc(cap * 2); memcpy(alt, buf, total); free(buf);
      buf = alt; cap = cap * 2;
      if (slot >= 0) srcbuf[slot] = buf;    // srcbuf must keep the live pointer for free()
    }
    want = cap - total - 1;
    if (want > 65536) want = 65536;
    n = read(fd, buf + total, want);
    if (!(n > 0)) break;
    total = total + n;
  }
  buf[total] = 0;
  close(fd);
  return buf;
}
