// tests/tz/test_tz.c - the DST oracle for src/kernel/tz.c (see that header).
// Native PC test (gcc x86): wired into `make check`. Includes the engine
// directly, same single-implementation rule as the kernel and host_linux builds.
// Prints `ok` and exits 0 only if every leg holds; on a miss prints the leg
// name and exits 1. Fixed epochs only - deterministic, no clock dependency.
#include <stdio.h>
#include "../../src/kernel/tz.c"

// the engine's own helper is static with a tz_ prefix; this file carries its
// own copy of the civil->epoch math (the spec blesses the duplication) so the
// test legs read as UTC wall times, not magic numbers.
static long t_days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return era * 146097L + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
// UTC epoch of a wall time: E(2026,3,29,1,0,0) = 2026-03-29 01:00:00 UTC
static long E(int y, int m, int d, int H, int M, int S) {
  return t_days_from_civil(y, m, d) * 86400L + (H * 3600L + M * 60L + S);
}

static int fails;
static void leg(const char *name, int got, int want) {
  if (got != want) { printf("FAIL %s: got %d want %d\n", name, got, want); fails = 1; }
}

// a reject leg: tz_parse must return -1, leave *out untouched (sentinels), and
// leave the previously loaded rule's tz_now unchanged.
static void reject(const char *name, const char *s) {
  tz_rule r;
  r.std_off = -12345; r.dst_off = -12345; r.has_dst = -12345;
  r.m1 = r.w1 = r.d1 = r.t1 = -12345;
  r.m2 = r.w2 = r.d2 = r.t2 = -12345;
  leg(name, tz_parse(s, &r), -1);
  leg(name, r.std_off, -12345);
  leg(name, r.has_dst, -12345);
  leg(name, tz_now(E(2026, 1, 15, 12, 0, 0)), 3600);   // Sweden winter still in effect
  leg(name, tz_now(E(2026, 7, 15, 12, 0, 0)), 7200);   // ... and summer
}

