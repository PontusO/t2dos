// umount [name] : remove the network filesystem at /mnt/name (default fs; /mnt/name works too)
// and its line in /etc/mount (see mount)
#include "sys.h"
#include "mtab.c"
int main(int argc, char **argv) {
  char *name; int r;
  if (argc > 2) { printf("usage: umount [name]\n"); return 1; }
  name = "fs";
  if (argc == 2) name = mt_name(argv[1]);
  r = mountat(name, "", 0, "");
  if (r == MOUNT_BUSY) { printf("umount: files under /mnt/%s are open\n", name); return 1; }
  mt_load();
  if (mt_put(name, 0) < 0) printf("umount: cannot write /etc/mount\n");
  if (r == MOUNT_BADNAME) { printf("umount: %s is not mounted\n", name); return 1; }
  return 0;
}
