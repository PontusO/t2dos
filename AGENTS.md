# AGENTS.md

This file provides guidance to LLM's when working with code in this repository.

T2DOS is a single-tasking, flat-memory, DOS-class OS for the RP2350. The kernel is an Arduino sketch; everything else (shell, tools, and the toolchain itself: `cc`, `as`, `ld`, `ar`, `nm`, `objdump`) is a binary built by that toolchain and loaded from the SD card. README.md has the user-facing details (bootstrapping, network protocol, language subset). Session conduct, current state and handover notes live in `docs/HANDOVER.md`; feature designs in `docs/superpowers/specs/`.

## Working within the context window

The window is small; spend it on the task, not on dumping files.

  - Build `cc.c` with gcc while debugging. Debug output goes in `#ifdef CC_DEBUG`
    blocks using `fprintf(stderr, ...)`, built with `-DCC_DEBUG`; remove them before
    committing. Never leave stdout probes in loop passes: a matmul inner loop prints
    thousands of lines. Prefer gdb (`-O0 -g`) with narrow breakpoints.
  - Make code changes with the edit tool, one change at a time. Don't restore a backup
    and re-apply patches through a script on every try.
  - Never print whole assembly files: `grep -n` or `sed -n 'A,Bp'` the function you
    care about. Pipe check output through `head -50` / `tail -15` or a narrow grep.
  - Don't re-read code you already read this round unless it changed.
  - When you find something important mid-task, write it to `docs/NOTES-<topic>.md`
    so it survives compaction; fold what matters into code comments before finishing.

## Board safety

The board is one serial port: only one program may hold `/dev/ttyACM*` at a time.

  - Never kill the user's `screen` session. A busy (or hung) port means stop and ask.
    `tools/board.py` takes its port from `$T2DOS_PORT`, else the first iLabs RP2350
    entry under `/dev/serial/by-id/`.
  - Board runs (`--board`, `--selfhost`, `bench.py --board`) need the user's go-ahead.
  - Bad encodings can hang the board: disassemble new codegen (`cc -S`, or
    `build/host/objdump`) before running it there.

## Build and test

```sh
make            # gcc builds compiler/cc and build/host/{as,ld,ar,nm,objdump}, then cc builds root/bin/*.bin
make check      # PC checks: asm vs GNU as, objdump vs GNU objdump, link tests, byte identity, driver
make ref        # build build/ref/cc (the pre-assembler compiler at build/ref/COMMIT) for check_ref
make host       # build the Linux/ARM kernel harness (needs arm-linux-gnueabihf-gcc)
make clean
```

Run `make` before any board test and `make ref` before `check_ref.py`; both fail
confusingly otherwise. Each checker's flags are in its own docstring
(`python3 tools/check_X.py` with no args prints it).

Triage - what you changed, what to run:

| change | PC check | board check |
|---|---|---|
| `cc.c` codegen (no `-O` path touched) | `check_ref.py` (byte identity), `compiler/tests/test*.c` vs gcc | `check_toolchain.py --selfhost` |
| anything in `cc/as/ld/binutils` | `make check`, `bench.py --pc` | `--board`, `--selfhost` |
| `as` encodings | `check_asm.py --diff N` (GNU oracle below) | |
| `ld` / objects / archives | `check_link.py --pc` | `check_link.py --board` |
| `objdump` | `check_asm.py --objdump-corpus` | |
| kernel / `host_linux.c` | `check_sh.py --pc`, `check_net.py --pc` | their `--board` variants (ask first) |
| jobs / pipes / spawn | (host_linux returns SPAWN_UNSUPPORTED) | `check_jobs.py --build|--board` |
| BASIC | `check_basic.py --pc` (also runs ASan leak checks) | `check_basic.py --board` |
| `-O` pass | plain and `-O` builds must agree behaviorally: compiler tests, BASIC cases under qemu, `bench.py --pc`; `make OPT=-O` toolchain must rebuild itself identically | `--selfhost` |

`bench.py --pc|--board` builds every `tests/bench/*.c` plain and with `-O`, checks
each `check` line against a native gcc build, and prints times and sizes: the
correctness-plus-speed one-liner for compiler work. gcc-vs-cc timing only exists
under qemu (gcc builds are armhf/glibc; the board cannot run them). The GNU oracle
is `~/.arduino15/packages/rp2040/tools/pqt-gcc/*/bin/arm-none-eabi-{as,objdump}`.

Testing means running binaries under qemu-arm with `host_linux`, which implements the
same syscall contract as the kernel against a root directory (default `./root`):

