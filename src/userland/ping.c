// ping host [-c count] : send echo requests, one a second, and show the round trips
#include "sys.h"
char ip[20];
int main(int argc, char **argv) {
  int count; int i; int r; int got; char *host;
  count = 4; host = 0;
  i = 1;
  while (i < argc) {
    if (!strcmp(argv[i], "-c") && i + 1 < argc) { count = atoi(argv[i + 1]); i = i + 2; }
    else if (!host) { host = argv[i]; i++; }
    else { host = 0; break; }
  }
  if (!host || count < 1) { printf("usage: ping host [-c count]\n"); return 1; }
  got = 0;
  for (i = 0; i < count; i++) {
    if (i > 0) sleep(1000);
    r = ping(host, ip);
    if (r == -3) { printf("network is down, use: wifi ssid password\n"); return 1; }
    if (r == -2) { printf("ping: cannot resolve %s\n", host); return 1; }
    if (r < 0) printf("%s: timeout\n", ip);
    else { printf("%s: %d ms\n", ip, r); got++; }
  }
  printf("%d sent, %d received\n", count, got);
  return got > 0 ? 0 : 1;
}