int main(void) {
  tz_rule r, sw, utc0;

  // no rule loaded yet: the active rule is UTC
  leg("unloaded", tz_now(E(2026, 7, 15, 12, 0, 0)), 0);

  // Sweden, both 2026 transitions at 1 s granularity. Default rules: both
  // flips land on 01:00 UTC (02:00 local standard being left).
  if (tz_parse("CET-1CEST,M3.5.0,M10.5.0", &sw) != 0) { printf("FAIL sweden parse\n"); return 1; }
  leg("sw std_off", sw.std_off, 3600);
  leg("sw dst_off", sw.dst_off, 7200);
  leg("sw jan", tz_offset(&sw, E(2026, 1, 15, 12, 0, 0)), 3600);
  leg("sw jul", tz_offset(&sw, E(2026, 7, 15, 12, 0, 0)), 7200);
  leg("sw dst-start-", tz_offset(&sw, E(2026, 3, 29, 0, 59, 59)), 3600);
  leg("sw dst-start", tz_offset(&sw, E(2026, 3, 29, 1, 0, 0)), 7200);
  leg("sw dst-end-", tz_offset(&sw, E(2026, 10, 25, 0, 59, 59)), 7200);
  leg("sw dst-end", tz_offset(&sw, E(2026, 10, 25, 1, 0, 0)), 3600);

  // w=1 is the first occurrence (pins week indexing the other way from w=5);
  // d=1 is Monday (0 = Sunday). 2026-01-05 is the first Monday, flip at 02:00
  // local standard = 01:00 UTC.
  if (tz_parse("CET-1CEST,M1.1.1,M12.1.0", &r) != 0) { printf("FAIL first-Monday parse\n"); return 1; }
  leg("first-Monday before", tz_offset(&r, E(2026, 1, 5, 0, 59, 59)), 3600);
  leg("first-Monday at", tz_offset(&r, E(2026, 1, 5, 1, 0, 0)), 7200);
  leg("first-Monday jan4", tz_offset(&r, E(2026, 1, 4, 12, 0, 0)), 3600);

  // explicit t is local time of the side being left: M10.5.0/3 ends at
  // 03:00 CEST = 01:00 UTC (same instant as the default here - the point is
  // that /3 is read against DST, not against UTC).
  if (tz_parse("CET-1CEST,M3.5.0,M10.5.0/3", &r) != 0) { printf("FAIL /3 parse\n"); return 1; }
  leg("/3 end-", tz_offset(&r, E(2026, 10, 25, 0, 59, 59)), 7200);
  leg("/3 end", tz_offset(&r, E(2026, 10, 25, 1, 0, 0)), 3600);

  // fractional, fixed and zero offsets (POSIX counts west-positive: -5:30 is
  // UTC+5:30; GMT+3 is UTC-3).
  if (tz_parse("IST-5:30", &r) != 0) { printf("FAIL IST parse\n"); return 1; }
  leg("IST", tz_offset(&r, E(2026, 7, 15, 12, 0, 0)), 19800);
  if (tz_parse("GMT+3", &r) != 0) { printf("FAIL GMT parse\n"); return 1; }
  leg("GMT+3", tz_offset(&r, E(2026, 1, 15, 12, 0, 0)), -10800);
  if (tz_parse("UTC0", &utc0) != 0) { printf("FAIL UTC0 parse\n"); return 1; }
  leg("UTC0", tz_offset(&utc0, E(2026, 7, 15, 12, 0, 0)), 0);

  // Southern hemisphere, DST the majority of the year: January is the tail of
  // the season that started the previous October (year wrap), July is standard.
  if (tz_parse("AEST-10AEDT,M10.1.0,M4.1.0/3", &r) != 0) { printf("FAIL AEST parse\n"); return 1; }
  leg("AEST jan", tz_offset(&r, E(2026, 1, 15, 12, 0, 0)), 39600);
  leg("AEST jul", tz_offset(&r, E(2026, 7, 15, 12, 0, 0)), 36000);

  // quoted names are accepted (the engine ignores the name itself)
  leg("quoted", tz_parse("<+05>-5", &r), 0);
  leg("quoted off", tz_offset(&r, E(2026, 1, 15, 12, 0, 0)), 18000);

  // rejects: each leaves the output struct and the loaded rule untouched.
  // The legs below load Sweden first and re-check tz_now inside reject().
  tz_load(&sw);
  leg("loaded winter", tz_now(E(2026, 1, 15, 12, 0, 0)), 3600);
  leg("loaded summer", tz_now(E(2026, 7, 15, 12, 0, 0)), 7200);
  reject("no rules", "EST5EDT");
  reject("one rule", "CET-1CEST,M3.5.0");
  reject("nonsense", "nonsense");
  reject("three rules", "CET+1CEST,M3.5.0,M10.5.0,M11.1.0");
  {
    char big[201];
    int i;
    for (i = 0; i < 200; i++) big[i] = 'A';
    big[200] = 0;
    reject("oversize", big);
  }
  reject("junk tail", "CET-1CEST,M3.5.0,M10.5.0,nonsense");
  reject("huge offset", "CET-99999999999999");

  // tz_load applies a parsed rule; tz_now is tz_offset over the active rule.
  tz_load(&utc0);
  leg("reloaded", tz_now(E(2026, 7, 15, 12, 0, 0)), 0);
  tz_load(&sw);

  // stateless per call: a decade of fixed epochs sees only the two expected
  // offsets, and interleaving another rule never poisons the next call.
  {
    int y;
    for (y = 2020; y <= 2030; y++) {
      long jan = E(y, 1, 15, 12, 0, 0), jul = E(y, 7, 15, 12, 0, 0);
      leg("decade winter", tz_offset(&sw, jan), 3600);
      leg("decade poison", tz_offset(&utc0, jan), 0);
      leg("decade winter again", tz_offset(&sw, jan), 3600);
      leg("decade summer", tz_offset(&sw, jul), 7200);
      leg("decade poison2", tz_offset(&utc0, jul), 0);
      leg("decade summer again", tz_offset(&sw, jul), 7200);
    }
  }

  if (fails) return 1;
  printf("ok\n");
  return 0;
}
