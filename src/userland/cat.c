#include "sys.h"
char buf[4096];
int main(int argc, char **argv) {
  int i; int fd; int n;
  for (i = 1; i < argc; i++) {
    fd = open(argv[i], OPEN_READ);
    if (fd < 0) { printf("cat: %s: not found\n", argv[i]); return 1; }
    while ((n = read(fd, buf, 4096)) > 0) write(1, buf, n);
    close(fd);
  }
  return 0;
}
