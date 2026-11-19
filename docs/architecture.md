# T2DOS architecture

Deep reference for how T2DOS works. AGENTS.md holds the build/test workflow and safety rules; this file holds the invariants you must not break when editing a subsystem. Read the section that matches what you are about to change, not the whole file.

## ABI and calling convention

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

## Toolchain

- **Toolchain pipeline:** `cc` writes GNU unified-syntax Thumb-2 assembly; `as` turns
  it into a T2DOS object (`"T2OB"`, see `lib/t2obj.h`), `ld` links objects and `ar`
  archives (`"T2AR"`, only needed members are linked) into the `.bin`. `cc` is also the
  driver (`-S`, `-c`, `-l`/`-L`, mixed inputs; temporaries in `/tmp`): on the board it
  `exec`s `/bin/as.bin` and `/bin/ld.bin`, on the PC it runs `build/host/as`/`ld` (or
  `$T2_TOOLS`; cc resolves that path relative to the current directory, so a stale
  `/tmp/build/host` can shadow it). Encodings live only in `as`; its accepted forms
  must match GNU as byte-for-byte and reject what GNU rejects (`check_asm.py --diff`);
  `objdump`'s text must equal GNU objdump's.
## Compiler and code model

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
## Kernel

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
## Other subsystems

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
