// one stat of /mnt/fs/top.txt per line typed: prints ok or fail, and fast (< 100 ms) or slow
#include "sys.h"
int main() {
  struct stat_t st; int t0; int r; int dt; int c;
  while (1) {
    c = getchar();
    if (c == 'q') return 0;
    if (c != '\n') continue;
    t0 = millis();
    r = stat("/mnt/fs/top.txt", &st);
    dt = millis() - t0;
    printf("%s %s\n", r == 0 ? "ok" : "fail", dt < 100 ? "fast" : "slow");
  }
  return 0;
}
