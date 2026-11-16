// rcp [-r] src dst : copy files and (with -r) directory trees to or from fileserver.py.
// The remote side is :path (the saved server), host:path or host:port:path.
#include "sys.h"
#include "net.c"
char buf[1024];
char rpath[200];
char lpath[200];
int nfiles;
int nbytes;

// the last part of a path
char *base(char *s) {
  while (strchr(s, '/') && strchr(s, '/')[1]) s = strchr(s, '/') + 1;
  return s;
}

// remote file -> local file; the byte count, or -1
int get_file(char *remote, char *local) {
  char req[220]; int sock; int out; int size; int n;
  sock = net_open(net_host); if (sock < 0) return -1;
  sprintf(req, "GET %s\n", remote);
  size = net_request(sock, req); if (size < 0) { close(sock); return -1; }
  out = open(local, OPEN_WRITE); if (out < 0) { printf("rcp: cannot write %s\n", local); close(sock); return -1; }
  n = net_recv_to(sock, out, size);
  if (n < 0) { if (n == -2) printf("cannot write %s\n", local); close(out); close(sock); return -1; }
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

// mkdir unless it is already a directory; 0 or -1
int mkdir_ok(char *path) {
  struct stat_t st;
  if (stat(path, &st) == 0 && st.isdir) return 0;
  if (mkdir(path) < 0) { printf("rcp: cannot make directory %s\n", path); return -1; }
  return 0;
}

// counts a copied file; 0, or -1 if n is -1
int count(int n) {
  if (n < 0) return -1;
  nfiles++; nbytes = nbytes + n;
  return 0;
}

// remote directory tree -> local directory; 0 or -1
int get_tree(char *remote, char *local) {
  char req[220]; char r2[200]; char l2[200]; char *list; char *line; char *nl; char *name;
  int sock; int size; int dir; int k; int ok;
  if (mkdir_ok(local) < 0) return -1;
  sock = net_open(net_host); if (sock < 0) return -1;
  sprintf(req, "LS %s\n", remote);
  size = net_request(sock, req); if (size < 0) { close(sock); return -1; }
  list = malloc(size + 1);
  if (!list) { printf("rcp: out of memory\n"); close(sock); return -1; }
  k = 0;                                        // the whole listing first: one request per connection
  while (k < size) {
    ok = recv(sock, list + k, size - k, 10000);
    if (ok <= 0) { printf("rcp: listing of %s stalled\n", remote); free(list); close(sock); return -1; }
    k = k + ok;
  }
  close(sock);
  list[size] = 0;
  ok = 0; line = list;
  while (ok == 0 && *line) {
    nl = strchr(line, '\n'); if (nl) *nl = 0;
    dir = !strncmp(line, "   <dir>  ", 10);
    name = line;                                // "   <dir>  name/" or "%8d  name"
    while (*name == ' ') name++;
    while (*name && *name != ' ') name++;
    while (*name == ' ') name++;
    k = strlen(name);
    if (dir && k > 0 && name[k - 1] == '/') name[k - 1] = 0;
    if (*name) {
      if (join(r2, remote, name) < 0 || join(l2, local, name) < 0) ok = -1;
      else if (dir) ok = get_tree(r2, l2);
      else ok = count(get_file(r2, l2));
    }
    if (!nl) break;
    line = nl + 1;
  }
  free(list);
  return ok;
}

// local directory tree -> remote directory; 0 or -1
int put_tree(char *local, char *remote) {
  char r2[200]; char l2[200]; struct dirent_t e; struct dirent_t *list; int d; int n; int i; int ok;
  if (net_mkdir(remote) < 0) return -1;
  d = opendir(local); if (d < 0) { printf("rcp: cannot read %s\n", local); return -1; }
  n = 0; while (readdir(d, &e)) n++;
  closedir(d);
  if (n == 0) return 0;
  list = malloc(n * sizeof(struct dirent_t));    // only a few directories can be open at once:
  if (!list) { printf("rcp: out of memory\n"); return -1; }   // read the entries, then recurse
  d = opendir(local); if (d < 0) { free(list); printf("rcp: cannot read %s\n", local); return -1; }
  i = 0; while (i < n && readdir(d, &list[i])) i++;
  closedir(d);
  n = i; ok = 0; i = 0;
  while (ok == 0 && i < n) {
    if (join(l2, local, list[i].name) < 0 || join(r2, remote, list[i].name) < 0) ok = -1;
    else if (list[i].isdir) ok = put_tree(l2, r2);
    else ok = count(put_file(l2, r2));
    i++;
  }
  free(list);
  return ok;
}

// drops trailing '/' (not from "/" itself)
int trim(char *s) {
  int k;
  k = strlen(s);
  while (k > 1 && s[k - 1] == '/') { k--; s[k] = 0; }
  return 0;
}

// the copy's result: with -r also the totals
int done(int r, int ok) {
  if (ok < 0) return 1;
  if (r) printf("%d files, %d bytes\n", nfiles, nbytes);
  return 0;
}

int main(int argc, char **argv) {
  int r; int a; char *src; char *dst; struct stat_t st; int isdir;
  r = 0; a = 1;
  if (argc > 1 && !strcmp(argv[1], "-r")) { r = 1; a = 2; }
  if (argc != a + 2) { printf("usage: rcp [-r] src dst\n"); return 1; }
  src = argv[a]; dst = argv[a + 1];
  if (!strchr(src, ':')) trim(src);
  if (!strchr(dst, ':')) trim(dst);
  if (!strchr(src, ':') == !strchr(dst, ':')) { printf("rcp: one side must be remote (host:path or :path)\n"); return 1; }
  if (strchr(src, ':')) {                       // download
    if (net_remote(src) < 0) return 1;
    strcpy(rpath, net_path);
    isdir = net_isdir(rpath); if (isdir < 0) return 1;
    if (isdir && !r) { printf("rcp: %s: is a directory, use -r\n", strchr(src, ':') + 1); return 1; }
    strcpy(lpath, dst);
    if (stat(dst, &st) == 0 && st.isdir) { if (join(lpath, dst, base(rpath)) < 0) return 1; }
    if (isdir) return done(r, get_tree(rpath, lpath));
    return done(r, count(get_file(rpath, lpath)));
  }
  if (stat(src, &st) < 0) { printf("rcp: %s: not found\n", src); return 1; }   // upload
  if (st.isdir && !r) { printf("rcp: %s: is a directory, use -r\n", src); return 1; }
  if (net_remote(dst) < 0) return 1;
  strcpy(rpath, net_path);
  isdir = net_isdir(rpath); if (isdir < 0) return 1;
  if (isdir) { strcpy(lpath, rpath); if (join(rpath, lpath, base(src)) < 0) return 1; }
  if (st.isdir) return done(r, put_tree(src, rpath));
  return done(r, count(put_file(src, rpath)));
}