```sh
printf 'ls\n' | qemu-arm ./host_linux -r root /bin/sh.bin       # drive the shell
compiler/cc -o root/bin/test.bin compiler/tests/test.c && qemu-arm ./host_linux -r root /bin/test.bin
```

A fresh `host_linux` that segfaults at startup is a stale or bad build (`make host`),
not your code. In this sandbox it may not start at all; direct `.bin` runs still work.
`compiler/tests/*.c` exercise the language subset; test.c returns 42, test4.c onward
are standard C so gcc builds them natively for reference output (only pointer `sizeof`
differs). On-board testing goes over the serial console (`put name size` + raw bytes,
`hexdump file`); `tools/board.py` wraps it (`put`, `run`, `fetch`, `expect_gcc`). If
the board hangs and sits on the switchable USB hub, power-cycle with exactly
`uhubctl -l 3-1.4 -p 4 -a cycle -d 3` (no other hub or port); the command right after
is often misread as no answer. `check_toolchain.py --selfhost` is the self-hosting
fixed point: the board cc rebuilds cc, as, ld, ar, nm, objdump byte-identically to
the PC builds. `tools/fileserver.py dir [port]` (default 7777) is the PC side of the
`get`/`send`/`rls` network tools.

Kernel flashing (iLabs CPico 2350, arduino-pico core `rp2040:rp2040`; the sketch
includes `sys.h` from the repo root):

```sh
FQBN=rp2040:rp2040:ilabs_cpico_2350:flash=8388608_7340032
arduino-cli compile --fqbn $FQBN --build-property "compiler.cpp.extra_flags=-I$PWD" --output-dir build/kernel kernel
arduino-cli upload  --fqbn $FQBN -p /dev/ttyACMx --input-dir build/kernel
```

