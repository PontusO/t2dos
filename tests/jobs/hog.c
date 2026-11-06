// hog.c - background job test program: calls getchar() forever.
// In a background job getchar() never returns (only the foreground reads keys),
// so this job stays blocked inside the system call until it is killed.
#include "sys.h"

int main() {
  while (1) getchar();
  return 0;
}
