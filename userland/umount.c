// umount : remove the network filesystem from /mnt/fs (see mount)
#include "sys.h"
int main(int argc, char **argv) {
  if (argc != 1) { printf("usage: umount\n"); return 1; }
  if (mount("", 0, "") < 0) { printf("umount: files under /mnt/fs are open\n"); return 1; }
  unlink("/etc/mount");
  return 0;
}
