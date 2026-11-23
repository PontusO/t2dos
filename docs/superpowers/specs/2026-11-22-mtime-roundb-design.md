# Round B design — file mtimes, `ls` timestamps, BASIC `DATE$`/`TIME$`

Date: 2026-11-22. Status: approved scope by owner ("Looks good to me" to the
scoped question message of 2026-11-22 evening: kernel stamping, `ls`
timestamps with DST-correct per-file rendering, BASIC `DATE$`/`TIME$`;
`make` skip-if-up-to-date and network-protocol mtimes stay OUT).
Depends on: the tz round (merged, `a44acd8`) — mtimes store UTC and render
through the kernel's active zone rule, exactly as that spec anticipated
("Round B stores UTC, renders through `localtime`").

## Why

The card is the machine. Its files should say when they were last written,
`ls` should show that in the owner's local time (correctly through DST), and
`basic` should be able to tell the date and time — the three pieces the
clock (Round A) and time-zone (Round C) rounds deliberately parked.

## Goals

1. **Real stamps on write.** The kernel registers a time callback with SDFS
   so every file the board writes gets its modification time from the wall
   clock. Until Round C the stamp path existed but was dead: SDFS's internal
   `dateTimeCB` falls back to `time(nullptr)` (always 0 on the RP2350)
   because nobody ever calls `SDFS.setTimeCallback()`.
2. **`ls` shows mtimes** in local time with the numeric-offset-correct
   calendar per file: a July file and a November file list with *their own*
   DST side, not today's. Format (owner-approved):

   ```
   /bin> ls
   2026-11-22 23:08    4342  tz.bin
   2026-11-22 23:08    1604  date.bin
   ----------          8896  cc.bin
          12 files, 245760 bytes
   ```

   `----------` (ten dashes) whenever no real time is known: `ramfs`
   (`/tmp`), `netfs` mounts, files written while the clock was unsynced.
   Directory lines keep today's `<dir>` form with no time column (FAT-style
   dir timestamps are not maintained; the owner sketch had files only).
3. **BASIC `DATE$`/`TIME$`** — read-only zero-arg string functions:
   `DATE$` = `"YYYY-MM-DD"`, `TIME$` = `"HH:MM:SS"`, both LOCAL (active
   zone rule, DST applied). This is the clock's first user inside `basic`.
4. **One new syscall to make per-file rendering honest.** `tzset(0)` answers
   the offset *now*; converting an August mtime in November needs the offset
   *then*. `SYS_tzlocal = 108`: `tzlocal(utc) -> utc + tz_now(utc)` — the
   local epoch for an arbitrary UTC second under the active rule. Plain
   kernel-side `tz_now` exposure; the engine does not change.

## Non-goals

- `make` skipping up-to-date targets. FAT timestamps have 2-second
  granularity; the make round chose "always rebuilds" deliberately for
  self-host correctness. Owner agreed: out.
- Preserving mtimes across `get`/`send`/`rcp`/mount: transfers stamp with
  the board clock at transfer (owner-approved). No protocol change;
  `tools/fileserver.py` is untouched.
- Access times, creation-time display, `cp` preservation, ramfs/netfs time
  tracking, per-directory summaries, `ls` sort/flag changes.

## Anti-goals

- A bogus year stamped while unsynced (FAT DOS fields cannot represent
  pre-1980 and wrap into garbage — e.g. a negative `tm_year - 80` field).
- `ls` lying about a file's DST side by applying today's offset.
- Corrupting an old userland binary's memory with a silently-widened
  `dirent_t` (see ABI note).

## Design

### 1. Stamping (kernel)

`setup()`, after `SDFS.begin()` succeeds: `SDFS.setTimeCallback(fs_time_cb)`
with

```c
#define MTIME_UNKNOWN 315532800   // 1980-01-01 00:00:00 UTC = FAT epoch floor
#define MTIME_MAX     2147483647  // int32 epoch ceiling, 2038-01-19 (OS-wide int cliff)
static time_t fs_time_cb(void) {
  unsigned long t = clock_now();
  if (!clock_valid() || t < MTIME_UNKNOWN + 60) return MTIME_UNKNOWN;
  return (time_t)(t - t % 60);    // whole minutes - see round-trip note
}
```

SDFS converts `time_t` → DOS fields via `localtime_r`, which on this libc
with no `TZ` machinery *is* UTC — so DOS fields hold the UTC calendar.
Storage truth stays UTC (tz spec contract). `MTIME_UNKNOWN` is the
sentinel: a real file can only carry it by being written in the first
minute of 1980, i.e. never.

