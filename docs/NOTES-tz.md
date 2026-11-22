# NOTES: time zones (round 2026-11-22) — DONE

Goal: kernel-owned POSIX TZ rule with DST: pure engine
`src/kernel/tz.c` + `tz.h`; `SYS_localtime = 106` and `SYS_tzset = 107`
in all four places; `/etc/tz` persistence + boot load; `date` prints
local time with the numeric offset, `-u` stays UTC; the `tz` tool shows,
sets (POSIX rule or `+N` shorthand) and clears. Spec:
`docs/superpowers/specs/2026-11-22-timezone-design.md` (Results there
too), plan `docs/superpowers/plans/2026-11-22-timezones.md`. SDD ledger
(all rulings): `.superpowers/sdd/2026-11-22-timezones/progress.md`,
board evidence trail in `task-5-report.md` beside it. On branch `tz`,
awaiting merge.

## Settled (don't re-investigate)

- POSIX counts zone offsets west-positive: `CET-1` is UTC+1 (a minus
  means *east*) and `GMT+3` is UTC-3, so `tzset("GMT+3")` applies
  `-10800`. This is documented in the README, the tool and `tz.c`'s
  header because it will bite. The `tz +N` shorthand means the opposite,
  "add N hours", and inverts the sign: `tz +2` applies rule `UTC-2`.
- Because a legal apply can return a negative offset, the `tzset` reject
  sentinel is *exactly* `-1` and `tzset(0)` queries never return `-1`:
  userland tests `== -1`, never `< 0` (the task 4 implementer caught
  exactly this trap on `GMT+3`).
- The offset is computed at display time, never inside `SYS_time`: UTC
  stays what NTP, the chip write-back and Round B's mtimes use, and DST
  then needs no kernel event, nothing cached, no transition hook.
- `date` derives its offset from `tzset(0)` (one syscall, whole minutes
  by construction), not `localtime - time`: at negative offsets a second
  straddle truncates toward zero in C division (d=-10799 -> -179 min)
  and renders "-02:59" for up to a second every minute.
- `/etc/tz` is plain text, the first *unsealed* settings file
  (deliberate: nothing secret about a zone; `/etc/wifi` stays sealed),
  and it loads *after* the filesystem mounts, next to the `/etc/mount`
  read - never in `rtc_init`, which runs before `SDFS.begin()`.
- The engine is not cc-subset and no userland program may embed it: the
  `tz` tool parses the std offset out of the rule string itself (a few
  lines) to label `std`/`dst`. The board-cc bootstrap question therefore
  never touches tz.c; only `cc.c`'s two builtin strings are bootstrap
  surface.
- `host_linux` picks up the `TZ` env var at process start, and the QEMU
  gate in `check_toolchain.py` *strips* `TZ` from the subprocess env,
  not merely "does not set it". Don't "simplify" that away: a developer
  or CI shell exporting `TZ` would make the `tzset(NULL)` -> 0 leg
  nondeterministic and leak into future QEMU cases.
- An omitted `/n` transition time means 02:00 local standard on both
  sides (documented in `tz.h`, pinned by the tests): a deliberate
  deviation from glibc (local of the side being left) that matches the
  real EU transition (02:00 CET = 01:00 UTC) for
  `CET-1CEST,M3.5.0,M10.5.0`.
- Selfhost flakiness this round (a corrupted hexdump fetch, gap at
  offset 8676; then a hard hang mid-cc-compile needing the AGENTS
  `uhubctl -l 3-1.4 -p 4 -a cycle -d 3`) is the pre-existing board-cc
  class in `docs/NOTES-board-cc-flaky.md`, not tz-caused, watch-only.
  The third attempt on a freshly booted board was a clean full PASS.

## Gates at completion

`make check` ok (11 toolchain cases, the new `tz` QEMU_RUN case added);
`check_ref` different: 0 with `date`/`tz` in the newer-builtins
exclusion (`localtime`, `tzset` added to `NEWER_BUILTINS`);
`check_toolchain --pc` 11 cases; `check_make --pc` 13/13;
`bench --pc` ok. Board (task-5-report.md, Challenger 2350 at
`a0ddc24`): fresh card `rule UTC offset +00:00`;
`CET-1CEST,M3.5.0,M10.5.0` -> winter `+01:00 std`, `date` = UTC + 1 h;
console reboot restored the rule from `/etc/tz` without re-setting (tz
applies over the epoch-ticking timer clock until NTP re-syncs);
`tz +2` -> `rule UTC-2 offset +02:00`, `date -u` intact; a bad rule
rejected with the previous kept; `tz UTC` cleared and unlinked the file;
15/15 console round-trips 0.37 s. `--selfhost` six IDENTICAL on the
third attempt (flakiness note above). Kernel: 262316 bytes flash (25%),
164884 bytes RAM (31%), tz share +1720 text / +48 bss.
