// tz.h - the pure POSIX TZ engine (kernel, host_linux and the PC unit tests
// all compile the same tz.c; it is the single DST implementation).
//
// Grammar: std offset [dst [offset] ,rule ,rule] with M m.w.d rules and
// optional /time. Names may be quoted with < >. A DST name without exactly
// two rules, non-M rule forms, a bare name (no offset) and junk are rejected.
//
// POSIX sign trap: offsets are west-POSITIVE, so local = UTC + std_off means
// CET-1 is UTC+1 and GMT+3 is UTC-3. Offsets are seconds east of UTC here
// (std_off = -parsed). dst_off defaults to std_off + 3600 when omitted.
//
// Sentinel edge: a caller that maps a reject to -1 collides with the legal
// -1-second offset of a sub-minute west zone (GMT+0:0:1 parses); no real
// POSIX rule does this, and SYS_tzset in sys.h documents the collision.
//
// Rule times: an explicit /n is local time of the side being left (the end
// rule's /3 is 03:00 DST = 01:00 UTC for Sweden). An omitted /n means 02:00
// local standard, so a Sweden-style string flips both ways at 01:00 UTC - the
// hour Europe actually changes at. t2 stores the resolved left-side time.
#ifndef TZ_H
#define TZ_H
#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
  int std_off, dst_off;         // seconds, local = UTC + off; dst_off = std_off + 3600 when the string omits it
  int has_dst;                  // 0: pure-standard rule, offset is always std_off
  int m1, w1, d1, t1;           // DST start rule: month 1..12, week 1..5 (5 = last), weekday 0..6 (0 = Sunday), t1 = seconds local (default 7200)
  int m2, w2, d2, t2;           // DST end rule, same shape
} tz_rule;

int  tz_parse(const char *s, tz_rule *r);   // 0 on success, -1 malformed (rejects: DST name without exactly two rules; non-M rule forms; junk; length > 96)
int  tz_offset(const tz_rule *r, long utc); // seconds to add to utc; also exported as the active-rule state below
void tz_load(const tz_rule *r);             // install as active rule (kernel and host keep exactly one)
int  tz_now(long utc);                      // tz_offset(active, utc); active with no load = UTC (offset 0)

#ifdef __cplusplus
}
#endif
#endif
