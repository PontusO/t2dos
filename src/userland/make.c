// make.c - the board tree's build executor: reads a small target-line build
// file and runs one cc call per .bin target (as -o for .s prereqs), always
// rebuilding: no timestamps (header prereqs are recorded for the day it grows
// them). Grammar and design: the spec doc 2026-11-21-make-utility-design.md.
// Tests: tools/check_make.py (--pc plans the tree with -n, --board runs the
// real build and byte-compares). Like the other userland tools it calls the
// kernel builtins by name (open/read/close, printf, strings, exec, exit).
//
// The programs it runs are fixed: /bin/cc.bin and /bin/as.bin, no PATH search.
// A target name IS its output path: a prereq is a target iff a line's target
// matches it exactly, otherwise it is a file.
//
// Dialect discipline (docs/NOTES-star-deref.md): the committed cc
// miscompiles rvalue reads through char * VARIABLES (a VARREF emits a second
// load), so no string is ever held in a char * variable here. Strings are
// int offsets into buf, or opool rows, or literal/argv addresses passed
// straight to a builtin; comparisons run over buf through int-indexed helpers
// below. cmd[] slots (the sh.c sargv idiom: a stored address passed straight
// to exec/printf) are the one pointer-valued store this program makes.
//
// usage: make [-n] [-f file] [target ...]   (default: the file's first target)

#define MAXLINE 128
#define MAXTARGETS 64
#define MAXPRE 16
#define MAXBUF 16384
#define MAXARGS 32

char buf[MAXBUF];                      // the whole build file (strings are offsets into it)
char namstr[MAXTARGETS * MAXLINE];     // target names, copied out (row k: k * MAXLINE)
char opool[8 * MAXLINE];               // .s -> .o output names (8 .s per target)
int tflag[MAXTARGETS * MAXARGS];       // build flag words: offsets of tokens in buf
int tpre[MAXTARGETS * MAXPRE];         // prerequisites: offsets of tokens in buf
int tvis[MAXTARGETS];                  // counts live in tpn/tfn scratch below
int tfn[MAXTARGETS], tpn[MAXTARGETS];
int ntargets;
int dry;                               // -n: print the plan, run nothing
int glen;                              // strlen cache for the current resolve

char *curfile;                         // -f argument, a main argv slot: for error messages
char tpk[MAXTARGETS * MAXPRE];      // prereq kind: 0 other/header, 1 .c/.o, 2 .s
int tplen[MAXTARGETS * MAXPRE];       // prereq string length, from the parse
char *cmd[MAXARGS];                    // the command being built / run (sh.c's sargv idiom)
char *curowner;                        // cmd[0] twin, for cap errors
int nw;

// ---- string helpers over buf, by offset (the safe idiom: int indexing only)

int xlen(int off) {                    // strlen(buf + off)
  int n;
  n = 0;
  while (buf[off + n]) n = n + 1;
  return n;
}

int xcmp(int a, int b) {               // strcmp(buf + a, buf + b)
  int i;
  i = 0;
  while (buf[a + i]) { if (buf[a + i] != buf[b + i]) return buf[a + i] - buf[b + i]; i = i + 1; }
  if (buf[b + i]) return -buf[b + i];
  return 0;
}

// buf[off..off+ln) vs literal bytes lit[0..ln): never index a char * param
// (the VARREF double-load family), only globals by int offset; literal
// lengths come from the kernel builtin, the proven literal pattern
int xlit(int off, int ln, char *lit) {
  int i;
  i = 0;
  while (i < ln) { if (buf[off + i] != lit[i]) return 0; i = i + 1; }
  return 1;
}

int endsint(int off, int ls, char *suf, int lf) {  // buf[off..] (length ls) ends with suf (length lf)?
  if (ls < lf) return 0;
  return xlit(off + ls - lf, lf, suf);
}

int endsnam(int k, int ls, char *suf, int lf) {    // same, for namstr row k
  int a, i;
  a = k * MAXLINE + ls - lf;
  if (ls < lf) return 0;
  i = 0;
  while (i < lf) { if (namstr[a + i] != suf[i]) return 0; i = i + 1; }
  return 1;
}

