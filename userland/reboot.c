// reboot [-b] : restart the board; -b restarts into the USB bootloader (the RP2350 drive)
#include "sys.h"
int main(int argc, char **argv) {
  int mode;
  mode = 0;
  if (argc == 2 && !strcmp(argv[1], "-b")) mode = 1;
  else if (argc != 1) { printf("usage: reboot [-b]\n"); return 1; }
  printf(mode ? "rebooting into the bootloader\n" : "rebooting\n");
  reboot(mode);
  printf("reboot is not supported here\n");
  return 1;
}
