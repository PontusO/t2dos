// mount() with a null host unmounts, like ""
#include "sys.h"
int main() {
  printf("mount %d\n", mount(0, 0, 0));
  return 0;
}
