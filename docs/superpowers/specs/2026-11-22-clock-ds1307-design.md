# Wall clock: DS1307 + NTP + timer fallback (design, 2026-11-22)

## Intent and why

T2DOS has no credible clock: `stat_t.mtime` is already in the ABI and the
kernel fills it from `f.getLastWrite()`, but the core's clock has no epoch
source, so every timestamp is seconds-since-boot wearing 1970 clothing.
This round adds a real wall clock - a DS1307 I2C RTC, synced from a public
NTP server over WiFi, ticking on the RP2350 timer when neither is (yet)
available - and exposes it as a `time()` syscall. The next round (file
mtimes, `ls`/`rls` display, the `make` timestamp pass `Makefile.board`
predicts) takes this clock as a given. Owner decisions: split A/B agreed;
the board is the Challenger 2350 WiFi (the only radio); `Wire` on the
variant-defined pins, not hardcoded GPIO.

## Goals

- One kernel-owned epoch clock with precedence: DS1307 at boot -> NTP when
  WiFi is up -> pure timer otherwise. The timer always ticks; sources only
  ever *correct* the base.
- `SYS_time`: seconds since 1970 plus a valid flag; four-places rule
  honoured; `host_linux` returns the real `time()`/valid=1 so every tool
  behaves the same under qemu.
- DS1307 behind `Wire` using the variant's `PIN_WIRE0_SDA/SCL` (the
  Challenger 2350 variant defines 20/21; its `PIN_WIRE1` is a dummy 31/31,
  so the variant definitions are the only safe source).
- `date` userland tool printing UTC.

## Non-goals

- File `mtime` semantics, `ls` timestamps, `make` incremental (round B -
  though this design keeps the core clock mirrored so `getLastWrite()`
  becomes honest "for free").
- Timezones and DST (UTC everywhere; tools display UTC).
- A `date` setter (manual time entry): no. An SNTP client with WiFi-saved
  at boot is the sync path; a setter can come later if it earns its keep.
- DS1302 support: none (bit-bang sibling, out by decision).

## Constraints

- Kernel additions follow the vault pattern (own `.cpp/.h` in
  `src/kernel/`, kernel-internal API, no new task-switch points; DS1307
  Wire transactions are sub-millisecond; NTP is one UDP packet with a
  non-blocking reply poll - the whole service runs from the console-poll
  hook (see Tests), never inside file/flash/WiFi calls).
- Userland code (`date.c`) stays in the committed cc's subset.
- `SYS_time = 105` (next free in the board block after `SYS_delay_us = 104`;
  `SYS_i2c_write/read = 101/102` already exist and stay untouched). The
  kernel already `#include <Wire.h>` and calls `Wire.begin()` in `setup()`
  (default instance, variant `PIN_WIRE0_SDA/SCL`) for those syscalls - the
  rtc module reuses that running bus, adds no `Wire.begin` of its own, and
  keeps its transactions short enough not to starve userland `i2c_*` use.
- The board cc must still parse all kernel-adjacent sources it compiles;
  the kernel itself is built by arduino-cli (not constrained by the subset).

## Clock model (my calls, as decided in the conversation)

- Kernel timekeeper: `base_epoch` + `(millis() - base_ms)/1000`, plus a
  `time_valid` flag. At boot `base_epoch = 0` (unsynced, still ticking).
- DS1307 probe (address `0x68`): valid iff register 0's clock-halt bit is
  clear AND the decoded epoch is in [2020-01-01, 2100). Invalid chip
  (unpopulated, dead battery) simply probes false - no special cases.
- NTP: on the `WL_CONNECTED` edge as seen from the console-poll service
  hook (`loop()` blocks in `run_bin` and is not revisited), one SNTP packet to
  `pool.ntp.org:123` via `WiFiUDP` (`beginPacket(host)` resolves via the
  core), reply polled with a ~2 s bound, transmit timestamp converted
  (NTP epoch offset 2208988800). Retried hourly while connected; no
  connection storm.
- On a successful NTP sync, always write the epoch back to the DS1307
  (start oscillator, set BCD registers) - a fresh chip gets healed and a
  dead-battery board still converges to real time after boot.
- Mirror every correction into the core clock (`settimeofday`, arduino-pico
  implements it) so `time(nullptr)` and LittleFS `getLastWrite()` agree -
  the bridge to round B.
- `SYS_time(valid_ptr)`: returns the running second; if `valid_ptr`
  non-null, stores 1 when synced, 0 while running on the unsynced timer.

## Tests

