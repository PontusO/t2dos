// tail [-n N | -N] file... : prints the last N lines (default 10) of each file.
// With several files each one starts with "==> name <==", as in GNU tail.
// It reads the file twice: once to count the lines, then from where the output starts.
// Exit 1 when a file cannot be read.
#include "sys.h"
char buf[2048];

int main(int argc, char **argv) {
  int i; int j; int lines; int fd; int n; int k; int err; int nfiles; int shown;
  int total; int last; int skip;
  struct stat_t st;
  lines = 10; i = 1;
  while (i < argc && argv[i][0] == '-' && argv[i][1]) {
    if (!strcmp(argv[i], "-n") && i + 1 < argc) { lines = atoi(argv[i + 1]); i = i + 2; }
    else if (argv[i][1] >= '0' && argv[i][1] <= '9') { lines = atoi(argv[i] + 1); i++; }
    else { printf("usage: tail [-n N] file...\n"); return 1; }
  }
  if (i >= argc || lines < 0) { printf("usage: tail [-n N] file...\n"); return 1; }
  nfiles = argc - i; err = 0; shown = 0;
  for (j = i; j < argc; j++) {
    if (stat(argv[j], &st) != 0) { printf("tail: %s: not found\n", argv[j]); err = 1; continue; }
    if (st.isdir) { printf("tail: %s: is a directory\n", argv[j]); err = 1; continue; }
    fd = open(argv[j], OPEN_READ);
    if (fd < 0) { printf("tail: %s: cannot open\n", argv[j]); err = 1; continue; }
    if (nfiles > 1) { if (shown) printf("\n"); printf("==> %s <==\n", argv[j]); }
    shown = 1;
    total = 0; last = '\n';                 // pass 1: count lines; a last line without '\n' counts
    while ((n = read(fd, buf, 2048)) > 0) {
      for (k = 0; k < n; k++) if (buf[k] == '\n') total++;
      last = buf[n - 1];
    }
    if (last != '\n') total++;
    skip = total - lines;                   // pass 2: skip that many lines, print the rest
    seek(fd, 0, SEEK_BEGIN);
    while ((n = read(fd, buf, 2048)) > 0) {
      k = 0;
      while (k < n && skip > 0) { if (buf[k] == '\n') skip--; k++; }
      if (k < n) write(1, buf + k, n - k);
    }
    close(fd);
  }
  return err;
}
