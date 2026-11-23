// make.c - the board tree's build executor: reads a small target-line build
// file and runs one cc call per .bin target (as -o for .s prereqs), always
// rebuilding: no timestamps (header prereqs are recorded for the day it grows
// them). Grammar and design: the spec doc 2026-11-21-make-utility-design.md,
// extended by 2026-11-23-make-variables-recipes-design.md (variables, recipes).
// Tests: tools/check_make.py (--pc plans the tree with -n, --board runs the
// real build and byte-compares). Like the other userland tools it calls the
// kernel builtins by name (open/read/close, printf, strings, exec, exit).
//
// The programs it runs are fixed: /bin/cc.bin and /bin/as.bin, no PATH search.
// A target name IS its output path: a prereq is a target iff a line's target
// matches it exactly, otherwise it is a file.
//
// Variables (spec 2026-11-23): NAME := v / NAME = v / NAME ?= v / NAME += v
// lines and command-line NAME=value; $(NAME) expands at USE time against the
// fully loaded table (loadfile is two-pass: definitions first, then rules), so
// forward references work and a CLI value may itself reference file vars. All
// four operators store values verbatim and none expands early; the nesting cap
// MAXEXPASS catches A := $(A) like GNU's recursion error. A TAB-indented line
// after a target is that target's recipe: stored at parse, expanded and run
// by run_recipe when the target resolves (a recipe'd .bin target gets no
// implicit cc call; $@ $< $^ expand per rule; @ silences the -v echo; word 0
// maps to /bin/<name>.bin unless it holds a /). `.PHONY: name ...` records its
// (expanded) names as phony rows and creates no target; `.DEFAULT_GOAL := v`
// (pass A) overrides the default goal. Both dot-names are reserved, and so is
// every other leading-dot target name: the check runs on the EXPANDED name.
//
// Dialect discipline (docs/NOTES-star-deref.md): the committed cc
// miscompiles rvalue reads through char * VARIABLES (a VARREF emits a second
// load), so no string is ever held in a char * variable here. Strings are
// int offsets into buf, epool, varval or namstr rows, or literal/argv
// addresses passed straight to a builtin; comparisons run over those arrays
// through int-indexed helpers below. cmd[] slots (the sh.c sargv idiom: a
// stored address passed straight to exec/printf) and req[] are the only
// pointer-valued stores this program makes.
//
// usage: make [-n] [-v] [-f file] [NAME=value ...] [target ...]
//   (default goal: .DEFAULT_GOAL if set, else the file's first target;
//    V=1 acts like -v)

#define MAXLINE 128
#define MAXTARGETS 64
#define MAXPRE 16
#define MAXBUF 16384
#define MAXARGS 32
#define MAXVAR 32
#define MAXVAL 512
#define MAXEXP 1024
#define MAXEXPASS 8
#define MAXRECI 8
#define MAXRAW 64                        // raw tokens per line, pre-expansion
#define MAXEXPW 64                       // expanded words per line (flags<=32 + prereqs<=16)
#define MAXPHONY 16                      // .PHONY names
#define MAXPHNAME 64                     // phony name rows

char buf[MAXBUF];                      // the whole build file (strings are offsets into it)
char namstr[MAXTARGETS * MAXLINE];     // target names, copied out (row k: k * MAXLINE)
char opool[8 * MAXLINE];               // .s -> .o output names (8 .s per target)
int tflag[MAXTARGETS * MAXARGS];       // build flag words: epool offsets after prep
int tpre[MAXTARGETS * MAXRAW];         // raw tokens in file order, then expanded prereqs
int tvis[MAXTARGETS];                  // counts live in tpn/tfn scratch below
int tfn[MAXTARGETS], tpn[MAXTARGETS];
int ntargets;
int dry;                               // -n: print the plan, run nothing
int verbose;                           // -v: echo each command before running it
int glen;                              // strlen cache for the current resolve

char *curfile;                         // -f argument, a main argv slot: for error messages
char tpk[MAXTARGETS * MAXPRE];      // prereq kind: 0 other/header, 1 .c/.o, 2 .s
int tplen[MAXTARGETS * MAXPRE];       // prereq string length, from prep
char *cmd[MAXARGS];                    // the command being built / run (sh.c's sargv idiom)
char *curowner;                        // cmd[0] twin, for cap errors
int nw;

