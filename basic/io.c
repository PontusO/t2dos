// io.c - the console and the board: the key queue, INPUT and LINE INPUT, INKEY$, and the
// screen (CLS, LOCATE, COLOR, BEEP, TAB, SPC, WRITE), SLEEP and the pin statements, and the
// files: OPEN CLOSE PRINT # WRITE # INPUT # LINE INPUT # EOF FREEFILE, KILL NAME MKDIR RMDIR
// CHDIR FILES, PIPE, SHELL and COMMAND$.
// basic.h describes the key queue. Handlers run through OP_FN (funcs.c): the compiler
// pushes the arguments, emit_fn() names the handler by token, which pops them.
#include "basic.h"

#define TK (toks[tp].tk)

int kq[64];                      // waiting keys, oldest first
int kqn;
int cur_fg;                      // colours set by COLOR (reset when the program ends)
int cur_bg;
char linebuf[256];               // the line INPUT / LINE INPUT is reading
int linen;
struct stat_t fst;             // scratch for stat() and readdir()
struct dirent_t fde;
char shcmd[256];                 // SHELL's command line and its words
char *shargv[16];
int in_val[32];                  // INPUT's values (str pointers, int, float bits) until OP_INGET
char in_str[32];                 // 1 where in_val is an owned str

// ---------------------------------------------------------------- keys
int key_poll() {
  int c; int brk;
  brk = 0;
  while (kbhit()) {
    c = getchar();
    if (c < 0) break;
    if (c == 3) brk = 1;
    else if (kqn < 64) { kq[kqn] = c; kqn++; }
  }
  if (brk) return -1;
  return kqn;
}

int kq_pop() {
  int c; int i;
  c = kq[0];
  for (i = 1; i < kqn; i++) kq[i - 1] = kq[i];
  kqn--;
  return c;
}

// The next key, waiting for it: from the queue first, else from the console. -1 at the end
// of the input (a file on the PC), -2 for Ctrl-C.
int key_wait() {
  int c;
  if (kqn > 0) return kq_pop();
  c = getchar();
  if (c == 3) return -2;
  return c;
}

// Waits ms milliseconds, looking for Ctrl-C; with stop, a waiting key (it stays in the
// queue) ends the wait early, as QBasic's SLEEP.
void wait_ms(int ms, int stop) {
  int t0; int left;
  t0 = millis();
  while (1) {
    left = ms - (millis() - t0);
    if (left <= 0) return;
    if (key_poll() < 0) { brk_at(pc - 1); return; }
    if (stop && kqn > 0) return;
    if (left > 10) left = 10;
    sleep(left);
  }
}

// INKEY$: QBasic's codes for the special keys, decoded from the terminal's sequences. The
// final letter of ESC [ x and ESC O x, and the number of ESC [ n ~, give the code letter.
int esc_code(int *len) {
  int c; int n;
  *len = 1;
  if (kqn < 3 || kq[0] != 27 || (kq[1] != '[' && kq[1] != 'O')) return 0;
  c = kq[2];
  if (c == 'A') { *len = 3; return 'H'; }
  if (c == 'B') { *len = 3; return 'P'; }
  if (c == 'C') { *len = 3; return 'M'; }
  if (c == 'D') { *len = 3; return 'K'; }
  if (c == 'H') { *len = 3; return 'G'; }
  if (c == 'F') { *len = 3; return 'O'; }
  if (kq[1] == '[' && kqn >= 4 && kq[3] == '~') {
    n = c;
    *len = 4;
    if (n == '1' || n == '7') return 'G';
    if (n == '4' || n == '8') return 'O';
    if (n == '5') return 'I';
    if (n == '6') return 'Q';
    if (n == '2') return 'R';
    if (n == '3') return 'S';
    *len = 1;
  }
  return 0;
}

// 1 if the queue holds only the beginning of an escape sequence: ESC, ESC [, ESC O,
// ESC [ digit
int esc_partial() {
  if (kqn == 0 || kq[0] != 27) return 0;
  if (kqn == 1) return 1;
  if (kq[1] != '[' && kq[1] != 'O') return 0;
  if (kqn == 2) return 1;
  if (kqn == 3 && kq[1] == '[' && kq[2] >= '0' && kq[2] <= '9') return 1;
  return 0;
}

// A key sequence may arrive in pieces: when the queue starts with an incomplete one, poll
// for up to 30 ms for the rest. Returns 0 on Ctrl-C (the break is made).
int esc_settle() {
  int t0;
  t0 = millis();
  while (esc_partial() && millis() - t0 < 30) {
    if (key_poll() < 0) { brk_at(pc - 1); return 0; }
    if (!esc_partial()) break;
    sleep(2);
  }
  return 1;
}

