#include "sys.h"
int main() {
  printf("sram free   %8d bytes\n", sysinfo(INFO_SRAM_FREE));
  printf("psram       %8d bytes, %d free\n", sysinfo(INFO_PSRAM_TOTAL), sysinfo(INFO_PSRAM_FREE));
  printf("disk        %8d KB, %d KB used\n", sysinfo(INFO_DISK_TOTAL_KB), sysinfo(INFO_DISK_USED_KB));
  return 0;
}
