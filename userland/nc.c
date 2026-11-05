// nc host port : raw TCP terminal. Ctrl-] quits.  nc -l port : wait for one connection, then the same.
#include "sys.h"
char buf[512];
int main(int argc, char **argv) {
  int sock; int n; int c; int i;
  if (argc < 3) { printf("usage: nc host port | nc -l port\n"); return 1; }
  if (!strcmp(argv[1], "-l")) {
    if (listen(atoi(argv[2])) < 0) { printf("listen failed\n"); return 1; }
    printf("listening on %d\n", atoi(argv[2]));
    sock = accept(-1);
    if (sock < 0) { printf("accept failed\n"); return 1; }
    printf("connected\n");
  } else {
    sock = connect(argv[1], atoi(argv[2]));
    if (sock < 0) { printf("cannot connect\n"); return 1; }
  }
  while (1) {
    n = recv(sock, buf, 512, 10);
    if (n < 0) { printf("\n[connection closed]\n"); break; }
    if (n > 0) write(1, buf, n);
    i = 0;
    while (kbhit() && i < 512) {
      c = getchar();
      if (c == 29) { close(sock); printf("\n[quit]\n"); return 0; }
      buf[i++] = c == '\r' ? '\n' : c;
    }
    if (i) send(sock, buf, i);
  }
  close(sock);
  return 0;
}
