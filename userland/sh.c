// sh.c - T2DOS shell. Builtins: cd pwd exit help jobs kill set unset read test [ true false.
// Everything else is /bin/name.bin or ./name.bin. Output redirection with "> file" and ">> file".
// Scripting: variables (name=value, $name, ${name}, $?, $1..$9, $#, $((expr))), quotes, * and ?
// patterns, if/then/else/fi, while/do/done, for/in/do/done, ";" between commands. "sh script args"
// and "name.sh args" run a script in this shell, sharing its variables.
// "cmd args &" runs cmd as a background job; the prompt reports jobs that have ended.
// Up and Down scroll through the last 16 command lines. Tab completes a command name (first word) or a
// file name; a second Tab lists the matches when there is more than one.
#include "sys.h"

char line[256];
char path[160];
char cwd[128];
struct job_t jb[MAX_JOBS];

// print the job table (all: the running jobs too); jobs() frees the slots of the done ones
int list_jobs(int all) {
  int n; int i;
  n = jobs(jb, MAX_JOBS);
  i = 0;
  while (i < n) {
    if (jb[i].state == JOB_DONE) printf("[%d] done %s (exit %d)\n", jb[i].id, jb[i].name, jb[i].exit_code);
    else if (all) printf("[%d] running %s\n", jb[i].id, jb[i].name);
    i++;
  }
  return n;
}

// command history: the last HIST lines, hist[(hfirst + i) % HIST] for i < hcount, oldest first
#define HIST 16
char hist[HIST][256];
int hfirst; int hcount;
char draft[256];                       // what was typed before Up was first pressed

int hist_add(char *s) {
  if (!*s) return 0;
  if (hcount && !strcmp(hist[(hfirst + hcount - 1) % HIST], s)) return 0;   // same as the last one
  if (hcount == HIST) { hfirst = (hfirst + 1) % HIST; hcount--; }
  strcpy(hist[(hfirst + hcount) % HIST], s);
  hcount++;
  return 1;
}
// the next key; an escape sequence comes back as 'A' + 256 (Up), 'B' + 256 (Down) or 0 (others)
int waitkey(int ms) { int t; t = millis(); while (!kbhit()) { if (millis() - t > ms) return -1; } return getchar(); }
// keys read while a script checked for Ctrl-C, kept for the next line
char kq[64]; int kqh; int kqn;
int getkey() {
  int c;
  if (kqn) { c = kq[kqh]; kqh = (kqh + 1) % 64; kqn--; }
  else c = getchar();
  if (c != 27) return c;
  c = waitkey(50);
  if (c != '[' && c != 'O') return 0;
  c = waitkey(50);
  while (c >= 0 && (c < 64 || c > 126)) c = waitkey(50);    // parameters, up to the final byte
  if (c == 'A' || c == 'B') return c + 256;
  return 0;
}
// replace the len characters on screen and in buf with s
int retype(char *buf, int len, char *s) {
  while (len) { printf("\b \b"); len--; }
  strcpy(buf, s); printf("%s", buf);
  return strlen(buf);
}

// Tab completion of the last word: a command name for the first word, else a file or directory name
#define MAXM 64
char mt[MAXM][64];                     // the matching names
int misdir[MAXM];
int nm;
char cpre[64];                         // the longest prefix all matches share
char cdir[160];
int cont;                              // reading the continuation lines of an if/while/for

