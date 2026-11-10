// main.c - BASIC's command line (`basic FILE [args]`), loading, error messages and the
// console output buffer.
#include "basic.h"

char *srcname;
int nerr;
int imm_mode;
int cmd_read;
int prog_argc;
char **prog_argv;

// ---------------------------------------------------------------- console output
// PRINT output collects here and goes out with write() at each end of line, when full, and
// before a message or the exit; out_col follows the cursor for ',' zones, out_row the
// console's row for LOCATE with the row left out (CLS and LOCATE set it, a new line adds
// one up to the last row, 24; until the first CLS or LOCATE it counts from row 0).
char outbuf[512];
int outn;
int out_col;
int out_row;
int out_fd;                      // 0: the console (fd 1), else a file PRINT # is writing

void out_flush() {
  if (outn > 0) write(out_fd ? out_fd : 1, outbuf, outn);
  outn = 0;
}

void out_char(int c) {
  outbuf[outn] = c; outn++;
  if (c == '\n' || c == '\r') {
    out_col = 0;
    if (c == '\n' && !out_fd && out_row < 24) out_row++;
  }
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

// ---------------------------------------------------------------- memory
// Every block the interpreter allocates comes from mem_alloc(). Out of memory is not a
// run-time error the program could get past: the message (with the line when a program line
// is compiling or running), the console back to normal, exit 1. The kernel's malloc takes
// PSRAM and only when that is full falls back to the SRAM that programs are loaded into, so
// BASIC stops while MEM_MARGIN bytes of PSRAM are still free. mem_left is the free PSRAM
// at the last look (sysinfo) less what was allocated since (blocks freed are not counted):
// a new look comes after MEM_LOOK bytes, or when the estimate says the block would not fit.
// A board without PSRAM (total 0) has no budget, only malloc's own answer.
#define MEM_MARGIN 65536
#define MEM_LOOK 32768
int mem_left;
int mem_since;
int mem_budget;                  // 1: PSRAM is there to keep a margin of
int compiling;                   // compile() is running: an error has toks[tp]'s line

void out_of_memory() {
  out_reset();
  con_restore();                 // the message in the normal colours
  nerr = 0;
  if (running) err_at(pc - 1, "Out of memory");
  else if (compiling && ntok > 0) err_line(toks[tp].line, "Out of memory");
  else {
    if (out_col > 0) out_char('\n');
    out_cstr("Out of memory\n");
    out_flush();
  }
  exit(1);
}

void mem_init() {
  mem_budget = sysinfo(INFO_PSRAM_TOTAL) > 0;
  mem_left = sysinfo(INFO_PSRAM_FREE); mem_since = 0;
}

void *mem_alloc(int n) {
  void *p;
  if (n < 0) out_of_memory();
  if (mem_budget && (mem_left - n < MEM_MARGIN || mem_since > MEM_LOOK)) {
    mem_left = sysinfo(INFO_PSRAM_FREE); mem_since = 0;
    if (mem_left - n < MEM_MARGIN) out_of_memory();
  }
  p = malloc(n);
  if (!p) out_of_memory();
  mem_left = mem_left - n - 16;  // 16: about what the allocator adds to a block
  mem_since = mem_since + n;
  return p;
}

// ---------------------------------------------------------------- loading
char *load_file(char *path, int *len) {
  struct stat_t st; int fd; char *buf; int got; int n;
  if (stat(path, &st) < 0 || st.isdir) return 0;
  fd = open(path, OPEN_READ);
  if (fd < 0) return 0;
  buf = mem_alloc(st.size + 1);
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
  keep = mem_alloc(64 * 4);
  memcpy(keep, kq, 64 * 4); nkeep = kqn;
  session_end();                  // (a RUN of a missing file also drops the variables)
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
  char *path; int quit; int k;
  imm_mode = 1; srcname = 0;
  out_cstr("T2DOS BASIC\n");
  quit = 0;
  while (!quit) {
    out_cstr("Ok\n");
    cmd_read = 1;
    k = read_line();
    cmd_read = 0;
    if (!k) break;
    if (linen == 0) continue;
    lex(linebuf, linen);
    nerr = 0;
    path = 0;
    for (k = 0; k < ntok; k++)    // procedures cannot live in a line that is dropped; RUN a file
      if ((toks[k].tk == K_SUB || toks[k].tk == K_FUNCTION || toks[k].tk == K_DECLARE) &&
          (k == 0 || toks[k - 1].tk == TK_COLON)) break;
    if (k < ntok) err_line(1, "Illegal in direct mode");
    else if (toks[0].tk == K_SYSTEM && toks[1].tk == TK_EOL) quit = 1;   // only as a whole line
    else if (toks[0].tk == K_RUN) {
      if (toks[1].tk == TK_STR && toks[2].tk == TK_EOL) {
        path = mem_alloc(str_len((char *)toks[1].val) + 1);
        memcpy(path, (char *)toks[1].val, str_len((char *)toks[1].val));
        path[str_len((char *)toks[1].val)] = 0;
      }
      else err_line(1, "Syntax error");
    }
    else {
      comp_line_reset();
      comp_line_mark();
      if (compile()) { vm_alloc(); vm_run(0); comp_line_settle(); }
      else comp_line_undo();      // its symbols and slots go: nothing of it ran
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
  mem_init();
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
