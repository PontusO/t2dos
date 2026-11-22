# Time zones (kernel-owned POSIX TZ with DST) Implementation Plan

**Goal:** Make `date` (and every future timestamp renderer) print local time from a global, reboot-surviving POSIX TZ rule the kernel evaluates itself - Sweden's DST handled by the clock, not by hand.

**Architecture:** One pure, dependency-free DST engine (`src/kernel/tz.c`) compiled into the kernel, `host_linux`, and native PC unit tests. Two new syscalls (`SYS_localtime=106`, `SYS_tzset=107`) through the four places; `SYS_tzset` persists to `/etc/tz` kernel-side (best effort) and the kernel loads it at boot after mount. The honest clock (`SYS_time`, DS1307, SNTP) stays UTC; only display adds the offset, computed at use time.

**Tech Stack:** C (kernel = Arduino sketch on RP2350; userland = the repo's own cc), gcc native tests, Python checkers in `tools/`.

**Spec:** `docs/superpowers/specs/2026-11-22-timezone-design.md` (binding; its `Results` section gets written by Task 6)

## Global Constraints

- Four-places rule: every syscall lands in `sys.h`, the builtin strings in `cc.c` `main()`, the switch in `src/kernel/kernel.ino`, the switch in `host_linux.c`. `sys.h` changes require `make host` before any QEMU run (Round A lesson).
- UTC stays the storage truth: nothing in NTP, DS1307 (`rtc.cpp`), or future mtimes may consume local time. `SYS_tzset` never touches `clock_correct`/`base_epoch`.
- The `SYS_time` builtin string `time 105` and everything else the reference cc knows must keep working: no `-O` output changes (`make check` byte-identity, `check_ref different: 0`). `cc.c` gets only a builtin-STRING change (bootstrap-safe, Round A precedent). Selfhost six tools stay IDENTICAL.
- `src/kernel/tz.c`: no stdlib, no Arduino includes, no dynamic allocation, no absolute addresses; compiles under arduino-cli (C in the sketch tree) and gcc. It is NOT required to be in the cc subset, and no userland program (including the `tz` tool) may embed it.
- Offsets mean `local = UTC + offset` (so POSIX `GMT+3` applies `-10800`); `-1` is the reject sentinel and can never be a real offset (rules are whole minutes). Grammar: `std offset [dst [offset] ,rule ,rule]`; a DST name without exactly two rules is rejected. Non-`M` rule forms rejected.
- Kernel file discipline: `/etc/tz` is written from inside the `SYS_tzset` switch at a non-task-switch point (like `wifi_store`, `kernel.ino:376`); the boot read is plain setup code.
- The `tz` tool adds a `root/bin` program: the same commit must keep `check_ref` green (`NEWER_BUILTINS += 'localtime', 'tzset'`) and re-run it (AGENTS papercut entry).
- Board work only per AGENTS.md board-safety rules; port from `$T2DOS_PORT`/by-id; flashing via `reboot -b` -> UF2 (Challenger); power-cycle only with the exact `uhubctl` command.
- Commits: one per task minimum (more allowed), messages prefixed `tz:`. Do not merge or push.

## Review Focus

- **POSIX sign inversion** - `GMT+3` must apply `-10800` and `CET-1` must mean UTC+1; a sign bug shows up as double error at tea time, silently. Pinned by Task 1 unit test and Task 2 QEMU case.
- **`/etc/tz` is untrusted SD input** - boot must survive a garbage, oversized (>= 200 chars), or half-valid file by staying UTC, never half-applying (names parsed, offsets wild). Pinned by Task 1 garbage-parse legs and Task 2 boot-load behavior.
- **Rule half-specified** - `CET-1CEST` (no rules) and `CET-1CEST,M3.5.0` (one rule) are rejects; after a reject the previously live rule must still answer `tzset(NULL)`. Pinned by Task 1 (engine) and Task 2 (QEMU reject leg).
- **Second-exact transitions, both hemispheres, year wrap** - Sweden's 2026 boundaries (`2026-03-29 01:00 UTC`, `2026-10-25 01:00 UTC`) flip at the exact second; `AEST-10AEDT,M10.1.0,M4.1.0/3` is DST across Jan 1. Pinned by Task 1 fixed-epoch legs.
- **QEMU determinism** - `time()`/`localtime()` called back-to-back may differ by one second; the QEMU case tolerates exactly that, and `check_toolchain.py` strips `TZ` from the qemu subprocess env so a dev shell's `TZ` cannot leak in. Pinned by Task 2.

---

### Task 1: The DST engine (`src/kernel/tz.c` + `tz.h`) with native unit tests

**Files:**
- Create: `src/kernel/tz.c`, `src/kernel/tz.h`
- Create: `tests/tz/test_tz.c`
- Modify: `Makefile` (`check:` target, line ~83: add a native gcc build+run of the test)

**Interfaces:**
- Produces (exact, both C, used by Tasks 2-3):
  ```c
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
  ```
- The name `tz_parse` accepts quoted `< >` names too (say so in the header comment - settles the spec's builder's call).

- [ ] **Step 1: write the failing unit test** `tests/tz/test_tz.c` (native, no cc subset needed; includes `../../src/kernel/tz.c`; prints `ok` and exits 0 only if every assert holds, else prints the failing leg name and exits 1). Legs, all with fixed epochs via explicit UTC civil->epoch math inside the test (the file may carry its own copy of the 5-line days-from-civil helper):
  - Sweden `CET-1CEST,M3.5.0,M10.5.0`: offset 3600 on a January and a July epoch; exactly 7200 across `2026-03-29 01:00:00 UTC` (test `...00:59:59` -> 3600, `...01:00:00` -> 7200); 7200 at `...2026-10-25 00:59:59`, 3600 at `01:00:00`.
  - `w=5` math: the Sweden legs above ARE last-Sunday legs; add `M1.1.0`-style first-Monday epoch to pin week indexing.
  - Default transition time: rule without `/n` flips at 02:00 local standard (`2026-03-29 01:00:00 UTC` above doubles as this).
  - `t` in local time of the side being left: with `M10.5.0/3`, `2026-10-25 00:59:59 UTC` is still DST and `01:00:00 UTC` is standard.
  - Fractional offsets: `IST-5:30` parses, offset 19800.
  - Fixed offsets: `GMT+3` -> -10800; `UTC0` -> 0.
  - Southern majority-DST: `AEST-10AEDT,M10.1.0,M4.1.0/3` -> 39600 on a January epoch (year wrap), 36000 on a July epoch.
  - Rejects (each must return -1 AND leave a previously loaded rule's `tz_now` unchanged): `EST5EDT` (name, no rules), `CET-1CEST,M3.5.0` (one rule), `nonsense`, `CET+1CEST,M3.5.0,M10.5.0,M11.1.0` (three rules), a 200-char string of `A`, `CET-1CEST,M3.5.0,M10.5.0,nonsense` (junk rule tail).
  - Stateless-per-call: same rule queried across a decade of fixed epochs returns only the two expected offsets (no hidden static that would let one call poison the next).

- [ ] **Step 2: run it, verify it fails** - `gcc -Wall -o build/host/tz_test tests/tz/test_tz.c` fails to compile (no tz.c yet). Expected: compile error, not a silent pass.
- [ ] **Step 3: implement** `src/kernel/tz.h`/`tz.c` per the Interfaces block. Approach notes: parse into a local `tz_rule` and only write `r`/the active slot on success; `tz_offset` computes the two transition instants for the UTC year of `utc` (rules are local-time - compute conservatively against that year and its neighbors; the spec blesses year-neighborhood comparison); Southern detection is `dst_start >= dst_end` in the year. No floats, no libc.
- [ ] **Step 4: run it, verify it passes** - same gcc command, then `build/host/tz_test` prints `ok`, exit 0.
- [ ] **Step 5: wire into `make check`** - in the `check:` target add one line, matching the surrounding style: `gcc -Wall -o build/host/tz_test tests/tz/test_tz.c && build/host/tz_test`.
- [ ] **Step 6: `make check 2>&1 | tail -1`** exits 0 (the new line runs inside it).
- [ ] **Step 7: commit** `git add src/kernel/tz.c src/kernel/tz.h tests/tz/test_tz.c Makefile && git commit -m "tz: pure POSIX TZ engine with native DST tests"`.

### Task 2: Syscall ceremony - `SYS_localtime`/`SYS_tzset` four places, boot load, QEMU gate

**Files:**
- Modify: `sys.h:29` (after `SYS_time = 105`)
- Modify: `src/compiler/cc.c:57` (builtin string, after `time 105`)
- Modify: `src/kernel/kernel.ino` (switch near `:835`; boot load after the fs mount + `/etc/mount` load in `setup()`; a `tz_store()` static modeled on `wifi_store` at `:376`)
- Modify: `host_linux.c:434` (switch) and the `Makefile` `host:` rule (line ~71: add `src/kernel/tz.c` to deps and the compile sources; pickup point: at startup, before first syscall dispatch, `getenv("TZ")` -> `tz_parse`/`tz_load`)
- Create: `tests/toolchain/tz.c`, `tests/toolchain/tz.expected`
- Modify: `tools/check_toolchain.py` (`QEMU_RUN = {'clock'}` -> add `'tz'`; where the qemu subprocess spawns, pass `env` = a copy of `os.environ` with `TZ` removed)
- Modify: `tools/check_ref.py:41` (`NEWER_BUILTINS` += `'localtime', 'tzset'`)

**Interfaces:**
- Consumes: Task 1 `tz_parse/tz_load/tz_now` (the kernel and the host compile the same `src/kernel/tz.c`).
- Produces: cc builtins `localtime(int*)` -> 106, `tzset(char*)` -> 107 (NULL arg queries); `/etc/tz` single-line file content = the exact accepted string.
- Kernel case shapes (bodies the implementer writes): `SYS_localtime` = valid flag as `SYS_time`, value `clock_now() + tz_now(clock_now())`; `SYS_tzset` = NULL -> `tz_now(clock_now())`; string -> `tz_parse` else -1, `tz_load`, persist string to `/etc/tz` best effort (file write failure ignored), return `tz_now(clock_now())`. Boot: after mount, read at most 96 bytes of `/etc/tz`, `tz_parse`+`tz_load`, any failure leaves UTC silently. `host_linux`: same syscalls via the engine, never a file; `TZ` env honored at startup only.

- [ ] **Step 1: write the failing QEMU case** `tests/toolchain/tz.c` (cc-subset, `#include "sys.h"`): `tzset(0)` must be 0 (harness strips TZ, host booted UTC); `tzset("GMT+3")` must return `-10800`; then `d = localtime(0) - time(0)` must satisfy `-10801 <= d <= -10800` (one-second straddle tolerated); `tzset("nonsense")` must be `-1`; `tzset(0)` must still be `-10800`; print `ok` exit 0. `tests/toolchain/tz.expected`: `ok` / `exit 0` (the established two-line format).
- [ ] **Step 2: register + run, verify it fails** - add `'tz'` to `QEMU_RUN`, the `TZ`-strip env, and the `NEWER_BUILTINS` line; `make && make host && python3 tools/check_toolchain.py --pc 2>&1 | tail -1` fails in the tz case (builtin unknown or syscall unimplemented).
- [ ] **Step 3: implement** - `sys.h` two enumerators (comments mirroring the `SYS_time` line's style); `cc.c` string gets `localtime 106 tzset 107` between `time 105` and `sin 112`; kernel switch + `tz_store` + boot load per Interfaces; `host_linux` switch + startup `getenv("TZ")` + Makefile `host:` sources; keep every existing case byte-identical.
- [ ] **Step 4: PC gates** - `make && make host && make check 2>&1 | tail -1 && python3 tools/check_ref.py 2>&1 | head -1 && python3 tools/check_toolchain.py --pc 2>&1 | tail -1 && TZ=Europe/Stockholm python3 tools/check_toolchain.py --pc 2>&1 | tail -1` -> all ok, `different: 0`, 11 toolchain cases (clock + tz run under QEMU), and the deliberate `TZ`-exported run stays green (proves the strip).
- [ ] **Step 5: kernel compile gate** - Round A's exact arduino-cli compile command (Challenger FQBN, `--library third_party/ESPHost`), exit 0, report sizes.
- [ ] **Step 6: commit** - message `tz: SYS_localtime/SYS_tzset four places, /etc/tz boot load, QEMU gate with TZ stripped`.

### Task 3: `date` prints local time (offset in the line), `-u` stays UTC

**Files:**
- Modify: `src/userland/date.c`

**Interfaces:**
- Consumes: Task 2 builtins (`localtime`, `tzset` for nothing here - only `localtime` and `time` are used).
- Produces: `date` prints `YYYY-MM-DD HH:MM:SS +hh:mm` (local; from `/etc/tz` live rule), `date -u` prints today's exact `YYYY-MM-DD HH:MM:SS UTC` line; the `unsynced: ...` note stays on both. The offset is derived as `localtime - time` (no new engine in userland).

- [ ] **Step 1: implement** - `main(int argc, char **argv)`: `-u` selects the existing UTC path verbatim; default path calls `localtime(&v)` for the fields and computes `off = localtime(&v) - time(&v)` (two calls - fine; the second line of fields comes from the same local value). Print `+hh:mm` / `-hh:mm` (floor-divide `off` by 60; whole minutes guaranteed). Keep the file cc-subset clean.
- [ ] **Step 2: PC verify** - `make && printf 'date\ndate -u\n' | TZ=CET-1CEST,M3.5.0,M10.5.0 qemu-arm ./host_linux -r root /bin/sh.bin` - both lines appear; run `TZ=CET-1CEST,M3.5.0,M10.5.0 qemu-arm ./host_linux -r root /bin/date.bin | grep -Eq '^[0-9]{4}-[0-9]{2}-[0-9]{2} [0-9]{2}:[0-9]{2}:[0-9]{2} [+-][0-9]{2}:[0-9]{2}$' && echo fmt-ok` (offset VALUE is date-dependent by design - the format is what this pins) and `TZ=CET-1CEST,M3.5.0,M10.5.0 qemu-arm ./host_linux -r root /bin/date.bin -u | grep -qE ' UTC$' && echo utc-ok`. (Passing `-u` through host_linux's direct-binary launch: run it via `printf 'date -u\n' | ... /bin/sh.bin | grep -qE ' UTC$'` instead if the harness ignores argv.)
- [ ] **Step 3: `check_ref` re-run** (papercut duty for changed userland): `python3 tools/check_ref.py 2>&1 | head -1` -> `different: 0`.
- [ ] **Step 4: commit** `tz: date prints local time with the UTC offset, -u stays UTC`.

### Task 4: the `tz` tool

**Files:**
- Create: `src/userland/tz.c` (tool; distinct from `src/kernel/tz.c`, the engine - never confuse the two in comments)
- Modify: `Makefile` (`PROGS`/`root/bin` build list - same entry pattern as `date`)

**Interfaces:**
- Consumes: Task 2 `tzset`/`localtime`; `open`/`read`/`unlink` builtins for `/etc/tz`.
- Produces (board + host product surface):
  - `tz` (no args): prints `rule <string|UTC>` + `offset +hh:mm` (from `tzset(0)`); when the rule string carries a DST name, prints `std` or `dst` - decided by parsing the std offset out of the rule text (few lines, whole-hour and `h:mm` forms only) and comparing with the live offset; when `/etc/tz` is absent but the live offset is not 0, prints `live only, not saved`.
  - `tz <POSIX-string>`: `tzset(str)`; `-1` -> error to stdout + `previous rule kept` + exit 1; success -> print the new state, then re-read `/etc/tz` and print `live only, not saved` unless it matches (this makes the host, which never persists, deterministic here).
  - `tz +N` / `tz -N` (whole hours): maps to `UTC-N` / `UTC+N` (POSIX sign flip in the tool, one comment saying why).
  - `tz UTC`: applies `UTC0` and `unlink`s `/etc/tz`.

- [ ] **Step 1: implement** the tool per the interfaces above.
- [ ] **Step 2: register + PC verify** - add to the Makefile program list; `make && printf 'tz\ntz GMT+3\ntz\ntz nonsense\ntz +2\ntz UTC\n' | TZ=CET-1CEST,M3.5.0,M10.5.0 qemu-arm ./host_linux -r root /bin/sh.bin` and check the transcript by eye against the interfaces (first line shows the TZ-env-provided CET rule; `tz GMT+3` reports -03:00 + `live only, not saved`; the reject keeps `-03:00` live; `tz +2` reports +02:00; `tz UTC` lands at +00:00). Deterministic assertions where the harness can carry them: pipe the same script through `grep -E '(-03:00|\+02:00|\+00:00|kept|not saved)' | wc -l` and compare to the expected count of 5 (the `tz GMT+3` line carries both `-03:00` and `not saved` but counts once).
- [ ] **Step 3: `check_ref` re-run** -> `different: 0` (`tz` in the newer-builtins class via Task 2's list entry - confirm, do not re-add).
- [ ] **Step 4: commit** `tz: tz tool - show, set (POSIX or +N shorthand), clear`.

### Task 5: Board verification (owner pre-approved; AGENTS board rules)

**Files:** none (the verification report is the artifact).

- [ ] **Step 1:** `make` (fresh), full PC chain: `make check 2>&1 | tail -1`, `check_ref`, `check_toolchain --pc`, `check_make --pc`, `bench.py --pc` tails; then `python3 tools/check_toolchain.py --selfhost` - six IDENTICAL (the `cc.c` string change rides here).
- [ ] **Step 2:** flash via `reboot -b` -> UF2; re-put `/bin/date.bin` and `/bin/tz.bin` if the flash wiped root.
- [ ] **Step 3:** legs, in order, recorded in the report: fresh card (no `/etc/tz`) `tz` prints UTC and `date` matches Round A output plus a `+00:00` field; `tz CET-1CEST,M3.5.0,M10.5.0` -> `date` flips within one call, offset field correct for the day; reboot -> restored without re-setting; `tz +2` -> +02:00; `tz nonsense` -> rejected, previous stands; `tz UTC` -> UTC and `/etc/tz` gone; `date -u` unchanged; ~15 console round-trips spot-checked at the usual ~0.4 s (TZ is pure computation - spot check, not a campaign).
- [ ] **Step 4:** write the evidence report to `.superpowers/sdd/2026-11-22-tz/board-report.md` (create the dir); any FAIL -> BLOCKED with per-poll timestamps, no ad-hoc fixes on the board.
- [ ] **Step 5:** no commit (no tree changes); if files had to be re-put, note it.

### Task 6: Closing record

**Files:**
- Modify: `docs/HANDOVER.md` (section 5, entry `0f`, house style - model: 0e)
- Modify: `docs/superpowers/specs/2026-11-22-timezone-design.md` (`## Results`: deviations + board numbers)
- Create: `docs/NOTES-tz.md` (house style: goal / settled-don't-reinvestigate / gates)
- Modify: `README.md` (`date`/`tz` user-facing text; the clock bullet stays true)

- [ ] **Step 1: write the docs** - numbers from the Task 5 report verbatim; NOTES settles at least: POSIX sign trap, `/etc/tz` = first unsealed settings file + boot-order point (after mount, NOT in `rtc_init`), QEMU_RUN `TZ`-strip, engine-not-cc-subset rule, `GMT+3`->`-10800` sentinel story.
- [ ] **Step 2: gates** - `make check 2>&1 | tail -1 && python3 tools/check_ref.py 2>&1 | head -1 && python3 tools/check_toolchain.py --pc 2>&1 | tail -1` -> green.
- [ ] **Step 3: commit** `tz: closing record - HANDOVER 0f, spec Results, NOTES-tz, README`.