char varnam[MAXVAR * MAXLINE];         // variable table (pass A + command line)
char varval[MAXVAR * MAXVAL];          // values verbatim (row k: k * MAXVAL)
int vused[MAXVAR], vcli[MAXVAR];       // assigned? / command-line (file lines never win)
int nvars;
char epool[MAXTARGETS * MAXEXP];       // per-target rows of expanded words (row i: i * MAXEXP)
char lbuf[MAXEXP], sbuf[MAXEXP];       // expansion scratch, ping-pong per pass
int exowner;                           // recipe context: target index, or -1 outside recipes
int tfirst[MAXTARGETS], tphat[MAXTARGETS];  // $< / $^ sources: epool offsets, -1 none
int wbuf[MAXEXPW], wn;                   // prep's expanded word offsets (bounded at entry)
char defgoal[MAXLINE];                 // .DEFAULT_GOAL value (pass A stores, main honors)
char phony[MAXPHONY * MAXPHNAME];      // .PHONY name rows (row k: k * MAXPHNAME)
int nphony;
int trcl[MAXTARGETS * MAXRECI];        // recipe line offsets in buf (stored now, run in Task 4)
int trcn[MAXTARGETS];
int lastt;                             // target the recipes attach to (-1: none yet)

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

int xlitl(int ln, char *lit) {                 // same idiom over lbuf[0..ln)
  int i;
  i = 0;
  while (i < ln) { if (lbuf[i] != lit[i]) return 0; i = i + 1; }
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

// the same two idioms over epool words (expanded tokens live there after prep)
int xlitep(int off, int ln, char *lit) {
  int i;
  i = 0;
  while (i < ln) { if (epool[off + i] != lit[i]) return 0; i = i + 1; }
  return 1;
}

int endsep(int off, int ls, char *suf, int lf) {   // endsint, for epool words
  if (ls < lf) return 0;
  return xlitep(off + ls - lf, lf, suf);
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

int xcmpnam_ep(int k, int off) {       // strcmp(namstr row k, epool + off)
  int a, i;
  a = k * MAXLINE;
  i = 0;
  while (namstr[a + i]) { if (namstr[a + i] != epool[off + i]) return namstr[a + i] - epool[off + i]; i = i + 1; }
  return namstr[a + i] - epool[off + i];
}

int xcmpnaml(int k) {                  // strcmp(namstr row k, lbuf)
  int a, i;
  a = k * MAXLINE;
  i = 0;
  while (namstr[a + i]) { if (namstr[a + i] != lbuf[i]) return namstr[a + i] - lbuf[i]; i = i + 1; }
  return namstr[a + i] - lbuf[i];
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

int lpos, lend;                        // run_recipe's tokenizer, over lbuf

int ntokl(void) {                      // ntok over lbuf (same shape, same globals style)
  int s;
  while (lpos < lend && (lbuf[lpos] == ' ' || lbuf[lpos] == '\t')) lpos = lpos + 1;
  if (lpos >= lend) return -1;
  s = lpos;
  while (lpos < lend && lbuf[lpos] && lbuf[lpos] != ' ' && lbuf[lpos] != '\t') lpos = lpos + 1;
  if (lpos < lend) { lbuf[lpos] = 0; lpos = lpos + 1; }
  return s;
}

// ---- variables: the table is fully loaded (pass A) before anything expands;
// values are stored verbatim and $(NAME) resolves at USE time (spec 2026-11-23)

int rowvarnl(int k) {                            // strlen(varnam row k)
  int a, n;
  a = k * MAXLINE; n = 0;
  while (varnam[a + n]) n = n + 1;
  return n;
}

// exact-length compare (a varnam row is not NUL-adjacent in its source: the
// name is followed by " :", " =" or ")" in buf/lbuf, never a NUL)
int xcmpvar(int k, int off, int len) {        // varnam row k vs buf[off..off+len): 1 equal
  int a, i;
  a = k * MAXLINE;
  i = 0;
  while (i < len) { if (varnam[a + i] != buf[off + i]) return 0; i = i + 1; }
  return 1;
}

int xcmpvarl(int k, int off, int len) {       // varnam row k vs lbuf[off..off+len): 1 equal
  int a, i;
  a = k * MAXLINE;
  i = 0;
  while (i < len) { if (varnam[a + i] != lbuf[off + i]) return 0; i = i + 1; }
  return 1;
}

int findvar(int off, int len) {         // buf token vs varnam rows, -1 none
  int i;
  i = 0;
  while (i < nvars) {
    if (rowvarnl(i) == len && xcmpvar(i, off, len) == 1) return i;
    i = i + 1;
  }
  return -1;
}

int findvarl(int off, int len) {        // lbuf token vs varnam rows, -1 none
  int i;
  i = 0;
  while (i < nvars) {
    if (rowvarnl(i) == len && xcmpvarl(i, off, len) == 1) return i;
    i = i + 1;
  }
  return -1;
}

// a definition line: [spaces]NAME op value, op one of ":=" "?=" "+=" "=".
// The name run stops at " \t:?+=" (tab included, so "  \tcmd" has an empty
// name: a recipe line, never a definition).
int isdef(int st, int e) {
  int i, ns;
  i = st;
  while (i < e && buf[i] == ' ') i = i + 1;
  if (i == e) return 0;
  ns = i;
  while (i < e && buf[i] != ' ' && buf[i] != '\t' && buf[i] != ':' && buf[i] != '?' && buf[i] != '=' && buf[i] != '+') i = i + 1;
  if (i == ns) return 0;
  while (i < e && buf[i] == ' ') i = i + 1;
  if (i >= e) return 0;
  if (buf[i] == ':' || buf[i] == '?' || buf[i] == '+') return i + 1 < e && buf[i + 1] == '=';
  return buf[i] == '=';
}

// name/value are buf ranges; op: 0 ":=" or "=", 1 "?=", 2 "+=". A from_cli
// row beats every file line (any operator, silently); ?= only assigns while
// the name is still undefined; += space-joins (behaves like := when undefined).
void setvar(int noff, int nlen, int voff, int vlen, int op, int from_cli) {
  int k, a, n;
  if (nlen > MAXLINE - 1) { printf("make: %s: variable name too long (cap %d)\n", curfile, MAXLINE); exit(1); }
  k = findvar(noff, nlen);
  if (k < 0) {
    if (nvars >= MAXVAR) { printf("make: %s: too many variables (cap %d)\n", curfile, MAXVAR); exit(1); }
    k = nvars; nvars = k + 1;
    n = 0;
    while (n < nlen) { varnam[k * MAXLINE + n] = buf[noff + n]; n = n + 1; }
    varnam[k * MAXLINE + n] = 0;
    varval[k * MAXVAL] = 0;
    vused[k] = 0; vcli[k] = 0;
  }
  if (vcli[k]) return;
  if (op == 1 && vused[k]) return;
  if (vlen > MAXVAL - 1) { printf("make: %s: value too long (cap %d)\n", curfile, MAXVAL); exit(1); }
  a = k * MAXVAL;
  if (op == 2 && vused[k]) {
    n = 0;
    while (varval[a + n]) n = n + 1;
    if (n + 1 + vlen > MAXVAL - 1) { printf("make: %s: value too long (cap %d)\n", curfile, MAXVAL); exit(1); }
    varval[a + n] = ' ';
    a = a + n + 1;
  }
  n = 0;
  while (n < vlen) { varval[a + n] = buf[voff + n]; n = n + 1; }
  varval[a + n] = 0;
  vused[k] = 1;
  if (from_cli) vcli[k] = 1;
}

// pass A per line: extract name/value/op per isdef (which said yes) and setvar
void scanvar(int st, int e, int lineno) {
  int i, ns, ne, k, op, vs, ve, n;
  i = st;
  while (i < e && buf[i] != '#') i = i + 1;
  if (i < e) e = i;                              // comment: the line ends there
  i = st;
  while (i < e && buf[i] == ' ') i = i + 1;
  ns = i;
  while (i < e && buf[i] != ' ' && buf[i] != '\t' && buf[i] != ':' && buf[i] != '?' && buf[i] != '=' && buf[i] != '+') i = i + 1;
  ne = i;
  if (ne == ns) return;
  k = i;
  while (k < e && buf[k] == ' ') k = k + 1;
  if (buf[k] == ':' && k + 1 < e && buf[k + 1] == '=') { op = 0; k = k + 2; }
  else if (buf[k] == '?' && k + 1 < e && buf[k + 1] == '=') { op = 1; k = k + 2; }
  else if (buf[k] == '+' && k + 1 < e && buf[k + 1] == '=') { op = 2; k = k + 2; }
  else { op = 0; k = k + 1; }                     // plain '='
  while (k < e && (buf[k] == ' ' || buf[k] == '\t')) k = k + 1;
  vs = k;
  ve = e;
  while (ve > vs && (buf[ve - 1] == ' ' || buf[ve - 1] == '\t')) ve = ve - 1;
  if (ne - ns == 13 && xlit(ns, 13, ".DEFAULT_GOAL")) {   // reserved: main honors it
    if (ve - vs > MAXLINE - 1) { printf("make: %s: value too long (cap %d)\n", curfile, MAXLINE); exit(1); }
    n = 0;
    while (n < ve - vs) { defgoal[n] = buf[vs + n]; n = n + 1; }
    defgoal[n] = 0;
    return;
  }
  setvar(ns, ne - ns, vs, ve - vs, op, 0);
}

void exlong(void) { printf("make: %s: expansion too long (cap %d)\n", curfile, MAXEXP); exit(1); }

// expand the buf token at src into lbuf (returns its length): innermost-first
// passes, each inlining every $(NAME) whose body holds no $( (undefined -> no
// text at all, so an empty value yields no token), until no $ is left. The
// final result is in lbuf. MAXEXPASS passes then error (this is also what
// catches A := $(A)). $@/$</$^ need a recipe context (exowner >= 0, Task 4).
int expandtok(int src) {
  int n, i, q, lastp, blen, k, out, passes, a, m, c;
  n = 0;
  while (buf[src + n]) {
    if (n >= MAXEXP - 1) exlong();
    lbuf[n] = buf[src + n];
    n = n + 1;
  }
  lbuf[n] = 0;
  passes = 0;
  while (1) {
    i = 0;
    while (i < n && lbuf[i] != '$') i = i + 1;
    if (i >= n) return n;                        // fixed point reached
    if (passes >= MAXEXPASS) { printf("make: %s: variable nesting too deep\n", curfile); exit(1); }
    passes = passes + 1;
    out = 0; i = 0;
    while (i < n) {
      if (lbuf[i] != '$') {
        if (out >= MAXEXP - 1) exlong();
        sbuf[out] = lbuf[i]; out = out + 1; i = i + 1;
        continue;
      }
      if (lbuf[i + 1] == '(') {
        lastp = i; q = i + 2;                    // innermost = last $( before the first )
        while (1) {
          if (lbuf[q] == 0) { printf("make: %s: unterminated $(\n", curfile); exit(1); }
          if (lbuf[q] == ')') break;
          if (lbuf[q] == '$' && lbuf[q + 1] == '(') { lastp = q; q = q + 2; continue; }
          q = q + 1;
        }
        c = i;                                   // text before the innermost $( is literal
        while (c < lastp) {
          if (out >= MAXEXP - 1) exlong();
          sbuf[out] = lbuf[c]; out = out + 1; c = c + 1;
        }
        blen = q - lastp - 2;
        k = findvarl(lastp + 2, blen);
        if (k >= 0) {
          a = k * MAXVAL; m = 0;
          while (varval[a + m]) {
            if (out >= MAXEXP - 1) exlong();
            sbuf[out] = varval[a + m]; out = out + 1; m = m + 1;
          }
        }
        i = q + 1;
      } else {
        c = lbuf[i + 1];
        if (c != '@' && c != '<' && c != '^') { printf("make: %s: bad $ reference\n", curfile); exit(1); }
        if (exowner < 0) { printf("make: %s: $ variable outside a recipe\n", curfile); exit(1); }
        a = -1;                                  // epool/namstr source for the auto var
        if (c == '@') a = exowner * MAXLINE;     // namstr row (read below, not epool)
        else if (c == '<') a = tfirst[exowner];
        else a = tphat[exowner];
        if (a >= 0) {
          m = 0;
          if (c == '@') {
            while (namstr[a + m]) {
              if (out >= MAXEXP - 1) exlong();
              sbuf[out] = namstr[a + m]; out = out + 1; m = m + 1;
            }
          } else {
            while (epool[a + m]) {
              if (out >= MAXEXP - 1) exlong();
              sbuf[out] = epool[a + m]; out = out + 1; m = m + 1;
            }
          }
        }
        i = i + 2;
      }
    }
    sbuf[out] = 0;
    n = 0;
    while (n < out) { lbuf[n] = sbuf[n]; n = n + 1; }
    lbuf[n] = 0;
  }
}

// ---- rules

// a .PHONY line: every token after the colon is expanded (the whole table is
// loaded: pass A) and copied into a phony row. No target is created and lastt
// is untouched: a recipe line after a .PHONY line still belongs to the last
// real target. The rows are consumed by is_phony below.
void read_phony(int colon, int e, int lineno) {
  int tok, nl, a, n;
  gpos = colon + 1; gend = e;
  while (1) {
    tok = ntok();
    if (tok < 0) break;
    nl = expandtok(tok);
    if (nphony >= MAXPHONY) { printf("make: %s: line %d: too many .PHONY names (cap %d)\n", curfile, lineno, MAXPHONY); exit(1); }
    if (nl > MAXPHNAME - 1) { printf("make: %s: line %d: .PHONY name too long (cap %d)\n", curfile, lineno, MAXPHNAME); exit(1); }
    a = nphony * MAXPHNAME;
    n = 0;
    while (n < nl) { phony[a + n] = lbuf[n]; n = n + 1; }
    phony[a + n] = 0;
    nphony = nphony + 1;
  }
}

// is the expanded prereq word (epool offset, length) a declared phony name?
// Phony prereqs that are targets already resolve through the target path (no
// exists check); this matcher exists for Task 7's board-semantics checks and
// the future timestamp round, which must never stat or date-check these names.
int is_phony(int off, int len) {
  int i, a, n;
  i = 0;
  while (i < nphony) {
    a = i * MAXPHNAME;
    n = 0;
    while (phony[a + n]) n = n + 1;
    if (n == len) {
      n = 0;
      while (n < len && phony[a + n] == epool[off + n]) n = n + 1;
      if (n == len) return 1;
    }
    i = i + 1;
  }
  return 0;
}

void parse_line(int start, int e, int lineno) {
  int i, colon, tok, nameoff, namelen, li, nl;
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
  // reserved dot-names (spec Goal 5), checked on the EXPANDED name: .PHONY is
  // consumed here (no target, lastt untouched); .DEFAULT_GOAL never reaches
  // parse_line (pass A consumed it); any other leading-dot name is an error.
  i = nameoff;
  while (i < namelen && !(buf[i] == '$' && buf[i + 1] == '(')) i = i + 1;
  if (i < namelen) {                             // target name holds $(...): expand it
    nl = expandtok(nameoff);                     // the whole table is loaded (pass A)
    if (nl > MAXLINE - 1) { printf("make: %s: target name too long (cap %d)\n", curfile, MAXLINE); exit(1); }
    if (nl == 6 && xlitl(nl, ".PHONY")) { read_phony(colon, e, lineno); return; }
    if (lbuf[0] == '.') { printf("make: %s: line %d: reserved target name %s\n", curfile, lineno, lbuf); exit(1); }
    if (find_lnam(nl) >= 0) { printf("make: %s: line %d: duplicate target %s\n", curfile, lineno, lbuf); exit(1); }
    if (ntargets >= MAXTARGETS) { printf("make: %s: too many targets (cap %d)\n", curfile, MAXTARGETS); exit(1); }
    i = ntargets; ntargets = i + 1;
    for (li = 0; li < nl; li = li + 1) namstr[i * MAXLINE + li] = lbuf[li];
    namstr[i * MAXLINE + li] = 0;
  } else {
    nl = namelen - nameoff;
    if (nl == 6 && xlit(nameoff, nl, ".PHONY")) { read_phony(colon, e, lineno); return; }
    if (buf[nameoff] == '.') { printf("make: %s: line %d: reserved target name %s\n", curfile, lineno, buf + nameoff); exit(1); }
    if (find_target(nameoff, nl) >= 0) {
      printf("make: %s: line %d: duplicate target %s\n", curfile, lineno, buf + nameoff); exit(1);
    }
    if (nl > MAXLINE - 1) { printf("make: %s: target name too long (cap %d)\n", curfile, MAXLINE); exit(1); }
    if (ntargets >= MAXTARGETS) { printf("make: %s: too many targets (cap %d)\n", curfile, MAXTARGETS); exit(1); }
    i = ntargets; ntargets = i + 1;
    for (li = 0; li < nl; li = li + 1) namstr[i * MAXLINE + li] = buf[nameoff + li];
    namstr[i * MAXLINE + li] = 0;
  }
  tfn[i] = 0; tpn[i] = 0; tvis[i] = 0;
  trcn[i] = 0;
  lastt = i;
  gpos = colon + 1; gend = e;                    // raw tokens, file order, into tpre:
  while (1) {                                    // no cap/type checks here, tokens
    tok = ntok();                                // may hold $(...) for prep to expand
    if (tok < 0) break;
    if (tpn[i] >= MAXRAW) { printf("make: %s: line %d: too many words (cap %d)\n", curfile, lineno, MAXRAW); exit(1); }
    tpre[i * MAXRAW + tpn[i]] = tok;
    tpn[i] = tpn[i] + 1;
  }
}

// a TAB-indented line: the current target's recipe. The line (comment
// stripped, leading blanks skipped) is stored as a buf offset and run by
// run_recipe when its target resolves; -n prints it RESOLVED. NUL-terminated
// in place: pass B owns these bytes once a line is classified.
void recipe_line(int st, int e, int lineno) {
  int i;
  i = st;
  while (i < e && buf[i] != '#') i = i + 1;
  if (i < e) e = i;
  i = st;
  while (i < e && (buf[i] == ' ' || buf[i] == '\t')) i = i + 1;
  if (i == e) return;                            // TAB-indented blank/comment line
  if (lastt < 0) { printf("make: %s: line %d: recipe before any target\n", curfile, lineno); exit(1); }
  if (trcn[lastt] >= MAXRECI) { printf("make: %s: too many recipe lines (cap %d)\n", curfile, MAXRECI); exit(1); }
  buf[e] = 0;
  trcl[lastt * MAXRECI + trcn[lastt]] = i;
  trcn[lastt] = trcn[lastt] + 1;
}

// two passes over the file: A loads every definition (so use-time expansion
// sees the full table: forward references work), B parses rules and recipes.
// A second cheap line scan; no line-index array.
void loadfile(char *path) {
  int fd, n, lineno, e, st, i;
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
    if (buf[st] != '\t' && isdef(st, e)) scanvar(st, e, lineno);
    if (e == n) break;
    st = e + 1; lineno = lineno + 1;
  }
  st = 0; lineno = 1; lastt = -1;
  while (st < n) {
    e = st;
    while (e < n && buf[e] != '\n') e = e + 1;
    i = st;
    while (i < e && buf[i] == ' ') i = i + 1;
    if (i < e && buf[i] == '\t') recipe_line(st, e, lineno);
    else if (!isdef(st, e)) parse_line(st, e, lineno);   // definitions: pass A had them
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

// does the expanded prereq word (epool offset, length) name a target? -1 none.
// Names were copied to namstr at the parse; xcmpnam_ep reads both sides by index.
int find_tok(int off, int len) {
  int i;
  i = 0;
  while (i < ntargets) {
    if (rownaml(i) == len && xcmpnam_ep(i, off) == 0) return i;
    i = i + 1;
  }
  return -1;
}

int find_tpre(int i, int j) {
  return find_tok(tpre[i * MAXRAW + j], tplen[i * MAXPRE + j]);
}


int rownaml(int k) {                             // strlen(namstr row k)
  int a, n;
  a = k * MAXLINE; n = 0;
  while (namstr[a + n]) n = n + 1;
  return n;
}

int find_lnam(int len) {                         // lbuf vs namstr rows, -1 none
  int i;
  i = 0;
  while (i < ntargets) {
    if (rownaml(i) == len && xcmpnaml(i) == 0) return i;
    i = i + 1;
  }
  return -1;
}

void addw(char *w) {
  if (nw >= MAXARGS) { printf("make: %s: too many words (cap %d)\n", curowner, MAXARGS); exit(1); }
  cmd[nw] = w; nw = nw + 1;
}

// the command line, space separated: what -n prints and what -v echoes
void print_cmd(void) {
  int j;
  j = 0;
  while (j < nw) { if (j) printf(" "); printf("%s", cmd[j]); j = j + 1; }
  printf("\n");
}

// the built command: printed under -n, run by exec() otherwise (exec is the
// synchronous foreground call sh uses for programs: it returns the child code).
// -v echoes the command first, so a long cc.bin run shows what is building.
void emit_cmd(void) {
  int j, r;
  if (dry) {
    print_cmd();
    return;
  }
  if (verbose) print_cmd();
  r = exec(cmd[0], nw, cmd);
  if (r != 0) {
    printf("make:");
    j = 0; while (j < nw) { printf(" %s", cmd[j]); j = j + 1; }
    printf(" failed (%d)\n", r);
    exit(r);
  }
}

// copy a literal/argv string to lbuf + p (NUL included), bounded by MAXEXP
void lput(int p, char *s) {
  int k;
  k = 0;
  while (s[k]) { if (p + k >= MAXEXP) exlong(); lbuf[p + k] = s[k]; k = k + 1; }
  if (p + k >= MAXEXP) exlong();
  lbuf[p + k] = 0;
}

// run (or, under -n, print) target i's recipe, one exec per stored line:
// expand in the target's context (exowner makes expandtok fill $@/$</$^),
// strip a leading @ into quiet (GNU: the -n plan still shows the line, the
// -v echo does not: save/restore verbose around emit_cmd), tokenize, and
// map word 0 — a word holding / runs as given, else /bin/<word>.bin is
// appended after the line's last token (past lend, so ntokl never reaches
// it) and cmd[0] points there. No word-0 existence check: GNU -n plans
// blindly; a real run reports the exec failure through emit_cmd.
void run_recipe(int i) {
  int n, ln, q, quiet, sv, tok, w, has, p, k;
  n = 0;
  while (n < trcn[i]) {
    exowner = i;
    ln = expandtok(trcl[i * MAXRECI + n]);
    exowner = -1;
    q = 0;
    while (q < ln && (lbuf[q] == ' ' || lbuf[q] == '\t')) q = q + 1;
    quiet = 0;
    if (q < ln && lbuf[q] == '@') { quiet = 1; q = q + 1; }
    lpos = q; lend = ln;
    nw = 0;
    tok = ntokl();
    if (tok >= 0) {                              // "@" alone: a no-op line, like GNU
      has = 0; w = tok;
      while (lbuf[w]) { if (lbuf[w] == '/') has = 1; w = w + 1; }
      if (has) addw(lbuf + tok);
      else {
        p = ln + 1;                              // "/bin/" + word + ".bin" + NUL
        if (p + (w - tok) + 10 > MAXEXP) exlong();
        lput(p, "/bin/");
        k = 0;
        while (k < w - tok) { lbuf[p + 5 + k] = lbuf[tok + k]; k = k + 1; }
        lput(p + 5 + (w - tok), ".bin");
        addw(lbuf + p);
      }
      while (1) { tok = ntokl(); if (tok < 0) break; addw(lbuf + tok); }
      sv = verbose;
      if (quiet) verbose = 0;
      emit_cmd();
      verbose = sv;
    }
    n = n + 1;
  }
}

// expand a target's raw tokens into its own epool row, then re-split the
// EXPANDED words with the old flag rule (leading -, -I takes the next word)
// and classify prereqs. Words live in the row, never in shared scratch, so a
// nested resolve() cannot clobber a parent's argv (Review Focus pin); tpre and
// tflag end up holding epool offsets (same int type: the indexed array swaps,
// no char * is ever aliased). tfirst/tphat point at $< / $^ sources.
void prep(int i) {
  int base, ep, j, ln, k, w, fl, pr, flagsdone, n;
  base = i * MAXEXP;
  ep = 0;
  wn = 0;
  exowner = -1;                                  // auto variables are recipe-only (Task 4)
  j = 0;
  while (j < tpn[i]) {                           // phase 1: raw token -> row words
    ln = expandtok(tpre[i * MAXRAW + j]);
    k = 0;
    while (1) {
      while (k < ln && (lbuf[k] == ' ' || lbuf[k] == '\t')) k = k + 1;
      if (k >= ln) break;
      w = k;
      while (k < ln && lbuf[k] != ' ' && lbuf[k] != '\t') k = k + 1;
      if (ep + (k - w) + 1 > MAXEXP) exlong();
      if (wn >= MAXEXPW) { printf("make: %s: too many words after expansion (cap %d)\n", curfile, MAXEXPW); exit(1); }
      n = 0;
      while (n < k - w) { epool[base + ep + n] = lbuf[w + n]; n = n + 1; }
      epool[base + ep + n] = 0;
      wbuf[wn] = base + ep; wn = wn + 1;         // flags<=32 + prereqs<=16 < 64
      ep = ep + (k - w) + 1;
    }
    j = j + 1;
  }
  fl = 0; pr = 0; flagsdone = 0;
  j = 0;
  while (j < wn) {                               // phase 2: split flags/prereqs
    if (!flagsdone && epool[wbuf[j]] == '-') {   // flags stop at the first plain word
      if (fl + 2 > MAXARGS) { printf("make: %s: too many flags (cap %d) (target %s)\n", curfile, MAXARGS, namstr + i * MAXLINE); exit(1); }
      tflag[i * MAXARGS + fl] = wbuf[j]; fl = fl + 1;
      if (xlitep(wbuf[j], 2, "-I")) {            // -I takes the next expanded word
        j = j + 1;
        if (j >= wn) { printf("make: %s: -I needs an argument (target %s)\n", curfile, namstr + i * MAXLINE); exit(1); }
        tflag[i * MAXARGS + fl] = wbuf[j]; fl = fl + 1;
      }
    } else {
      flagsdone = 1;
      if (pr >= MAXPRE) { printf("make: %s: too many prereqs (cap %d) (target %s)\n", curfile, MAXPRE, namstr + i * MAXLINE); exit(1); }
      n = 0;
      while (epool[wbuf[j] + n]) n = n + 1;
      tpre[i * MAXRAW + pr] = wbuf[j];
      tplen[i * MAXPRE + pr] = n;
      if (endsep(wbuf[j], n, ".c", 2) || endsep(wbuf[j], n, ".o", 2)) tpk[i * MAXPRE + pr] = 1;
      else if (endsep(wbuf[j], n, ".s", 2)) tpk[i * MAXPRE + pr] = 2;
      pr = pr + 1;
    }
    j = j + 1;
  }
  tfn[i] = fl; tpn[i] = pr;
  if (pr > 0) {
    tfirst[i] = tpre[i * MAXRAW];
    w = ep;                                      // $^: intact space-joined copy at row end
    j = 0;
    while (j < pr) {
      if (j) {
        if (ep >= MAXEXP) exlong();
        epool[base + ep] = ' '; ep = ep + 1;
      }
      k = wbuf[wn - pr + j];                     // wbuf is stable; tpre is being rewritten
      n = 0;
      while (epool[k + n]) {
        if (ep >= MAXEXP) exlong();
        epool[base + ep] = epool[k + n]; ep = ep + 1; n = n + 1;
      }
      j = j + 1;
    }
    if (ep >= MAXEXP) exlong();
    epool[base + ep] = 0;
    tphat[i] = base + w;
  } else {
    tfirst[i] = -1; tphat[i] = -1;
  }
}

void resolve(int i) {
  int k, j, isbin, rot, nl;
  if (tvis[i] == 2) return;                   // planned/built already: once is once
  if (tvis[i] == 1) { printf("make: cycle at target %s\n", namstr + i * MAXLINE); exit(1); }
  tvis[i] = 1;
  prep(i);                                    // exactly once: the tvis guards above
  nl = rownaml(i);
  isbin = endsnam(i, nl, ".bin", 4);
  curowner = namstr + i * MAXLINE;
  if (isbin) {
    // pass 1: resolve/check prereqs and assemble .s files (their as calls run
    // or print first); the .o rows join the cc command built after. A target
    // WITH a recipe honors its prereqs but makes zero toolchain calls: the
    // as emissions are guarded too, and pass 2 is skipped whole.
    rot = 0;
    j = 0;
    while (j < tpn[i]) {
      k = find_tpre(i, j);
      if (k >= 0) resolve(k);                 // target prereq: build it first
      else {
        if (!exists(epool + tpre[i * MAXRAW + j])) {
          printf("make: %s: no such prerequisite %s\n", namstr + i * MAXLINE, epool + tpre[i * MAXRAW + j]); exit(1);
        }
        if (trcn[i] == 0 && tpk[i * MAXPRE + j] == 2) {
          if (rot >= 8) { printf("make: %s: too many .s prereqs (cap 8)\n", namstr + i * MAXLINE); exit(1); }
          strcpy(opool + rot * MAXLINE, epool + tpre[i * MAXRAW + j]);
          opool[rot * MAXLINE + tplen[i * MAXPRE + j] - 1] = 'o';          // last char .s -> .o
          nw = 0; addw("/bin/as.bin"); addw("-o"); addw(opool + rot * MAXLINE); addw(epool + tpre[i * MAXRAW + j]);
          emit_cmd();
          rot = rot + 1;
        }
      }
      j = j + 1;
    }
    if (trcn[i] == 0) {
      // pass 2: one cc call: program, flags, -o target, sources in file order,
      // then the assembled .o rows; headers and other files are never inputs
      nw = 0;
      addw("/bin/cc.bin");
      j = 0; while (j < tfn[i]) { addw(epool + tflag[i * MAXARGS + j]); j = j + 1; }
      addw("-o"); addw(namstr + i * MAXLINE);
      j = 0;
      while (j < tpn[i]) {
        if (find_tpre(i, j) < 0 && tpk[i * MAXPRE + j] == 1) addw(epool + tpre[i * MAXRAW + j]);
        j = j + 1;
      }
      j = 0;
      while (j < rot) { addw(opool + j * MAXLINE); j = j + 1; }
      emit_cmd();
    }
  } else {
    j = 0;
    while (j < tpn[i]) {
      k = find_tpre(i, j);
      if (k >= 0) resolve(k);
      else if (!exists(epool + tpre[i * MAXRAW + j])) {
        printf("make: %s: unknown prerequisite %s\n", namstr + i * MAXLINE, epool + tpre[i * MAXRAW + j]); exit(1);
      }
      j = j + 1;
    }
  }
  curowner = namstr + i * MAXLINE;            // nested resolves moved it: recipe errors name this target
  run_recipe(i);                              // both branches: after prereqs, before done
  tvis[i] = 2;
}

int main(int argc, char **argv) {
  char *fpath;
  char *req[MAXTARGETS];                      // argv slots, like grep.c's argv use
  int i, nreq, k, j, noff, voff, li, bpos;
  fpath = "Makefile"; nreq = 0; dry = 0; verbose = 0; exowner = -1; bpos = 0;
  curfile = "Makefile";                          // CLI setvar errors before loadfile has one
  i = 1;
  while (i < argc) {
    if (strcmp(argv[i], "-n") == 0) dry = 1;
    else if (strcmp(argv[i], "-v") == 0) verbose = 1;
    else if (strcmp(argv[i], "-f") == 0) {
      i = i + 1;
      if (i >= argc) { printf("make: -f needs a file\n"); exit(1); }
      fpath = argv[i];
    } else {
      j = 0;
      while (argv[i][j] && argv[i][j] != '=') j = j + 1;
      if (argv[i][j] == '=') {                // NAME=value (GNU): a variable, not a target
        if (bpos + 2 * strlen(argv[i]) + 2 >= MAXBUF) { printf("make: command line too long\n"); exit(1); }
        // copy name and value into buf scratch and setvar from there (the
        // argv[i][j] walk is grep.c's proven idiom; no pointer arithmetic on
        // char *). loadfile overwrites the scratch later; setvar's table
        // copies are what survive.
        noff = bpos;
        li = 0;
        while (li < j) { buf[bpos] = argv[i][li]; bpos = bpos + 1; li = li + 1; }
        buf[bpos] = 0; bpos = bpos + 1;
        voff = bpos;
        li = j + 1;
        while (argv[i][li]) { buf[bpos] = argv[i][li]; bpos = bpos + 1; li = li + 1; }
        buf[bpos] = 0; bpos = bpos + 1;
        setvar(noff, j, voff, bpos - voff - 1, 0, 1);   // before loadfile: the file cannot override
        if (j == 1 && buf[noff] == 'V' && buf[voff]) verbose = 1;   // V=1 acts like -v
      } else {
        if (nreq >= MAXTARGETS) { printf("make: too many targets named\n"); exit(1); }
        req[nreq] = argv[i]; nreq = nreq + 1;
      }
    }
    i = i + 1;
  }
  loadfile(fpath);
  if (ntargets == 0) { printf("make: %s: no targets\n", fpath); exit(1); }
  if (nreq == 0) {
    if (defgoal[0]) {                            // .DEFAULT_GOAL beats the first target
      k = find_req(defgoal);
      if (k < 0) { printf("make: unknown target %s\n", defgoal); exit(1); }
      resolve(k);
    } else resolve(0);
  } else {
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
