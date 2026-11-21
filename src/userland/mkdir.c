#include "sys.h"
int main(int argc, char **argv) {
  int i;
  for (i = 1; i < argc; i++) if (mkdir(argv[i]) < 0) { printf("mkdir: %s failed\n", argv[i]); return 1; }
  return 0;
}