**Minute truncation is load-bearing (builder-check finding):** SDFS's
`dateTimeCB` writes *raw* `tm_sec` into the 5-bit DOS field, while
`FatToTimeT` reads it back as spec-compliant `sec * 2` (with seconds >= 32
carrying into the minute) — so only second 0 round-trips; a verbatim stamp
of 23:08:45 reads back as 23:09:26. Truncating to whole minutes makes
`getLastWrite()`/`fileTime()` exact, and nothing displayed is finer than a
minute. PC tests cannot see this (the FAT path is board-only); the board
task checks a written file's minute equals the write minute.

Never call `setTimeCallback` before `begin()` (the callback outlives a
failed mount otherwise).

`SYS_stat` is already correct (`f.getLastWrite()`); no change.
`host_linux` `SYS_stat` is already correct (real `st_mtime`); no change.

### 2. `dirent_t` gains mtime (four-places-adjacent ABI)

`sys.h`: `struct dirent_t { char name[64]; int size; int isdir; int mtime; };`
(new field appended — first 72 bytes keep their meaning).

- Kernel `SYS_readdir`: `e->mtime = (int)dirs[d].fileTime();` —
  `Dir::fileTime()` is core FS (`FS.h:149`) and SDFS overrides it
  (`SDFS.h:456`); no extra SD opens. `ramfs`/`netfs` do not override it:
  the base returns 0 → unknown. Netfs `nfs_readdir` fills are untouched;
  the kernel/host netfs readdir paths set `mtime = 0`.
- `host_linux` `SYS_readdir`: it already `stat()`s every entry for size —
  add `o->mtime = st.st_mtime;` (free) and `o->mtime = 0` on its netfs
  readdir branch.
- **ABI note (deliberate, accepted):** kernel and card binaries ship
  together — `root/bin` is build output, and the self-host round rebuilds
  everything on-board. A *stale* binary that allocates the old 72-byte
  `dirent_t` lets the kernel write 4 bytes past it. The authoritative
  re-`put` rule is `grep -l 'readdir\|dirent_t' src/*/*.c` — the current
  set is **{ls, rcp, sh, basic}** (builder-check correction: `basic` counts
  — a *global* `struct dirent_t` at `src/basic/io.c:18`; `rls` does NOT —
  it speaks the net `LS` protocol, never the syscall). The board task
  re-puts the grep result, not this prose. No compat syscall: one
  kernel+bin set, per repo precedent (`stat_t` already carries fields old
  code never reads).

### 3. `SYS_tzlocal = 108` (four places)

- `sys.h`: `SYS_tzlocal = 108, // tzlocal(utc) -> local epoch for utc under the active rule (UTC until a zone is set)`
- `cc.c` builtin string: `tzlocal 108` after `tzset 107` (string-only
  change; bootstrap + byte-identity hold).
- Kernel switch: `case SYS_tzlocal: { long t = (long)A(0); return (int)(t + tz_now(t)); }`
- `host_linux` switch: same through the same engine.
- `tools/check_ref.py` `NEWER_BUILTINS` += `tzlocal`.

`localtime(&v)` stays "the clock *now*"; `tzlocal(t)` is "the instant `t`".
Both pure kernel-side; userland never learns the offset for a past instant
as a number (it does not need it — it needs the local wall fields, which
`tzlocal` gives, format-able as-if-UTC like `date.c` already does).

### 4. `ls`

Default output per file: `%04d-%02d-%02d %02d:%02d  %8d  %s` — civil fields
of `tzlocal(mtime)` formatted as-if-UTC (Hinnant civil-from-days, the
`date.c` shape; small local copies, duplication blessed by house practice),
or `----------` + two spaces for any mtime outside
`MTIME_UNKNOWN < mtime < MTIME_MAX` (both bounds exclusive). Then size and
name exactly as now; `<dir>` lines and the `N files, M bytes` trailer
unchanged. Single file argument (the `stat` fast path at the top of `list()`)
gains the same prefix. No flags. Reads `tzlocal` via the builtin; cc-subset;
both bounds defined locally (userland never includes kernel headers; a
comment names them and why).

**Why the upper bound (builder-check finding, controller ruling):** files
written *before* this round were stamped from `time(nullptr) = 0`, which
SDFS wrapped into plausible-looking junk (~2097-12) already sitting on
cards. The clamp's two ends are both principled, not an arbitrary year:
below is unrepresentable (pre-DOS-floor or the sentinel), above is past the
int32 epoch ceiling the whole OS already lives under (2038 cliff). Net
effect on the owner's card: old pre-Round-B files show `----------` until
rewritten — truthful, since their stamps are indeed junk. NOTES + README
carry one line saying so.