// target whose name equals buf[off..off+len), -1 none
int find_target(int off, int len) {
  int i, nl;
  i = 0;
  while (i < ntargets) {
    nl = xlen(i * MAXLINE);            // names live in namstr; xlen over buf is wrong there
    if (nl == len && xcmpnam(i, off) == 0) return i;
    i = i + 1;
  }
  return -1;
}

int xcmpnam(int k, int off) {          // strcmp(namstr row k, buf + off)
  int a, i;
  a = k * MAXLINE;
  i = 0;
  while (namstr[a + i]) { if (namstr[a + i] != buf[off + i]) return namstr[a + i] - buf[off + i]; i = i + 1; }
  return namstr[a + i] - buf[off + i];
}

int exists(char *path) {
  int fd;
  fd = open(path, 0);                  // OPEN_READ: opening it is the existence test
  if (fd < 0) return 0;
  close(fd);
  return 1;
}

// ---- tokenizer: the line is [gstart, gend) in buf, gpos walks it

int gpos, gend;

int ntok(void) {                       // next token offset, -1 at the end; NULs separators
  int s;
  while (gpos < gend && (buf[gpos] == ' ' || buf[gpos] == '\t')) gpos = gpos + 1;
  if (gpos >= gend) return -1;
  s = gpos;
  while (gpos < gend && buf[gpos] && buf[gpos] != ' ' && buf[gpos] != '\t') gpos = gpos + 1;
  if (gpos < gend) { buf[gpos] = 0; gpos = gpos + 1; }
  return s;
}

void parse_line(int start, int e, int lineno) {
  int i, colon, flagsdone, tok, nameoff, namelen, li;
  buf[e] = 0;                                    // line boundary, so tokens end cleanly
  i = start;
  while (i < e && buf[i] != '#') i = i + 1;
  if (i < e) e = i;                              // comment: the line ends there
  i = start;
  while (i < e && (buf[i] == ' ' || buf[i] == '\t')) i = i + 1;
  if (i == e) return;                            // blank or comment-only line
  colon = -1;
  i = start;
  while (i < e) { if (buf[i] == ':') { colon = i; break; } i = i + 1; }
  if (colon < 0) { printf("make: %s: line %d: no colon: %s\n", curfile, lineno, buf + start); exit(1); }
  buf[colon] = 0;
  i = colon + 1;
  while (i < e) {
    if (buf[i] == ':') { printf("make: %s: line %d: second colon\n", curfile, lineno); exit(1); }
    i = i + 1;
  }
  nameoff = start;
  while (nameoff < colon && (buf[nameoff] == ' ' || buf[nameoff] == '\t')) nameoff = nameoff + 1;
  namelen = colon;
  while (namelen > nameoff && (buf[namelen - 1] == ' ' || buf[namelen - 1] == '\t')) namelen = namelen - 1;
  buf[namelen] = 0;
  if (namelen == nameoff) return;                // blank line
  if (find_target(nameoff, namelen - nameoff) >= 0) {
    printf("make: %s: line %d: duplicate target %s\n", curfile, lineno, buf + nameoff); exit(1);
  }
  if (ntargets >= MAXTARGETS) { printf("make: %s: too many targets (cap %d)\n", curfile, MAXTARGETS); exit(1); }
  i = ntargets; ntargets = i + 1;
  for (li = 0; li < namelen - nameoff; li = li + 1) namstr[i * MAXLINE + li] = buf[nameoff + li];
  namstr[i * MAXLINE + li] = 0;
  tfn[i] = 0; tpn[i] = 0; tvis[i] = 0;
  flagsdone = 0;
  gpos = colon + 1; gend = e;
  while (1) {
    tok = ntok();
    if (tok < 0) break;
    if (!flagsdone && buf[tok] == '-') {         // build flags stop at the first plain token
      if (tfn[i] + 2 > MAXARGS) { printf("make: %s: line %d: too many flags (cap %d)\n", curfile, lineno, MAXARGS); exit(1); }
      tflag[i * MAXARGS + tfn[i]] = tok; tfn[i] = tfn[i] + 1;
      if (xlit(tok, 2, "-I")) {               // -I takes the next token as its argument
        tok = ntok();
        if (tok < 0) { printf("make: %s: line %d: -I needs an argument (target %s)\n", curfile, lineno, buf + nameoff); exit(1); }
        tflag[i * MAXARGS + tfn[i]] = tok; tfn[i] = tfn[i] + 1;
      }
    } else {
      flagsdone = 1;
      if (tpn[i] >= MAXPRE) { printf("make: %s: line %d: too many prereqs (cap %d)\n", curfile, lineno, MAXPRE); exit(1); }
      tpre[i * MAXPRE + tpn[i]] = tok;
      tplen[i * MAXPRE + tpn[i]] = xlen(tok);
      if (endsint(tok, tplen[i * MAXPRE + tpn[i]], ".c", 2) || endsint(tok, tplen[i * MAXPRE + tpn[i]], ".o", 2))
        tpk[i * MAXPRE + tpn[i]] = 1;
      else if (endsint(tok, tplen[i * MAXPRE + tpn[i]], ".s", 2)) tpk[i * MAXPRE + tpn[i]] = 2;
      tpn[i] = tpn[i] + 1;
    }
  }
}

