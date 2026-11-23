# T2DOS handover

This document is for the next maintainer of T2DOS. It covers how the project is put together, how to work on it safely, what state it is in today (2026-11-18), and what is still open.

Read these three files first, in this order:

1. `CLAUDE.md`: build and test commands, architecture, and the rules the code depends on. It is short and every line matters.
2. `README.md`: the user's manual. It describes every command, the C subset, BASIC, the network tools and the Android app as the user sees them.
3. This file: working rules, project state and the backlog.

The design notes for each larger feature are in `docs/superpowers/specs/` (what and why) and `docs/superpowers/plans/` (how it was built, task by task). Read the matching spec before changing a feature.

## 1. What the project is

T2DOS is a single-tasking DOS-style OS for the RP2350 (Cortex-M33). The kernel is an Arduino sketch (`kernel/`). Everything else runs as `.bin` programs loaded from the SD card (or the flash file system): the shell, the editor, a BASIC interpreter, network tools, and a full self-hosting C toolchain (`cc`, `as`, `ld`, `ar`, `nm`, `objdump`).

The most important property: **the toolchain rebuilds itself on the board, byte for byte identical to the PC build.** Almost every rule below exists to protect that.

Main parts:

| Directory | What it is |
|---|---|
| `kernel/` | Arduino sketch: syscalls, loader, tasks and preemption, RAM `/tmp`, WiFi, telnetd, netfs, vault (sealed WiFi secrets) |
| `host_linux.c` | The same syscall contract on Linux, run under `qemu-arm`. This is how almost all testing happens |
| `sys.h` | The one ABI header (syscall numbers, flags, structs) |
| `compiler/cc.c` | Single-pass C compiler and driver; `-O` adds a peephole pass, register promotion and frame trimming |
| `assembler/`, `linker/`, `binutils/`, `lib/` | `as`, `ld`, `ar`/`nm`/`objdump`, object format |
| `userland/` | Shell, editor and the command-line tools |
| `basic/` | QBasic-style interpreter (bytecode compiler + VM) |
| `android/` | "T2DOS Terminal", a Kotlin telnet client for the board |
| `tools/` | Test scripts (`check_*.py`), `board.py` (serial console helper), `bench.py`, `fileserver.py` |
| `tests/` | Test cases per area |

## 2. Hardware and the user's setup

- The board in daily use is the **Challenger 2350 WiFi/BLE** (ESP32-C6 running ESP-Hosted). It needs the patched `third_party/ESPHost` library; the compile command is in `CLAUDE.md`.
- It is on a switchable USB hub. If it hangs, power-cycle it with exactly:
  `uhubctl -l 3-1.4 -p 4 -a cycle -d 3`
  Never touch any other hub or port. The first command after a power cycle often looks like it got no answer; retry once.
- The user connects to the board in two ways:
  - USB serial, often through a `screen` session on the PC;
  - telnet over WiFi from their phone, using the Android app.
  Both share the same console.
- The user's PC runs `tools/fileserver.py` on port 7777. That is their own process: leave it alone, and use a different port for your tests.
- `/etc/mount` on the board holds the user's netfs mount (`/mnt/host`). `/etc/wifi` holds the sealed WiFi credentials. Don't overwrite either.

## 3. Rules you must follow

These came from real incidents. Treat them as hard rules.

### Board safety

- **Never kill the user's `screen` session.** If the serial port is busy, stop and ask the user to close it.
- **Don't run board tests while the user's phone is connected.** The console is shared, so test output and the user's session mix and both break. Ask first.
- **Never write any OTP row except the vault rows** (page 60, rows 0xf00-0xf08, already written on this board). OTP writes are permanent.
- **Don't run `check_telnetd.py --board` without asking.** It replaces the user's telnetd password.
- **Check new code generation before it runs on the board.** Use `cc -S` or `build/host/objdump`. A bad encoding can hang the board.
- When you upload with `board.py put`, it waits for the board to answer "receiving N bytes" before it sends anything. Keep it that way. An earlier version sent too soon, and the shell read the binary data as commands; that created junk files on the board twice.

### Git

- Commit and push only when the user asks. Merging into `main` and pushing both need the user's OK every time.
- Do feature work on a branch in a separate worktree (`git worktree add ../t2dos-NAME -b NAME`), then fast-forward `main` once it is approved.
- Don't use `git stash`, `reset --hard`, `rebase`, `cherry-pick`, force-push, `git clean`, or scratch branches in the user's checkout unless the user explicitly asks.
- Never stage unrelated changes. No `git add -A` or `git add .` when the tree has other edits.

