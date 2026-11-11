// net.c - small client helpers, #include "net.c" (found in /lib on the board)
// Protocol (see tools/fileserver.py), one request per connection:
//   GET name\n            -> OK size\n + size bytes   |  ERR text\n
//   PUT name size\n + bytes -> OK\n                    |  ERR text\n
//   LS [dir]\n            -> OK size\n + listing text
// Names are relative to the server's root directory.  /etc/remote keeps a default server
// (line 1, host[:port], set by wifi -s or rcd) and the remote directory (line 2, set by rcd).
char net_line[128];
char net_host[64];    // the server, after net_load() or net_pick(); "" if none
char net_dir[128];    // the remote directory, always starting with '/'
char net_path[200];   // net_resolve()'s result

// read one line from a socket (without the newline), -1 on timeout/close
int net_readline(int fd, char *buf, int max) {
  int n; int r; char c;
  n = 0;
  while (n < max - 1) {
    r = recv(fd, &c, 1, 10000);
    if (r <= 0) return -1;
    if (c == '\n') break;
    buf[n++] = c;
  }
  buf[n] = 0;
  return n;
}

// "host:port" -> connects, default port 7777
int net_open(char *hostport) {
  char host[64]; char *colon; int port; int fd;
  strcpy(host, hostport);
  port = 7777;
  colon = strchr(host, ':');
  if (colon) { *colon = 0; if (colon[1]) port = atoi(colon + 1); }
  if (net_status() != NET_UP) { printf("network is down, use: wifi ssid password\n"); return -1; }
  fd = connect(host, port);
  if (fd < 0) printf("cannot connect to %s:%d\n", host, port);
  return fd;
}

// send request, expect "OK size" or "OK"; returns size (0 for bare OK), -1 on error
int net_request(int fd, char *req) {
  send(fd, req, strlen(req));
  if (net_readline(fd, net_line, 128) < 0) { printf("no reply\n"); return -1; }
  if (strncmp(net_line, "OK", 2)) { printf("server: %s\n", net_line); return -1; }
  return net_line[2] == ' ' ? atoi(net_line + 3) : 0;
}

// receive exactly size bytes into fd (a file fd or 1 for the console)
int net_recv_to(int sock, int out, int size) {
  char buf[1024]; int got; int n;
  got = 0;
  while (got < size) {
    n = recv(sock, buf, size - got > 1024 ? 1024 : size - got, 10000);
    if (n <= 0) { printf("\ntransfer stalled after %d bytes\n", got); return -1; }
    write(out, buf, n); got += n;
  }
  return got;
}

// copies host[:port] to dst without a ":" or ":7777" ending, so equal servers compare equal;
// -1 if it is too long
int net_norm(char *dst, char *src) {
  int k;
  if (strlen(src) > 63) { printf("server name too long\n"); return -1; }
  strcpy(dst, src);
  k = strlen(dst);
  if (k > 5 && !strcmp(dst + k - 5, ":7777")) dst[k - 5] = 0;
  else if (k > 1 && dst[k - 1] == ':') dst[k - 1] = 0;
  return 0;
}

// 1 if s names a server rather than a file: it has a ':', ends in ".local" (mDNS) or is a
// dotted IPv4 address
int net_is_host(char *s) {
  int dots; int digits; int k;
  if (strchr(s, ':')) return 1;
  k = strlen(s);
  if (k > 6 && !strcmp(s + k - 6, ".local")) return 1;
  dots = 0; digits = 0;
  while (*s) {
    if (*s == '.') { if (!digits) return 0; dots++; digits = 0; }
    else if (*s >= '0' && *s <= '9') { digits++; if (digits > 3) return 0; }
    else return 0;
    s++;
  }
  return dots == 3 && digits > 0;
}

// reads /etc/remote into net_host and net_dir; 1 if a server is set
int net_load() {
  char buf[200]; int fd; int r; char *nl;
  net_host[0] = 0; strcpy(net_dir, "/");
  fd = open("/etc/remote", OPEN_READ);
  if (fd < 0) return 0;
  r = read(fd, buf, 199); close(fd);
  if (r < 0) r = 0;
  buf[r] = 0;
  nl = strchr(buf, '\n');
  if (nl) {
    *nl = 0;
    if (nl[1] == '/' && strlen(nl + 1) < 128) {
      strcpy(net_dir, nl + 1);
      nl = strchr(net_dir, '\n'); if (nl) *nl = 0;
    }
  }
  if (strlen(buf) < 64) strcpy(net_host, buf);
  return net_host[0] != 0;
}

// writes net_host and net_dir to /etc/remote; -1 on error
int net_save() {
  int fd;
  mkdir("/etc");
  fd = open("/etc/remote", OPEN_WRITE);
  if (fd < 0) { printf("cannot write /etc/remote\n"); return -1; }
  write(fd, net_host, strlen(net_host)); write(fd, "\n", 1);
  write(fd, net_dir, strlen(net_dir)); write(fd, "\n", 1);
  close(fd);
  return 0;
}

// net_path = name taken from the remote directory (from "/" if name starts with '/'),
// with "." and ".." folded; -1 if it gets too long
int net_resolve(char *name) {
  char *s; char *e; int len; int k;
  if (name[0] == '/') strcpy(net_path, "/"); else strcpy(net_path, net_dir);
  s = name;
  while (*s) {
    while (*s == '/') s++;
    if (!*s) break;
    e = s; while (*e && *e != '/') e++;
    len = e - s;
    if (len == 2 && s[0] == '.' && s[1] == '.') {
      k = strlen(net_path) - 1;               // net_path ends in '/' only when it is "/"
      while (k > 0 && net_path[k] != '/') k--;
      net_path[k > 0 ? k : 1] = 0;
    } else if (len != 1 || s[0] != '.') {
      k = strlen(net_path);
      if (k + len + 2 > 200) { printf("remote path too long\n"); return -1; }
      if (k > 1) { net_path[k] = '/'; k++; }
      memcpy(net_path + k, s, len); net_path[k + len] = 0;
    }
    s = e;
  }
  return 0;
}

// picks the server for rls/get/send: argv[1] if it looks like a host (returns 1, the
// arguments used), else the saved one (returns 0); -1 if there is none.  Sets net_host and
// net_dir: the saved directory applies only to the saved server, another one starts at "/".
int net_pick(int argc, char **argv) {
  char h[64];
  net_load();
  if (argc > 1 && net_is_host(argv[1])) {
    if (net_norm(h, argv[1]) < 0) return -1;
    if (strcmp(h, net_host)) { strcpy(net_host, h); strcpy(net_dir, "/"); }
    return 1;
  }
  if (!net_host[0]) { printf("no server: give host[:port] or set one with wifi -s host[:port]\n"); return -1; }
  return 0;
}
