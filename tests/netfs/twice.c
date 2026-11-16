// the same /mnt/fs file open twice: the reader keeps the blocks it has read while a writer
// replaces the file; a new open sees the new content
#include "sys.h"
char a[64];
int main() {
  int r; int w; int n;
  r = open("/mnt/fs/t.txt", OPEN_READ);
  n = read(r, a, 63); a[n > 0 ? n : 0] = 0;
  printf("reader: %s", a);
  w = open("/mnt/fs/t.txt", OPEN_WRITE);
  write(w, "new\n", 4);
  printf("close %d\n", close(w));
  seek(r, 0, SEEK_BEGIN);
  n = read(r, a, 63); a[n > 0 ? n : 0] = 0;
  printf("reader again: %s", a);
  close(r);
  r = open("/mnt/fs/t.txt", OPEN_READ);
  n = read(r, a, 63); a[n > 0 ? n : 0] = 0;
  close(r);
  printf("new open: %s", a);
  return 0;
}
