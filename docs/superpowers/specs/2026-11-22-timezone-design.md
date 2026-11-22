# Time zones: kernel-owned TZ with POSIX DST rules (design, 2026-11-22)

## Intent and why

The wall clock landed by Round A (`2026-11-22-clock-ds1307-design.md`, merged as
`e8592f7`) is UTC end to end - deliberate then, wrong for daily use now: the owner
lives in Sweden. Typing `date` should print the wall time being lived, and the
setting behind that must be global and survive reboot, because Round B (file
mtimes, `ls`/`make` timestamps) is parked and should render local with the same
story. Sweden is UTC+1 in winter and UTC+2 in summer, so the setting cannot be a
bare number: the kernel owns a time-zone *rule* and computes the offset in effect
for any instant (owner's pick, "B": daylight saving handled by the clock module,
not by hand).

## Goals

- `date` prints local time by default, and makes clear which clock you are
  reading (numeric UTC offset in the line); `date -u` keeps today's UTC output.
- One global setting: a POSIX TZ string (`CET-1CEST,M3.5.0,M10.5.0` for Sweden),
  settable live with a `tz` tool, persisted by the kernel to `/etc/tz`, loaded at
  boot. This makes `/etc/tz` the first *unsealed* settings file - precedent for
  kernel persistence exists: `/etc/wifi` and `/etc/telnetd` (sealed there, plain
  here; nothing secret about a time zone).
- The kernel computes the current offset itself from the rule (POSIX semantics:
  transitions in local time, `Mm.w.d` rules, default 02:00; Southern-Hemisphere
  zones where DST is the majority of the year work, because DST-start > DST-end
  already means "in effect between the transitions"; a malformed rule is
  rejected, never applied).
- A pure, dependency-free engine (`src/kernel/tz.c` + `tz.h`: parse a rule string
  into a small struct; `tz_offset(rule, utc_epoch) -> seconds`) is the *single*
  implementation. It compiles into the kernel (arduino-cli), into `host_linux`
  (gcc), and into native PC unit tests, so the DST math is gated deterministically
  on the PC - the same code that runs on the board.
- Two new syscalls, four places each (`sys.h`, cc builtin strings, kernel switch,
  `host_linux`), continuing Round A's ceremony:
  - `SYS_localtime = 106` - `localtime(&valid)`: UTC epoch + the offset in effect
    at that instant. Same valid-pointer convention as `SYS_time` (`A(0)`, NULL
    allowed). With no rule set this is exactly `SYS_time`.
  - `SYS_tzset = 107` - `tzset(str)`: parse and validate `str`; on success apply
    it live and attempt to persist to `/etc/tz` (kernel-side, like
    `SYS_wifi_save` persists `/etc/wifi`). `-1` means rejected: nothing changed
    (a real zone offset is whole minutes and can never be -1, so the sentinel
    is safe). Any other value means applied live, and is the applied offset
    (seconds, sign such that local = UTC + offset - the sign itself is
    semantic, e.g. `GMT+3` applies `-10800`). Persistence is best effort and
    does not roll back: a zone set on a card-less board works until reboot, and
    the `tz` tool - which reads `/etc/tz` back to confirm - says "live only,
    not saved" when the file is not there. `tzset(NULL)` queries the current
    offset without changing anything.
  - cc builtins: `localtime 106`, `tzset 107` in the string block in `cc.c`
    `main()`, next to `time 105`.
- Boot order: the rule loads from `/etc/tz` *after* the filesystem mounts
  (`rtc_init()` runs before `SDFS.begin()` - TZ load belongs after mount, next to
  the `/etc/mount` load). No rule (fresh card, deleted file) means offset 0:
  today's exact behavior, nothing surprises a fresh card.
- `tz` userland tool: `tz` prints the current rule (from `/etc/tz`, `UTC` when
  absent) and the live offset; when the rule names a DST side it also says
  which is in effect, computed tool-side by comparing the live offset with the
  std offset parsed out of the rule string (a few lines of plain cc-subset
  parsing - the tool does not embed the engine, so tz.c stays out of the cc
  subset question); `tz CET-1CEST,M3.5.0,M10.5.0`
  sets it; `tz +2` is accepted shorthand for a fixed +2 (maps to `UTC-2` - POSIX
  counts offsets west-positive, the tool maps the intuitive "add hours" sign for
  the bare form only); `tz UTC` clears (live offset 0, removes `/etc/tz`).
