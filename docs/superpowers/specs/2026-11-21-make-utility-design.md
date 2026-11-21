# make: a build executor for the board tree

Date: 2026-11-21. Status: short design, awaiting approval.

## Problem and why

The compiler source (`compiler/cc.c`, 4621 lines) needs to be split
into separate files so it stays editable on the board. Multi-file
builds already work (`basic.bin` is one `cc` call over seven `.c`
files; `cc -c` + `ld` work too), so what is missing is an
orchestrator: today `check_toolchain --selfhost` pushes one `.c` per
tool to the board and issues each `cc` command explicitly from the PC
over serial. The board tree has no build tool at all.

Owner ruling: make is for keeping split sources and selfhost tidy.
**Always rebuild; timestamps are explicitly out of scope** and can be
added later (the kernel's `stat()` already fills `mtime`, so the data
for it exists).

## How make works

`make.bin` is a userland tool (`userland/make.c` built into
`root/bin/make.bin` by the PC Makefile, like every other tool).

Usage: `make [-n] [-f file] [target ...]`. Default file `Makefile` in
the current directory; default target: the file's first target.

### The build file

A list of target lines, nothing else (name: `Makefile.board` in the
repo, version controlled, pushed to the board tree by the checkers):

    cc.bin: -O -I /lib /src/compiler/cc.c sys.h
    tools: cc.bin as.bin ld.bin ar.bin nm.bin objdump.bin
    all: tools basic.bin

- `target: prereq ...`. Leading prereq tokens starting with `-` are
  the target's build flags; `-I` consumes the token after it as its
  argument (a `-I` at end of line is an error). Everything else is a
  prerequisite: a file (source, header, `.o`) or another target.
- `#` starts a comment (to end of line). One target per line. No
  recipes, variables, wildcards, or pattern rules.
- Cycles: error out naming the cycle's target.

### The recipe (built-in knowledge, not parsed)

For a `.bin` target: one `cc` call with its flags, all `.c`
prerequisites and any `.o` prerequisites passed as inputs, `-o` the
target; each `.s` prerequisite is first assembled with `as` into a
same-named `.o`. Header/file prerequisites are not inputs: they exist
for the future timestamp pass and to tell whoever stages the SD card
which files a target needs. A target whose prerequisites are only
other targets (a collection, like `tools`) resolves each prerequisite
target first (recursively) and runs nothing itself.

This mirrors the PC Makefile's `CCBIN` recipe, so a byte-identity
fixed point (`make all` on the board vs `root/bin/*.bin`) stays
provable.

### Running commands

Foreground child run with `exec(path, argc, argv)`, the same call sh
uses for programs: synchronous, returns the child's exit code, and
the child's console output (cc diagnostics) reaches the user. A
nonzero exit stops make with a nonzero exit; missing prerequisites
report the target and the missing file.

### Dry run

`-n` prints each command make would run (one line per `cc`/`as`, in
order, after target resolution) and exits without spawning. This is
the PC-side test wedge: `host_linux` has no spawn (`SPAWN_UNSUPPORTED`),
so plan verification runs under qemu and the executing build is proven
on the board.

## Testing

- `tools/check_make.py --pc`: stage sources + `Makefile.board` under
  `root/`, run `make.bin -n` under qemu `host_linux`, compare the
  printed plan against the expected command list (and error cases:
  cycle, missing prerequisite, bad `-I`).
- `tools/check_make.py --board`: push make.bin, sources and
  `Makefile.board`, run `make all` (or a named list) on the board,
  fetch the outputs and require byte equality with `root/bin/*.bin`.
  This is the proof the executor really executes: same shape as the
  selfhost check, but driven by make itself.

## Approaches considered

- **A (picked): build file with target lines, recipes as built-in
  knowledge.** The board only ever runs cc/as/ld, so recipes are
  knowledge, not parsing; the build file stays readable at a glance
  over serial.
