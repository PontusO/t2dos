// edit.c - full-screen text editor for an ANSI/VT100 terminal (screen, minicom, picocom).
//   edit file
// Keys: arrows, Home End PgUp PgDn, Backspace Del Enter Tab
//       ^O save   ^X quit   ^F find (^F again, Enter: next match)   ^S also saves, ^Q also quits
//       ^B mark (again: clear)   ^K cut   ^C copy   ^U paste
//       With a mark, ^K and ^C take the text from the mark to the cursor, both included; without one,
//       the current line (repeated ^K or ^C collect the following lines into one block).
// ^O is the advertised save key: with flow control on (screen's default for serial lines)
// the terminal swallows ^S as XOFF and freezes its output until ^Q.
// Enter copies the indentation of the current line, except while text is being pasted.
// The window size comes from the terminal (cursor position report), else 80x24.
#include "sys.h"

enum { K_UP = 1000, K_DOWN, K_RIGHT, K_LEFT, K_HOME, K_END, K_PGUP, K_PGDN, K_DEL, K_NONE };

char *b; int cap; int gs; int ge;      // gap buffer: text is b[0..gs) followed by b[ge..cap)
int cur; int want;                     // cursor offset in the text, preferred display column
int top; int left;                     // offset of the first line shown, first column shown
int H; int rows; int cols;             // screen height, text rows (H - 1), screen width
int dirty; int quitarm;
int mark;                              // the mark's text offset, -1 when there is none
char *clip; int clen; int ccap;        // the cut/copy buffer
int cliplines;                         // 1 when the last ^K/^C took a whole line (not a block)
char *fname;
char msg[128];
char ob[4096]; int on;                 // output buffer, one write() per screen update
char findbuf[64];
char num[16];
char line[256];
char indent[64];

int flush() { if (on) write(1, ob, on); on = 0; return 0; }
int outc(int c) { if (on >= 4000) flush(); ob[on++] = c; return 0; }
int out(char *s) { while (*s) { outc(*s); s++; } return 0; }
int outn(int v) { sprintf(num, "%d", v); return out(num); }
int csi(char *s) { outc(27); outc('['); return out(s); }
int gotoxy(int r, int c) { outc(27); outc('['); outn(r + 1); outc(';'); outn(c + 1); outc('H'); return 0; }

// ------------------------------------------------------------------ text
int len() { return gs + cap - ge; }
int at(int i) { if (i < gs) return b[i]; return b[i + ge - gs]; }
int gap_to(int pos) {
  while (gs > pos) { ge--; gs--; b[ge] = b[gs]; }
  while (gs < pos) { b[gs] = b[ge]; gs++; ge++; }
  return 0;
}
int ins(int c) {                       // every edit clears the mark
  if (gs == ge) { strcpy(msg, "buffer full"); return 0; }
  gap_to(cur); b[gs] = c; gs++; cur++; dirty = 1; mark = -1;
  return 1;
}
int del(int pos) {                     // delete the character at pos, cursor goes to pos
  if (pos < 0 || pos >= len()) return 0;
  gap_to(pos); ge++; cur = pos; dirty = 1; mark = -1;
  return 1;
}
int lstart(int p) { while (p > 0 && at(p - 1) != '\n') p--; return p; }
int lend(int p) { int n; n = len(); while (p < n && at(p) != '\n') p++; return p; }
int cwidth(int c, int col) { if (c == '\t') return 8 - col % 8; return 1; }
int col_of(int p) {
  int i; int col;
  i = lstart(p); col = 0;
  while (i < p) { col += cwidth(at(i), col); i++; }
  return col;
}
int pos_at_col(int p, int col) {       // p is a line start
  int c; int x; int n;
  n = len(); x = 0;
  while (p < n) {
    c = at(p);
    if (c == '\n' || x + cwidth(c, x) > col) break;
    x += cwidth(c, x); p++;
  }
  return p;
}
int up() { int ls; ls = lstart(cur); if (ls == 0) return 0; cur = pos_at_col(lstart(ls - 1), want); return 1; }
int down() { int le; le = lend(cur); if (le >= len()) return 0; cur = pos_at_col(le + 1, want); return 1; }
int more_below() {                     // is there text below the last screen row?
  int p; int r; int n;
  n = len(); p = top; r = 0;
  while (r < rows) { p = lend(p); if (p >= n) return 0; p++; r++; }
  return 1;
}
// the marked block [s, e): sel_s()/sel_e(), only while mark >= 0. Both ends are in it: the
// character at the mark and the one under the cursor (none past the end of the text)
int sel_s() { if (mark < cur) return mark; return cur; }
int sel_e() { int e; e = mark; if (mark < cur) e = cur; if (e < len()) e++; return e; }
int lineno() { int i; int n; n = 1; i = 0; while (i < cur) { if (at(i) == '\n') n++; i++; } return n; }

