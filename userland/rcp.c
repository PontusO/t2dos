// rcp [-r] src dst : copy files and (with -r) directory trees to or from fileserver.py.
// The remote side is :path (the saved server), host:path or host:port:path.
#include "sys.h"
#include "net.c"
char buf[1024];
char rpath[200];
char lpath[200];

// the last part of a path
char *base(char *s) {
  while (strchr(s, '/') && strchr(s, '/')[1]) s = strchr(s, '/') + 1;
  return s;
}

// remote file -> local file; the byte count, or -1
int get_file(char *remote, char *local) {
  char req[220]; int sock; int out; int size;
  sock = net_open(net_host); if (sock < 0) return -1;
  sprintf(req, "GET %s\n", remote);
  size = net_request(sock, req); if (size < 0) { close(sock); return -1; }
  out = open(local, OPEN_WRITE); if (out < 0) { printf("rcp: cannot write %s\n", local); close(sock); return -1; }
  if (net_recv_to(sock, out, size) < 0) { close(out); close(sock); return -1; }
  close(out); close(sock);
  printf("%s: %d bytes\n", local, size);
  return size;
}

// local file -> remote file; the byte count, or -1
int put_file(char *local, char *remote) {
  char req[220]; int sock; int in; int n; struct stat_t st;
  if (stat(local, &st) < 0) { printf("rcp: %s: not found\n", local); return -1; }
  in = open(local, OPEN_READ); if (in < 0) { printf("rcp: cannot read %s\n", local); return -1; }
  sock = net_open(net_host); if (sock < 0) { close(in); return -1; }
  sprintf(req, "PUT %s %d\n", remote, st.size);
  send(sock, req, strlen(req));
  while ((n = read(in, buf, 1024)) > 0)
    if (send(sock, buf, n) < 0) { printf("connection lost\n"); close(in); close(sock); return -1; }
  close(in);
  if (net_readline(sock, net_line, 128) < 0 || strncmp(net_line, "OK", 2)) { printf("server: %s\n", net_line); close(sock); return -1; }
  close(sock);
  printf("%s: %d bytes\n", remote, st.size);
  return st.size;
}

// dst = dir/name, or -1 if it does not fit
int join(char *dst, char *dir, char *name) {
  int k;
  k = strlen(dir);
  if (k + strlen(name) + 2 > 200) { printf("rcp: path too long: %s/%s\n", dir, name); return -1; }
  strcpy(dst, dir);
  if (k == 0 || dir[k - 1] != '/') { dst[k] = '/'; k++; }
  strcpy(dst + k, name);
  return 0;
}

int main(int argc, char **argv) {
  int r; int a; char *src; char *dst; struct stat_t st; int isdir;
  r = 0; a = 1;
  if (argc > 1 && !strcmp(argv[1], "-r")) { r = 1; a = 2; }
  if (argc != a + 2) { printf("usage: rcp [-r] src dst\n"); return 1; }
  src = argv[a]; dst = argv[a + 1];
  if (!strchr(src, ':') == !strchr(dst, ':')) { printf("rcp: one side must be remote (host:path or :path)\n"); return 1; }
  if (strchr(src, ':')) {                       // download
    if (net_remote(src) < 0) return 1;
    strcpy(rpath, net_path);
    isdir = net_isdir(rpath); if (isdir < 0) return 1;
    if (isdir && !r) { printf("rcp: %s: is a directory, use -r\n", strchr(src, ':') + 1); return 1; }
    strcpy(lpath, dst);
    if (stat(dst, &st) == 0 && st.isdir) { if (join(lpath, dst, base(rpath)) < 0) return 1; }
    return get_file(rpath, lpath) < 0 ? 1 : 0;
  }
  if (stat(src, &st) < 0) { printf("rcp: %s: not found\n", src); return 1; }   // upload
  if (st.isdir && !r) { printf("rcp: %s: is a directory, use -r\n", src); return 1; }
  if (net_remote(dst) < 0) return 1;
  strcpy(rpath, net_path);
  isdir = net_isdir(rpath); if (isdir < 0) return 1;
  if (isdir) { strcpy(lpath, rpath); if (join(rpath, lpath, base(src)) < 0) return 1; }
  return put_file(src, rpath) < 0 ? 1 : 0;
}
