// send host[:port] local [remote] : upload a file to fileserver.py
#include "sys.h"
#include "net.c"
char req[160];
char buf[1024];
int main(int argc, char **argv) {
  int sock; int in; int n; struct stat_t st; char *remote; char *b; int t0;
  if (argc < 3) { printf("usage: send host[:port] localfile [remotename]\n"); return 1; }
  if (stat(argv[2], &st) < 0 || st.isdir) { printf("%s: not a file\n", argv[2]); return 1; }
  b = argv[2]; while (strchr(b, '/')) b = strchr(b, '/') + 1;
  remote = argc > 3 ? argv[3] : b;
  in = open(argv[2], OPEN_READ);
  sock = net_open(argv[1]); if (sock < 0) return 1;
  sprintf(req, "PUT %s %d\n", remote, st.size);
  send(sock, req, strlen(req));
  t0 = millis();
  while ((n = read(in, buf, 1024)) > 0) if (send(sock, buf, n) < 0) { printf("connection lost\n"); return 1; }
  close(in);
  if (net_readline(sock, net_line, 128) < 0 || strncmp(net_line, "OK", 2)) { printf("server: %s\n", net_line); close(sock); return 1; }
  close(sock);
  printf("%s: %d bytes in %d ms\n", remote, st.size, millis() - t0);
  return 0;
}
