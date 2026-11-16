// writes 20000 bytes to /mnt/fs/full.bin in one call: with a small cache the count comes back short
#include "sys.h"
int main() {
  int w; int n; char *b;
  b = malloc(20000);
  memset(b, 'z', 20000);
  w = open("/mnt/fs/full.bin", OPEN_WRITE);
  n = write(w, b, 20000);
  printf("wrote %d\n", n);
  printf("close %d\n", close(w));
  return 0;
}
