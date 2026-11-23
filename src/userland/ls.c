#include "sys.h"
#define MTIME_UNKNOWN 315532800   // 1980-01-01 UTC: FAT floor / unsynced sentinel (mirror of the kernel bounds; userland never includes kernel headers)
#define MTIME_MAX     2147483647  // int32 epoch ceiling: above it the stamp is junk (pre-stamping card files show ~2097) or past this OS's int time
                                  // (within one zone offset below it tzlocal can int-wrap and render a 1901 date - unreachable from NTP/DS1307, accepted)

// timestamp prefix for a file mtime e: civil fields of tzlocal(e) formatted as-if-UTC
// (Hinnant civil-from-days, the date.c shape; a small local copy, duplication blessed),
// or "----------  " for stamps outside MTIME_UNKNOWN < mtime < MTIME_MAX (both bounds exclusive)
int stamp(int e) {
  int t; int days; int z; int era; int doe; int yoe; int y; int doy; int mp; int d; int m;
  if (e <= MTIME_UNKNOWN || e >= MTIME_MAX) { printf("----------  "); return 0; }
  t = tzlocal(e);
  days = t / 86400;
  z = days + 719468;
  era = (z >= 0 ? z : z - 146096) / 146097;
  doe = z - era * 146097;
  yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  y = yoe + era * 400;
  doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  mp = (5 * doy + 2) / 153;
  d = doy - (153 * mp + 2) / 5 + 1;
  m = mp < 10 ? mp + 3 : mp - 9;
  y += m <= 2;
  printf("%04d-%02d-%02d %02d:%02d  ", y, m, d, t / 3600 % 24, t / 60 % 60);
  return 0;
}
int list(char *dir) {
  struct dirent_t e; int d; int files; int bytes; struct stat_t st;
  if (stat(dir, &st) == 0 && !st.isdir) { stamp(st.mtime); printf("%8d  %s\n", st.size, dir); return 0; }
  d = opendir(dir);
  if (d < 0) { printf("ls: %s: not found\n", dir); return 1; }
  files = 0; bytes = 0;
  while (readdir(d, &e)) {
    if (e.isdir) printf("   <dir>  %s/\n", e.name);
    else { stamp(e.mtime); printf("%8d  %s\n", e.size, e.name); files++; bytes = bytes + e.size; }
  }
  closedir(d);
  printf("%d files, %d bytes\n", files, bytes);
  return 0;
}
int main(int argc, char **argv) {
  int i;
  if (argc < 2) return list(".");
  for (i = 1; i < argc; i++) { if (argc > 2) printf("%s:\n", argv[i]); list(argv[i]); }
  return 0;
}
