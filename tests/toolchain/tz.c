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
  int r = tzset("CET-1CEST,M3.5.0,M10.5.0");   // the live offset depends on the date: either side proves the apply, the fixed-epoch legs below carry the DST proof
  if (r != 3600 && r != 7200) { printf("cet %d\n", r); return 1; }
  int l = tzlocal(1752573600);   // 2025-07-15 10:00 UTC, +02:00
  if (l != 1752580800) { printf("tzlocal %d\n", l); return 1; }
  l = tzlocal(1763812800);       // 2025-11-22 12:00 UTC, +01:00
  if (l != 1763816400) { printf("tzlocal %d\n", l); return 1; }
  printf("ok\n");
  return 0;
}
