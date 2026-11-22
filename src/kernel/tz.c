// tz.c - see tz.h. Pure POSIX TZ engine: no libc, no Arduino, no allocation,
// no floats, fixed struct only - it compiles into the kernel (arm-none-eabi),
// host_linux (gcc) and the native unit tests from this one file.
//
// tz_offset is stateless (a pure function of rule + instant); the only state
// is the single active rule behind tz_load/tz_now. Transitions are computed
// per call for the UTC year around the epoch, so a DST flip needs no kernel
// event and a clock correction can never straddle a stale offset.
#include "tz.h"

#define TZ_MAX_STR 96                   // longer strings are rejected outright
#define TZ_MAX_T   (167 * 3600 + 59 * 60 + 59) // POSIX rule-time bound, +-167:59:59

static tz_rule tz_active;               // the one active rule; has_dst=0 with std_off=0 is UTC
static int tz_active_set;

// civil <-> fixed days (Howard Hinnant's algorithm), signed variant; tz.c is
// self-contained so it carries its own copy (the spec blesses the duplication).
static long tz_days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  long yoe = y - era * 400;
  long doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return era * 146097L + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
static void tz_civil_from_days(long z, int *py, int *pm) {
  z += 719468;
  long era = (z >= 0 ? z : z - 146096) / 146097;
  long doe = z - era * 146097;
  long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  long mp = (5 * doy + 2) / 153;
  int m = (int)(mp + (mp < 10 ? 3 : -9));
  *pm = m;
  *py = (int)(yoe + era * 400 + (m <= 2));
}

static int tz_leap(int y) { return y % 4 == 0 && (y % 100 != 0 || y % 400 == 0); }

static int tz_mlen(int y, int m) {
  static const int len[12] = {31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31};
  if (m == 2 && tz_leap(y)) return 29;
  return len[m - 1];
}

// day-of-month of the w-th (1..5, 5 = last) d (0 = Sunday) in month m of year y.
// 1970-01-01 was a Thursday, and Sunday=0 puts it at (0 + 4) % 7.
static int tz_nth_weekday(int y, int m, int w, int d) {
  int first = (int)((tz_days_from_civil(y, m, 1) + 4) % 7);
  int day = 1 + (d - first + 7) % 7;                 // first occurrence
  if (w < 5) return day + (w - 1) * 7;               // 1+6+21 <= 28, always in month
  int last = tz_mlen(y, m);
  int lastdow = (int)((tz_days_from_civil(y, m, last) + 4) % 7);
  return last - ((lastdow - d + 7) % 7);             // last occurrence
}

// UTC instant of one rule fired in year y: the rule time is local time of the
// side being left (std_off before a DST start, dst_off before a DST end), so
// subtract that offset to get UTC. Southern zones need no special case: their
// start already sorts after their end within a year.
static long tz_rule_utc(int y, int m, int w, int d, long t, int left_off) {
  return tz_days_from_civil(y, m, tz_nth_weekday(y, m, w, d)) * 86400L + t - left_off;
}

// ---- parsing: every helper returns the next position, or 0 on a miss;
// results land in a local struct and *r is written only on full success.

static int tz_digit(char c) { return c >= '0' && c <= '9'; }
static int tz_alpha(char c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }

// [+-]hh[:mm[:ss]], west-positive as POSIX says; value in *out.
static const char *tz_parse_offset(const char *p, int *out) {
  int neg = 0, h = 0, m = 0, s = 0;
  if (*p == '+') p++;
  else if (*p == '-') { neg = 1; p++; }
  if (!tz_digit(*p)) return 0;
  while (tz_digit(*p)) {                    // cap the accumulator: a 96-char digit run must not overflow int
    if (h < 1000) h = h * 10 + (*p++ - '0');
    else p++;
  }
  if (*p == ':') {
    if (!tz_digit(p[1]) || !tz_digit(p[2])) return 0;
    m = (p[1] - '0') * 10 + (p[2] - '0');
    p += 3;
    if (*p == ':') {
      if (!tz_digit(p[1]) || !tz_digit(p[2])) return 0;
      s = (p[1] - '0') * 10 + (p[2] - '0');
      p += 3;
    }
  }
  if (h > 167 || m > 59 || s > 59) return 0;
  *out = neg ? -(h * 3600 + m * 60 + s) : h * 3600 + m * 60 + s;
  return p;
}

// std/dst name: <quoted> (anything but < > inside) or letters only, per the
// POSIX grammar - a name must stop before its offset, so `UTC0` is the name
// `UTC` at offset 0, not one alphanumeric name. 1..16 letters. The name
// itself is not kept (the kernel stores no display copy), so this only
// validates and advances.
static const char *tz_parse_name(const char *p) {
  int n;
  if (*p == '<') {
    p++;
    for (n = 0; *p && *p != '>'; p++) {
      if (*p == '<') return 0;
      n++;
    }
    if (*p != '>' || n == 0) return 0;
    return p + 1;
  }
  for (n = 0; n < 16 && tz_alpha(*p); p++) n++;
  return n ? p : 0;
}