int lower(int c) { if (c >= 'A' && c <= 'Z') return c + 32; return c; }
// s starts with prefix p, ignoring case (names on the card are FAT)
int has_prefix(char *s, char *p) {
  while (*p) { if (lower(*s) != lower(*p)) return 0; s++; p++; }
  return 1;
}
int add_match(char *s, int n, int isdir) {
  int i;
  if (n > 63) return 0;
  i = 0;
  while (i < nm) { if (!strncmp(mt[i], s, n) && !mt[i][n]) return 0; i++; }   // already there
  if (nm == MAXM) return 0;
  memcpy(mt[nm], s, n); mt[nm][n] = 0; misdir[nm] = isdir; nm++;
  return 1;
}
// add the entries of dir that start with p; progs: only *.bin files, shown without .bin
int scan_dir(char *dir, char *p, int progs) {
  struct dirent_t e; int d; int n;
  d = opendir(dir);
  if (d < 0) return 0;
  while (readdir(d, &e)) {
    if (!has_prefix(e.name, p)) continue;
    if (progs && !strchr(p, '.')) {
      n = strlen(e.name);
      if (!e.isdir && n > 4 && !strcmp(e.name + n - 4, ".bin")) add_match(e.name, n - 4, 0);
    }
    else if (!progs || !e.isdir) add_match(e.name, strlen(e.name), e.isdir);   // "name.b" finds name.bin
  }
  closedir(d);
  return 0;
}
int find_matches(char *w, int cmd) {
  char *b; char *slash; char *q;
  nm = 0;
  if (cmd) {
    b = "cd pwd exit help jobs kill set unset read test true false ";
    while (*b) {
      q = b; while (*q != ' ') q++;
      if (q - b >= strlen(w) && has_prefix(b, w)) add_match(b, q - b, 0);
      b = q + 1;
    }
    scan_dir("/bin", w, 1);
    scan_dir(".", w, 1);
    return nm;
  }
  slash = 0; q = w;
  while (*q) { if (*q == '/') slash = q; q++; }
  if (!slash) { scan_dir(".", w, 0); return nm; }
  if (slash == w) strcpy(cdir, "/");
  else { memcpy(cdir, w, slash - w); cdir[slash - w] = 0; }
  scan_dir(cdir, slash + 1, 0);
  return nm;
}
// list the matches in columns, then show the prompt and the line again
int show_matches(char *buf, int len) {
  int i; int w; int cols; int n;
  w = 0; i = 0;
  while (i < nm) { n = strlen(mt[i]) + misdir[i]; if (n > w) w = n; i++; }
  w = w + 2; cols = 80 / w; if (cols < 1) cols = 1;
  putchar('\n');
  i = 0;
  while (i < nm) {
    printf("%s%s", mt[i], misdir[i] ? "/" : "");
    if (i % cols == cols - 1 || i == nm - 1) putchar('\n');
    else { n = strlen(mt[i]) + misdir[i]; while (n < w) { putchar(' '); n++; } }
    i++;
  }
  buf[len] = 0;
  if (cont) printf("> %s", buf); else printf("%s> %s", getcwd(cwd, 128), buf);
  return 0;
}
// Tab at the end of buf: returns the new length. again: the key before was Tab too
int complete(char *buf, int len, int max, int again) {
  int ws; int i; int cmd; int base; int plen; int n; char *last;
  ws = len;
  while (ws > 0 && buf[ws - 1] != ' ' && buf[ws - 1] != '>') ws--;
  cmd = 1; i = 0;
  while (i < ws) { if (buf[i] != ' ') cmd = 0; i++; }
  buf[len] = 0;
  base = ws;                           // where the name being completed starts
  i = ws; while (i < len) { if (buf[i] == '/') { cmd = 0; base = i + 1; } i++; }
  if (!find_matches(buf + ws, cmd)) return len;
  plen = len - base;                   // characters typed of the name
  strcpy(cpre, mt[0]);                 // the longest common prefix, ignoring case
  i = 1;
  while (i < nm) {
    n = 0; while (cpre[n] && lower(cpre[n]) == lower(mt[i][n])) n++;
    cpre[n] = 0; i++;
  }
  if (nm == 1) { last = misdir[0] ? "/" : " "; }
  else last = "";
  if (nm > 1 && strlen(cpre) <= plen) {   // nothing to add
    if (again) show_matches(buf, len);
    return len;
  }
  if (base + strlen(cpre) + strlen(last) >= max) return len;
  while (len > base) { printf("\b \b"); len--; }    // retype the name in its real case
  strcpy(buf + base, cpre); strcpy(buf + base + strlen(cpre), last);
  printf("%s", buf + base);
  return strlen(buf);
}

int readline(char *buf, int max) {
  int len; int c; int h; int tabs;
  len = 0; h = hcount;                 // h: the history line shown, hcount = the line being typed
  tabs = 0;
  while (1) {
    c = getkey();
    if (c == 9) {                      // a second Tab that adds nothing lists the matches
      c = complete(buf, len, max, tabs);
      tabs = c == len; len = c;
      continue;
    }
    tabs = 0;
    if (c == '\r' || c == '\n') { buf[len] = 0; putchar('\n'); hist_add(buf); return len; }
    if (c == 8 || c == 127) { if (len) { len--; printf("\b \b"); } }
    else if (c == 3) { buf[0] = 0; putchar('\n'); return -1; }
    else if (c == 'A' + 256) {         // Up: an older line
      if (h > 0) {
        if (h == hcount) { buf[len] = 0; strcpy(draft, buf); }
        h--; len = retype(buf, len, hist[(hfirst + h) % HIST]);
      }
    }
    else if (c == 'B' + 256) {         // Down: a newer line, then back to what was typed
      if (h < hcount) {
        h++;
        if (h == hcount) len = retype(buf, len, draft);
        else len = retype(buf, len, hist[(hfirst + h) % HIST]);
      }
    }
    else if (c >= 32 && c < 256 && len < max - 1) { buf[len++] = c; putchar(c); }
  }
}