- The `time()` four-places change needs an executing PC gate: PC mode of
  `check_toolchain` never runs case binaries (execution with `.expected`
  is board mode only, under the *kernel* clock - which is unsynced at
  check time), so `check_toolchain.py` gains a small PC-only step: cases
  flagged `QEMU` are built with `cc`, run under `qemu-arm host_linux`,
  and compared to `.expected`. `clock.c` asserts `time(&v) > 1700000000
  && v == 1` and prints `ok` - true under `host_linux` (real clock), and
  the case is skipped in board mode (`BOARD_SKIP`), where the kernel
  clock is only synced opportunistically. `host_linux` implementing
  `SYS_time` as `time(NULL)`/valid=1 is thus the PC-side contract test.
- A `compiler/tests/test16.c` gcc-diffed case is NOT possible (no portable
  `time` builtin), same reasoning as `static_init.c`: a pinned pipeline
  case is the right shape.
- Board (the only path to the hardware truth, owner pre-approved; owner
  confirmed 2026-11-22: **no DS1307 on the bench** - the chip-precedence
  paths are implemented and code-reviewed but board-verify only their
  probe-false branch, which is exactly the chip-less state the bench is
  in): `date` output within seconds of `date -u` on the PC after the
  WiFi-up NTP edge; the clock ticks across a reboot-with-WiFi-off until
  the next sync (timer fallback); console `kbhit` paths measurably
  unaffected by the sync hook; `wifi` tool SSID flow unchanged. Boot-from-
  chip and heal-a-live-chip tests are deferred to a future round that has
  a chip (noted in Results).
- The sync hook runs from the console-poll path (the `telnetd_service()`
  pattern, called from the console `getc`/`kbhit`/`peek` wrappers): NTP
  sync and RTC correction progress while the console is polled and stall
  while a foreground program never touches it. Owner accepted this
  trade-off 2026-11-22 (the scheduler-idle-hook alternative risks the
  no-switch-inside-WiFi invariant). The reply poll is non-blocking
  (deadlines in `millis()` checked per poll, ~2 s bound), never a busy
  wait.

## What's left to the builder

- Exact BCD/epoch helpers (a small `days_from_civil`/reverse pair in
  `rtc.cpp` - 2000..2099 range, leap years by rule).
- `date` tool formatting and its one-line README entry.

## Results (landed 2026-11-22)

Implemented as designed, with the promised deferral (no DS1307 on the
bench: chip-boot and heal-a-live-chip tests wait for a round that has a
chip) and four deviations, all ledgered:

- Two esp_hosted bind fixes the design did not anticipate, both
  board-proven through fix rounds on Task 2: (1) `udp.begin()` is
  actually required - core 6.3.2 registers the `udp_recv` callback only
  in `begin()`/`listen()` (the lazy-socket reading from review held for
  the pico-WiFi path, not esp_hosted), so without it replies never
  arrive; (2) the bind must happen at the `WL_CONNECTED` edge, not in
  `rtc_init` at boot: the lwIP stack (`ESPHostLwIP::initHW`) only comes
  up with `WiFi.mode`/`WiFi.begin`, so a pcb created earlier binds
  against a stack that does not exist yet. `udp_bound` clears on the
  reconnect edge; a reconnect while a reply is pending costs up to 62 s
  extra first-resync latency (parked, harmless).
- The spec's hot-plug re-probe is gated on `!valid`: a synced clock has
  nothing to pick up, and while connected the hourly write-back heals
  the chip anyway; the literal reading would cost two I2C transactions
  per minute forever for the one path "offline board + chip hot-plugged
  later", which is not this bench.
- `check_sh.py` has no `--board` mode (the AGENTS triage table's "their
  `--board` variants" is not universal) - the console regression was
  substituted: script-case replay, pipe/redirect smoke, and 15 console
  round-trips all 0.38 s.
- Hourly resync is unverifiable inside the test window; the code-level
  `next_try = now_ms + NTP_HOURLY_MS` arm is consistent with
  observation - the 60 s retry loop demonstrably stops cycling after a
  sync and the timer holds.

Verification on the Challenger, across the three passes
(`c126b88`/`10a4d87`/`d608c04`; `--selfhost` ran in the first pass on
`c126b88` - the two later fix commits are kernel-only): unsynced ticks from
the epoch with 0 s drift over 90 s; first NTP sync <=16 s from boot,
board-vs-PC delta 1 s, holds over +127 s; reboot round-trip synced 4 s
after power-on+join; no DNS stall across three passes (~45 round-trips
uniform 0.38 s, spanning both join edges); `rtc_write` to the absent
0x68 is silent; `--selfhost` six IDENTICAL + on-board tests 2m28.8s.
PC gates green - `check_toolchain --pc` at 10 cases, `check_ref`
different: 0 with `date.c` in the "newer builtins" class
(`check_ref.py`'s `NEWER_BUILTINS` gained `'time'` - the plan's "the
corpus uses no `time`" went stale when Task 3 added `date.c`, and no
later gate re-ran `make check` until this pass; ruling and evidence in
`.superpowers/sdd/2026-11-22-clock-ds1307/task-5-report.md`).
