// date : print the wall clock as YYYY-MM-DD HH:MM:SS +hh:mm (local zone), UTC with -u, and say so when it is not synced
#include "sys.h"
int main(int argc, char **argv) {
  int utc; int v; int t; int off; int a; int days;
  int z; int era; int doe; int yoe; int y; int doy; int mp; int d; int m;
  utc = argc > 1 && !strcmp(argv[1], "-u");
  v = 0;
  t = utc ? time(&v) : localtime(&v);
  off = utc ? 0 : tzset(0); // whole minutes by construction; queries never return -1
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
  if (utc) printf("%04d-%02d-%02d %02d:%02d:%02d UTC\n", y, m, d, t / 3600 % 24, t / 60 % 60, t % 60);
  else {
    a = off < 0 ? -off : off;
    printf("%04d-%02d-%02d %02d:%02d:%02d %c%02d:%02d\n", y, m, d, t / 3600 % 24, t / 60 % 60, t % 60, off < 0 ? '-' : '+', a / 3600, a / 60 % 60);
  }
  if (!v) printf("unsynced: no chip, no NTP\n");
  return 0;
}
