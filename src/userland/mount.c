// mount [host[:port] [dir] [name]] : show directory dir (default /) of fileserver.py on the PC
// at /mnt/name (default fs; name may also be written /mnt/name; up to 4 mounts).  mount alone
// lists them.  The mounts are kept in /etc/mount (see lib/mtab.c), which the kernel reads at boot.
#include "sys.h"
#include "net.c"
#include "mtab.c"
char host[64];
char dir[200];
char line[300];
int main(int argc, char **argv) {
  char h[64]; char nm[16]; char hs[64]; char ds[200]; char *c; char *name; int port; int r; int at;
  if (argc == 1) {
    if (mt_load() == 0) { printf("nothing mounted\n"); return 1; }
    at = 0;
    while ((at = mt_get(at, nm, hs, ds)) >= 0) printf("%s %s /mnt/%s\n", hs, ds, nm);
    return 0;
  }
  if (argc > 4) { printf("usage: mount [host[:port] [dir] [name]]\n"); return 1; }
  if (net_norm(h, argv[1]) < 0) return 1;
  strcpy(host, h); port = 7777;
  c = strchr(host, ':');
  if (c) { *c = 0; if (c[1]) port = atoi(c + 1); }
  if (argc < 3) strcpy(dir, "/");
  else {
    if (strlen(argv[2]) > 190) { printf("mount: directory name too long\n"); return 1; }
    if (argv[2][0] == '/') strcpy(dir, argv[2]); else sprintf(dir, "/%s", argv[2]);
  }
  name = "fs";
  if (argc == 4) name = mt_name(argv[3]);
  r = mountat(name, host, port, dir);
  if (r == MOUNT_NOTDIR) { printf("mount: %s: not a directory\n", argc > 2 ? argv[2] : "/"); return 1; }
  if (r == MOUNT_BUSY) { printf("mount: files under /mnt/%s are open\n", name); return 1; }
  if (r == MOUNT_FULL) { printf("mount: too many mounts (4)\n"); return 1; }
  if (r == MOUNT_BADNAME) { printf("mount: bad name %s\n", argv[3]); return 1; }
  if (r < 0) { printf("mount: cannot reach %s\n", argv[1]); return 1; }
  mt_load();
  sprintf(line, "%s %s %s\n", name, h, dir);
  if (mt_put(name, line) < 0) { printf("mount: cannot write /etc/mount\n"); return 1; }
  printf("%s %s /mnt/%s\n", h, dir, name);
  return 0;
}