- Everything honest stays UTC: DS1307 read/write-back, SNTP, console/telnet,
  future file mtimes (Round B stores UTC, renders through `localtime`). The
  offset is computed at use time from the UTC epoch - nothing caches it, so DST
  transitions need no kernel event and a correction via `clock_correct()` can
  never straddle a stale offset.

## Non-goals

- Zoneinfo databases, named-zone data files, leap seconds.
- Per-process or per-session `TZ` (no environment crossing `spawn` exists; the
  global setting is the product).
- `basic` `DATE$`/`TIME$` exposure, file mtimes, `ls` timestamps - Round B.
- Time-zone changes surviving anything the kernel does not: there is no runtime
  other than the kernel; nothing else needs to agree.
- Non-`M` POSIX rule forms (`n` Julian day, week form without month): rejected
  by the parser as malformed.

## Constraints

- The four-places rule for both syscalls; `sys.h` changes mean `make host`
  rebuild (Round A lesson).
- `tz.c` engine: no stdlib, no Arduino includes, no dynamic allocation - fixed
  buffers; it must compile under arduino-cli (C in the sketch tree) and gcc. It
  does not have to sit inside the cc subset and no userland program (including
  `tz`) may embed it - so the board-cc bootstrap question never touches the
  engine; only the two builtin strings in `cc.c` do.
- Kernel file/flash invariants: the `/etc/tz` write happens in the `SYS_tzset`
  switch at a point with no task switch (same discipline as every file syscall);
  the boot read is plain setup code before `loop()`.
- `host_linux` compiles `src/kernel/tz.c` (Makefile change) and implements both
  syscalls with it - libc is *not* the host's engine; tz.c is the thing under
  test. At process start `host_linux` initializes from the `TZ` environment
  variable when set (a free, standard PC-side knob for QEMU_RUN cases), else UTC.
  Persistence is host-side irrelevant: no file write in `SYS_tzset` on the host.
- No absolute addresses; engine state lives in the kernel's data segment (r9
  offsets) like everything else; the kernel keeps only the parsed struct - the
  rule *string* is read from `/etc/tz` by the tool, so no display copy is kept
  in the kernel.
- Console safety: the `clock_service()` coupling from Round A is untouched; TZ
  changes and offsets are pure computation - no new service hooks.
- `check_ref`: the userland `tz` tool and `date` now use builtins the reference
  cc predates - add `localtime`, `tzset` to `NEWER_BUILTINS` the same commit that
  lands them (the papercut from Round A: a task adding a userland program owes
  a `check_ref` re-run).
- Bootstrap: only the builtin-string change in `cc.c` (a string; Round A
  precedent - old committed cc compiles it). Selfhost six tools must stay
  IDENTICAL.

## Design notes (my calls, as decided in the conversation)

- Approach: kernel owns the zone (option 2 of three); userland-only was rejected
  (the SD card is editable and gone; `basic` would need its own engine; Round B
  would re-solve it per tool); making `SYS_time` return local was rejected (it
  would put a shifting clock under NTP, the chip, and mtimes).
