// wifi ssid password : join a network.  wifi : show status.
#include "sys.h"
int main(int argc, char **argv) {
  int st;
  if (argc >= 3) {
    printf("joining %s...\n", argv[1]);
    if (net_join(argv[1], argv[2]) < 0) { printf("failed\n"); return 1; }
  }
  st = net_status();
  if (st == NET_UP) printf("connected, ip %s\n", net_ip());
  else printf(st == NET_CONNECTING ? "connecting\n" : "not connected\n");
  return st == NET_UP ? 0 : 1;
}