On the Challenger 2350 WiFi/BLE use `FQBN=rp2040:rp2040:challenger_2350_wifi6_ble5:flash=8388608_7340032,espwifitype=esp_hosted` and add `--library third_party/ESPHost` (the default `esp_at` build has no WiFi driver; the core's bundled ESPHost hangs in the GPIO IRQ once WiFi is up, so the patched copy is required). A hard hang that stops accepting serial input needs a manual reset or BOOTSEL and a `.uf2` copy; reflashing keeps the flash filesystem. `kernel/vault.cpp` (wifi -k, AES-128-GCM) uses OTP page 60 rows 0xf00-0xf08 only: never write any other OTP row.

## Architecture

- **One ABI, four places.** `sys.h` defines syscall numbers, flags and structs. Adding
  or changing a syscall means updating all of, or it works on PC and hangs the board:
  1. `sys.h` (numbers are frozen; add new ones in the gaps of their group),
  2. the builtin name/number string in `cc.c` `main()` (and the list there of builtins
     returning `char *`); `as`, `ld` and the binutils use the same builtins,
  3. the dispatch `switch` in `kernel/kernel.ino`,
  4. the dispatch `switch` in `host_linux.c`.
- **Bootstrap rule.** `compiler/cc.c`, `assembler/as.c`, `linker/ld.c`, `binutils/*.c`
  and `lib/t2obj.c` must stay inside the subset the committed `cc` compiles, because
  they rebuild themselves on the board. No floats, initializers, function pointers,
  unsigned/short, 2-D arrays or multi-line macros in `cc.c`; the subset still lacks
  `static`, `const`, do-while, goto, `sizeof expr`, user variadic functions, long long
  and struct assignment (rejected). Style is deliberately primitive (`p = p + 1`,
  assignments separate from declarations, all functions return `int`). When in doubt,
  compile your change with the committed cc and `build/ref/cc`: they are the parsers
  of record. `#include <...>` is ignored by `cc`; libc calls in `cc.c` map onto the
  syscall builtins. Don't use a new language feature in `cc.c` in the same change that
  adds it (an older `cc` must still build it).
- **Toolchain pipeline:** `cc` writes GNU unified-syntax Thumb-2 assembly; `as` turns
  it into a T2DOS object (`"T2OB"`, see `lib/t2obj.h`), `ld` links objects and `ar`
  archives (`"T2AR"`, only needed members are linked) into the `.bin`. `cc` is also the
  driver (`-S`, `-c`, `-l`/`-L`, mixed inputs; temporaries in `/tmp`): on the board it
  `exec`s `/bin/as.bin` and `/bin/ld.bin`, on the PC it runs `build/host/as`/`ld` (or
  `$T2_TOOLS`; cc resolves that path relative to the current directory, so a stale
  `/tmp/build/host` can shadow it). Encodings live only in `as`; its accepted forms
  must match GNU as byte-for-byte and reject what GNU rejects (`check_asm.py --diff`);
  `objdump`'s text must equal GNU objdump's.
- **Code model rule:** no absolute addresses. A data symbol's value is its data offset
  (used against `r9`), a text symbol's its code offset. T2DOS pseudo-instructions:
  `fnaddr rd, sym`, `movw.16`, `.t2init` (run-before-main; `ld` builds the stub). `cc`
  picks each data reference's form (`addw` < 4 KB, `movw.16` < 64 KB, else movw/movt)
  from the linked offset via the driver's `dbase` tracking; `-S`/`-c` use the long form
  unless `-mshort-data`; `extern` references always use movw/movt.
- **Calling convention:** `r7` frame pointer, `r8` = kernel `sys()` entry, `r9` = data
  base, all args on the stack left to right (first arg deepest), caller pops, result in
  `r0`. Kernel side: `sys(num, nargs, argv)` with `argv[nargs-1]` the first argument
  (the `A(i)` macro; never declare a local `n` inside `sys()`).
- **Binary format:** `"T2CC"`, then code size, data size, a reserved 0, entry offset
  (all 32-bit LE), then code bytes, then data bytes. Loaded by `run_bin` in
  `host_linux.c` and the loader in `kernel.ino`.
- **Compiler design:** single pass, no AST: tokenizer (`next`), preprocessor, `expr`,
  `stmt` append assembly lines to a per-function buffer (`.L<n>` labels; `lastload`
  counts lines for `lvalue()`'s rollback); the buffer is written when the function
  ends. With `-O`: `peep()` rewrites the buffer (every rewrite that changes a register
  or flags checks `dead()`/`fdead()`), then `licm_loops()` hoists invariant address
  math (see `docs/superpowers/specs/2026-11-19-cc-licm-design.md`), then `peep()` again
  so hoisted pairs fold, then `promote()` and `trimframe()`. Without `-O` the output
  must stay byte-identical (`check_ref`). Floats are 32-bit in `r0` as their bit
  pattern; only arithmetic/comparisons/conversions move them through `s0`/`s1`; with
  `-O`, `promote()` keeps float locals and parameters in s16-s31 (callee-saved; a value
  in s0-s15 never lives across a label, branch or call, which `sdead()` relies on).
- **Kernel memory model:** program code in SRAM; data, the 256 KB stack and `malloc`
  in PSRAM. PSRAM shares the QSPI controller with flash: nothing may touch PSRAM while
  flash is written (hence the SRAM `kstack` in `sys_entry`). Program mallocs are per
  `Proc`, zeroed on allocation (`cc` depends on that) and freed on exit. `/tmp` is a
  PSRAM RAM filesystem (`kernel/ramfs.cpp`): writes stop at `TMP_MARGIN` (1 MB) free;
  contents lost at reset; checked by `check_toolchain.py --board` (`check_ramtmp`).
- **Tasks:** program code is preempted, kernel code never is. Foreground + up to 4
  jobs; all switches go through `k_yield()`/`k_wait()` (getchar, kbhit, sleep, recv,
  accept, pipe waits, kill, and the 10 ms preemption alarm in `preempt_isr`). Never add
  a switch point inside a file, flash or WiFi call (LittleFS is not reentrant). New
  per-program kernel state must join the per-task swap list (`t2_sys_sp`, cwd,
  stdout_fd, procs, printf buffer). Ctrl-C ends a foreground program via `brk_check`
  unless it called `conctl(CON_BREAK, 0, 0)`; anything reading raw console bytes must.
  Details of `preempt_tramp`, pipes and the type-ahead ring: comments in `kernel.ino`.
- **BASIC (`basic/`):** QBasic-style bytecode interpreter, `root/bin/basic.bin`; VM in
  `vm.c`, yields every 1024 ops, `mem_alloc` is the only allocator (64 KB PSRAM
  margin). cc accepts one `-I`, so it builds with `-I basic`. Maths built-ins are
  syscalls 112-130 (`lib/math.h`, TFLOAT); user syscalls start at 131.
- **printf/sprintf and string/memory helpers run kernel-side**, so the format subset is
  whatever `kernel.ino` and `host_linux.c` implement; keep them in sync.
- **Includes:** `#include "x"` resolves relative to the *current directory*, then the
  `-I` dir, then `/lib`. The Makefile runs from the repo root with `-I lib`, so
  userland picks up the top-level `sys.h` and `lib/net.c`. Never add a second `sys.h`.
- **`root/`** is the SD card image. `root/bin`, `root/lib` and `root/src/*` are build
  output (gitignored); copy `root/` to the card.
