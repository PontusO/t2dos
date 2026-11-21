// head [-n N | -N] file... : prints the first N lines (default 10) of each file.
// With several files each one starts with "==> name <==", as in GNU head.
// Exit 1 when a file cannot be read.
#include "sys.h"
char buf[2048];

int main(int argc, char **argv) {
  int i; int j; int lines; int fd; int n; int k; int left; int err; int nfiles; int shown;
  struct stat_t st;
  lines = 10; i = 1;
  while (i < argc && argv[i][0] == '-' && argv[i][1]) {
    if (!strcmp(argv[i], "-n") && i + 1 < argc) { lines = atoi(argv[i + 1]); i = i + 2; }
    else if (argv[i][1] >= '0' && argv[i][1] <= '9') { lines = atoi(argv[i] + 1); i++; }
    else { printf("usage: head [-n N] file...\n"); return 1; }
  }
  if (i >= argc || lines < 0) { printf("usage: head [-n N] file...\n"); return 1; }
  nfiles = argc - i; err = 0; shown = 0;
  for (j = i; j < argc; j++) {
    if (stat(argv[j], &st) != 0) { printf("head: %s: not found\n", argv[j]); err = 1; continue; }
    if (st.isdir) { printf("head: %s: is a directory\n", argv[j]); err = 1; continue; }
    fd = open(argv[j], OPEN_READ);
    if (fd < 0) { printf("head: %s: cannot open\n", argv[j]); err = 1; continue; }
    if (nfiles > 1) { if (shown) printf("\n"); printf("==> %s <==\n", argv[j]); }
    shown = 1;
    left = lines;
    while (left > 0 && (n = read(fd, buf, 2048)) > 0) {
      k = 0;
      while (k < n && left > 0) { if (buf[k] == '\n') left--; k++; }
      write(1, buf, k);
    }
    close(fd);
  }
  return err;
}
