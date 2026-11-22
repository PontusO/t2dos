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
  p = "extern static"; i = Extern; while (i <= Static) { next(); id->tk = i; i = i + 1; }
  // builtin host functions: name, syscall number (see sys.h)
  p = "exit 0 exec 1 millis 2 sleep 3 sysinfo 4 spawn 5 yield 6 jobs 7 kill 8 reboot 9 putchar 16 getchar 17 kbhit 18 conctl 19 redirect 20 open 32 close 33 read 34 write 35 seek 36 stat 37 unlink 38 rename 39 mkdir 40 rmdir 41 opendir 42 readdir 43 closedir 44 chdir 45 getcwd 46 mkpipe 47 mount 48 mountat 49 malloc 64 free 65 memset 66 memcmp 67 memcpy 68 strlen 69 strcmp 70 strcpy 71 printf 72 sprintf 73 atoi 74 strncmp 75 strchr 76 net_join 80 net_status 81 net_ip 82 connect 83 send 84 recv 85 listen 86 accept 87 sock_avail 88 wifi_save 89 wifi_saved 90 ping 91 telnetd 92 gpio_mode 96 gpio_write 97 gpio_read 98 analog_read 99 pwm_write 100 i2c_write 101 i2c_read 102 spi_transfer 103 delay_us 104 time 105 localtime 106 tzset 107 sin 112 cos 113 tan 114 asin 115 acos 116 atan 117 atan2 118 sinh 119 cosh 120 tanh 121 exp 122 log 123 log10 124 pow 125 sqrt 126 fabs 127 floor 128 ceil 129 fmod 130";
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
// holds the src/ tree (src/compiler/cc -> build/host/), run by system().
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
  for (j = 0; j < 3; j++) { sl = strrchr(buf, '/'); if (sl) *sl = 0; else strcpy(buf, "."); }   // <root>/src/compiler/cc -> <root>
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