- `SYS_localtime` returns local *epoch* so every formatter (date, Round B's ls)
  formats it with the same civil-days math already written for UTC: `gmtime` of
  the local epoch *is* the wall clock. No new formatting engine anywhere.
- POSIX sign trap: `CET-1` means UTC+1 (positive = west). Documented in the tool
  output, the README, and `tz.c`'s header comment, because it will bite.
- Offset-at-use-time, not cached: DST correctness is then just
  `tz_offset(rule, utc)` - a pure function - which is exactly what the PC unit
  test pins with fixed epochs across both transitions (e.g. 2026-03-29 01:00 UTC
  boundary for Sweden, Southern-Hemisphere ordering, default `/2` times, fixed
  offset, malformed strings, and the `w=5` last-Sunday-of-month computation).
- Rule grammar accepted: `std offset [dst [offset] ,rule ,rule]` - a zone is
  either pure-standard (no DST name) or names DST *and* supplies both transition
  rules (the DST offset may be omitted, POSIX default std+1h, which is what
  Sweden's `CET-1CEST,M3.5.0,M10.5.0` uses); a DST name with no rules, or with
  only one rule, is rejected (this is the ambiguity POSIX leaves
  implementation-defined, and we refuse it rather than guess a mirror). Name
  quoting with `< >` supported or rejected - builder's
  call, but say which in the code and README; Sweden never needs it. Southern
  rules work because DST-start > DST-end already means "between the transitions".
- Display format: `date` prints `YYYY-MM-DD HH:MM:SS +01:00` (and `-u` prints the
  existing `... UTC` line); the unsynced note stays. Exact spacing is the
  builder's, but the offset and the UTC escape hatch are product surface.

## Tests

- **PC unit (the DST oracle)**: `tests/tz/test_tz.c` includes `../../src/kernel/tz.c`
  (or links it per the Makefile's existing native-test pattern) and asserts, with
  fixed epochs: Sweden winter/summer offsets, both 2026 transitions at 1 s
  granularity around the boundary, `w=5` last-Sunday math, default transition
  time, fixed-offset strings, southern rule with DST the majority of the year
  (e.g. `AEST-10AEDT,M10.1.0,M4.1.0/3`), DST-name-without-rules and
  one-rule rejects, malformed rejects,
  and that `tzset`-struct reuse never reads freed/static state across calls.
  Wired into `make check`; deterministic, no clock dependency.
- **QEMU_RUN `tz` case** (`tests/toolchain/tz.c` + `.expected`, extends the
  Round-A `QEMU_RUN` step): guest program calls `tzset(NULL)` (expect 0 under no
  env), `tzset("GMT+3")` then prints `localtime(NULL) - time(NULL)` (expect
  `-10800`, which pins the POSIX sign), then a `tzset("nonsense")` reject leg
  (expect -1 and the following offset still `-10800`). DST legs live in the PC
  unit test (only fixed epochs are deterministic); the QEMU case's job is
  the syscall path, the sign convention, and reject semantics. Host side:
  `host_linux` honors `TZ=` at startup, so `check_toolchain.py` must strip `TZ`
  from the `qemu-arm host_linux` subprocess environment - not merely "not set
  it", because a developer/CI shell exporting `TZ` would make the
  `tzset(NULL)` -> 0 leg nondeterministic and leak into future QEMU cases. The
  `tz` case starts from UTC by construction.
- **`check_ref`**: after landing, `different: 0` with `date`/`tz` in the
  newer-builtins class.
- **Selfhost**: `check_toolchain.py --selfhost` six IDENTICAL (builtin-string
  change is in `cc.c`).
- **Board** (owner pre-approved board work; port rules from AGENTS.md): `tz`
  prints UTC on a fresh card; `tz CET-1CEST,M3.5.0,M10.5.0` flips `date` to the
  local line within one `date`; reboot restores it from `/etc/tz`; `tz +2`
  shorthand shifts by two hours; malformed string leaves the previous zone
  standing; `date -u` unchanged; console round-trips unaffected (TZ is pure
  computation, so this is a spot check, not a regression campaign). DS1307 legs
  stay deferred (no chip on the bench, Round A ruling).
- **Kernel compile gate**: the exact arduino-cli compile command from Round A
  (Challenger FQBN, patched ESPHost library), sizes reported.

## What's left to the builder

- Engine internals: struct layout, transition-time computation (derive both
  transitions for the year containing the epoch from UTC; a year-neighborhood
  comparison is fine), and where the year-arithmetic helpers live (the civil-days
  math from `date.c`/`rtc.cpp` suggests a shared shape - but `tz.c` stays
  self-contained; duplication of a 5-line helper is acceptable).
- `tz`/`date` exact output strings beyond the offset requirement; `-u` argument
  parsing in `date.c`.
- `host_linux` TZ-env pickup point (before first syscall dispatch is enough).
- Whether `tzset` also accepts the full string via `TZ=` naming a zone absent
  from any database - no: only POSIX-style strings, always, both sides.
