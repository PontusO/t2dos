// main.c - BASIC's command line (`basic FILE [args]`), loading, error messages and the
// console output buffer.
#include "basic.h"

char *srcname;
int nerr;
int imm_mode;
int prog_argc;
char **prog_argv;

// ---------------------------------------------------------------- console output
// PRINT output collects here and goes out with write() at each end of line, when full, and
// before a message or the exit; out_col follows the cursor for ',' zones.
char outbuf[512];
int outn;
int out_col;
int out_fd;                      // 0: the console (fd 1), else a file PRINT # is writing

void out_flush() {
  if (outn > 0) write(out_fd ? out_fd : 1, outbuf, outn);
  outn = 0;
}

void out_char(int c) {
  outbuf[outn] = c; outn++;
  if (c == '\n' || c == '\r') out_col = 0;
  else if (c == 8) { if (out_col > 0) out_col--; }
  else out_col++;
  if (c == '\n' || outn >= 512) out_flush();
}

void out_str(char *s, int n) {
  int i;
  for (i = 0; i < n; i++) out_char(s[i]);
}

void out_cstr(char *s) { out_str(s, strlen(s)); }

// ---------------------------------------------------------------- errors
// The first error is reported as FILE:LINE: MESSAGE on a line of its own (after the output
// so far); it ends compiling (the parser runs to the end of the tokens without further
// messages) and running (the VM loop stops).
int err_line(int line, char *msg) {
  char num[16];
  out_reset();
  if (nerr == 0) {
    if (out_col > 0) out_char('\n');
    fmt_int(num, line);
    if (imm_mode) { out_cstr("LINE "); out_cstr(num); }
    else { out_cstr(srcname); out_char(':'); out_cstr(num); }
    out_cstr(": ");
    out_cstr(msg); out_char('\n');
    out_flush();
  }
  nerr++;
  running = 0;
  if (ntok > 0) tp = ntok - 1;    // the compiler sees TK_EOF from here on
  return 0;
}

int err_at(int pos, char *msg) { return err_line(line_of(pos), msg); }

int brk_at(int pos) {
  char num[16];
  out_reset();
  if (nerr == 0) {
    if (out_col > 0) out_char('\n');
    if (imm_mode) out_cstr("Break");
    else {
      out_cstr("Break at "); out_cstr(srcname); out_char(':');
      fmt_int(num, line_of(pos)); out_cstr(num);
    }
    out_char('\n');
    out_flush();
  }
  nerr++;
  running = 0;
  return 0;
}

// ---------------------------------------------------------------- loading
char *load_file(char *path, int *len) {
  struct stat_t st; int fd; char *buf; int got; int n;
  if (stat(path, &st) < 0 || st.isdir) return 0;
  fd = open(path, OPEN_READ);
  if (fd < 0) return 0;
  buf = malloc(st.size + 1);
  got = 0;
  while (got < st.size) {
    n = read(fd, buf + got, st.size - got);
    if (n <= 0) break;
    got = got + n;
  }
  close(fd);
  *len = got;
  return buf;
}

// ---------------------------------------------------------------- sessions
// Everything the interpreter owns is made by session_start() and given back by
// session_end(); RUN in immediate mode ends one and starts the next, so a program begins
// with fresh variables.
void session_start() {
  lex_init(); vm_init(); comp_init(); funcs_init(); io_init();
  nerr = 0;
}

void session_end() {
  out_reset();
  if (out_col > 0) out_char('\n');   // the shell's prompt starts on a line of its own
  out_flush();
  io_free();
  vm_free(); comp_free(); lex_free();
}

// Loads, compiles and runs the file srcname in the current session; returns the exit code.
int run_program() {
  char *src; int len;
  src = load_file(srcname, &len);
  if (!src) { out_cstr(srcname); out_cstr(": File not found\n"); out_flush(); return 1; }
  lex(src, len);
  free(src);
  if (compile()) {
    vm_alloc();
    vm_run(0);
  }
  return nerr != 0;
}

// ---------------------------------------------------------------- immediate mode
// `basic` alone: each line is compiled and run at once against the variables of the lines
// before (the symbols and the slot store stay, the code, labels and stacks do not). RUN "file"
// runs a program with fresh variables and comes back; SYSTEM leaves.
void run_file(char *path) {
  int *keep; int nkeep;
  keep = malloc(64 * 4);
  memcpy(keep, kq, 64 * 4); nkeep = kqn;
  session_end();
  session_start();
  imm_mode = 0; srcname = path;
  run_program();
  session_end();
  session_start();
  imm_mode = 1; srcname = 0;
  memcpy(kq, keep, 64 * 4); kqn = nkeep;
  free(keep);
}

int immediate() {
  char *path; int quit;
  imm_mode = 1; srcname = 0;
  out_cstr("T2DOS BASIC\n");
  quit = 0;
  while (!quit) {
    out_cstr("Ok\n");
    if (!read_line()) break;
    if (linen == 0) continue;
    lex(linebuf, linen);
    nerr = 0;
    path = 0;
    if (toks[0].tk == K_SYSTEM && toks[1].tk == TK_EOL) quit = 1;
    else if (toks[0].tk == K_RUN) {
      if (toks[1].tk == TK_STR && toks[2].tk == TK_EOL) {
        path = malloc(str_len((char *)toks[1].val) + 1);
        memcpy(path, (char *)toks[1].val, str_len((char *)toks[1].val));
        path[str_len((char *)toks[1].val)] = 0;
      }
      else err_line(1, "Syntax error");
    }
    else {
      comp_line_reset();
      if (compile()) { vm_alloc(); vm_run(0); }
      else vm_alloc();            // the slots a failed line made are still symbols
      vm_line_reset();
      io_line_reset();
    }
    lex_free_toks();
    if (out_col > 0) out_char('\n');
    out_flush();
    if (path) { run_file(path); free(path); }
  }
  return 0;
}

int main(int argc, char **argv) {
  int code;
  session_start();
  if (argc < 2) code = immediate();
  else {
    srcname = argv[1];
    prog_argc = argc - 2; prog_argv = argv + 2;
    code = run_program();
  }
  session_end();
  return code;
}