// M m.w.d[/time]: week 5 means last, time defaults to 02:00 local standard
// (t2 callers resolve that against the offsets - see tz.h). t_explicit says
// whether /n was present.
static const char *tz_parse_rule(const char *p, int *pm, int *pw, int *pd, int *pt, int *t_explicit) {
  int m, w, d, t = 7200, ex = 0;
  const char *q;
  if (*p++ != 'M') return 0;
  if (!tz_digit(*p)) return 0;
  m = *p++ - '0';
  if (tz_digit(*p)) m = m * 10 + (*p++ - '0');
  if (*p++ != '.' || !tz_digit(*p)) return 0;
  w = *p++ - '0';
  if (*p++ != '.' || !tz_digit(*p)) return 0;
  d = *p++ - '0';
  if (*p == '/') {
    int v;
    q = tz_parse_offset(p + 1, &v);
    if (!q || v < -TZ_MAX_T || v > TZ_MAX_T) return 0;
    t = v; ex = 1; p = q;
  }
  if (m < 1 || m > 12 || w < 1 || w > 5 || d > 6) return 0;
  *pm = m; *pw = w; *pd = d; *pt = t; *t_explicit = ex;
  return p;
}

int tz_parse(const char *s, tz_rule *r) {
  tz_rule t;
  const char *p = s, *q;
  int n = 0, off;
  int t1_explicit, t2_explicit;
  if (!s) return -1;
  while (s[n]) { if (n >= TZ_MAX_STR) return -1; n++; }
  if (n == 0) return -1;

  t.has_dst = 0;
  t.m1 = t.w1 = t.d1 = t.t1 = 0;
  t.m2 = t.w2 = t.d2 = t.t2 = 0;

  if (!(p = tz_parse_name(p))) return -1;
  if (!(p = tz_parse_offset(p, &off))) return -1;   // bare names (no offset) are junk
  t.std_off = -off;                                 // POSIX west-positive -> seconds east

  q = tz_parse_name(p);                             // an unclaimed tail must be the DST name...
  if (!q || !*q) {
    if (!*p) { *r = t; return 0; }                  // ...or nothing: pure standard
    return -1;
  }
  p = q;
  t.has_dst = 1;
  t.dst_off = t.std_off + 3600;                     // POSIX default
  q = tz_parse_offset(p, &off);                     // ...and its offset is optional
  if (q) { t.dst_off = -off; p = q; }

  if (*p++ != ',') return -1;                       // a DST name demands both rules
  if (!(p = tz_parse_rule(p, &t.m1, &t.w1, &t.d1, &t.t1, &t1_explicit))) return -1;
  if (*p++ != ',') return -1;
  if (!(p = tz_parse_rule(p, &t.m2, &t.w2, &t.d2, &t.t2, &t2_explicit))) return -1;
  if (*p) return -1;

  // t1's default 02:00 is already left-side time (a DST start is left in
  // standard); only the end rule's default needs resolving against the gap.
  if (!t2_explicit) t.t2 = 7200 + (t.dst_off - t.std_off);  // 02:00 local standard, as left-side time
  *r = t;
  return 0;
}

int tz_offset(const tz_rule *r, long utc) {
  int y, i, state, have = 0;
  long best = 0;
  long days;

  if (!r->has_dst) return r->std_off;
  days = utc >= 0 ? utc / 86400 : -((-utc + 86399) / 86400);   // floor
  tz_civil_from_days(days, &y, &i);

  // The offset in effect is the one installed by the latest transition at or
  // before utc. Transitions repeat every year, so the UTC year of utc plus
  // its two neighbors always bracket the epoch (rule times up to +-167 h can
  // push a transition out of the year its rule is named after). None found
  // means the rule predates the candidates: standard is the only safe read.
  state = r->std_off;
  for (i = y - 1; i <= y + 1; i++) {
    long s = tz_rule_utc(i, r->m1, r->w1, r->d1, r->t1, r->std_off);
    long f = tz_rule_utc(i, r->m2, r->w2, r->d2, r->t2, r->dst_off);
    if (s <= utc && (!have || s > best)) { best = s; state = r->dst_off; have = 1; }
    if (f <= utc && (!have || f > best)) { best = f; state = r->std_off; have = 1; }
  }
  return state;
}

void tz_load(const tz_rule *r) { tz_active = *r; tz_active_set = 1; }

int tz_now(long utc) { return tz_active_set ? tz_offset(&tz_active, utc) : 0; }
