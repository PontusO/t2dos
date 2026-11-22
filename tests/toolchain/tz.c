// tz.c - the localtime(int*)/tzset(char*) builtins (SYS_localtime/SYS_tzset).
// QEMU_RUN: the host boots UTC (check_toolchain strips TZ), so tzset(0) starts
// at 0; tzset("GMT+3") must apply the POSIX sign (west-positive: UTC-3); a
// malformed string is rejected (-1) and leaves the zone standing. The DST legs
// live in the PC unit test (tests/tz) - only fixed epochs are deterministic.
#include "sys.h"
int main() {
  if (tzset(0) != 0) { printf("tzset0 %d\n", tzset(0)); return 1; }
  if (tzset("GMT+3") != -10800) { printf("gmt3 %d\n", tzset("GMT+3")); return 1; }
  int d = localtime(0) - time(0);   // one-second straddle tolerated
  if (d < -10801 || d > -10800) { printf("diff %d\n", d); return 1; }
  if (tzset("nonsense") != -1) { printf("accept nonsense\n"); return 1; }
  if (tzset(0) != -10800) { printf("reject moved the zone %d\n", tzset(0)); return 1; }
  if (tzset("CET-1CEST,M3.5.0,M10.5.0") != 3600) { printf("cet %d\n", tzset("CET-1CEST,M3.5.0,M10.5.0")); return 1; }   // winter now (Nov)
  if (tzlocal(1752573600) != 1752580800) { printf("tzlocal %d\n", tzlocal(1752573600)); return 1; }   // 2025-07-15 10:00 UTC, +02:00
  if (tzlocal(1763812800) != 1763816400) { printf("tzlocal %d\n", tzlocal(1763812800)); return 1; }   // 2025-11-22 12:00 UTC, +01:00
  printf("ok\n");
  return 0;
}
