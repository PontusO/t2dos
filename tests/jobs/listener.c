// listener.c - background job test program: listener [port]
// listen()s on port (default 7001) and then sleeps forever, holding the listener
// until it is killed. Prints "listen N" with listen's result.
#include "sys.h"

int main(int argc, char **argv) {
  int port;
  port = 7001;
  if (argc > 1) port = atoi(argv[1]);
  printf("listen %d\n", listen(port));
  while (1) sleep(1000);
  return 0;
}
