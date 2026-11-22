# NOTES: wall clock (round 2026-11-22) — DONE

Goal: kernel wall clock - DS1307 probe + SNTP + `millis()` timer,
precedence chip > NTP > unsynced-timer; `SYS_time = 105` in all four
places, the `time(&valid)` builtin, `/bin/date`. Spec:
`docs/superpowers/specs/2026-11-22-clock-ds1307-design.md` (Results
there too), plan `docs/superpowers/plans/2026-11-22-clock-ds1307.md`.
SDD ledger (all rulings): `.superpowers/sdd/2026-11-22-clock-ds1307/progress.md`,
board evidence trail in `task-4-report.md` beside it. Merged to `main`
2026-11-22.

## Settled (don't re-investigate)

- esp_hosted UDP bind ordering (two board-proven defects): the core
  (6.3.2) registers the `udp_recv` callback only in
  `UdpContext::listen()`, reached only from `WiFiUDP::begin()` - there
  is no lazy socket on this path, so a bare `beginPacket` sends but
  never receives; and the lwIP stack itself (`ESPHostLwIP::initHW`) only
  comes up with `WiFi.mode`/`WiFi.begin`, so `udp.begin(0)` must run at
  the `WL_CONNECTED` edge, not in `rtc_init` before the radio init.
  `udp_bound` clears on the reconnect edge; a reconnect while a reply
  is pending costs up to 62 s extra first-resync latency (parked).
- Service coupling: `clock_service()` runs only from the console
  `getc`/`kbhit`/`peek` wrappers (the `telnetd_service()` pattern) -
  owner-accepted: sync progresses while the console is polled, stalls
  under a foreground program that never touches it; the scheduler-idle-
  hook alternative was rejected over the no-switch-inside-WiFi
  invariant. Reply polling is non-blocking (`millis()` deadlines
  checked per poll, 2 s bound); retries 60 s after a failure, resync
  hourly after a success.
- `SYS_time`'s valid-flag pointer is the *first* syscall arg, `A(0)`
  (the plan text said `A(1)` - plan bug, corrected); `host_linux`
  implements `SYS_time` as `time(NULL)` with valid=1 - that is the
  PC-side contract test.
- `QEMU_RUN` in `check_toolchain.py`: names cases that PC mode also
  *runs* under `qemu-arm host_linux` and diffs against `.expected`
  (PC mode previously never executed case binaries; execution with
  `.expected` was board-only). The `clock` case asserts real time and
  valid=1 under the harness clock; board mode skips it (the kernel
  clock is only opportunistically synced at check time).
- No DS1307 on the bench (owner-confirmed at design time - spec
  `2026-11-22-clock-ds1307-design.md`, Tests section: "owner confirmed
  2026-11-22: no DS1307 on the bench"). Only the probe-false branch
  is board-verified: chip-less boots tick unsynced from the epoch, the
  absent `0x68` probe never hangs, and `rtc_write` write-back NACKs
  silently. Boot-from-chip, heal-a-live-chip and hourly resync stay
  untested - deferred to a round that has a chip.
- Re-probe gated on `!valid` (ruling against the spec's literal
  hot-plug clause): a synced clock has nothing to pick up and the
  hourly write-back heals a connected chip anyway; the literal reading
  costs two I2C transactions per minute forever for "offline board +
  chip hot-plugged later", not this bench. Chip value window
  `[1577836800, 4102444800)` = 2020..2100, outside it = garbage.
- `check_sh.py` has no `--board` mode (the AGENTS triage phrase "their
  `--board` variants" is not universal); the board console regression
  was substituted (script replay + pipe/redirect smoke + timed console
  round-trips). Flashing this bench needs `reboot -b` before
  `arduino-cli upload` (no auto-UF2; plain upload says "No drive to
  deploy").

## Gates at completion

`make check` ok (end-to-end); `check_ref` different: 0 - 39 identical,
`date.c` in the "newer builtins" class: `check_ref.py`'s
`NEWER_BUILTINS` gained `'time'`, the established exclusion mechanism
(`wifi.c` & co.) for corpus files using builtins the reference cc
predates; `check_toolchain --pc` 10 cases (the `clock` QEMU_RUN case
added); `check_make --pc` 13/13; `bench --pc` ok. Board
(task-4-report, across the three passes
`c126b88`/`10a4d87`/`d608c04`): sync <=16 s from boot, and after a
reboot 4 s after the join edge (~19 s uptime), 1 s board-vs-PC delta
holding +127 s, 0 s drift over 90 s unsynced, ~45 console round-trips
uniform 0.38 s; `--selfhost` six IDENTICAL 2m28.8s (first pass, run on
`c126b88` - the two later fix commits are kernel-only).
