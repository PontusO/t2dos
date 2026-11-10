// io.c - the console and the board: the key queue, INPUT and LINE INPUT, INKEY$, and the
// screen (CLS, LOCATE, COLOR, BEEP, TAB, SPC, WRITE), SLEEP and the pin statements.
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

// Waits ms milliseconds, looking for Ctrl-C; with stop, a key (taken from the queue) ends
// the wait early.
void wait_ms(int ms, int stop) {
  int t0; int left;
  t0 = millis();
  while (1) {
    left = ms - (millis() - t0);
    if (left <= 0) return;
    if (key_poll() < 0) { brk_at(pc - 1); return; }
    if (stop && kqn > 0) { kq_pop(); return; }
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

void h_inkeys() {
  int c; int len; int letter; char *s;
  yield();
  sp++; vs[sp] = 0; vt[sp] = 0;
  if (key_poll() < 0) { brk_at(pc - 1); return; }
  if (kqn == 0) return;
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
// After an ESC, drop the rest of the key sequence (an arrow key) from a line being typed.
// Returns 0 on Ctrl-C.
int skip_esc() {
  int c;
  if (key_poll() < 0) { brk_at(pc - 1); return 0; }
  if (kqn > 0 && (kq[0] == '[' || kq[0] == 'O')) {
    kq_pop();
    while (kqn > 0) {
      c = kq_pop();
      if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || c == '~') break;
    }
  }
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

void h_input() {
  int nv; int fl; int *types; char *pr; int ok;
  nv = fn_n; fl = code[pc]; types = code + pc + 1; pc = pc + 1 + nv;
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
  int fl; char *pr;
  fl = code[pc]; pc++;
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

void h_color() {
  int fg; int bg;
  bg = vs[sp]; fg = vs[sp - 1]; sp = sp - 2;
  if (fg > 31 || bg > 31 || fg < -1 || bg < -1) { vm_err("Illegal function call"); return; }
  if (fg >= 0) cur_fg = fg & 7;
  if (bg >= 0) cur_bg = bg & 7;
  out_flush();
  conctl(CON_COLOR, cur_fg, cur_bg);
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
  int t;
  next();
  if (TK == TK_HASH) { bad_token(); return; }
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
  if (kqn > 0) { kq_pop(); return; }
  c = key_wait();
  if (c == -2) brk_at(pc - 1);
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
}

// at the end of the program: strs an aborted INPUT left, the colours back to normal
void io_free() {
  int i;
  in_free(32);
  if (cur_fg != 7 || cur_bg != 0) { conctl(CON_COLOR, 7, 0); cur_fg = 7; cur_bg = 0; }
}
