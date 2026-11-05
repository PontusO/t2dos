// write name : type lines into a file, end with a single "." on a line.
#include "sys.h"
char l[256];
int main(int argc, char **argv) {
  int fd; int len; int c;
  if (argc < 2) { printf("usage: write name\n"); return 1; }
  fd = open(argv[1], OPEN_WRITE);
  if (fd < 0) { printf("write: cannot write %s\n", argv[1]); return 1; }
  printf("enter text, end with a single . on a line\n");
  len = 0;
  while (1) {
    c = getchar();
    if (c == '\r' || c == '\n') {
      l[len] = 0; putchar('\n');
      if (!strcmp(l, ".")) break;
      l[len++] = '\n'; write(fd, l, len); len = 0;
    }
    else if ((c == 8 || c == 127) && len) { len--; printf("\b \b"); }
    else if (len < 254 && (c >= 32 || c == 9)) { l[len++] = c; putchar(c); }
  }
  close(fd);
  return 0;
}