- B: a GNU Makefile subset with recipe lines (`$@`, `$<`, tab
  commands): can express the PC Makefile exactly, but drags
  shell-quoting and variable machinery into the cc subset for board
  builds that never need it.
- C: no build file, hardcoded tree conventions: smallest tool, but
  every new tool means editing and rebuilding make itself
  (bootstrap-awkward) and it silently diverges from the PC Makefile.

## Goals / non-goals

Goals: `make` builds the board toolchain from a build file; dry-run
plan tests under qemu; board byte identity; make.c stays inside the
committed cc's subset (bootstrap); `Makefile.board` version
controlled. Non-goals (this round): timestamps/incremental builds,
recipes, variables, parallelism, splitting `cc.c` itself (the next
round's job, with make as its tool), changing
`check_toolchain --selfhost` (it keeps its explicit commands until
the split lands).

## Constraints

- `userland/make.c` inside the language subset the committed cc
  compiles (it is built BY that cc as `root/bin/make.bin`, so this is
  a hard build-time fact, not a convention; like other userland tools
  it is not gcc-buildable, the kernel builtins are called by name).
- PC behavior unchanged: GNU make keeps driving PC builds; adding
  `make.bin` to the Makefile must not alter any existing output
  (`check_ref` byte identity still applies to the toolchain).
- Board runs follow the usual rules: one program on the serial port,
  board gates need the go-ahead.
- No absolute addresses; no new syscalls (exec/jobs/stat already
  exist), so the four-place syscall rule is not touched.

## Left to the builder

- Exact plan-line format for `-n` (plain `cc -O -I /lib -o ...`, good
  enough to diff).
- Board-side path style in `Makefile.board` (absolute `/src` `/lib`
  recommended; `host_linux` maps `/` into its `-r` root so the same
  file works in the qemu test).
- Target resolution bookkeeping (visited marks), argv limits, and
  error message wording.

## Settled at the builder check

- Which programs make runs: fixed `/bin/cc.bin` and `/bin/as.bin`
  (the board tool locations, same ones sh would find by name); no
  PATH search, no override flags.
- A target's name IS its output path verbatim (`/src/cc.bin: ...`),
  so collections must spell prerequisites exactly as the producing
  line spells its target; a prerequisite is a target iff it names a
  defined target, otherwise it is a file. This keeps matching to one
  string compare and matches how selfhost already names files.

## Results (2026-11-21)

Built as specified: `userland/make.c` (target-line grammar exactly as
above - flags are leading `-` tokens, `-I` takes the next token,
collections recurse, cycles error, always-rebuild, `-n` prints the plan,
`.s` prereqs assemble with `/bin/as.bin -o` first, target name IS the
output path). No deviations from the settled design. The argv cap
(MAXARGS 32) was not hit: the largest recipe is basic.bin with 12
entries. `make root/bin/make.bin` builds it with the committed cc
(bootstrap-clean); `check_ref` still reports `different: 0`.

Checker (`tools/check_make.py`):

- `--pc`: `ok: 13 make plan cases` (plan order, collections, recursion
  once, .s prereq, flags, child failure exit, missing file vs target,
  unknown target, -n, argv cap).
- `--board [--tools a,b]`: stages `Makefile.board` (sources `/src/...`,
  mirrored `/lib` headers; cc-included `.c` files staged but NOT
  prereqs - listing them would make cc compile them twice), runs make,
  byte-compares each tool against `root/bin`, and checks a child failure
  stops make with the child's code.
- Fresh-boot board run: cc 206823 B / 19 s, ld 3 s, nm 2 s, ar 3 s,
  objdump 6 s and sh all PASS byte-identical; the board-fail case
  PASSes. `assembler/as.c` and the multi-source `basic.bin` FAIL - the
  on-board cc cannot write its first `.s` to `/tmp` for those two
  invocations even straight from `sh`, a pre-existing board-side defect
  unrelated to make (see `docs/NOTES-board-cc-flaky.md`, filed in the
  HANDOVER backlog).
