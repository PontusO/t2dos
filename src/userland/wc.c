// wc [-l] [-w] [-c] file... : counts lines, words and bytes, in that order.
// Without options it prints all three; with several files a "total" line follows.
// The columns are as wide as the total byte count, like GNU wc.  Exit 1 on an error.
#include "sys.h"
char buf[2048];
int nl[3];      // per file: lines, words, bytes
int tot[3];
int show[3];
int width;
char fmt[8];       // "%Nd" for the column width (printf has no %*d)

int digits(int v) { int d; d = 1; while (v >= 10) { v = v / 10; d++; } return d; }

int print_counts(int *c, char *name) {
  int k; int first;
  first = 1;
  for (k = 0; k < 3; k++) {
    if (!show[k]) continue;
    if (!first) printf(" ");
    printf(fmt, c[k]);
    first = 0;
  }
  printf(" %s\n", name);
  return 0;
}

int count_file(char *name) {
  int fd; int n; int k; int c; int inword;
  struct stat_t st;
  nl[0] = 0; nl[1] = 0; nl[2] = 0;
  if (stat(name, &st) != 0) { printf("wc: %s: not found\n", name); return -1; }
  if (st.isdir) { printf("wc: %s: is a directory\n", name); return -1; }
  fd = open(name, OPEN_READ);
  if (fd < 0) { printf("wc: %s: cannot open\n", name); return -1; }
  inword = 0;
  while ((n = read(fd, buf, 2048)) > 0) {
    nl[2] = nl[2] + n;
    for (k = 0; k < n; k++) {
      c = buf[k];
      if (c == '\n') nl[0]++;
      if (c == ' ' || c == '\n' || c == '\t' || c == '\r' || c == 11 || c == 12) inword = 0;
      else if (!inword) { inword = 1; nl[1]++; }
    }
  }
  close(fd);
  if (n < 0) { printf("wc: %s: read error\n", name); return -1; }
  return 0;
}

int main(int argc, char **argv) {
  int i; int j; int k; int any; int err; int nfiles; int nshow; int size;
  char *o; struct stat_t st;
  show[0] = 0; show[1] = 0; show[2] = 0; any = 0;
  i = 1;
  while (i < argc && argv[i][0] == '-' && argv[i][1]) {
    o = argv[i] + 1;
    while (*o) {
      if (*o == 'l') show[0] = 1;
      else if (*o == 'w') show[1] = 1;
      else if (*o == 'c') show[2] = 1;
      else { printf("wc: unknown option -%c\n", *o); return 1; }
      any = 1; o++;
    }
    i++;
  }
  if (i >= argc) { printf("usage: wc [-l] [-w] [-c] file...\n"); return 1; }
  if (!any) { show[0] = 1; show[1] = 1; show[2] = 1; }
  nfiles = argc - i;
  nshow = show[0] + show[1] + show[2];
  size = 0;                            // column width from the total size, as GNU wc does
  for (j = i; j < argc; j++) if (stat(argv[j], &st) == 0 && !st.isdir) size = size + st.size;
  width = digits(size);
  if (nshow == 1 && nfiles == 1) width = 1;
  sprintf(fmt, "%%%dd", width);
  tot[0] = 0; tot[1] = 0; tot[2] = 0; err = 0;
  for (j = i; j < argc; j++) {
    if (count_file(argv[j]) < 0) { err = 1; continue; }
    for (k = 0; k < 3; k++) tot[k] = tot[k] + nl[k];
    print_counts(nl, argv[j]);
  }
  if (nfiles > 1) print_counts(tot, "total");
  return err;
}