void kq_unshift(int c) {
  int i;
  if (kqn >= 64) kqn = 63;
  for (i = kqn; i > 0; i--) kq[i] = kq[i - 1];
  kq[0] = c;
  kqn++;
}

void h_inkeys() {
  int c; int len; int letter; char *s;
  yield();
  sp++; vs[sp] = 0; vt[sp] = 0;
  if (key_poll() < 0) { brk_at(pc - 1); return; }
  if (kqn == 0) return;
  if (!esc_settle()) return;
  letter = esc_code(&len);
  if (letter) {
    s = str_new(2); s[0] = 0; s[1] = letter;
    while (len > 0) { kq_pop(); len--; }
  }
  else {
    c = kq_pop();
    s = str_new(1); s[0] = c;
  }
  vs[sp] = (int)s; vt[sp] = 1;
}

// ---------------------------------------------------------------- INPUT, LINE INPUT
// After an ESC was read from a line being typed: take the key sequence it begins (an arrow
// key) off the queue so that none of it is typed; a lone ESC is dropped, the text after it
// stays. Returns 0 on Ctrl-C.
int skip_esc() {
  int len;
  kq_unshift(27);
  if (key_poll() < 0) { brk_at(pc - 1); return 0; }
  if (!esc_settle()) return 0;
  esc_code(&len);
  while (len > 0) { kq_pop(); len--; }
  return 1;
}

// Reads a line into linebuf/linen, echoing it (and handling Backspace) as a terminal
// program must here. Returns 0 if the program ended (Ctrl-C, or no more input).
int read_line() {
  int c;
  linen = 0;
  out_flush();
  while (1) {
    c = key_wait();
    if (c == -2) { brk_at(pc - 1); return 0; }
    if (c == -1) {
      if (linen > 0) break;
      vm_err("Input past end");
      return 0;
    }
    if (c == 13 || c == 10) break;
    if (c == 27) { if (!skip_esc()) return 0; continue; }
    if (c == 8 || c == 127) {
      if (linen > 0) { linen--; out_char(8); out_char(' '); out_char(8); out_flush(); }
      continue;
    }
    if (c >= 32 && linen < 255) { linebuf[linen] = c; linen++; out_char(c); out_flush(); }
  }
  out_char('\n');
  return 1;
}

// Frees the strs among the first n values INPUT holds.
void in_free(int n) {
  int i;
  for (i = 0; i < n; i++) {
    if (in_str[i]) str_free((char *)in_val[i]);
    in_str[i] = 0; in_val[i] = 0;
  }
}

// One field of an INPUT line for a variable of type t, from linebuf[i]: sets *val (and
// in_str[k]) and returns the position after the field. *more is 1 if a comma ended it.
// Returns -1 if the text is not a valid value.
int in_field(int i, int t, int k, int *more) {
  int j; int e; int type; int v; int used; int neg; float f; int n;
  n = linen;
  *more = 0;
  in_val[k] = 0; in_str[k] = 0;
  while (i < n && linebuf[i] == ' ') i++;
  if (t == T_STR && i < n && linebuf[i] == '"') {
    j = i + 1;
    while (j < n && linebuf[j] != '"') j++;
    e = j;
    if (j < n) j++;
    while (j < n && linebuf[j] == ' ') j++;
    if (j < n && linebuf[j] != ',') return -1;
    in_val[k] = (int)str_from(linebuf + i + 1, e - i - 1); in_str[k] = 1;
    if (j < n) { j++; *more = 1; }
    return j;
  }
  j = i;
  while (j < n && linebuf[j] != ',') j++;
  e = j;
  while (e > i && linebuf[e - 1] == ' ') e--;
  if (j < n) { j++; *more = 1; }
  if (t == T_STR) { in_val[k] = (int)str_from(linebuf + i, e - i); in_str[k] = 1; return j; }
  if (e == i) return j;                                    // nothing typed: 0
  neg = 0;
  if (linebuf[i] == '-' || linebuf[i] == '+') { neg = linebuf[i] == '-'; i++; }
  if (i >= e) return -1;
  used = num_scan(linebuf + i, e - i, &type, &v);
  if (used != e - i || type == T_NONE) return -1;
  if (type == T_INT) f = v; else f = bitsf(v);
  if (neg) f = -f;
  if (t == T_SNG) { in_val[k] = fbits(f); return j; }
  if (!(f < 2147483648.0 && f > -2147483649.0)) return -1;
  in_val[k] = f2i(f);
  return j;
}

