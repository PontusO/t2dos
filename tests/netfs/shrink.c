// a file shrinks on the server while it is open for reading: the read past its new end is an
// error, not zeros
#include "sys.h"
char a[16];
int main() {
  int r; int w; int n;
  r = open("/mnt/fs/shrink.bin", OPEN_READ);
  n = read(r, a, 10);
  w = open("/mnt/fs/shrink.bin", OPEN_WRITE);
  write(w, "x", 1);
  close(w);
  seek(r, 5000, SEEK_BEGIN);
  n = read(r, a, 10);
  printf("read %d\n", n);
  close(r);
  return 0;
}
