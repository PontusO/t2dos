// wr name text : writes text and a newline to name, prints close()'s result
#include "sys.h"
int main(int argc, char **argv) {
  int w;
  if (argc != 3) { printf("usage: wr name text\n"); return 2; }
  w = open(argv[1], OPEN_WRITE);
  if (w < 0) { printf("open failed\n"); return 1; }
  write(w, argv[2], strlen(argv[2])); write(w, "\n", 1);
  printf("close %d\n", close(w));
  return 0;
}
