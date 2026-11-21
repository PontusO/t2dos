// send [host[:port]] local [remote] : upload a file to fileserver.py
#include "sys.h"
#include "net.c"
char req[256];
int main(int argc, char **argv) {
  int sock; int in; struct stat_t st; char *b; int t0; int a;
  a = net_pick(argc, argv); if (a < 0) return 1;
  if (argc < a + 2 || argc > a + 3) { printf("usage: send [host[:port]] localfile [remotename]\n"); return 1; }
  if (stat(argv[a + 1], &st) < 0 || st.isdir) { printf("%s: not a file\n", argv[a + 1]); return 1; }
  b = argv[a + 1]; while (strchr(b, '/')) b = strchr(b, '/') + 1;
  if (net_resolve(argc > a + 2 ? argv[a + 2] : b) < 0) return 1;
  in = open(argv[a + 1], OPEN_READ); if (in < 0) { printf("cannot read %s\n", argv[a + 1]); return 1; }
  sprintf(req, "PUT %s %d\n", net_path, st.size);
  t0 = millis();
  sock = net_ask(req, in, st.size);
  close(in);
  if (sock < 0) return 1;
  close(sock);
  printf("%s: %d bytes in %d ms\n", net_path, st.size, millis() - t0);
  return 0;
}