### 5. BASIC `DATE$`/`TIME$`

Two zero-arg `T_STR` builtins in `src/basic` (`funcs.c` registration table +
handlers), compiled into both the board build (syscalls: `localtime(0)` →
civil-from-days formatting in cc-subset C) and the PC build (`pcsys.c`
shim). Local time = the kernel's `localtime`, so the zone rule and DST apply
for free. Unsynced clock: they still render (epoch-based local, e.g.
`1970-01-01`) — same honesty rule as `date`, which keeps printing under
`unsynced`. BASIC keeps compiling under the board's own cc (bootstrap; the
`tzlocal`/`tzset` builtins cc now knows don't conflict — `basic` doesn't
call them and the selfhost fixed point must hold).

`src/basic/io.c:430` (`fst.size > 0 || fst.mtime > 0`) is pipe-polling
(pipe stat returns `mtime = nbounds`, unchanged by this round). Settled at
builder check: `f_path` is only ever set for `/pipe/` paths (`io.c:395-397`),
so no regular file reaches that escape and stamping cannot change the
behavior — no change there, one NOTES line so nobody re-opens it.

### 6. Docs (closing task)

HANDOVER entry `0g`; spec `## Results`; `docs/NOTES-roundb-mtimes.md`
(the `MTIME_UNKNOWN` semantics, the UTC-in-DOS-fields trick and its
`localtime_r == UTC` dependency, the stale-`dirent_t` card hazard, the
`tzlocal` vs `tzset(0)` DST distinction, io.c:430 finding); README `ls`
sample with the timestamp column, `tzlocal` syscall row, BASIC `DATE$`/`TIME$`
mention.

## Tests