// All fields of the line into in_val, or -1 (nothing kept) if the count or a value is wrong.
int in_parse(int *types, int nv) {
  int i; int k; int more;
  i = 0;
  for (k = 0; k < nv; k++) {
    i = in_field(i, types[k], k, &more);
    if (i < 0 || (k < nv - 1 && !more) || (k == nv - 1 && more)) { in_free(k + 1); return -1; }
  }
  return 0;
}

// ---------------------------------------------------------------- files
// File numbers 1 to 15 index the tables. f_mode: 0 closed, 1 INPUT, 2 OUTPUT, 3 APPEND.
// An INPUT file has a 512-byte read buffer (f_buf, f_pos of f_len used); f_eof is 1 after a
// read that returned 0 (the end of the file, or of a pipe's message) and 2 after an error;
// f_path keeps a pipe's name for stat(); f_col is the column of an output file.
int f_fd[16];
int f_mode[16];
char *f_buf[16];
int f_pos[16];
int f_len[16];
int f_eof[16];
char *f_path[16];
int f_col[16];
int out_idx;                     // the file PRINT # is writing, and the console's column
int out_save;
char path1[200];                 // the paths of the statement running
char path2[200];

// Switches PRINT's output back to the console (after PRINT #, or when an error ends it).
void out_reset() {
  if (out_fd) {
    out_flush();
    f_col[out_idx] = out_col;
    out_col = out_save;
    out_fd = 0;
  }
}

// 1 if n is an open file usable for input (want 1) or output (want 2, OUTPUT or APPEND);
// else the run-time error.
int file_ok(int n, int want) {
  if (n < 1 || n > 15 || !f_mode[n]) { vm_err("Bad file number"); return 0; }
  if ((want == 1 && f_mode[n] != 1) || (want == 2 && f_mode[n] == 1)) { vm_err("Bad file mode"); return 0; }
  return 1;
}

void file_close(int n) {
  if (!f_mode[n]) return;
  close(f_fd[n]);
  if (f_buf[n]) free(f_buf[n]);
  if (f_path[n]) free(f_path[n]);
  f_buf[n] = 0; f_path[n] = 0; f_mode[n] = 0; f_pos[n] = 0; f_len[n] = 0; f_eof[n] = 0;
}

void files_close_all() {
  int n;
  for (n = 1; n < 16; n++) file_close(n);
}

// A str into dst as a path: \ becomes /. 0 (the error Path not found) if it is empty or too long.
int to_path(char *dst, char *s) {
  int n; int i; int c;
  n = str_len(s);
  if (n == 0 || n > 150) { vm_err("Path not found"); return 0; }
  for (i = 0; i < n; i++) {
    c = s[i];
    if (c == '\\') c = '/';
    dst[i] = c;
  }
  dst[n] = 0;
  return 1;
}

// pops a str operand into dst (and frees it)
int pop_path(char *dst) {
  char *s; int ok;
  s = (char *)vs[sp]; vt[sp] = 0; sp--;
  ok = to_path(dst, s);
  str_free(s);
  return ok;
}

// OPEN path, number: fn_n is 1 INPUT, 2 OUTPUT, 3 APPEND
void h_open() {
  int n; int mode; int fd; char *s; int ok;
  mode = fn_n;
  n = vs[sp]; s = (char *)vs[sp - 1]; vt[sp - 1] = 0; sp = sp - 2;
  ok = to_path(path1, s);
  str_free(s);
  if (!ok) return;
  if (n < 1 || n > 15) { vm_err("Bad file number"); return; }
  if (f_mode[n]) { vm_err("File already open"); return; }
  fd = open(path1, mode == 1 ? OPEN_READ : mode == 2 ? OPEN_WRITE : OPEN_APPEND);
  if (fd < 0) { vm_err(mode == 1 ? "File not found" : "Path not found"); return; }
  f_fd[n] = fd; f_mode[n] = mode; f_pos[n] = 0; f_len[n] = 0; f_eof[n] = 0; f_col[n] = 0;
  if (mode == 1) f_buf[n] = malloc(512);
  if (strncmp(path1, "/pipe/", 6) == 0) {
    f_path[n] = malloc(strlen(path1) + 1);
    strcpy(f_path[n], path1);
  }
}

// CLOSE: fn_n 0 closes all, 1 the file number on the stack
void h_close() {
  int n;
  if (fn_n == 0) { files_close_all(); return; }
  n = vs[sp]; sp--;
  if (n < 1 || n > 15) { vm_err("Bad file number"); return; }
  file_close(n);
}

