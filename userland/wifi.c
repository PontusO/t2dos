// wifi ssid password [-s host[:port]] : join a network, optionally saving the default file
// server.  wifi -s host[:port] : only save the server.  wifi : show the status and the server.
#include "sys.h"
#include "net.c"
int main(int argc, char **argv) {
  int st; int i; int k; char *pos[2]; char *server; char h[64];
  k = 0; server = 0;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-s")) {
      if (i + 1 >= argc) { printf("usage: wifi [ssid password] [-s host[:port]]\n"); return 1; }
      i++; server = argv[i];
    } else if (k < 2) { pos[k] = argv[i]; k++; }
    else { printf("usage: wifi [ssid password] [-s host[:port]]\n"); return 1; }
  }
  if (k == 1) { printf("usage: wifi [ssid password] [-s host[:port]]\n"); return 1; }
  if (server) {
    net_load();
    if (net_norm(h, server) < 0) return 1;
    if (strcmp(h, net_host)) { strcpy(net_host, h); strcpy(net_dir, "/"); }  // same server: keep the dir
    if (net_save() < 0) return 1;
    printf("server %s\n", net_host);
  }
  if (k == 2) {
    printf("joining %s...\n", pos[0]);
    if (net_join(pos[0], pos[1]) < 0) { printf("failed\n"); return 1; }
  }
  st = net_status();
  if (st == NET_UP) printf("connected, ip %s\n", net_ip());
  else printf(st == NET_CONNECTING ? "connecting\n" : "not connected\n");
  if (!server && net_load()) printf("server %s %s\n", net_host, net_dir);
  return st == NET_UP ? 0 : 1;
}