### Writing style (the user's preference)

- No em dashes anywhere: code, comments, commits, docs. Use a colon, comma, parentheses or a new sentence instead.
- Plain developer wording. Write "checked", not "verified empirically", and "the test files", not "the corpus".

### Code rules (details in `CLAUDE.md`)

- **The bootstrap subset:** `cc.c`, `as.c`, `ld.c`, `binutils/*.c` and `lib/t2obj.c` may only use what the *committed* `cc` can compile:
  - none of: floats, initializers, function pointers, unsigned/short, `static`, `const`, do-while, goto, `sizeof expr` or struct assignment;
  - all functions return `int`;
  - if you add a language feature, don't use it in `cc.c` in the same change.
- **Plain output must not change:** output without `-O` must stay byte-identical to the reference compiler. `python3 tools/check_ref.py` must report `different: 0`.
- **Adding or changing a syscall touches four places:**
  - `sys.h`;
  - the builtin list in `cc.c` `main()`;
  - the switch in `kernel/kernel.ino`;
  - the switch in `host_linux.c`.
- **Programs that read raw console bytes** must call `conctl(CON_BREAK, 0, 0)`, so that Ctrl-C reaches them as a byte and does not kill them.
- **Don't add task switches inside file, flash or WiFi calls.** PSRAM shares the bus with flash, and LittleFS is not reentrant.

## 4. How to test

There is no single test runner. Run what matches what you changed. For details, see the list in `CLAUDE.md`.

| You changed | Run at least |
|---|---|
| anything | `make && make check && python3 tools/check_ref.py` |
| `cc.c` | the above, plus `compiler/tests/*.c` under qemu, `check_toolchain.py --pc`, `check_frame.py`, and a `make OPT=-O` build with `check_basic.py --pc`, `check_sh.py --pc` and `check_asm.py --corpus` |
| `as`/`ld`/binutils | `check_asm.py --corpus`, `check_asm.py --diff N`, `check_link.py --pc` |
| kernel or `host_linux.c` | `check_sh.py --pc`, `check_net.py --pc`, `check_netfs.py`, `check_telnetd.py` (PC mode); then flash and run the `--board` variants (ask first) |
| BASIC | `check_basic.py --pc` (it also runs every case under AddressSanitizer) |
| shell, editor | `check_sh.py --pc`, `check_telnetd.py` (edit check) |
| Android app | `python3 tools/check_android.py`; Gradle unit tests in `android/` (JDK 17: `JAVA_HOME=/usr/lib/jvm/java-17-openjdk-amd64`) |
| before any merge that touches the toolchain | `check_toolchain.py --selfhost` on the board: the board rebuilds the toolchain byte-identically |

Notes:

- `check_jobs.py --board` only passes when the board shell's current folder is `/`. A failure in `ctrl_c_busy` usually means the shell was left in another folder.
- Board timing is sensitive to code alignment. A loop that moves by 2 bytes can run 5-6% faster or slower, so compare benchmarks over several runs before calling a change a regression.
- Tests leave temporary files in `root/tmp/`. That folder is build output.

## 5. State on 2026-11-18

`main` and `origin/main` are at the `tz` merge (`a44acd8`). The working tree is clean. Everything below is merged and pushed.

Recent work, newest first:

