// mount [host[:port] [dir]] : show directory dir (default /) of fileserver.py on the PC at
// /mnt/fs.  mount alone shows the current one.  The setting is kept in /etc/mount, which the
// kernel reads at boot.
#include "sys.h"
#include "net.c"
char buf[300];
char host[64];
char dir[200];
int main(int argc, char **argv) {
  char h[64]; char *c; int port; int r; int fd;
  if (argc == 1) {
    fd = open("/etc/mount", OPEN_READ);
    if (fd < 0) { printf("nothing mounted\n"); return 1; }
    r = read(fd, buf, 299); close(fd);
    if (r < 0) r = 0;
    buf[r] = 0;
    c = strchr(buf, '\n');
    if (c) { *c = ' '; c = strchr(c, '\n'); if (c) *c = 0; }
    printf("%s\n", buf);
    return 0;
  }
  if (argc > 3) { printf("usage: mount [host[:port] [dir]]\n"); return 1; }
  if (net_norm(h, argv[1]) < 0) return 1;
  strcpy(host, h); port = 7777;
  c = strchr(host, ':');
  if (c) { *c = 0; if (c[1]) port = atoi(c + 1); }
  if (argc < 3) strcpy(dir, "/");
  else {
    if (strlen(argv[2]) > 190) { printf("mount: directory name too long\n"); return 1; }
    if (argv[2][0] == '/') strcpy(dir, argv[2]); else sprintf(dir, "/%s", argv[2]);
  }
  r = mount(host, port, dir);
  if (r == MOUNT_NOTDIR) { printf("mount: %s: not a directory\n", argc > 2 ? argv[2] : "/"); return 1; }
  if (r == MOUNT_BUSY) { printf("mount: files under /mnt/fs are open\n"); return 1; }
  if (r < 0) { printf("mount: cannot reach %s\n", argv[1]); return 1; }
  mkdir("/etc");
  fd = open("/etc/mount", OPEN_WRITE);
  if (fd < 0) { printf("mount: cannot write /etc/mount\n"); return 1; }
  write(fd, h, strlen(h)); write(fd, "\n", 1);
  write(fd, dir, strlen(dir)); write(fd, "\n", 1);
  close(fd);
  printf("%s %s\n", h, dir);
  return 0;
}
