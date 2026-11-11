// rls [host[:port]] [dir] : list files on the server (default: the one set by wifi -s or rcd)
#include "sys.h"
#include "net.c"
char req[256];
int main(int argc, char **argv) {
  int sock; int size; int a;
  a = net_pick(argc, argv); if (a < 0) return 1;
  if (argc > a + 2) { printf("usage: rls [host[:port]] [dir]\n"); return 1; }
  if (net_resolve(argc > a + 1 ? argv[a + 1] : ".") < 0) return 1;
  sock = net_open(net_host); if (sock < 0) return 1;
  sprintf(req, "LS %s\n", net_path);
  size = net_request(sock, req); if (size < 0) { close(sock); return 1; }
  net_recv_to(sock, 1, size);
  close(sock);
  return 0;
}