// ------------------------------------------------------------------ screen
int scroll() {                         // bring the cursor into view, return its screen row
  int p; int n; int c;
  if (cur < top) top = lstart(cur);
  n = 0; p = top;
  while (1) { p = lend(p); if (p >= cur) break; p++; n++; }
  while (n >= rows) { top = lend(top) + 1; n--; }
  c = col_of(cur);
  if (c < left) left = c;
  if (c >= left + cols) left = c - cols + 1;
  return n;
}

int status() {
  int n; int i;
  gotoxy(rows, 0); csi("7m");
  sprintf(line, " %s%s  line %d col %d   ", fname, dirty ? " [modified]" : "", lineno(), col_of(cur) + 1);
  n = strlen(line);
  if (msg[0]) { i = 0; while (msg[i] && n < 250) { line[n] = msg[i]; n++; i++; } }
  else { strcpy(line + n, mark >= 0 ? "^K cut  ^C copy  ^B clear mark" : "^O save ^X quit ^F find ^B mark ^K cut ^C copy ^U paste"); n = strlen(line); }
  i = 0;
  while (i < cols - 1) { outc(i < n ? line[i] : ' '); i++; }
  csi("0m");
  return 0;
}

int draw() {
  int r; int p; int n; int x; int c; int w; int k; int cy; int more; int s0; int s1; int hl; int on2;
  cy = scroll();
  s0 = -1; s1 = -1; if (mark >= 0) { s0 = sel_s(); s1 = sel_e(); }
  csi("?25l");
  n = len(); p = top; more = 1;
  r = 0;
  while (r < rows) {
    gotoxy(r, 0); csi("K");
    if (more) {
      x = 0; hl = 0;
      while (p < n && (c = at(p)) != '\n') {
        w = cwidth(c, x);
        k = x;
        on2 = p >= s0 && p < s1;       // inside the marked block: reverse video
        if (on2 != hl) { csi(on2 ? "7m" : "0m"); hl = on2; }
        while (k < x + w) {
          if (k >= left && k < left + cols) {
            if (c == '\t') outc(' ');
            else if (c < 32 || c == 127) outc('?');
            else outc(c);
          }
          k++;
        }
        x += w; p++;
      }
      if (hl) csi("0m");
      if (p >= n) more = 0; else p++;
    }
    else { csi("2m"); outc('~'); csi("0m"); }
    r++;
  }
  status();
  gotoxy(cy, col_of(cur) - left); csi("?25h");
  flush();
  return 0;
}

// ------------------------------------------------------------------ keyboard
int waitkey(int ms) {
  int t;
  t = millis();
  while (!kbhit()) { if (millis() - t > ms) return -1; }
  return getchar();
}
int letterkey(int c) {
  if (c == 'A') return K_UP;
  if (c == 'B') return K_DOWN;
  if (c == 'C') return K_RIGHT;
  if (c == 'D') return K_LEFT;
  if (c == 'H') return K_HOME;
  if (c == 'F') return K_END;
  return K_NONE;
}
int getkey() {                         // ESC [ x, ESC O x, ESC [ n ~, ESC [ n ; m x
  int c; int n;
  c = getchar();
  if (c != 27) return c;
  c = waitkey(50);
  if (c < 0) return 27;
  if (c != '[' && c != 'O') return K_NONE;
  c = waitkey(50);
  if (c < '0' || c > '9') return letterkey(c);
  n = 0;
  while (c >= '0' && c <= '9') { n = n * 10 + c - '0'; c = waitkey(50); }
  while (c == ';' || (c >= '0' && c <= '9')) c = waitkey(50);    // modifiers
  if (c != '~') return letterkey(c);
  if (n == 1 || n == 7) return K_HOME;
  if (n == 4 || n == 8) return K_END;
  if (n == 3) return K_DEL;
  if (n == 5) return K_PGUP;
  if (n == 6) return K_PGDN;
  return K_NONE;
}

