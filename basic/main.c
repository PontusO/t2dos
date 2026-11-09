// main.c - BASIC's command line (`basic FILE [args]`), loading, error messages and the
// console output buffer.
#include "basic.h"

char *srcname;
int nerr;
int prog_argc;
char **prog_argv;

// ---------------------------------------------------------------- console output
// PRINT output collects here and goes out with write() at each end of line, when full, and
// before a message or the exit; out_col follows the cursor for ',' zones.
char outbuf[512];
int outn;
int out_col;

void out_flush() {
  if (outn > 0) write(1, outbuf, outn);
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
  if (nerr == 0) {
    if (out_col > 0) out_char('\n');
    out_cstr(srcname); out_char(':');
    fmt_int(num, line); out_cstr(num); out_cstr(": ");
    out_cstr(msg); out_char('\n');
    out_flush();
  }
  nerr++;
  running = 0;
  if (ntok > 0) tp = ntok - 1;    // the compiler sees TK_EOF from here on
  return 0;
}

int err_at(int pos, char *msg) { return err_line(line_of(pos), msg); }

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

int main(int argc, char **argv) {
  char *src; int len;
  if (argc < 2) { out_cstr("usage: basic FILE [args]\n"); out_flush(); return 1; }
  srcname = argv[1];
  prog_argc = argc - 2; prog_argv = argv + 2;
  src = load_file(srcname, &len);
  if (!src) { out_cstr(srcname); out_cstr(": File not found\n"); out_flush(); return 1; }
  lex_init(); vm_init(); comp_init(); funcs_init();
  lex(src, len);
  free(src);
  if (compile()) {
    vm_alloc();
    vm_run(0);
  }
  if (out_col > 0) out_char('\n');   // the shell's prompt starts on a line of its own
  out_flush();
  vm_free(); comp_free(); lex_free();
  if (nerr) return 1;
  return 0;
}
