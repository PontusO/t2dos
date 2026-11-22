// rtc.h - the wall clock: one kernel-owned UTC epoch clock (see rtc.cpp). A DS1307 on I2C
// seeds it at boot, SNTP over WiFi corrects it while connected, and it always ticks on
// millis() in between. Like vault, a self-contained hardware module with a kernel-internal
// API; clock_service() runs from the console-poll hook only, never inside a file, flash
// or WiFi operation.
#pragma once

// at boot, after Wire.begin: probe the DS1307 and seed the clock if the chip is alive.
void rtc_init(void);
// from the console-poll hook: re-probe the chip, drive the SNTP machine. Never blocks.
void clock_service(void);
// the running second since 1970 UTC; ticks from boot even while unsynced.
unsigned long clock_now(void);
// 1 once the chip or NTP has seeded a real time, 0 while ticking on the boot timer.
int clock_valid(void);
