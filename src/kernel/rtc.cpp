// rtc.cpp - see rtc.h. One UTC timekeeper: base_epoch + (millis()-base_ms)/1000, always
// ticking; the DS1307 and SNTP only ever correct the base, they never tick themselves.
// millis() wraps after ~49.7 days: the hourly NTP sync rebases base_ms, which makes the
// wrap moot in practice, and an unsynced board running 49+ days offline is a dev bench,
// not a deployment. Kernel-side Wire use is atomic against the SYS_i2c_* syscalls: both
// run in the same task context between switch points, so no bus lock is needed.
// Nothing here blocks or switches tasks: Wire transactions are one shot each (sub-ms,
// a NACK on an absent chip returns immediately, no retry loops) and the UDP reply is
// polled with a millis() deadline, never a busy wait.
#include <Arduino.h>
#include <Wire.h>
#include <WiFi.h>
#include <WiFiUdp.h>
#include <sys/time.h>
#include "rtc.h"

#define DS1307_ADDR 0x68
#define NTP_EPOCH 2208988800UL      // NTP seconds at the Unix epoch (1900..1970)
#define EPOCH_MIN 1577836800UL      // 2020-01-01: below this the chip holds garbage
#define EPOCH_MAX 4102444800UL      // 2100-01-01
#define NTP_RETRY_MS 60000          // one retry a minute after a failed request, never a loop
#define NTP_HOURLY_MS 3600000          // resync hourly while connected
#define PROBE_MS 60000              // chip re-probe period (hot-plug support)
#define NTP_REPLY_MS 2000           // reply bound, polled per service call

static uint32_t base_epoch;         // the epoch seconds as of base_ms
static uint32_t base_ms;
static int valid;                   // 1 once chip or NTP has seeded a real time

static uint32_t next_try;           // millis at which the next NTP request may go out
static uint32_t rx_deadline;        // non-zero while one request awaits its reply
static uint32_t next_probe;         // millis of the next DS1307 re-probe
static int last_connected;

static WiFiUDP udp;

unsigned long clock_now(void) { return base_epoch + (uint32_t)((millis() - base_ms) / 1000); }
int clock_valid(void) { return valid; }

// the one correction path: rebase, mark valid, and mirror into the core clock so
// time() and LittleFS's getLastWrite() agree with us (the round-B bridge).
static void clock_correct(uint32_t t) {
  base_epoch = t; base_ms = millis(); valid = 1;
  struct timeval tv; tv.tv_sec = t; tv.tv_usec = 0;
  settimeofday(&tv, NULL);
}

// civil <-> fixed days (Howard Hinnant's algorithm); only ever called with the
// 2020..2100 range pinned above, so 32-bit days and unsigned division are safe.
static uint32_t days_from_civil(int y, int m, int d) {
  y -= m <= 2;
  int era = (y >= 0 ? y : y - 399) / 400;
  uint32_t yoe = y - era * 400;
  uint32_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
  return era * 146097 + yoe * 365 + yoe / 4 - yoe / 100 + doy - 719468;
}
static void civil_from_days(uint32_t z, int *y, int *m, int *d) {
  z += 719468;
  uint32_t era = z / 146097;
  uint32_t doe = z - era * 146097;
  uint32_t yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
  uint32_t doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
  uint32_t mp = (5 * doy + 2) / 153;
  *d = doy - (153 * mp + 2) / 5 + 1;
  *m = mp + (mp < 10 ? 3 : -9);
  *y = yoe + era * 400 + (*m <= 2);
}

static int bcd2bin(uint8_t b) { return (b >> 4) * 10 + (b & 15); }
static uint8_t bin2bcd(int v) { return (v / 10) << 4 | v % 10; }

// read regs 0..6 (the pointer write and the read are one transaction each, no retry:
// an absent 0x68 just NACKs / returns 0 bytes, which is the common bench state).
// Valid iff the oscillator runs (CH clear), the fields decode in range (a chip with a
// dead battery reads back nonsense BCD) and the epoch is 2020..2100. 1 on success.
static int ds1307_read(uint32_t *epoch) {
  Wire.beginTransmission(DS1307_ADDR); Wire.write(0);
  if (Wire.endTransmission() != 0) return 0;
  if (Wire.requestFrom(DS1307_ADDR, 7) != 7) return 0;
  uint8_t r[7];
  for (int i = 0; i < 7; i++) r[i] = Wire.read();
  if (r[0] & 0x80) return 0;                          // CH set: oscillator halted
  int s = bcd2bin(r[0]), m = bcd2bin(r[1]), h = bcd2bin(r[2] & 0x3f),   // 24 h mode (the default)
      d = bcd2bin(r[4]), mo = bcd2bin(r[5]), y = 2000 + bcd2bin(r[6]);
  if (s > 59 || m > 59 || h > 23 || d < 1 || d > 31 || mo < 1 || mo > 12) return 0;
  uint32_t e = days_from_civil(y, mo, d) * 86400UL + (h * 60 + m) * 60UL + s;
  if (e < EPOCH_MIN || e >= EPOCH_MAX) return 0;
  *epoch = e;
  return 1;
}