// PRINT # / WRITE #: fn_n 0 sends the output to the file whose number is on the stack,
// 1 back to the console
void h_setout() {
  int n;
  if (fn_n == 1) { out_reset(); return; }
  n = vs[sp]; sp--;
  if (!file_ok(n, 2)) return;
  out_flush();
  out_save = out_col;
  out_idx = n;
  out_fd = f_fd[n];
  out_col = f_col[n];
}

// Reads more into the file's buffer. A pipe is polled with stat() until a message has data
// or ends, so that Ctrl-C works and a removed pipe ends the wait. Returns the bytes (0: the
// end, which sets f_eof to 1; -1: an error or Ctrl-C, f_eof 2).
int fl_fill(int n) {
  int r;
  f_pos[n] = 0; f_len[n] = 0;
  if (!f_buf[n]) { f_eof[n] = 2; return -1; }
  if (f_path[n]) {
    while (1) {
      if (stat(f_path[n], &fst) < 0) { f_eof[n] = 2; return -1; }
      if (fst.size > 0 || fst.mtime > 0) break;
      if (key_poll() < 0) { brk_at(pc - 1); f_eof[n] = 2; return -1; }
      sleep(5);
    }
  }
  r = read(f_fd[n], f_buf[n], 512);
  if (r < 0) { f_eof[n] = 2; return -1; }
  if (r == 0) { f_eof[n] = 1; return 0; }
  f_eof[n] = 0; f_len[n] = r;
  return r;
}

// The next byte of an input file, or -1 at its end.
int fl_getc(int n) {
  int c;
  if (f_pos[n] >= f_len[n] && fl_fill(n) <= 0) return -1;
  c = f_buf[n][f_pos[n]] & 255;
  f_pos[n]++;
  return c;
}

// The next line as a malloc'd block of *len bytes (at most 32767; the line end and a CR before
// it are dropped); 0 at the end of the file. On a pipe the end of a message with nothing read
// is not the end: the next message is waited for.
char *fl_readline(int n, int *len) {
  char *b; char *t; int cap; int l; int c; int got;
  cap = 128; b = malloc(cap); l = 0; got = 0;
  while (1) {
    c = fl_getc(n);
    if (c < 0) {
      if (l > 0 || !running) break;
      if (f_path[n] && f_eof[n] == 1) { f_eof[n] = 0; continue; }
      free(b);
      return 0;
    }
    if (c == 10) break;
    if (l < STR_MAX) {
      if (l + 1 >= cap) { t = malloc(cap * 2); memcpy(t, b, l); free(b); b = t; cap = cap * 2; }
      b[l] = c; l++;
    }
  }
  if (l > 0 && b[l - 1] == 13) l--;
  *len = l;
  return b;
}

// INPUT #n: the fields go to in_val like the console's INPUT; a new line is read for the
// first variable and after a field that no comma follows.
void file_input(int n, int *types, int nv) {
  int k; int i; int more; char *b; int len;
  in_free(32);
  i = 0; more = 0;
  for (k = 0; k < nv; k++) {
    if (k == 0 || !more) {
      b = fl_readline(n, &len);
      if (!b) { if (running) vm_err("Input past end of file"); in_free(32); return; }
      if (len > 255) len = 255;
      memcpy(linebuf, b, len); linen = len; i = 0;
      free(b);
    }
    i = in_field(i, types[k], k, &more);
    if (i < 0) { in_free(32); vm_err("Type mismatch"); return; }
  }
}

// EOF(n): -1 if nothing more can be read. Looking may wait on a pipe.
void h_eof() {
  int n; int c;
  n = vs[sp];
  vs[sp] = 0;
  if (!file_ok(n, 1)) return;
  if (f_pos[n] < f_len[n]) return;
  if (f_path[n] && f_eof[n] == 1) { vs[sp] = -1; return; }
  c = fl_getc(n);
  if (c < 0) vs[sp] = -1;
  else f_pos[n]--;
}

void h_freefile() {
  int n;
  sp++; vt[sp] = 0; vs[sp] = 0;
  for (n = 1; n < 16; n++)
    if (!f_mode[n]) { vs[sp] = n; return; }
  vm_err("Bad file number");
}

int fn_eof() {
  next();
  if (!need(TK_LPAR)) return T_INT;
  accept(TK_HASH);
  expr_to(T_INT);
  if (!need(TK_RPAR)) return T_INT;
  emit_fn(K_EOF, 0);
  return T_INT;
}

int fn_freefile() {
  next();
  if (TK == TK_LPAR) { next(); if (!need(TK_RPAR)) return T_INT; }
  emit_fn(K_FREEFILE, 0);
  return T_INT;
}

