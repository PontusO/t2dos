// rcd [host[:port]] [dir] : set the remote directory (and server) used by rls, get and send.
// rcd alone shows them.  The directory is checked on the server before it is saved.
#include "sys.h"
#include "net.c"
char req[256];
int main(int argc, char **argv) {
  int a; int sock;
  if (argc == 1) {
    if (!net_load()) { printf("no server set\n"); return 1; }
    printf("%s %s\n", net_host, net_dir);
    return 0;
  }
  a = net_pick(argc, argv); if (a < 0) return 1;
  if (argc > a + 2) { printf("usage: rcd [host[:port]] [dir]\n"); return 1; }
  if (net_resolve(argc > a + 1 ? argv[a + 1] : (a ? "/" : ".")) < 0) return 1;
  if (strlen(net_path) > 127) { printf("remote path too long\n"); return 1; }
  sprintf(req, "LS %s\n", net_path);
  sock = net_ask(req, -1, 0); if (sock < 0) return 1;
  net_drain(sock, net_size);                    // the listing is not needed
  close(sock);
  strcpy(net_dir, net_path);
  if (net_save() < 0) return 1;
  printf("%s %s\n", net_host, net_dir);
  return 0;
}