- **PC, deterministic (QEMU):**
  - `check_toolchain --pc`: extend the `tz` case with `tzlocal` legs on
    fixed epochs (July mtime → local of a +02:00 day, November → +01:00,
    no zone → identity). Harness still *strips* `TZ` from the ambient env;
    the case sets its own `TZ='CET-1CEST,M3.5.0,M10.5.0'` explicitly — the
    engine parses POSIX strings from `getenv("TZ")`, zone *names* do not.
  - `check_sh --pc` (or a `check_toolchain` case if sh env plumbing fights):
    python stages three files with `os.utime` — one at `2026-07-15 12:00`
    UTC, one at `2026-11-22 12:00` UTC, one past `MTIME_MAX` (junk-stamp
    analog) — plus one at epoch 0; the sh session runs
    `tz CET-1CEST,M3.5.0,M10.5.0` (in-process zone set; the host engine
    parses the POSIX string) then `ls` the staged dir; expect July local
    rendering for the July file, November for the November file, and
    `----------` for the past-ceiling and epoch-0 files. No ambient-TZ
    dependency anywhere (zone set in-process, mtimes staged absolutely).
  - `check_basic --pc`: format assertions inside BASIC (`LEN(DATE$)=10`,
    `MID$(DATE$,5,1)="-"`, `LEN(TIME$)=8`, `MID$(TIME$,3,1)=":"`), no
    clock-value pinning; ASan leak leg keeps running them.
  - `check_ref`: `different: 0`, `tzlocal` in the newer-builtins exclusion.
  - `check_net --pc`: netfs readdir entries list with `----` (or the
    current format's equivalent), pipe semantics untouched.
- **Kernel compile gate** every kernel-touching task (Challenger FQBN,
  `--library third_party/ESPHost`), sizes reported.
- **Board (pre-approved by owner's "do this while I sleep" + scoped yes):**
  synced clock: write file → `ls` shows *minute-exact* now (truncate note)
  with `+01:00` (winter); reboot → zone restored, mtimes survive; `get`
  download → stamped at transfer; unsynced boot window: write →
  `----------`; a pre-existing old card file (junk stamp) → `----------`;
  `/tmp` file → `----------`; mounted `/mnt/fs` listing → `----------`;
  `basic` prints sane `DATE$`/`TIME$`; re-put ALL bins from the ABI-note
  grep (`ls rcp sh basic`); console latency unchanged for a directory
  listing (per-entry `fileTime()` rides the same dir reads — spot-check,
  not a campaign); `--selfhost` six IDENTICAL.

## Constraints (from AGENTS.md / architecture.md, binding)

- New syscall in all four places; `cc.c` string-only; no-`-O` output
  byte-identical (`check_ref`); toolchain sources stay cc-subset (bootstrap
  — `basic`, `ls` compile under the committed cc); no absolute addresses;
  never touch PSRAM during flash writes / no task-switch points in file,
  flash or WiFi calls (`fs_time_cb` is called from inside SDFS write paths —
  it only reads `clock_now()`/`clock_valid()`, which the clock round already
  calls from syscall context; no switch point, no allocation).
- Board safety: one holder on the port, never kill `screen`, power-cycle
  only via the exact `uhubctl -l 3-1.4 -p 4 -a cycle -d 3`.

## Left to the builder

Exact placement of the `fs_time_cb`/registration lines in `setup()`; the
`check_sh` vs `check_toolchain` home for the `ls` leg (per-case env must
survive the TZ strip — a case-local explicit `TZ` is required either way);
BASIC function/handler plumbing inside `funcs.c`/`lex.c` conventions;
whether `ls`'s civil-from-days lives in a small shared helper or per-file
copy (either fine; duplication blessed).

## Results (landed 2026-11-23, branch `mtimes`, merged same day)

Implemented as designed, all four code tasks clean through review, with
these deviations and findings, all ledgered in
`.superpowers/sdd/2026-11-22-mtimes/progress.md`:

- A brief-internal epoch/date inconsistency was caught at implementation:
  the pinned epoch 1752573600 is 10:00 UTC, not the "12:00 UTC / 14:00"
  the prose suggested. Ruling: the epoch is authoritative and the golden
  `12:00` local at +02:00 is correct; the staging comment was corrected to
  10:00 UTC.
- Two seasonal landmines in the plan's own gates, both found by builder
  check / implementer vigilance and fixed to season-independent forms:
  the tz.c `tzset("CET-1CEST,M3.5.0,M10.5.0") == 3600` leg only holds in
  winter (every future summer run would fail), so it accepts 3600 or 7200
  while the fixed-epoch legs stay the year-round DST proof; and the
  `ls_times` golden embedded the tz tool's live `offset +01:00 std` echo,
  which flips in DST, so the case moved to an explicit
  `TZ='CET-1CEST,M3.5.0,M10.5.0'` in `ls_times.env` (applied *after* the
  harness TZ strip, the `check_basic` NAME.env convention), the `.in`
  dropped the tz line, and the golden is an ls-only transcript whose date
  column is mtime-derived hence season-independent. Seasonal-landmine
  tally in persistent gates is now zero.
- `ls_times` went beyond the spec letter: boundary pins `sentinel.bin`
  stamped exactly at `MTIME_UNKNOWN` and `ceiling.bin` exactly at
  `MTIME_MAX` (each must render `----`; they pin the exclusive bounds'
  off-by-one), and a per-case line-sorted comparison because readdir
  order over the staged dir is not byte-exact-stable (documented in
  `check_sh.py`; all other cases keep byte-exact goldens).
- Clarification from Task 2, tested as such: unsynced-window stamps
  arrive via `readdir` as the sentinel 315532800, not 0 - the exclusive
  dash window handles it, and the `sentinel.bin` pin keeps the equality
  case honest (the board unsynced leg is the live witness).
- BASIC notes: assigning to `DATE$` is now a syntax error (was an
  unsup-word error; both errors, the name is reserved); `pcsys.c`
  hoists `<time.h>` above the syscall macro renames so no source ever
  sees libc's `localtime` prototype post-macro.

Verification on the Challenger at `6aba4ab` (full transcript in
`.superpowers/sdd/2026-11-22-mtimes/task-5-report.md`): kernel 262428
bytes (+112 vs the tz round, matching the task-2 gate); synced writes
stamp minute-exact with winter `+01:00`; `get` downloads stamped at
transfer; pre-existing junk stamps (`cat.bin`, `wc.bin`) and ramfs and
mounted `/mnt/fs` entries all list `----------`; the unsynced-boot write
leg won the ~9-14 s window by 0.4 s on attempt 3 of a single-session
serial race (attempts 1-2 lost to sync speed and a pyserial re-open race,
harness-side); zone and mtimes survive reboot; 10/10 `ls /bin`
round-trips uniform 0.57 s; `--selfhost` six IDENTICAL on the first
attempt. PC gates: `make check` ok, `check_ref` different: 0 with
`tzlocal` in the newer-builtins class, `check_toolchain --pc` 12 cases,
`check_sh --pc` 6 cases (the new `ls_times`), `check_basic --pc` 140
cases ASan-clean, `check_net --pc` ok, `bench --pc` ok.