// COMMAND$: the arguments after the program's file name, joined by spaces
void h_commands() {
  int i; int n; int j; char *s; char *a;
  n = 0;
  for (i = 0; i < prog_argc; i++) n = n + strlen(prog_argv[i]) + 1;
  if (n > 0) n--;
  if (n > STR_MAX) n = STR_MAX;
  s = str_new(n);
  n = 0;
  for (i = 0; i < prog_argc; i++) {
    if (i > 0 && n < str_len(s)) { s[n] = ' '; n++; }
    a = prog_argv[i];
    for (j = 0; a[j] && n < str_len(s); j++) { s[n] = a[j]; n++; }
  }
  sp++; vs[sp] = (int)s; vt[sp] = 1;
}

int fn_commands() { next(); emit_fn(K_COMMANDS, 0); return T_STR; }

// OPEN path FOR INPUT|OUTPUT|APPEND AS [#]n
void st_open() {
  int m;
  next();
  expr_to(T_STR);
  if (!need(K_FOR)) return;
  m = TK;
  if (m != K_INPUT && m != K_OUTPUT && m != K_APPEND) { bad_token(); return; }
  next();
  if (!need(K_AS)) return;
  accept(TK_HASH);
  expr_to(T_INT);
  emit_fn(K_OPEN, m == K_INPUT ? 1 : m == K_OUTPUT ? 2 : 3);
  if (!at_end()) bad_token();
}

// CLOSE [[#]n [, [#]n ...]]: with no number, every file
void st_close() {
  next();
  if (at_end()) { emit_fn(K_CLOSE, 0); return; }
  while (1) {
    accept(TK_HASH);
    expr_to(T_INT);
    emit_fn(K_CLOSE, 1);
    if (!accept(TK_COMMA)) break;
  }
  if (!at_end()) bad_token();
}

// ---------------------------------------------------------------- file system statements
// KILL f$ / MKDIR d$ / RMDIR d$ / CHDIR d$ / PIPE name$ / SHELL cmd$: one string
void st_str1() {
  int k;
  k = TK; next();
  expr_to(T_STR);
  emit_fn(k, 0);
  if (!at_end()) bad_token();
}

void st_name() {
  next();
  expr_to(T_STR);
  if (!need(K_AS)) return;
  expr_to(T_STR);
  emit_fn(K_NAME, 0);
  if (!at_end()) bad_token();
}

void st_files() {
  next();
  if (at_end()) { emit(OP_PUSHI); emit_arg(0); }
  else expr_to(T_STR);
  emit_fn(K_FILES, 0);
  if (!at_end()) bad_token();
}

void h_kill() {
  if (!pop_path(path1)) return;
  if (unlink(path1) < 0) vm_err("File not found");
}

void h_name() {
  if (!pop_path(path2)) return;
  if (!pop_path(path1)) return;
  if (stat(path1, &fst) < 0) { vm_err("File not found"); return; }
  if (rename(path1, path2) < 0) vm_err("Path/File access error");
}

void h_mkdir() {
  if (!pop_path(path1)) return;
  if (mkdir(path1) < 0) vm_err("Path/File access error");
}

void h_rmdir() {
  if (!pop_path(path1)) return;
  if (rmdir(path1) < 0) {
    if (stat(path1, &fst) < 0) vm_err("Path not found");
    else vm_err("Path/File access error");
  }
}

void h_chdir() {
  if (!pop_path(path1)) return;
  if (chdir(path1) < 0) vm_err("Path not found");
}

void h_pipe() {
  if (!pop_path(path1)) return;
  if (mkpipe(path1) < 0) vm_err("Path/File access error");
}

// FILES [dir$]: the names in the directory (the current one if none), sorted, one per line;
// a directory's name ends in /
void h_files() {
  char *s; int d; int cnt; int i; int j; char *names; int ok; char t[68];
  s = (char *)vs[sp]; vt[sp] = 0; sp--;
  if (str_len(s) == 0) { getcwd(path1, 128); ok = 1; }
  else ok = to_path(path1, s);
  str_free(s);
  if (!ok) return;
  d = opendir(path1);
  if (d < 0) { vm_err("Path not found"); return; }
  names = malloc(256 * 68);
  cnt = 0;
  while (cnt < 256 && readdir(d, &fde)) {
    if (fde.name[0] == '.' && (fde.name[1] == 0 || (fde.name[1] == '.' && fde.name[2] == 0))) continue;
    strcpy(names + cnt * 68, fde.name);
    if (fde.isdir) strcpy(names + cnt * 68 + strlen(fde.name), "/");
    cnt++;
  }
  closedir(d);
  for (i = 1; i < cnt; i++) {
    memcpy(t, names + i * 68, 68);
    j = i;
    while (j > 0 && strcmp(names + (j - 1) * 68, t) > 0) {
      memcpy(names + j * 68, names + (j - 1) * 68, 68);
      j--;
    }
    memcpy(names + j * 68, t, 68);
  }
  for (i = 0; i < cnt; i++) { out_cstr(names + i * 68); out_char('\n'); }
  free(names);
}

