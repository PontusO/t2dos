#include "sys.h"
int main(int argc, char **argv) {
  int i;
  for (i = 1; i < argc; i++) if (rmdir(argv[i]) < 0) { printf("rmdir: %s failed (not empty?)\n", argv[i]); return 1; }
  return 0;
}
