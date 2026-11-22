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

`main` and `origin/main` are at `97f1394`. The working tree is clean. Everything below is merged and pushed.

Recent work, newest first:

0. **`make`** (`userland/make.c` → `/bin/make.bin`, spec `2026-11-21-make-utility-design.md`, on branch `cc-window` awaiting merge): a target-line build executor; `Makefile.board` builds the whole toolchain tree on-board (`/src/...` sources, headers mirrored in `/lib`), always rebuilds, `-n` prints the plan. `tools/check_make.py --pc` runs the plan cases under the PC harness, `--board [--tools a,b]` builds and byte-compares each tool. Board status: all eight tools build byte-identically through make (cc 36 s, basic 19 s, the rest 2-11 s). An earlier round's two failures (as.c, basic) were root-caused and fixed: `readfile`'s 1 MB-per-file mallocs crossed the PSRAM floor `/tmp` allocates from since `de0a5bc` moved /tmp to PSRAM; read buffers are now sized to the file (`docs/NOTES-board-cc-flaky.md`).
0b. **The cc is split** (spec/plan `2026-11-21-cc-split-*.md`, ledger `docs/NOTES-cc-split.md`): `compiler/cc.c` is now eleven sources plus `compiler/cc.h` (shared types, `extern` globals, prototypes - the `basic.h` pattern): `state` (globals, readfile), `emit` (assembly output), `opt`/`fp`/`regs`/`licm` (the four -O passes), `lex`, `types`, `expr`, `stmt`, and `cc.c` (cfile->asm, as/ld runner, driver). `make`'s `CC_SRC` wildcard and `Makefile.board` list them; `check_ref` dropped its `compiler/cc.c` entry (ref cc predates `extern`), and the split is gated by behaviour plus on-board byte identity: `check_make --board` and `--selfhost` both rebuild `cc.bin` byte-identically (26-36 s, was 19 single-file; `cc.bin` 207403 -> 226256 bytes from per-TU data layout).
0c. **`static`** (spec/plan `2026-11-21-static-keyword-*.md`, ledger `docs/NOTES-static-keyword.md`): full `static` - internal linkage for file-scope vars/functions (linkage = omitting `.global`: `ld` resolves and dup-checks only global definitions, so `as`/`ld` are untouched; static vars get classes `CSta`/`CStaArr`, live in the data image with no label, and are referenced through `ldref`'s `.Ldata+off` form) and static local variables (same storage; initializers follow the global path - constants into the image, coded init spliced into `.Linit`, once before main). Guards: `static`+`extern`, `static main`, redefinitions. Tests: `test15.c` vs gcc (plain and `-O`), three `pipeline_errors` cases, a two-file same-static-names case; `check_ref` excludes test15 by name (ref cc predates `static`). The toolchain sources do not *use* `static` yet - an adoption round (retiring cc.h's globally-unique-name rule) waits for the new cc to be the committed one.
0d. **Sources moved into `src/`** (spec/plan `2026-11-21-src-move-*.md`, ledger `docs/NOTES-src-move.md`): `compiler`, `assembler`, `linker`, `binutils`, `lib`, `basic`, `userland` and `kernel` now live under `src/` (git moves, contents untouched except one deliberate edit: the PC-only `init_tools` in `cc.c` walks up to the repo root, one level deeper). `sys.h`, `host_linux.c`, `tools/`, `tests/`, `examples/`, `docs/` stay at the root. The board layout is unchanged by design: sources mirror under `/src` exactly as before, so `Makefile.board` and the `check_make` fixture only changed in comments; the checkers gained the `src/` prefix in their repo paths and the staging mapping (`src/X` <-> `/src/X`). `make ref`'s `git show` path stays historic (pre-move blob). Gates: full clean rebuild byte-identical, `check_ref` different: 0, `check_make --pc/--board`, `--selfhost` - see commit messages.
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
