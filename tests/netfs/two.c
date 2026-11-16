// two PORT1 PORT2 : several mounts at once (two fileserver.py's on 127.0.0.1); one line per check
#include "sys.h"
char buf[64];
int put(char *name, char *text) {
  int fd = open(name, OPEN_WRITE);
  if (fd < 0) return -9;
  write(fd, text, strlen(text));
  return close(fd);
}
int get(char *name) {
  int fd = open(name, OPEN_READ); int n;
  buf[0] = 0;
  if (fd < 0) return -1;
  n = read(fd, buf, 63); if (n < 0) n = 0; buf[n] = 0;
  close(fd);
  return 0;
}
int main(int argc, char **argv) {
  int p1; int p2; int fd; int n; int a; int b; int c; struct stat_t st;
  if (argc != 3) { printf("usage: two port1 port2\n"); return 2; }
  p1 = atoi(argv[1]); p2 = atoi(argv[2]);
  printf("a %d\n", mountat("a", "127.0.0.1", p1, "/"));
  printf("b %d\n", mountat("b", "127.0.0.1", p2, "/"));
  a = put("/mnt/a/x.txt", "one\n"); b = put("/mnt/b/y.txt", "two\n");
  printf("w %d %d\n", a, b);
  printf("mv %d\n", rename("/mnt/a/x.txt", "/mnt/b/x.txt"));
  mkdir("/mnt/a/d1");
  printf("mvdir %d\n", rename("/mnt/a/d1", "/mnt/b/d1"));
  fd = open("/mnt/b/y.txt", OPEN_READ);
  printf("reb %d\n", mountat("a", "127.0.0.1", p1, "/"));
  n = read(fd, buf, 63); if (n < 0) n = 0; buf[n] = 0;
  printf("r %s", buf);
  printf("busy %d\n", mountat("b", "", 0, ""));
  close(fd);
  printf("c %d\n", mountat("c", "127.0.0.1", p1, "/"));
  printf("d %d\n", mountat("d", "127.0.0.1", p1, "/"));
  printf("full %d\n", mountat("e", "127.0.0.1", p1, "/"));
  a = mountat("bad/n", "127.0.0.1", p1, "/"); b = mountat("..", "127.0.0.1", p1, "/");
  c = mountat("sixteen_chars_xx", "127.0.0.1", p1, "/");
  printf("bad %d %d %d\n", a, b, c);
  printf("ax %d\n", stat("/mnt/ax", &st));
  printf("um %d\n", mountat("c", "", 0, ""));
  printf("um2 %d\n", mountat("c", "", 0, ""));
  get("/mnt/b/y.txt");
  printf("r2 %s", buf);
  a = mount("", 0, ""); b = mount("", 0, "");
  printf("fs %d %d\n", a, b);
  return 0;
}