void loadfile(char *path) {
  int fd, n, lineno, e, st;
  curfile = path;
  fd = open(path, 0);
  if (fd < 0) { printf("make: cannot read %s\n", path); exit(1); }
  n = read(fd, buf, MAXBUF - 1);
  close(fd);
  if (n < 0) { printf("make: cannot read %s\n", path); exit(1); }
  if (n >= MAXBUF - 1) { printf("make: %s: too big (cap %d)\n", path, MAXBUF); exit(1); }
  buf[n] = 0;
  st = 0; lineno = 1;
  while (st < n) {                               // st == n: the file's final NL, no phantom line
    e = st;
    while (e < n && buf[e] != '\n') e = e + 1;
    parse_line(st, e, lineno);
    if (e == n) break;
    st = e + 1; lineno = lineno + 1;
  }
}

// ---- planning

// an argv slot vs the namstr rows: kernel strcmp gets argv addresses straight
// (every tool's working pattern)
int find_req(char *name) {
  int i;
  i = 0;
  while (i < ntargets) { if (strcmp(namstr + i * MAXLINE, name) == 0) return i; i = i + 1; }
  return -1;
}

// does buf token (offset, length) name a target? -1 none. Names were copied
// to namstr at the parse; xcmpnam reads both sides by index.
int find_tok(int off, int len) {
  int i;
  i = 0;
  while (i < ntargets) {
    if (rownaml(i) == len && xcmpnam(i, off) == 0) return i;
    i = i + 1;
  }
  return -1;
}

int find_tpre(int i, int j) {
  return find_tok(tpre[i * MAXPRE + j], tplen[i * MAXPRE + j]);
}


int rownaml(int k) {                             // strlen(namstr row k)
  int a, n;
  a = k * MAXLINE; n = 0;
  while (namstr[a + n]) n = n + 1;
  return n;
}

void addw(char *w) {
  if (nw >= MAXARGS) { printf("make: %s: too many words (cap %d)\n", curowner, MAXARGS); exit(1); }
  cmd[nw] = w; nw = nw + 1;
}

// the built command: printed under -n, run by exec() otherwise (exec is the
// synchronous foreground call sh uses for programs: it returns the child code)
void emit_cmd(void) {
  int j, r;
  if (dry) {
    j = 0;
    while (j < nw) { if (j) printf(" "); printf("%s", cmd[j]); j = j + 1; }
    printf("\n");
    return;
  }
  r = exec(cmd[0], nw, cmd);
  if (r != 0) {
    printf("make:");
    j = 0; while (j < nw) { printf(" %s", cmd[j]); j = j + 1; }
    printf(" failed (%d)\n", r);
    exit(r);
  }
}

