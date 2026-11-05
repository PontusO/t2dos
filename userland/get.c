// get host[:port] remote [local] : fetch a file from fileserver.py
#include "sys.h"
#include "net.c"
char req[160];
int main(int argc, char **argv) {
  int sock; int out; int size; int t0; char *local; char *b;
  if (argc < 3) { printf("usage: get host[:port] remotefile [localfile]\n"); return 1; }
  local = argc > 3 ? argv[3] : argv[2];
  b = local; while (strchr(b, '/')) b = strchr(b, '/') + 1;     // basename for the default
  if (argc <= 3) local = b;
  sock = net_open(argv[1]); if (sock < 0) return 1;
  sprintf(req, "GET %s\n", argv[2]);
  size = net_request(sock, req); if (size < 0) { close(sock); return 1; }
  out = open(local, OPEN_WRITE); if (out < 0) { printf("cannot write %s\n", local); close(sock); return 1; }
  t0 = millis();
  if (net_recv_to(sock, out, size) < 0) { close(out); close(sock); return 1; }
  close(out); close(sock);
  printf("%s: %d bytes in %d ms\n", local, size, millis() - t0);
  return 0;
}
