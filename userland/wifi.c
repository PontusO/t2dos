// wifi ssid password [-k] [-s host[:port]] : join a network.  -k also saves it, encrypted with
// a key only this board has, and the board then joins it at boot.  wifi -f : forget the saved
// network.  wifi -s host[:port] : only save the default file server.  wifi : show the status.
#include "sys.h"
#include "net.c"
int usage() { printf("usage: wifi [ssid password [-k]] [-f] [-s host[:port]]\n"); return 1; }
int main(int argc, char **argv) {
  int st; int i; int k; char *pos[2]; char *server; char h[64]; int keep; int forget; char saved[33]; int r;
  k = 0; server = 0; keep = 0; forget = 0;
  for (i = 1; i < argc; i++) {
    if (!strcmp(argv[i], "-s")) {
      if (i + 1 >= argc) return usage();
      i++; server = argv[i];
    } else if (!strcmp(argv[i], "-k")) keep = 1;
    else if (!strcmp(argv[i], "-f")) forget = 1;
    else if (k < 2) { pos[k] = argv[i]; k++; }
    else return usage();
  }
  if (k == 1 || (keep && k != 2) || (keep && forget)) return usage();
  if (server) {
    net_load();
    if (net_norm(h, server) < 0) return 1;
    if (strcmp(h, net_host)) { strcpy(net_host, h); strcpy(net_dir, "/"); }  // same server: keep the dir
    if (net_save() < 0) return 1;
    printf("server %s\n", net_host);
  }
  if (forget) {
    if (wifi_save(0, 0) < 0) { printf("cannot remove /etc/wifi\n"); return 1; }
    printf("saved network forgotten\n");
  }
  if (k == 2) {
    printf("joining %s...\n", pos[0]);
    if (net_join(pos[0], pos[1]) < 0) { printf("failed\n"); return 1; }
    if (keep) {
      if (wifi_save(pos[0], pos[1]) < 0) printf("could not save the network (no key store on this board?)\n");
      else printf("saved %s, joined at boot\n", pos[0]);
    }
  }
  st = net_status();
  if (st == NET_UP) printf("connected, ip %s\n", net_ip());
  else printf(st == NET_CONNECTING ? "connecting\n" : "not connected\n");
  if (!server && net_load()) printf("server %s %s\n", net_host, net_dir);
  if (!keep && !forget) {
    r = wifi_saved(saved, 33);
    if (r > 0) printf("saved network %s\n", saved);
    else if (r < 0) printf("/etc/wifi cannot be opened (saved on another board, or damaged)\n");
  }
  return st == NET_UP ? 0 : 1;
}
