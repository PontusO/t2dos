// writes /mnt/fs/nc.txt and ends without closing it: the end of the program saves it
#include "sys.h"
int main() {
  int w;
  w = open("/mnt/fs/nc.txt", OPEN_WRITE);
  write(w, "data\n", 5);
  return 0;
}