0. **`make`** (`userland/make.c` → `/bin/make.bin`, spec `2026-11-21-make-utility-design.md`, on branch `cc-window` awaiting merge): a target-line build executor; `Makefile.board` builds the whole toolchain tree on-board (`/src/...` sources, headers mirrored in `/lib`), always rebuilds, `-n` prints the plan. `tools/check_make.py --pc` runs the plan cases under the PC harness, `--board [--tools a,b]` builds and byte-compares each tool. Board status: all eight tools build byte-identically through make (cc 36 s, basic 19 s, the rest 2-11 s). An earlier round's two failures (as.c, basic) were root-caused and fixed: `readfile`'s 1 MB-per-file mallocs crossed the PSRAM floor `/tmp` allocates from since `de0a5bc` moved /tmp to PSRAM; read buffers are now sized to the file (`docs/NOTES-board-cc-flaky.md`).
0b. **The cc is split** (spec/plan `2026-11-21-cc-split-*.md`, ledger `docs/NOTES-cc-split.md`): `compiler/cc.c` is now eleven sources plus `compiler/cc.h` (shared types, `extern` globals, prototypes - the `basic.h` pattern): `state` (globals, readfile), `emit` (assembly output), `opt`/`fp`/`regs`/`licm` (the four -O passes), `lex`, `types`, `expr`, `stmt`, and `cc.c` (cfile->asm, as/ld runner, driver). `make`'s `CC_SRC` wildcard and `Makefile.board` list them; `check_ref` dropped its `compiler/cc.c` entry (ref cc predates `extern`), and the split is gated by behaviour plus on-board byte identity: `check_make --board` and `--selfhost` both rebuild `cc.bin` byte-identically (26-36 s, was 19 single-file; `cc.bin` 207403 -> 226256 bytes from per-TU data layout).
0c. **`static`** (spec/plan `2026-11-21-static-keyword-*.md`, ledger `docs/NOTES-static-keyword.md`): full `static` - internal linkage for file-scope vars/functions (linkage = omitting `.global`: `ld` resolves and dup-checks only global definitions, so `as`/`ld` are untouched; static vars get classes `CSta`/`CStaArr`, live in the data image with no label, and are referenced through `ldref`'s `.Ldata+off` form) and static local variables (same storage; initializers follow the global path - constants into the image, coded init spliced into `.Linit`, once before main). Guards: `static`+`extern`, `static main`, redefinitions. Tests: `test15.c` vs gcc (plain and `-O`), three `pipeline_errors` cases, a two-file same-static-names case; `check_ref` excludes test15 by name (ref cc predates `static`). The toolchain sources do not *use* `static` yet - an adoption round (retiring cc.h's globally-unique-name rule) waits for the new cc to be the committed one.
0d. **Sources moved into `src/`** (spec/plan `2026-11-21-src-move-*.md`, ledger `docs/NOTES-src-move.md`): `compiler`, `assembler`, `linker`, `binutils`, `lib`, `basic`, `userland` and `kernel` now live under `src/` (git moves, contents untouched except one deliberate edit: the PC-only `init_tools` in `cc.c` walks up to the repo root, one level deeper). `sys.h`, `host_linux.c`, `tools/`, `tests/`, `examples/`, `docs/` stay at the root. The board layout is unchanged by design: sources mirror under `/src` exactly as before, so `Makefile.board` and the `check_make` fixture only changed in comments; the checkers gained the `src/` prefix in their repo paths and the staging mapping (`src/X` <-> `/src/X`). `make ref`'s `git show` path stays historic (pre-move blob). Gates: full clean rebuild byte-identical, `check_ref` different: 0, `check_make --pc/--board`, `--selfhost` - see commit messages.
0e. **Wall clock** (spec/plan `2026-11-22-clock-ds1307-*.md`, ledger `docs/NOTES-clock.md`, merged 2026-11-22): `src/kernel/rtc.cpp` keeps UTC from three sources by precedence - a DS1307 on I2C `0x68` (probed at boot and re-probed while unsynced; values outside 2020..2100 are garbage), SNTP to `pool.ntp.org` once WiFi is up (hourly resync while connected), and a `millis()` timer that ticks from the epoch in the unsynced state. `time(&valid)` reaches it through `SYS_time = 105` in all four places (`sys.h`, cc's builtin strings, the kernel switch, `host_linux`) plus a new `/bin/date` tool. `clock_service()` rides the console `getc`/`kbhit`/`peek` wrappers (the `telnetd_service()` pattern; owner-accepted coupling - sync progresses while the console is polled, stalls under foreground programs; a scheduler idle hook was rejected over the no-switch-inside-WiFi invariant). Board-proven, two fix rounds: esp_hosted registers its UDP recv callback only in `begin()`, and its lwIP stack only exists after `WiFi.mode`/`begin`, so the socket binds at the `WL_CONNECTED` edge, never in `rtc_init`. `check_toolchain` gained a `QEMU_RUN` PC step (`clock` case: real time and valid=1 under `host_linux`). No DS1307 on the bench (owner-confirmed at design time, spec Tests section): the chip paths are code-reviewed, only the probe-false branch is board-verified (write-back to the absent chip is silent); chip-boot and heal tests are deferred. Board, across three passes (`c126b88`/`10a4d87`/`d608c04`): sync <=16 s from boot, and after a reboot 4 s after the join edge (~19 s uptime); 1 s board-vs-PC delta holding over +127 s; zero drift over 90 s unsynced; ~45 console round-trips uniform 0.38 s (no DNS stall); `--selfhost` six IDENTICAL 2m28.8s (first pass, run on `c126b88` - the two later fix commits are kernel-only, so the toolchain side is unaffected).
0f. **Time zones** (spec/plan `2026-11-22-timezone-*.md`, ledger `docs/NOTES-tz.md`, merged 2026-11-22): `date` prints local time with the numeric UTC offset (`-u` keeps the UTC line), from a kernel-owned POSIX zone rule. `src/kernel/tz.c` (+ `tz.h`) is the single pure engine (parse, `tz_offset(rule, utc)`), compiled into the kernel, `host_linux` and the native PC test; it is deliberately not cc-subset and no userland program embeds it (the `tz` tool parses the std offset itself, a few lines). `SYS_localtime = 106` and `SYS_tzset = 107` landed in all four places; the offset is computed at display time, never inside `SYS_time`, so UTC stays what NTP, the chip and future mtimes see. `tzset` applies a validated rule live and persists it to `/etc/tz`, the first *unsealed* settings file (nothing secret about a zone; `/etc/wifi` stays sealed), loaded at boot after the filesystem mounts, never in `rtc_init` (which predates `SDFS.begin`). Rulings not to re-litigate: POSIX counts offsets west-positive (`CET-1` is UTC+1; `GMT+3` applies `-10800`), which makes the reject sentinel exactly `-1` - a legal apply or a `tzset(0)` query can return a negative offset, so userland tests `== -1`, never `< 0`; an omitted `/n` transition time means 02:00 local standard on both sides (deliberately not glibc's local-of-the-side-being-left: it matches the real EU transition, 02:00 CET, for the Sweden string). Board PASS on `a0ddc24` (Challenger): `CET-1CEST,M3.5.0,M10.5.0` renders winter `+01:00 std` with `date` = host UTC + 1 h, reboot restores the rule from `/etc/tz` without re-setting it, the `+2`/reject/clear legs are clean, 15/15 console round-trips at 0.37 s; `--selfhost` six IDENTICAL on the third attempt (a corrupted fetch at offset 8676, then a hang mid-cc-compile needing the AGENTS hub power cycle - the known board-cc flakiness class per `docs/NOTES-board-cc-flaky.md`, not tz-caused, watch-only).
0g. **File mtimes** (spec/plan `2026-11-22-mtime-roundb-*.md`, ledger `docs/NOTES-roundb-mtimes.md`, on branch `mtimes` awaiting merge): every card write is stamped from the wall clock - `setup()` registers `SDFS.setTimeCallback(fs_time_cb)` after a successful mount (never before `begin()`: the callback would outlive a failed mount). The callback truncates to whole minutes, load-bearing, not cosmetics: SDFS writes *raw* `tm_sec` into the 5-bit DOS field while `FatToTimeT` reads it back spec-compliant as `sec * 2`, so only second 0 round-trips (a verbatim 23:08:45 reads back 23:09:26); nothing displayed is finer than a minute. DOS fields hold the UTC calendar because `localtime_r` on this libc, with no `TZ` machinery, *is* UTC - storage stays UTC per the tz contract. `dirent_t` gains `int mtime` (kernel `SYS_readdir` fills `Dir::fileTime()`, `host_linux` `st_mtime`; ramfs/netfs stay 0 = unknown); the widening ships without a compat syscall (accepted, `stat_t` precedent) but a *stale* binary still allocating 72 bytes takes a 4-byte kernel overwrite, so the authoritative re-`put` rule is `grep -l 'readdir\|dirent_t' src/*/*.c` - this round {basic ls rcp sh}. `SYS_tzlocal = 108` in all four places: `tzlocal(utc) -> utc + tz_now(utc)`, the offset *of that instant* - `ls` renders every file's own DST side, never today's offset on a past mtime (`tzset(0)` answers *now*, `tzlocal` the instant). `ls` prefixes `%04d-%02d-%02d %02d:%02d` civil fields of `tzlocal(mtime)` (the `date.c` shape), or `----------` for any mtime outside `MTIME_UNKNOWN < mtime < MTIME_MAX` - both bounds exclusive and both principled: below is the FAT epoch floor / unsynced sentinel (unrepresentable), above is past the int32 ceiling 2038 the whole OS lives under, which clamps the junk ~2097 stamps pre-stamping cards already carry from `time(nullptr) = 0` (old files show `----` until rewritten - truthful, their stamps are junk). `basic` gains read-only `DATE$` (`YYYY-MM-DD`) and `TIME$` (`HH:MM:SS`), local through the active rule, reserved keywords via the INKEY$/TIMER pattern (assignment is a syntax error). Rulings not to re-litigate and gate determinism: persistent gates must be season-independent and ambient-TZ-proof - this round found and fixed two seasonal landmines from the plan (the tz.c `tzset("CET...") == 3600` leg became year-round 3600-or-7200; the `ls_times` golden dropped the tz transcript echo and pins its zone in `ls_times.env`, mtimes staged absolutely). Board PASS on `6aba4ab` (Challenger): kernel 262428 bytes (+112 vs the tz round); synced minute-exact stamping with winter `+01:00`; `get` downloads stamped at transfer; pre-existing junk stamps (`cat.bin`, `wc.bin`) and ramfs/`/mnt/fs` list `----------`; the unsynced-boot window (~9-14 s) was won by 0.4 s on attempt 3 of a single-session serial race; zone and mtimes survive reboot; 10/10 `ls /bin` round-trips uniform 0.57 s (payload-sized, per-entry `fileTime()` free); `--selfhost` six IDENTICAL on the first attempt.
1. **cc -O cheaper function entry and exit** (`trimframe()` in `cc.c`; spec `2026-11-17-cc-calls-design.md`):
   - functions with no locals no longer set up a frame;
   - small frames use `sub sp, #N`;
   - early returns in frameless functions pop straight to `pc`.
   - Board results: `bench.bas` went from about 284k to about 305k statements/s, and the `-O` `basic.bin` from 122 KB to 117 KB.
   - The board's `/bin` still has binaries built before this change; copy `root/` to update them.
2. **T2DOS Terminal (Android)**: a telnet client with a screen emulator for the codes T2DOS sends, extra keys, a list of boards, a saved password per board, and a foreground service that keeps the session alive in the background (`android/`, spec `2026-11-17-android-terminal-design.md`).
3. **Shell line editing**: left/right, Home/End and Delete in `sh` (`userland/sh.c`).
4. **`edit` handles terminal resizes** (the phone rotating).
5. **telnetd**:
   - `exit` in a telnet session logs that session out, and the kernel restarts `sh`;
   - `umount` removes its empty `/mnt/NAME` folder (commit `2f2bff1`).
6. **netfs**: `mount NAME host` and `umount NAME`, several mounts listed in `/etc/mount`.

## 6. Backlog

None of these are urgent. Everything the user uses day to day works.

**The user would notice these first:**

1. **Slow start after a reboot.** The board takes about 10 s to rejoin WiFi before telnetd listens. Measure where the time goes in `setup()` first, then try a faster join to the known network.
2. **Pinch zoom in the app** sends a new terminal size at every step, so `edit` redraws over and over. It should send one size when the pinch ends.
3. **Passwords with characters outside Latin-1** make auto-login fail with no message.
4. **Some phone keyboards** (glide typing, voice, some Samsung keyboards) hold text back while composing, and nothing shows until they hand it over. Gboard works.

**Smaller items in the app:**

5. Keys typed before the connection is up are dropped.
6. If the phone's key store is broken, saving a password crashes the app.
7. Drawing creates an object per cell per frame. That is slow for very large bursts of output.
8. The connection thread only catches network errors. This is a safety net only; no way to trigger another error was found.

**Board side (telnetd):**

9. A client that connects and never answers holds the password prompt for up to 60 s.
10. Kernel messages ("out of memory", "sh exited", the recovery prompt) go only to USB, never to telnet.
11. A client that stops reading can stall board output for up to 2 s before it is dropped.
12. In `host_linux` only, `conctl` codes end up in the file when output is redirected (the board doesn't do this).

**Compiler (`cc -O`):**

13. **Align loop starts to 4 bytes with `-O`.** This should make board timings steadier and win back the alignment loss in `tests/bench/loop.c`.
14. **Dead code after an inlined return:** when a `return` becomes an inline `pop`, the unreachable code behind it is kept. This only costs size.
15. **Jump tables for `switch`** were looked at and put aside: the hot paths (the BASIC VM) dispatch through function-pointer tables, not `switch`, so the gain would be small.

**Older:**

16. The multi-mount netfs review left 8 minor items that were never written down here. Rerun a review of `userland/mount.c`, `umount.c`, `lib/mtab.c` and the netfs parts of the kernel to collect them again.

## 7. Working with the user

- The user tests on real hardware and gives short answers ("Go ahead", "Merge and push"). Before merging, show the results: board numbers, what was checked, and what is left.
- For a new feature, agree on a short design first and put it in `docs/superpowers/specs/YYYY-MM-DD-NAME-design.md`.
- When you need the board while the user may be on it, ask first. A clash with their live session wastes both your time and theirs.
- Keep reports short: what changed, what was checked, and what is left. Mention a failure or a skipped step plainly.
