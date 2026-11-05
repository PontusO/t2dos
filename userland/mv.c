#include "sys.h"
int main(int argc, char **argv) {
  if (argc != 3) { printf("usage: mv old new\n"); return 1; }
  if (rename(argv[1], argv[2]) < 0) { printf("mv failed\n"); return 1; }
  return 0;
}