int term_size() {                      // move far down-right, ask where the cursor ended up
  int c; int r; int k;
  k = sysinfo(INFO_CONSOLE); cols = k & 65535; H = k >> 16;
  if (cols < 20) cols = 80;
  if (H < 4) H = 24;
  csi("999;999H"); csi("6n"); flush();
  if (waitkey(500) != 27) return 0;
  if (waitkey(100) != '[') return 0;
  r = 0; while ((c = waitkey(100)) >= '0' && c <= '9') r = r * 10 + c - '0';
  if (c != ';') return 0;
  k = 0; while ((c = waitkey(100)) >= '0' && c <= '9') k = k * 10 + c - '0';
  if (c == 'R' && r >= 4 && k >= 20) { H = r; cols = k; }
  return 0;
}

// ------------------------------------------------------------------ commands
int save() {
  int fd; int w; int n;
  fd = open(fname, OPEN_WRITE);
  if (fd < 0) { sprintf(msg, "cannot write %s", fname); return 0; }
  w = 0; n = len();
  if (gs > 0) w += write(fd, b, gs);
  if (cap > ge) w += write(fd, b + ge, cap - ge);
  close(fd);
  if (w != n) { sprintf(msg, "write error: %d of %d bytes", w, n); return 0; }
  dirty = 0; sprintf(msg, "saved %d bytes", n);
  return 1;
}

int prompt(char *label) {              // edit findbuf on the status line; 1 = Enter, 0 = cancel
  int c; int n;
  n = strlen(findbuf);
  while (1) {
    gotoxy(rows, 0); csi("0m"); csi("K"); out(label); out(findbuf); flush();
    c = getkey();
    if (c == 13 || c == 10) return 1;
    if (c == 27 || c == 3 || c == 24 || c == 7) return 0;
    if ((c == 127 || c == 8) && n > 0) { n--; findbuf[n] = 0; }
    else if (c >= 32 && c < 127 && n < 62) { findbuf[n] = c; n++; findbuf[n] = 0; }
  }
}
int find_from(int start) {
  int n; int m; int i; int j;
  n = len(); m = strlen(findbuf);
  if (!m) return -1;
  i = start;
  while (i + m <= n) {
    j = 0;
    while (j < m && at(i + j) == findbuf[j]) j++;
    if (j == m) return i;
    i++;
  }
  return -1;
}
int find() {
  int p;
  if (!prompt("find: ")) { strcpy(msg, "cancelled"); return 0; }
  p = find_from(cur + 1);
  if (p < 0) p = find_from(0);
  if (p < 0) { sprintf(msg, "not found: %s", findbuf); return 0; }
  cur = p; want = col_of(cur);
  return 1;
}

// put text [s, e) in the clipboard, after what is there when append is set
int clip_put(int s, int e, int append) {
  int n; char *nb;
  if (!append) clen = 0;
  n = clen + e - s;
  if (n > ccap) {
    nb = malloc(n + 1024);
    if (!nb) { strcpy(msg, "out of memory"); return 0; }
    if (clen) memcpy(nb, clip, clen);
    if (clip) free(clip);
    clip = nb; ccap = n + 1024;
  }
  while (s < e) { clip[clen] = at(s); clen++; s++; }
  return 1;
}
// the current line with its newline: [lstart, end)
int line_end() { int e; e = lend(cur); if (e < len()) e++; return e; }
// ^K and ^C: the marked block, else the current line (again right after: the next line too)
int cut(int append) {
  int s; int e;
  cliplines = mark < 0;
  if (mark >= 0) { s = sel_s(); e = sel_e(); append = 0; }
  else { s = lstart(cur); e = line_end(); }
  if (s == e) { strcpy(msg, "nothing to cut"); return 0; }
  if (!clip_put(s, e, append)) return 0;
  gap_to(s); ge += e - s; cur = s; dirty = 1; mark = -1;
  sprintf(msg, "cut %d bytes", clen);
  return 1;
}
int copy(int append) {
  int s; int e;
  cliplines = mark < 0;
  if (mark >= 0) { s = sel_s(); e = sel_e(); append = 0; }
  else { s = lstart(cur); e = line_end(); }
  if (s == e) { strcpy(msg, "nothing to copy"); return 0; }
  if (!clip_put(s, e, append)) return 0;
  if (mark >= 0) mark = -1;
  else cur = e;                        // to the next line, so ^C again takes it too
  sprintf(msg, "copied %d bytes", clen);
  return 1;
}
int paste() {
  int i;
  if (!clen) { strcpy(msg, "nothing to paste"); return 0; }
  if (clen > ge - gs) { strcpy(msg, "buffer full"); return 0; }
  i = 0; while (i < clen) { ins(clip[i]); i++; }
  return 1;
}

