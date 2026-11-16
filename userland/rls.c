// rls [host[:port]] [dir] : list files on the server (default: the one set by wifi -s or rcd)
#include "sys.h"
#include "net.c"
char req[256];
int main(int argc, char **argv) {
  int sock; int a;
  a = net_pick(argc, argv); if (a < 0) return 1;
  if (argc > a + 2) { printf("usage: rls [host[:port]] [dir]\n"); return 1; }
  if (net_resolve(argc > a + 1 ? argv[a + 1] : ".") < 0) return 1;
  sprintf(req, "LS %s\n", net_path);
  sock = net_ask(req, -1, 0); if (sock < 0) return 1;
  net_recv_to(sock, 1, net_size);
  close(sock);
  return 0;
}