// SHELL cmd$: runs the program named by the first word, found like sh does (NAME.bin in
// the current directory, NAME, then /bin/NAME.bin; a name with a '.' is tried as it is and
// then in /bin), with the other words, split on spaces, as its arguments.
void h_shell() {
  char *s; int n; int argc; int i; int k; int found; char *p; int dot;
  s = (char *)vs[sp]; vt[sp] = 0; sp--;
  n = str_len(s);
  if (n > 250) n = 250;
  for (i = 0; i < n; i++) {
    shcmd[i] = s[i];
    if (s[i] == '\\') shcmd[i] = '/';
  }
  shcmd[n] = 0;
  str_free(s);
  argc = 0; p = shcmd;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    if (argc < 15) { shargv[argc] = p; argc++; }
    while (*p && *p != ' ') p++;
    if (*p) { *p = 0; p++; }
  }
  if (argc == 0) return;
  shargv[argc] = 0;
  dot = strchr(shargv[0], '.') != 0;
  found = 0;
  for (k = 0; k < 3 && !found; k++) {
    if (dot && k == 2) break;
    if (strlen(shargv[0]) > 150) break;
    path1[0] = 0;
    if (k == 2 || (dot && k == 1)) strcpy(path1, "/bin/");
    strcpy(path1 + strlen(path1), shargv[0]);
    if (!dot && k != 1) strcpy(path1 + strlen(path1), ".bin");
    if (stat(path1, &fst) == 0 && !fst.isdir) found = 1;
  }
  if (!found) { vm_err("File not found"); return; }
  out_flush();
  exec(path1, argc, shargv);
  out_col = 0;
}

void h_input() {
  int nv; int fl; int *types; char *pr; int ok; int fn;
  nv = fn_n; fl = code[pc]; types = code + pc + 1; pc = pc + 1 + nv;
  if (fl & 4) {                                            // INPUT #n
    fn = vs[sp]; sp--;
    if (file_ok(fn, 1)) file_input(fn, types, nv);
    return;
  }
  pr = 0;
  if (fl & 1) { pr = (char *)vs[sp]; vt[sp] = 0; sp--; }
  in_free(32);
  while (1) {
    out_str(pr, str_len(pr));
    if (fl & 2) out_cstr("? ");
    if (!read_line()) break;
    if (in_parse(types, nv) == 0) break;
    out_cstr("?Redo from start\n");
  }
  str_free(pr);
}

void h_lineinput() {
  int fl; char *pr; int fn; char *b; int len;
  fl = code[pc]; pc++;
  if (fl & 4) {                                            // LINE INPUT #n
    fn = vs[sp]; sp--;
    if (!file_ok(fn, 1)) return;
    b = fl_readline(fn, &len);
    if (!b) { if (running) vm_err("Input past end of file"); return; }
    in_free(1);
    in_val[0] = (int)str_from(b, len); in_str[0] = 1;
    free(b);
    return;
  }
  pr = 0;
  if (fl & 1) { pr = (char *)vs[sp]; vt[sp] = 0; sp--; }
  out_str(pr, str_len(pr));
  str_free(pr);
  if (!read_line()) return;
  in_free(1);
  in_val[0] = (int)str_from(linebuf, linen); in_str[0] = 1;
}

// pushes INPUT's value k; a str moves to the stack
void op_inget() {
  int k; int t;
  k = code[pc]; t = code[pc + 1]; pc = pc + 2;
  sp++;
  vs[sp] = in_val[k]; vt[sp] = 0;
  if (t == T_STR) { vt[sp] = 1; in_val[k] = 0; in_str[k] = 0; }
}

// ---------------------------------------------------------------- screen
void h_cls() { out_flush(); conctl(CON_CLEAR, 0, 0); out_col = 0; }

void h_locate() {
  int row; int col;
  col = vs[sp]; row = vs[sp - 1]; sp = sp - 2;
  if (row == -1) row = 1;                                  // left out
  if (col == -1) col = 1;
  if (row < 1 || col < 1 || row > 255 || col > 255) { vm_err("Illegal function call"); return; }
  out_flush();
  conctl(CON_GOTO, col - 1, row - 1);
  out_col = col - 1;
}