// write regs 6..0, reg 0 last and with CH clear: the oscillator only starts on a
// complete, consistent time, so a part-write (dead battery) leaves the clock cleanly
// halted instead of running on garbage. Chip absent: both transactions NACK, ignored.
static void rtc_write(uint32_t epoch) {
  int y, mo, d;
  uint32_t days = epoch / 86400, sec = epoch % 86400;
  civil_from_days(days, &y, &mo, &d);
  uint8_t dow = (days + 4) % 7 + 1;   // DS1307 counts Sunday 1; 1970-01-01 was a Thursday
  Wire.beginTransmission(DS1307_ADDR);
  Wire.write(1); Wire.write(bin2bcd((sec / 60) % 60)); Wire.write(bin2bcd(sec / 3600));
  Wire.write(dow); Wire.write(bin2bcd(d)); Wire.write(bin2bcd(mo)); Wire.write(bin2bcd(y - 2000));
  if (Wire.endTransmission() != 0) return;
  Wire.beginTransmission(DS1307_ADDR);
  Wire.write(0); Wire.write(bin2bcd(sec % 60));
  Wire.endTransmission();
}

// SNTP: one request, its reply polled per service call under a 2 s bound, hourly resync
// while connected, 60 s backoff on DNS miss, send failure or no reply. beginPacket(host)
// resolves via the core. A success also writes back to the chip: a fresh (or dead-
// battery) DS1307 gets healed and boot-from-chip works on the next reboot.
static void sntp_service(uint32_t now_ms) {
  if (rx_deadline) {
    if (udp.parsePacket() >= 48) {
      uint8_t r[48];
      udp.read(r, 48);
      rx_deadline = 0;
      next_try = now_ms + NTP_HOURLY_MS;
      uint32_t t = ((uint32_t)r[40] << 24 | r[41] << 16 | r[42] << 8 | r[43]) - NTP_EPOCH;   // server tx time
      if (t >= EPOCH_MIN && t < EPOCH_MAX) {          // sanity: never trust a wild timestamp
        clock_correct(t);
        rtc_write(t);
      }
    } else if ((int32_t)(now_ms - rx_deadline) >= 0) {
      rx_deadline = 0;
      next_try = now_ms + NTP_RETRY_MS;
    }
    return;
  }
  int connected = WiFi.status() == WL_CONNECTED;
  if (connected && !last_connected) next_try = 0;     // the WL_CONNECTED edge asks for a sync now
  last_connected = connected;
  if (!connected || (int32_t)(now_ms - next_try) < 0) return;
  uint8_t req[48] = {0x1B};                           // LI 0, VN 3, Mode 3 (client); payload zero
  if (!udp.beginPacket("pool.ntp.org", 123) || !udp.write(req, 48) || !udp.endPacket()) {
    next_try = now_ms + NTP_RETRY_MS;
    return;
  }
  rx_deadline = now_ms + NTP_REPLY_MS;
}

void rtc_init(void) {
  base_ms = millis();               // ticks from 1970 (valid=0) until something seeds us
  udp.begin(0);                     // bind the rx side up front: the core registers its lwIP
                                    // recv callback only in begin()/listen(), so without this
                                    // the reply to a beginPacket-sent request is never read.
                                    // Port 0 asks lwIP for a random one from its ephemeral
                                    // range; binding while WiFi is still down is fine.
  next_probe = millis() + PROBE_MS;
  uint32_t t;
  if (ds1307_read(&t)) clock_correct(t);
}

// the console-poll hook: probe re-try + the SNTP machine, deadline driven, so it
// returns in microseconds whenever nothing is due.
void clock_service(void) {
  uint32_t now_ms = millis();
  if (!valid && (int32_t)(now_ms - next_probe) >= 0) {   // a chip hot-plugged after boot is picked up
    next_probe = now_ms + PROBE_MS;
    uint32_t t;
    if (ds1307_read(&t)) clock_correct(t);
  }
  sntp_service(now_ms);
}