int find_prog(char *name) {
  struct stat_t st;
  if (strchr(name, '.')) sprintf(path, "%s", name); else sprintf(path, "%s.bin", name);
  if (stat(path, &st) == 0 && !st.isdir) return 1;
  if (strchr(name, '.')) sprintf(path, "/bin/%s", name); else sprintf(path, "/bin/%s.bin", name);
  if (stat(path, &st) == 0 && !st.isdir) return 1;
  return 0;
}

// ---- variables: name=value, $name, ${name}, $? (last exit code), $# and $0..$9 (script arguments)
#define NV 64
char vname[NV][32];
char vval[NV][256];
int nv;
int status;                            // $?
char *sargv[10]; int sargc;            // the script's $0..$9 and $# (0 at the prompt)
int depth;                             // scripts running (0: at the prompt)
int stop;                              // exit in a script: unwind to run_script
int brk;                               // Ctrl-C in a loop: abandon the loops and scripts

int isname(int c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9'); }
int find_var(char *s) { int i; i = 0; while (i < nv) { if (!strcmp(vname[i], s)) return i; i++; } return -1; }
char *get_var(char *s) { int i; i = find_var(s); if (i < 0) return ""; return vval[i]; }
int set_var(char *s, char *v) {
  int i;
  if (strlen(s) > 31) return -1;
  i = find_var(s);
  if (i < 0) { if (nv == NV) { printf("sh: too many variables\n"); return -1; } i = nv++; strcpy(vname[i], s); }
  if (strlen(v) > 255) { memcpy(vval[i], v, 255); vval[i][255] = 0; } else strcpy(vval[i], v);
  return 0;
}
int unset_var(char *s) {
  int i;
  i = find_var(s);
  if (i < 0) return 0;
  nv--;
  if (i < nv) { strcpy(vname[i], vname[nv]); strcpy(vval[i], vval[nv]); }
  return 0;
}

// ---- $((...)): integers with + - * / % ( ), comparisons < <= > >= == != (1 or 0), unary - and !
char *ap;
int a_cmp();
int a_skip() { while (*ap == ' ' || *ap == '\t') ap++; return 0; }
int a_prim() {
  int v; char nm[32]; int k;
  a_skip();
  if (*ap == '(') { ap++; v = a_cmp(); a_skip(); if (*ap == ')') ap++; return v; }
  if (*ap == '-') { ap++; return -a_prim(); }
  if (*ap == '+') { ap++; return a_prim(); }
  if (*ap == '!') { ap++; return !a_prim(); }
  if (*ap >= '0' && *ap <= '9') { v = 0; while (*ap >= '0' && *ap <= '9') { v = v * 10 + *ap - '0'; ap++; } return v; }
  if (isname(*ap)) {                   // a bare variable name
    k = 0; while (isname(*ap)) { if (k < 31) nm[k++] = *ap; ap++; }
    nm[k] = 0;
    return atoi(get_var(nm));
  }
  return 0;
}
int a_mul() {
  int v; int w; int op;
  v = a_prim();
  while (1) {
    a_skip(); op = *ap;
    if (op != '*' && op != '/' && op != '%') return v;
    ap++; w = a_prim();
    if (op == '*') v = v * w;
    else if (!w) { printf("sh: division by zero\n"); v = 0; }
    else if (op == '/') v = v / w;
    else v = v % w;
  }
}
int a_add() {
  int v; int op;
  v = a_mul();
  while (1) {
    a_skip(); op = *ap;
    if (op != '+' && op != '-') return v;
    ap++;
    if (op == '+') v = v + a_mul(); else v = v - a_mul();
  }
}
int a_cmp() {
  int v; int w; int op; int eq;
  v = a_add();
  while (1) {
    a_skip(); op = *ap; eq = 0;
    if (op == '=' || op == '!') { if (ap[1] != '=') return v; }
    else if (op != '<' && op != '>') return v;
    ap++;
    if (*ap == '=') { eq = 1; ap++; }
    w = a_add();
    if (op == '=') v = v == w;
    else if (op == '!') v = v != w;
    else if (op == '<') v = eq ? v <= w : v < w;
    else v = eq ? v >= w : v > w;
  }
}

// ---- expansion into out[o..max): returns the new o
int xput(char *out, int o, int max, char *s) {
  while (*s && o < max - 1) { out[o] = *s; o++; s++; }
  out[o] = 0;
  return o;
}
int expand_str(char *s, char *out, int max);
// *pp points at '$': append its value, step *pp past it
int dollar(char **pp, char *out, int o, int max) {
  char *p; char nm[32]; char inner[256]; char ex[256]; int k; int d; char num[16];
  p = *pp + 1;
  if (*p == '(' && p[1] == '(') {      // $((expr))
    p = p + 2; d = 0; k = 0;
    while (*p && !(d == 0 && *p == ')' && p[1] == ')')) {
      if (*p == '(') d++;
      if (*p == ')') d--;
      if (k < 255) inner[k++] = *p;
      p++;
    }
    inner[k] = 0;
    if (*p) p = p + 2;
    *pp = p;
    expand_str(inner, ex, 256);
    ap = ex;
    sprintf(num, "%d", a_cmp());
    return xput(out, o, max, num);
  }
  if (*p == '?') { *pp = p + 1; sprintf(num, "%d", status); return xput(out, o, max, num); }
  if (*p == '#') { *pp = p + 1; sprintf(num, "%d", sargc); return xput(out, o, max, num); }
  if (*p >= '0' && *p <= '9') {
    *pp = p + 1; k = *p - '0';
    if (k <= sargc && sargv[k]) return xput(out, o, max, sargv[k]);
    return o;
  }
  k = 0;
  if (*p == '{') {
    p++;
    while (*p && *p != '}') { if (k < 31) nm[k++] = *p; p++; }
    if (*p) p++;
  }
  else while (isname(*p)) { if (k < 31) nm[k++] = *p; p++; }
  nm[k] = 0;
  if (!k) { *pp = *pp + 1; return xput(out, o, max, "$"); }   // a lone $
  *pp = p;
  return xput(out, o, max, get_var(nm));
}
int expand_str(char *s, char *out, int max) {
  int o;
  o = 0; out[0] = 0;
  while (*s) {
    if (*s == '$') o = dollar(&s, out, o, max);
    else { if (o < max - 1) { out[o] = *s; o++; out[o] = 0; } s++; }
  }
  return o;
}

// ---- words: quotes ("..." expands $, '...' is literal), \x, $ expansion, > and >> redirection, * and ? patterns
#define MAXW 64
#define WBUF 2048
char wbuf[WBUF];
int wo;
char *wv[MAXW]; int wglob[MAXW]; int wq[MAXW];
int nw;
char *redir; int append;
char gbuf[WBUF];
char *av[MAXW]; int ac;

int wch(int c) { if (wo < WBUF - 1) { wbuf[wo] = c; wo++; } return 0; }
int wdollar(char **pp) { wo = dollar(pp, wbuf, wo, WBUF); return 0; }
int tokenize(char *p) {
  int rd; int quoted; int glob; int start;
  nw = 0; wo = 0; redir = 0; append = 0; rd = 0;
  while (1) {
    while (*p == ' ' || *p == '\t') p++;
    if (!*p || *p == '#') break;
    if (*p == '>') { p++; append = 0; if (*p == '>') { append = 1; p++; } rd = 1; continue; }
    start = wo; quoted = 0; glob = 0;
    while (*p && *p != ' ' && *p != '\t' && *p != '>') {
      if (*p == '"') {
        p++; quoted = 1;
        while (*p && *p != '"') {
          if (*p == '$') wdollar(&p);
          else if (*p == '\\' && (p[1] == '"' || p[1] == '\\' || p[1] == '$')) { wch(p[1]); p = p + 2; }
          else { wch(*p); p++; }
        }
        if (*p) p++;
      }
      else if (*p == '\'') { p++; quoted = 1; while (*p && *p != '\'') { wch(*p); p++; } if (*p) p++; }
      else if (*p == '\\' && p[1]) { wch(p[1]); p = p + 2; }
      else if (*p == '$') wdollar(&p);
      else { if (*p == '*' || *p == '?') glob = 1; wch(*p); p++; }
    }
    wch(0);
    if (!quoted && wbuf[start] == 0) { wo = start; continue; }   // an unquoted empty expansion: no word
    if (rd) { redir = wbuf + start; rd = 0; continue; }
    if (nw < MAXW - 1) { wv[nw] = wbuf + start; wglob[nw] = glob; wq[nw] = quoted; nw++; }
  }
  return nw;
}

// * and ? patterns, ignoring case (FAT names)
int gmatch(char *p, char *s) {
  while (*p) {
    if (*p == '*') {
      p++;
      if (!*p) return 1;
      while (*s) { if (gmatch(p, s)) return 1; s++; }
      return 0;
    }
    if (!*s) return 0;
    if (*p != '?' && lower(*p) != lower(*s)) return 0;
    p++; s++;
  }
  return !*s;
}
int go;
// add the names matching pattern w (wildcards in its last part only) to av, sorted; 0 if none
int glob_word(char *w) {
  char dir[160]; char *slash; char *q; char *pat; struct dirent_t e; int d; int first; int i; int j; char *t;
  slash = 0; q = w;
  while (*q) { if (*q == '/') slash = q; q++; }
  if (!slash) { strcpy(dir, "."); pat = w; }
  else if (slash == w) { strcpy(dir, "/"); pat = w + 1; }
  else { memcpy(dir, w, slash - w); dir[slash - w] = 0; pat = slash + 1; }
  d = opendir(dir);
  if (d < 0) return 0;
  first = ac;
  while (readdir(d, &e)) {
    if (!gmatch(pat, e.name) || ac == MAXW - 1) continue;
    if (slash && go + (slash - w) + strlen(e.name) + 2 >= WBUF) continue;
    if (!slash && go + strlen(e.name) + 1 >= WBUF) continue;
    av[ac] = gbuf + go;
    if (slash) { memcpy(gbuf + go, w, slash - w + 1); go = go + (slash - w) + 1; }
    strcpy(gbuf + go, e.name); go = go + strlen(e.name) + 1;
    ac++;
  }
  closedir(d);
  i = first + 1;                       // insertion sort of the new names
  while (i < ac) {
    t = av[i]; j = i;
    while (j > first && strcmp(av[j - 1], t) > 0) { av[j] = av[j - 1]; j--; }
    av[j] = t; i++;
  }
  return ac - first;
}
// tokenize, then expand the patterns into av/ac
int words(char *cmd) {
  int i;
  tokenize(cmd);
  ac = 0; go = 0; i = 0;
  while (i < nw) {
    if (!wglob[i] || !glob_word(wv[i])) { if (ac < MAXW - 1) av[ac++] = wv[i]; }
    i++;
  }
  av[ac] = 0;
  return ac;
}

// ---- builtins
// test / [ ]: returns 0 (true) or 1 (false), 2 on a usage error
int do_test(int n, char **a) {
  int neg; int r; struct stat_t st; int x; int y; char *op;
  if (!strcmp(a[0], "[")) {
    if (strcmp(a[n - 1], "]")) { printf("[: missing ]\n"); return 2; }
    n--;
  }
  a++; n--; neg = 0;
  if (n > 0 && !strcmp(a[0], "!")) { neg = 1; a++; n--; }
  if (n == 0) r = 0;
  else if (n == 1) r = a[0][0] != 0;
  else if (n == 2) {
    if (!strcmp(a[0], "-z")) r = a[1][0] == 0;
    else if (!strcmp(a[0], "-n")) r = a[1][0] != 0;
    else if (!strcmp(a[0], "-e")) r = stat(a[1], &st) == 0;
    else if (!strcmp(a[0], "-f")) r = stat(a[1], &st) == 0 && !st.isdir;
    else if (!strcmp(a[0], "-d")) r = stat(a[1], &st) == 0 && st.isdir;
    else { printf("test: unknown test %s\n", a[0]); return 2; }
  }
  else if (n == 3) {
    op = a[1]; x = atoi(a[0]); y = atoi(a[2]);
    if (!strcmp(op, "=") || !strcmp(op, "==")) r = !strcmp(a[0], a[2]);
    else if (!strcmp(op, "!=")) r = strcmp(a[0], a[2]) != 0;
    else if (!strcmp(op, "-eq")) r = x == y;
    else if (!strcmp(op, "-ne")) r = x != y;
    else if (!strcmp(op, "-lt")) r = x < y;
    else if (!strcmp(op, "-le")) r = x <= y;
    else if (!strcmp(op, "-gt")) r = x > y;
    else if (!strcmp(op, "-ge")) r = x >= y;
    else { printf("test: unknown operator %s\n", op); return 2; }
  }
  else { printf("test: too many arguments\n"); return 2; }
  if (neg) r = !r;
  return r ? 0 : 1;
}
// name=value
int is_assign(char *s) {
  if (!isname(*s) || (*s >= '0' && *s <= '9')) return 0;
  while (isname(*s)) s++;
  return *s == '=';
}
int run_script(char *name, int argc, char **argv);
int ends_with(char *s, char *t) { int a; int b; a = strlen(s); b = strlen(t); return a >= b && !strcmp(s + a - b, t); }

// one simple command; sets status
int run_cmd(char *cmd) {
  int fd; int old; int r; int t0; int bg; int i; char *eq; char ln[256]; char *a0; int rd;
  if (!words(cmd)) return status;
  if (is_assign(av[0])) {              // a=1 b=2 ...
    i = 0; while (i < ac && is_assign(av[i])) i++;
    if (i < ac) { printf("sh: %s: assignments go on a line of their own\n", av[i]); status = 1; return 1; }
    i = 0; while (i < ac) { eq = strchr(av[i], '='); *eq = 0; set_var(av[i], eq + 1); i++; }
    status = 0;
    return 0;
  }
  bg = 0;
  if (!strcmp(av[ac - 1], "&") && !wq[nw - 1]) { bg = 1; ac--; av[ac] = 0; if (!ac) return status; }
  a0 = av[0];
  if (!strcmp(a0, "exit")) {
    r = ac > 1 ? atoi(av[1]) : 0;
    if (depth) { status = r; stop = 1; return r; }
    exit(r);
  }
  fd = 0; old = 0; rd = redir && !bg;      // redir is global: a script run below reuses it
  if (rd) {                  // a builtin's output can be redirected too
    fd = open(redir, append ? OPEN_APPEND : OPEN_WRITE);
    if (fd < 0) { printf("cannot write %s\n", redir); status = 1; return status; }
    old = redirect(fd);
  }
  r = -1000;                           // not a builtin
  if (!strcmp(a0, "cd")) { r = 0; if (chdir(ac > 1 ? av[1] : "/") < 0) { printf("cd: no such directory\n"); r = 1; } }
  else if (!strcmp(a0, "pwd")) { printf("%s\n", getcwd(cwd, 128)); r = 0; }
  else if (!strcmp(a0, "help")) {
    printf("builtins: cd pwd exit help jobs kill set unset read test [ true false.  programs: ls /bin.\n");
    printf("cmd > file redirects output, cmd >> file appends.\n");
    printf("cmd & runs in the background, jobs, kill N\n");
    printf("scripts: sh file args, name=value $name $? $1 $# $((i + 1)) \"quotes\" *.c\n");
    printf("if cmd; then ...; else ...; fi   while cmd; do ...; done   for x in a b; do ...; done\n");
    r = 0;
  }
  else if (!strcmp(a0, "jobs")) { list_jobs(1); r = 0; }
  else if (!strcmp(a0, "kill")) {
    r = 0;
    if (ac < 2) { printf("usage: kill N\n"); r = 1; }
    else if (kill(atoi(av[1])) < 0) { printf("kill: no job %s\n", av[1]); r = 1; }
  }
  else if (!strcmp(a0, "set")) { i = 0; while (i < nv) { printf("%s=%s\n", vname[i], vval[i]); i++; } r = 0; }
  else if (!strcmp(a0, "unset")) { i = 1; while (i < ac) { unset_var(av[i]); i++; } r = 0; }
  else if (!strcmp(a0, "read")) {
    r = 1;
    if (ac < 2) printf("usage: read name\n");
    else if (readline(ln, 256) >= 0) { set_var(av[1], ln); r = 0; }
  }
  else if (!strcmp(a0, "test") || !strcmp(a0, "[")) r = do_test(ac, av);
  else if (!strcmp(a0, "true")) r = 0;
  else if (!strcmp(a0, "false")) r = 1;
  else if ((!strcmp(a0, "sh") && ac > 1) || ends_with(a0, ".sh")) {
    if (!strcmp(a0, "sh")) r = run_script(av[1], ac - 1, av + 1);
    else if (find_prog(a0)) r = run_script(path, ac, av);   // ./name.sh, else /bin/name.sh
    else { printf("%s: not found\n", a0); r = 127; }
  }
  if (r != -1000) {
    if (rd) { redirect(old); close(fd); }
    status = r;
    return r;
  }
  if (!find_prog(a0)) {
    printf("%s: not found\n", a0);
    if (rd) { redirect(old); close(fd); }
    status = 127; return status;
  }
  if (bg) {                            // a job: its stdout is the redirect target, whose fd is the job's now
    if (redir) {
      fd = open(redir, append ? OPEN_APPEND : OPEN_WRITE);
      if (fd < 0) { printf("cannot write %s\n", redir); status = 1; return status; }
      old = redirect(fd);
    }
    r = spawn(path, ac, av);
    if (redir) { redirect(old); if (r < 0) close(fd); }
    if (r > 0) {
      printf("[%d] %s\n", r, a0);
      yield();                         // the job runs to its first wait (a server makes its pipes) before the next command
      status = 0; return 0;
    }
    if (r == SPAWN_NOTFOUND) printf("%s: not found\n", a0);
    else if (r == SPAWN_BADPROG) printf("%s: not a valid program\n", a0);
    else if (r == SPAWN_NOMEM) printf("%s: out of memory\n", a0);
    else if (r == SPAWN_FULL) printf("too many jobs\n");
    else printf("%s: cannot run in the background (%d)\n", a0, r);
    status = 1; return status;
  }
  t0 = millis();
  r = exec(path, ac, av);
  if (redir) { redirect(old); close(fd); }
  if (r < -1) printf("%s: not a valid program (%d)\n", a0, r);
  else if (r && !depth) printf("[exit %d, %d ms]\n", r, millis() - t0);   // scripts test $? instead
  status = r;
  return r;
}

// ---- command lists: a script or a prompt line is split at newlines and ; into commands
// (then, do and else become commands of their own), which run_list runs with if/while/for
int kw(char *c, char *k) {
  int n;
  n = strlen(k);
  return !strncmp(c, k, n) && (c[n] == 0 || c[n] == ' ' || c[n] == '\t');
}
char *skipws(char *p) { while (*p == ' ' || *p == '\t') p++; return p; }
int push_cmd(char **cmds, int n, int max, char *c) {
  char *p;
  c = skipws(c);
  while (kw(c, "then") || kw(c, "do") || kw(c, "else")) {     // "then cmd": two commands
    p = c; while (*p && *p != ' ' && *p != '\t') p++;
    if (!*p) break;
    *p = 0;
    if (n < max) cmds[n++] = c;
    c = skipws(p + 1);
  }
  if (*c && n < max) cmds[n++] = c;
  return n;
}
int split_cmds(char *buf, char **cmds, int max) {
  char *p; char *c; int n; int q; int ws;
  n = 0; p = buf; c = buf; q = 0; ws = 1;
  while (*p) {
    if (q) { if (*p == q) q = 0; else if (*p == '\\' && q == '"' && p[1]) p++; }
    else if (*p == '"' || *p == '\'') q = *p;
    else if (*p == '\\' && p[1] && p[1] != '\n') p++;
    else if (*p == '#' && ws) { *p = 0; p++; while (*p && *p != '\n') p++; if (!*p) break; }
    if (!q && (*p == '\n' || *p == ';' || *p == '\r')) { *p = 0; n = push_cmd(cmds, n, max, c); c = p + 1; }
    ws = *p == ' ' || *p == '\t' || *p == 0;
    p++;
  }
  n = push_cmd(cmds, n, max, c);
  if (n == max) printf("sh: script too long\n");
  return n;
}
int opener(char *c) { return kw(c, "if") || kw(c, "while") || kw(c, "for"); }
int closer(char *c) { return kw(c, "fi") || kw(c, "done"); }
// the end (fi/done) of the block opened at cmds[i]; *mid: its then/do, *alt: its else; -1 if missing
int block(char **cmds, int i, int b, int *mid, int *alt) {
  int d; int j;
  d = 0; *mid = -1; *alt = -1; j = i + 1;
  while (j < b) {
    if (opener(cmds[j])) d++;
    else if (closer(cmds[j])) { if (!d) return j; d--; }
    else if (!d && *mid < 0 && (kw(cmds[j], "then") || kw(cmds[j], "do"))) *mid = j;
    else if (!d && kw(cmds[j], "else")) *alt = j;
    j++;
  }
  return -1;
}
int check_break() {
  int c;
  if (brk) return 1;
  while (!brk && kqn < 64 && kbhit()) {
    c = getchar();
    if (c < 0) break;
    if (c == 3) { printf("^C\n"); brk = 1; }
    else if (kqn < 64) { kq[(kqh + kqn) % 64] = c; kqn++; }
  }
  return brk;
}
int run_list(char **cmds, int a, int b);
// the condition of if/while: the rest of cmds[i] after the keyword, then cmds[i+1..mid)
int run_cond(char **cmds, int i, int klen, int mid) {
  char *c;
  c = skipws(cmds[i] + klen);
  if (*c) run_cmd(c);
  if (mid > i + 1) run_list(cmds, i + 1, mid);
  return status;
}
// for var in words: copy the words, since the body reuses the word buffers
int run_for(char **cmds, int i, int mid, int end) {
  char *buf; char *v[MAXW]; int n; int k; int o; char var[32];
  words(cmds[i]);
  if (ac < 2 || strlen(av[1]) > 31) { printf("sh: for: no variable\n"); status = 1; return 1; }
  strcpy(var, av[1]);
  buf = malloc(2 * WBUF + 16);
  n = 0; o = 0;
  if (ac > 2 && !strcmp(av[2], "in")) k = 3;
  else k = ac;                         // no "in": the script's arguments
  if (k < ac) while (k < ac) { strcpy(buf + o, av[k]); v[n++] = buf + o; o = o + strlen(av[k]) + 1; k++; }
  else if (ac == 2) { k = 1; while (k <= sargc) { strcpy(buf + o, sargv[k]); v[n++] = buf + o; o = o + strlen(sargv[k]) + 1; k++; } }
  status = 0; k = 0;
  while (k < n && !stop && !check_break()) { set_var(var, v[k]); run_list(cmds, mid + 1, end); k++; }
  free(buf);
  return status;
}
int run_list(char **cmds, int a, int b) {
  int i; int mid; int alt; int end; int s; char *c;
  i = a;
  while (i < b && !stop && !brk) {
    c = cmds[i];
    if (opener(c)) {
      end = block(cmds, i, b, &mid, &alt);
      if (end < 0 || mid < 0) {
        if (kw(c, "if")) printf("sh: if without %s\n", mid < 0 ? "then" : "fi");
        else printf("sh: %s without %s\n", kw(c, "for") ? "for" : "while", mid < 0 ? "do" : "done");
        status = 2; return 2;
      }
      if (kw(c, "if")) {
        s = run_cond(cmds, i, 2, mid);
        status = 0;
        if (!stop && !brk) {
          if (!s) run_list(cmds, mid + 1, alt < 0 ? end : alt);
          else if (alt >= 0) run_list(cmds, alt + 1, end);
        }
      }
      else if (kw(c, "while")) {
        while (!stop && !check_break()) {
          if (run_cond(cmds, i, 5, mid)) break;
          run_list(cmds, mid + 1, end);
        }
        if (!stop) status = 0;
      }
      else run_for(cmds, i, mid, end);
      i = end + 1;
    }
    else if (closer(c) || kw(c, "then") || kw(c, "do") || kw(c, "else")) {
      printf("sh: unexpected %s\n", c); status = 2; return 2;
    }
    else { run_cmd(c); i++; }
  }
  return status;
}
// how many if/while/for blocks text leaves open (the prompt then asks for more lines)
#define IBUF 4096
char ibuf[IBUF];
char tbuf[IBUF];
char *icmds[512];
int open_blocks(char *text) {
  int n; int i; int d;
  strcpy(tbuf, text);
  n = split_cmds(tbuf, icmds, 512);
  d = 0; i = 0;
  while (i < n) { if (opener(icmds[i])) d++; else if (closer(icmds[i])) d--; i++; }
  return d;
}

#define MAXC 2048
int run_script(char *name, int argc, char **argv) {
  int fd; char *buf; int n; char **cmds; char *save[10]; int savec; char *ab; int o; int i;
  fd = open(name, OPEN_READ);
  if (fd < 0) { printf("sh: cannot open %s\n", name); return 1; }
  buf = malloc(65536); n = read(fd, buf, 65535); close(fd);
  if (n < 0) n = 0;
  buf[n] = 0;
  i = 0; while (i < 10) { save[i] = sargv[i]; i++; }
  savec = sargc;
  ab = malloc(1024); o = 0; i = 0;     // $0..$9 are copies: the word buffers get reused
  while (i < 10) {
    sargv[i] = 0;
    if (i < argc && o + strlen(argv[i]) < 1023) { sargv[i] = ab + o; strcpy(ab + o, argv[i]); o = o + strlen(argv[i]) + 1; }
    i++;
  }
  sargc = argc - 1; if (sargc > 9) sargc = 9;
  cmds = malloc(MAXC * 4);
  n = split_cmds(buf, cmds, MAXC);
  depth++; status = 0;
  run_list(cmds, 0, n);
  depth--; stop = 0;
  i = 0; while (i < 10) { sargv[i] = save[i]; i++; }
  sargc = savec;
  free(cmds); free(ab); free(buf);
  return status;
}

int main(int argc, char **argv) {
  struct stat_t st; int n; int k;
  conctl(CON_BREAK, 0, 0);       // Ctrl-C discards the line and stops scripts (see brk)
  if (argc > 1) return run_script(argv[1], argc - 1, argv + 1);
  if (stat("/autoexec.sh", &st) == 0) { argv[0] = "/autoexec.sh"; run_script("/autoexec.sh", 1, argv); }
  printf("T2DOS shell, type help\n");
  while (1) {
    list_jobs(0);                      // report the jobs that have ended
    brk = 0;
    printf("%s> ", getcwd(cwd, 128));
    cont = 0;
    if (readline(line, 256) <= 0) continue;
    strcpy(ibuf, line); k = strlen(ibuf);
    while (open_blocks(ibuf) > 0) {    // an if/while/for still open: read more lines
      printf("> "); cont = 1;
      n = readline(line, 256);
      if (n < 0) { ibuf[0] = 0; break; }
      if (k + n + 2 < IBUF) { ibuf[k++] = '\n'; strcpy(ibuf + k, line); k = k + n; }
    }
    cont = 0;
    n = split_cmds(ibuf, icmds, 512);
    run_list(icmds, 0, n);
  }
  return 0;
}