// QBasic's colour numbers (0 black, 1 blue, 2 green, 3 cyan, 4 red, 5 magenta, 6 brown,
// 7 white) to conctl's ANSI order
int ansi_color(int c) {
  char *map;
  map = "04261537";
  return map[c & 7] - '0';
}

void h_color() {
  int fg; int bg;
  bg = vs[sp]; fg = vs[sp - 1]; sp = sp - 2;
  if (fg > 31 || bg > 31 || fg < -1 || bg < -1) { vm_err("Illegal function call"); return; }
  if (fg >= 0) cur_fg = fg & 7;
  if (bg >= 0) cur_bg = bg & 7;
  out_flush();
  conctl(CON_COLOR, ansi_color(cur_fg), ansi_color(cur_bg));
}

void h_beep() { out_flush(); putchar(7); }

// TAB(n): to column n (1-based) of this line, or of the next if already past it
void h_tab() {
  int n; int cur;
  n = vs[sp]; sp--;
  if (n < 1) n = 1;
  if (n > 80) n = 80;
  cur = out_col + 1;
  if (n < cur) { out_char('\n'); cur = 1; }
  while (cur < n) { out_char(' '); cur++; }
}

void h_spc() {
  int n;
  n = vs[sp]; sp--;
  if (n < 0) n = 0;
  if (n > 255) n = 255;
  while (n > 0) { out_char(' '); n--; }
}

// WRITE item: fn_n is the item's type; 0 a comma, 9 the end of the line
void h_write() {
  char b[32]; int n; char *s;
  if (fn_n == 0) { out_char(','); return; }
  if (fn_n == 9) { out_char('\n'); return; }
  if (fn_n == T_INT) { n = fmt_int(b, vs[sp]); sp--; out_str(b, n); return; }
  if (fn_n == T_SNG) { n = fmt_sng(b, fs[sp]); sp--; out_str(b, n); return; }
  s = (char *)vs[sp]; vt[sp] = 0; sp--;
  out_char('"'); out_str(s, str_len(s)); out_char('"');
  str_free(s);
}

void st_write() {
  int t; int file;
  next();
  file = 0;
  if (TK == TK_HASH) {                                     // WRITE #n, items: to file n
    next(); expr_to(T_INT);
    emit_fn(K_PRINT, 0);
    file = 1;
    if (!at_end() && !need(TK_COMMA)) return;
  }
  if (!at_end()) {
    while (1) {
      t = expr();
      emit_fn(K_WRITE, t);
      if (!accept(TK_COMMA)) break;
      emit_fn(K_WRITE, 0);
    }
  }
  if (!at_end()) { bad_token(); return; }
  emit_fn(K_WRITE, 9);
  if (file) emit_fn(K_PRINT, 1);
}

// ---------------------------------------------------------------- statements
// An optional integer argument: its value, or -1 when it is left out
void opt_int() {
  if (TK == TK_COMMA || at_end()) { emit(OP_PUSHI); emit_arg(-1); }
  else expr_to(T_INT);
}

void st_cls() { next(); emit_fn(K_CLS, 0); }

void st_locate() {
  next();
  opt_int();
  if (accept(TK_COMMA)) opt_int(); else { emit(OP_PUSHI); emit_arg(-1); }
  emit_fn(K_LOCATE, 0);
  if (!at_end()) bad_token();
}

void st_color() {
  next();
  opt_int();
  if (accept(TK_COMMA)) opt_int(); else { emit(OP_PUSHI); emit_arg(-1); }
  emit_fn(K_COLOR, 0);
  if (!at_end()) bad_token();
}

void st_beep() { next(); emit_fn(K_BEEP, 0); }

void st_width() { next(); while (!at_end()) next(); }

// SLEEP [seconds]: a wait for a key when there is no number (or it is 0)
void st_sleep() {
  next();
  if (at_end()) { emit(OP_PUSHF); emit_arg(fbits(0.0)); }
  else expr_to(T_SNG);
  emit_fn(K_SLEEP, 0);
}

void h_sleep() {
  float f; int c;
  f = fs[sp]; sp--;
  if (f > 0) {
    if (f > 2000000.0) f = 2000000.0;
    wait_ms((int)(f * 1000.0), 1);
    return;
  }
  if (kqn > 0) return;                                     // the key stays queued
  c = key_wait();
  if (c == -2) brk_at(pc - 1);
  else if (c >= 0) kq_unshift(c);
}

void st_pause() {
  next();
  expr_to(T_INT);
  emit_fn(K_PAUSE, 0);
}

void h_pause() {
  int ms;
  ms = vs[sp]; sp--;
  if (ms < 0) { vm_err("Illegal function call"); return; }
  wait_ms(ms, 0);
}

