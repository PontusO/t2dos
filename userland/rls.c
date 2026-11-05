// rls host[:port] [dir] : list files on the server
#include "sys.h"
#include "net.c"
char req[160];
int main(int argc, char **argv) {
  int sock; int size;
  if (argc < 2) { printf("usage: rls host[:port] [dir]\n"); return 1; }
  sock = net_open(argv[1]); if (sock < 0) return 1;
  sprintf(req, "LS %s\n", argc > 2 ? argv[2] : "");
  size = net_request(sock, req); if (size < 0) { close(sock); return 1; }
  net_recv_to(sock, 1, size);
  close(sock);
  return 0;
}
