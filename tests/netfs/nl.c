// a newline in a /mnt/fs name must not reach the server as a second request
#include "sys.h"
int main() {
  struct stat_t st;
  printf("stat %d\n", stat("/mnt/fs/x\nRM victim.txt", &st));
  printf("open %d\n", open("/mnt/fs/y\nRM victim.txt", OPEN_WRITE));
  printf("mkdir %d\n", mkdir("/mnt/fs/z\rRM victim.txt"));
  return 0;
}
