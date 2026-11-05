#include "sys.h"
int main(int argc, char **argv) {
  int i; int r;
  r = 0;
  for (i = 1; i < argc; i++) if (unlink(argv[i]) < 0) { printf("rm: %s failed\n", argv[i]); r = 1; }
  return r;
}
