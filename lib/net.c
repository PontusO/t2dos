// net.c - small client helpers, #include "net.c" (found in /lib on the board)
// Protocol (see tools/fileserver.py), one request per connection:
//   GET name\n            -> OK size\n + size bytes   |  ERR text\n
//   PUT name size\n + bytes -> OK\n                    |  ERR text\n
//   LS [dir]\n            -> OK size\n + listing text
char net_line[128];

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
  if (colon) { *colon = 0; port = atoi(colon + 1); }
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