void resolve(int i) {
  int k, j, isbin, rot, nl;
  if (tvis[i] == 2) return;                   // planned/built already: once is once
  if (tvis[i] == 1) { printf("make: cycle at target %s\n", namstr + i * MAXLINE); exit(1); }
  tvis[i] = 1;
  nl = rownaml(i);
  isbin = endsnam(i, nl, ".bin", 4);
  curowner = namstr + i * MAXLINE;
  if (isbin) {
    // pass 1: resolve/check prereqs and assemble .s files (their as calls run
    // or print first); the .o rows join the cc command built after
    rot = 0;
    j = 0;
    while (j < tpn[i]) {
      k = find_tpre(i, j);
      if (k >= 0) resolve(k);                 // target prereq: build it first
      else {
        if (!exists(buf + tpre[i * MAXPRE + j])) {
          printf("make: %s: no such prerequisite %s\n", namstr + i * MAXLINE, buf + tpre[i * MAXPRE + j]); exit(1);
        }
        if (tpk[i * MAXPRE + j] == 2) {
          if (rot >= 8) { printf("make: %s: too many .s prereqs (cap 8)\n", namstr + i * MAXLINE); exit(1); }
          strcpy(opool + rot * MAXLINE, buf + tpre[i * MAXPRE + j]);
          opool[rot * MAXLINE + tplen[i * MAXPRE + j] - 1] = 'o';          // last char .s -> .o
          nw = 0; addw("/bin/as.bin"); addw("-o"); addw(opool + rot * MAXLINE); addw(buf + tpre[i * MAXPRE + j]);
          emit_cmd();
          rot = rot + 1;
        }
      }
      j = j + 1;
    }
    // pass 2: one cc call: program, flags, -o target, sources in file order,
    // then the assembled .o rows; headers and other files are never inputs
    nw = 0;
    addw("/bin/cc.bin");
    j = 0; while (j < tfn[i]) { addw(buf + tflag[i * MAXARGS + j]); j = j + 1; }
    addw("-o"); addw(namstr + i * MAXLINE);
    j = 0;
    while (j < tpn[i]) {
      if (find_tpre(i, j) < 0 && tpk[i * MAXPRE + j] == 1) addw(buf + tpre[i * MAXPRE + j]);
      j = j + 1;
    }
    j = 0;
    while (j < rot) { addw(opool + j * MAXLINE); j = j + 1; }
    emit_cmd();
  } else {
    j = 0;
    while (j < tpn[i]) {
      k = find_tpre(i, j);
      if (k >= 0) resolve(k);
      else if (!exists(buf + tpre[i * MAXPRE + j])) {
        printf("make: %s: unknown prerequisite %s\n", namstr + i * MAXLINE, buf + tpre[i * MAXPRE + j]); exit(1);
      }
      j = j + 1;
    }
  }
  tvis[i] = 2;
}

int main(int argc, char **argv) {
  char *fpath;
  char *req[MAXTARGETS];                      // argv slots, like grep.c's argv use
  int i, nreq, k;
  fpath = "Makefile"; nreq = 0; dry = 0;
  i = 1;
  while (i < argc) {
    if (strcmp(argv[i], "-n") == 0) dry = 1;
    else if (strcmp(argv[i], "-f") == 0) {
      i = i + 1;
      if (i >= argc) { printf("make: -f needs a file\n"); exit(1); }
      fpath = argv[i];
    } else {
      if (nreq >= MAXTARGETS) { printf("make: too many targets named\n"); exit(1); }
      req[nreq] = argv[i]; nreq = nreq + 1;
    }
    i = i + 1;
  }
  loadfile(fpath);
  if (ntargets == 0) { printf("make: %s: no targets\n", fpath); exit(1); }
  if (nreq == 0) resolve(0);
  else {
    i = 0;
    while (i < nreq) {
      k = find_req(req[i]);
      if (k < 0) { printf("make: unknown target %s\n", req[i]); exit(1); }
      resolve(k);
      i = i + 1;
    }
  }
  return 0;
}

