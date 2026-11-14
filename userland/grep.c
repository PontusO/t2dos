// grep [-i] [-v] [-n] [-c] pattern file... : prints the lines that match pattern.
// The pattern is a small regular expression: c matches itself, . any character,
// x* any number of x (also .*), ^ the start and $ the end of the line.
// -i ignores case, -v prints the lines that do not match, -n puts the line number in
// front, -c prints only the count.  With several files each line starts with "file:".
// Exit 0 when a line was selected, 1 when none was, 2 on an error.
#include "sys.h"
char buf[2048];
char line[1024];
int icase;

int low(int c) { if (c >= 'A' && c <= 'Z') return c + 32; return c; }
int same(int p, int c) { if (p == '.') return 1; if (icase) return low(p) == low(c); return p == c; }

int match_here();
// c* at the start of re: try the rest after 0, 1, 2 ... copies of c
int match_star(int c, char *re, char *s) {
  while (1) {
    if (match_here(re, s)) return 1;
    if (!*s || !same(c, *s)) return 0;
    s++;
  }
  return 0;
}
int match_here(char *re, char *s) {
  if (!re[0]) return 1;
  if (re[1] == '*') return match_star(re[0], re + 2, s);
  if (re[0] == '$' && !re[1]) return !*s;
  if (*s && same(re[0], *s)) return match_here(re + 1, s + 1);
  return 0;
}
int match(char *re, char *s) {
  if (re[0] == '^') return match_here(re + 1, s);
  while (1) {
    if (match_here(re, s)) return 1;
    if (!*s) return 0;
    s++;
  }
  return 0;
}

int main(int argc, char **argv) {
  int invert; int number; int count; int i; int j; int fd; int n; int k; int len; int lineno;
  int found; int err; int nfiles; int c; int hit; int sel;
  char *re; char *o; struct stat_t st;
  icase = 0; invert = 0; number = 0; count = 0;
  i = 1;
  while (i < argc && argv[i][0] == '-' && argv[i][1]) {
    o = argv[i] + 1;
    while (*o) {
      if (*o == 'i') icase = 1;
      else if (*o == 'v') invert = 1;
      else if (*o == 'n') number = 1;
      else if (*o == 'c') count = 1;
      else { printf("grep: unknown option -%c\n", *o); return 2; }
      o++;
    }
    i++;
  }
  if (argc - i < 2) { printf("usage: grep [-i] [-v] [-n] [-c] pattern file...\n"); return 2; }
  re = argv[i]; i++;
  nfiles = argc - i;
  found = 0; err = 0;
  for (j = i; j < argc; j++) {
    if (stat(argv[j], &st) != 0) { printf("grep: %s: not found\n", argv[j]); err = 1; continue; }
    if (st.isdir) { printf("grep: %s: is a directory\n", argv[j]); err = 1; continue; }
    fd = open(argv[j], OPEN_READ);
    if (fd < 0) { printf("grep: %s: cannot open\n", argv[j]); err = 1; continue; }
    len = 0; lineno = 0; sel = 0;
    while (1) {
      n = read(fd, buf, 2048);
      if (n < 0) { printf("grep: %s: read error\n", argv[j]); err = 1; break; }
      k = 0;
      while (k < n || (n == 0 && len > 0)) {
        if (n > 0) c = buf[k]; else c = '\n';      // a last line without a newline
        k++;
        if (c == '\r') continue;
        if (c != '\n' && len < 1023) { line[len] = c; len++; continue; }
        if (c != '\n') continue;                   // past 1023 characters: the rest is not seen
        line[len] = 0; lineno++;
        hit = match(re, line);
        if (hit != invert) {
          sel++;
          if (!count) {
            if (nfiles > 1) printf("%s:", argv[j]);
            if (number) printf("%d:", lineno);
            printf("%s\n", line);
          }
        }
        len = 0;
        if (n == 0) break;
      }
      if (n == 0) break;
    }
    close(fd);
    if (count) { if (nfiles > 1) printf("%s:", argv[j]); printf("%d\n", sel); }
    if (sel) found = 1;
  }
  if (err) return 2;
  return found ? 0 : 1;
}