int newline() {                        // Enter: break the line, keep the indentation unless pasting
  int ls; int n; int i;
  ls = lstart(cur); n = 0;
  if (!kbhit()) while (ls + n < cur && n < 63 && (at(ls + n) == ' ' || at(ls + n) == '\t')) { indent[n] = at(ls + n); n++; }
  if (!ins('\n')) return 0;
  i = 0; while (i < n) { ins(indent[i]); i++; }
  return 1;
}

int main(int argc, char **argv) {
  int fd; int n; int k; int i; int size; int lastcr; int lastk; struct stat_t st;
  if (argc < 2) { printf("usage: edit file\n"); return 1; }
  fname = argv[1];
  size = 0; msg[0] = 0;
  if (stat(fname, &st) == 0) {
    if (st.isdir) { printf("edit: %s is a directory\n", fname); return 1; }
    size = st.size;
  }
  else strcpy(msg, "new file");
  cap = size + 262144;
  b = malloc(cap);
  if (!b) { printf("edit: out of memory\n"); return 1; }
  gs = 0;
  if (size > 0) {
    fd = open(fname, OPEN_READ);
    if (fd < 0) { printf("edit: cannot read %s\n", fname); return 1; }
    while (gs < size && (n = read(fd, b + gs, size - gs)) > 0) gs += n;
    close(fd);
  }
  ge = cap; cur = 0; want = 0; top = 0; left = 0; dirty = 0; quitarm = 0; lastcr = 0; lastk = 0;
  mark = -1; clip = 0; clen = 0; ccap = 0; cliplines = 0;

  csi("?1049h");                       // alternate screen: the shell's screen comes back on exit
  term_size();
  rows = H - 1;
  while (1) {
    draw();
    k = getkey();
    if (k != 24 && k != 17) quitarm = 0;
    if (k != K_NONE) msg[0] = 0;
    if (k == 24 || k == 17) {             // ^X ^Q
      if (dirty && !quitarm) { strcpy(msg, "unsaved changes, press again to quit"); quitarm = 1; }
      else break;
    }
    else if (k == 15 || k == 19) save();          // ^O ^S
    else if (k == 6) find();                      // ^F
    else if (k == 2) {                            // ^B
      if (mark >= 0) { mark = -1; strcpy(msg, "mark cleared"); }
      else { mark = cur; strcpy(msg, "mark set"); }
    }
    else if (k == 11) { cut(lastk == 11 && cliplines); want = col_of(cur); }     // ^K
    else if (k == 3) { copy(lastk == 3 && cliplines); want = col_of(cur); }      // ^C
    else if (k == 21) { paste(); want = col_of(cur); }              // ^U
    else if (k == 13) { newline(); want = col_of(cur); }
    else if (k == 10) { if (!lastcr) newline(); want = col_of(cur); }
    else if (k == 127 || k == 8) { if (cur > 0) del(cur - 1); want = col_of(cur); }
    else if (k == K_DEL) { del(cur); want = col_of(cur); }
    else if (k == 9 || (k >= 32 && k < 127) || (k >= 128 && k < 256)) { ins(k); want = col_of(cur); }
    else if (k == K_LEFT) { if (cur > 0) cur--; want = col_of(cur); }
    else if (k == K_RIGHT) { if (cur < len()) cur++; want = col_of(cur); }
    else if (k == K_UP) up();
    else if (k == K_DOWN) down();
    else if (k == K_HOME) { cur = lstart(cur); want = 0; }
    else if (k == K_END) { cur = lend(cur); want = col_of(cur); }
    else if (k == K_PGUP) { i = 1; while (i < rows) { if (!up()) break; if (top > 0) top = lstart(top - 1); i++; } }
    else if (k == K_PGDN) { i = 1; while (i < rows) { if (!down()) break; if (more_below()) top = lend(top) + 1; i++; } }
    lastcr = k == 13;
    if (k != K_NONE) lastk = k;
  }
  csi("2J"); csi("H"); csi("?1049l"); csi("?25h"); flush();
  return 0;
}
