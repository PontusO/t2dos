// put name size : receive exactly size raw bytes from the console into a file.
// From a PC: (printf 'put cc.bin %d\n' $(stat -c%s cc.bin); cat cc.bin) > /dev/ttyACM0
#include "sys.h"
char buf[512];
int main(int argc, char **argv) {
  int fd; int size; int got; int n; int i; int t0;
  conctl(CON_BREAK, 0, 0);       // the bytes are raw: a 3 is data
  if (argc < 3) { printf("usage: put name size\n"); return 1; }
  size = atoi(argv[2]);
  fd = open(argv[1], OPEN_WRITE);
  if (fd < 0) { printf("put: cannot write %s\n", argv[1]); return 1; }
  printf("receiving %d bytes\n", size);
  got = 0; t0 = millis();
  while (got < size) {
    n = 0;
    while (n < 512 && got + n < size && kbhit()) { buf[n++] = getchar(); }
    if (n) { write(fd, buf, n); got = got + n; t0 = millis(); }
    else if (millis() - t0 > 5000) break;
  }
  close(fd);
  if (got == size) printf("ok, %d bytes\n", got); else printf("timeout after %d bytes\n", got);
  return got == size ? 0 : 1;
}
