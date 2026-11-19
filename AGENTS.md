# AGENTS.md

This file provides guidance to LLM's when working with code in this repository.

T2DOS is a single-tasking, flat-memory, DOS-class OS for the RP2350. The kernel is an Arduino sketch; everything else (shell, tools, and the toolchain itself: `cc`, `as`, `ld`, `ar`, `nm`, `objdump`) is a binary built by that toolchain and loaded from the SD card. README.md has the user-facing details (bootstrapping, network protocol, language subset). Session conduct, current state and handover notes live in `docs/HANDOVER.md`; feature designs in `docs/superpowers/specs/`.

## Working within the context window

The context is compacted at about 200K tokens (opencode reserves a buffer against the
262K window) and only a summary survives. Everything you read or print stays in the
window until then, so keep it small.

  - Find code with `grep -n` first, then read at most ~60 lines around the hit.
    Don't re-read code you already read this round unless it changed.
  - Cap command output: `| head -40`, `| tail -20` or a narrow grep; for suites show
    only the end, e.g. `make check 2>&1 | tail -20`. Never print whole assembly files:
    write them with `cc -S -o out.s` and `grep -n` / `sed -n 'A,Bp'` the function.
  - Make code changes with the edit tool, one change at a time; `git diff <file>`
    shows what you changed. Don't restore a backup and re-apply patches through a
    script on every try.
  - Build `cc.c` with gcc while debugging. Debug output goes in `#ifdef CC_DEBUG`
    blocks using `fprintf(stderr, ...)`, built with `-DCC_DEBUG`; remove them before
    committing. Never leave stdout probes in loop passes: a matmul inner loop prints
    thousands of lines. Prefer gdb (`-O0 -g`) with narrow breakpoints.
  - After each change run one quick, targeted check (the triage table below says
    which); run the full suites only once it passes.
  - Keep findings in `docs/NOTES-<topic>.md` as you go: goal, current hypothesis,
    what is ruled out, next step. After a compaction, read it first and do not
    re-investigate what it marks settled. Fold what matters into code comments or
    the commit message before finishing; `/notes/` (gitignored) is there if you
    want a throwaway file instead.

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

`docs/architecture.md` holds the subsystem invariants (ABI, bootstrap rule, code model,
-O pipeline, kernel memory and tasks, BASIC). Read the section that matches what you are
about to change; do not load the whole file. The ones that break silently if missed:

  - A new or changed syscall must land in all four places: `sys.h`, the builtin strings
    in `cc.c` `main()`, the switch in `kernel/kernel.ino`, the switch in `host_linux.c`.
  - Toolchain sources (`compiler/cc.c`, `assembler/as.c`, `linker/ld.c`, `binutils/*.c`,
    `lib/t2obj.c`) must stay inside the subset the committed cc compiles (bootstrap);
    when in doubt, compile your change with the committed cc and `build/ref/cc`.
  - No absolute addresses anywhere: data symbols are offsets against `r9`, text symbols
    code offsets.
  - Without `-O` the output must stay byte-identical (`check_ref`).
  - Never touch PSRAM while flash is written, and never add a task switch point inside a
    file, flash or WiFi call.

There is no test runner; `make check` plus the triage table above is the workflow.