// PINMODE pin, mode / PINWRITE pin, value / PWM pin, value
void st_pin2() {
  int k;
  k = TK; next();
  expr_to(T_INT);
  if (!need(TK_COMMA)) return;
  expr_to(T_INT);
  emit_fn(k, 0);
}

void h_pinmode() { sp = sp - 2; gpio_mode(vs[sp + 1], vs[sp + 2]); }
void h_pinwrite() { sp = sp - 2; gpio_write(vs[sp + 1], vs[sp + 2]); }
void h_pwm() { sp = sp - 2; pwm_write(vs[sp + 1], vs[sp + 2]); }

int fn_pin() {
  int k;
  k = TK; next();
  if (!need(TK_LPAR)) return T_INT;
  expr_to(T_INT);
  if (!need(TK_RPAR)) return T_INT;
  emit_fn(k, 0);
  return T_INT;
}

void h_pinread() { vs[sp] = gpio_read(vs[sp]); }
void h_analogread() { vs[sp] = analog_read(vs[sp]); }

int fn_inkeys() { next(); emit_fn(K_INKEYS, 0); return T_STR; }

// ---------------------------------------------------------------- registration
void io_init() {
  cur_fg = 7; cur_bg = 0;
  kqn = 0;
  stmt_tab[K_CLS] = st_cls; fnop[K_CLS] = h_cls;
  stmt_tab[K_LOCATE] = st_locate; fnop[K_LOCATE] = h_locate;
  stmt_tab[K_COLOR] = st_color; fnop[K_COLOR] = h_color;
  stmt_tab[K_BEEP] = st_beep; fnop[K_BEEP] = h_beep;
  stmt_tab[K_WIDTH] = st_width;
  stmt_tab[K_SLEEP] = st_sleep; fnop[K_SLEEP] = h_sleep;
  stmt_tab[K_WRITE] = st_write; fnop[K_WRITE] = h_write;
  stmt_tab[K_PAUSE] = st_pause; fnop[K_PAUSE] = h_pause;
  stmt_tab[K_PINMODE] = st_pin2; fnop[K_PINMODE] = h_pinmode;
  stmt_tab[K_PINWRITE] = st_pin2; fnop[K_PINWRITE] = h_pinwrite;
  stmt_tab[K_PWM] = st_pin2; fnop[K_PWM] = h_pwm;
  fnop[K_TAB] = h_tab;
  fnop[K_SPC] = h_spc;
  fnop[K_INPUT] = h_input;
  fnop[K_LINE] = h_lineinput;
  fn_tab[K_INKEYS] = fn_inkeys; fnop[K_INKEYS] = h_inkeys;
  fn_tab[K_PINREAD] = fn_pin; fnop[K_PINREAD] = h_pinread;
  fn_tab[K_ANALOGREAD] = fn_pin; fnop[K_ANALOGREAD] = h_analogread;
  optab[OP_INGET] = op_inget;
  stmt_tab[K_OPEN] = st_open; fnop[K_OPEN] = h_open;
  stmt_tab[K_CLOSE] = st_close; fnop[K_CLOSE] = h_close;
  fnop[K_PRINT] = h_setout;
  stmt_tab[K_KILL] = st_str1; fnop[K_KILL] = h_kill;
  stmt_tab[K_MKDIR] = st_str1; fnop[K_MKDIR] = h_mkdir;
  stmt_tab[K_RMDIR] = st_str1; fnop[K_RMDIR] = h_rmdir;
  stmt_tab[K_CHDIR] = st_str1; fnop[K_CHDIR] = h_chdir;
  stmt_tab[K_PIPE] = st_str1; fnop[K_PIPE] = h_pipe;
  stmt_tab[K_SHELL] = st_str1; fnop[K_SHELL] = h_shell;
  stmt_tab[K_NAME] = st_name; fnop[K_NAME] = h_name;
  stmt_tab[K_FILES] = st_files; fnop[K_FILES] = h_files;
  fn_tab[K_EOF] = fn_eof; fnop[K_EOF] = h_eof;
  fn_tab[K_FREEFILE] = fn_freefile; fnop[K_FREEFILE] = h_freefile;
  fn_tab[K_COMMANDS] = fn_commands; fnop[K_COMMANDS] = h_commands;
}

// at the end of the program: strs an aborted INPUT left, the colours back to normal
void io_free() {
  int i;
  in_free(32);
  out_reset();
  files_close_all();
  if (cur_fg != 7 || cur_bg != 0) { conctl(CON_COLOR, 7, 0); cur_fg = 7; cur_bg = 0; }
}
