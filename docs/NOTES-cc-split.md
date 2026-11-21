# NOTES: splitting compiler/cc.c into files (round 2026-11-21) - DONE

Goal: pure code move of `compiler/cc.c` into `compiler/{cc.h,state,emit,opt,fp,
regs,licm,lex,types,expr,stmt}.c` — plan `docs/superpowers/plans/2026-11-21-cc-split.md`,
spec `docs/superpowers/specs/2026-11-21-cc-split-design.md`.

## Rulings made during execution (final review: merge Yes, no Critical/Important)

- `static char *shquote` (gcc-only runner helper) dropped from `cc.h` - T2CC
  has no `static` and it is not a cross-file interface.
- The `L_INS/L_LAB/L_BR/L_DEL` enum went to `cc.h`, not `emit.c` - shared by
  all -O passes (gcc undeclared-error caught it).
- The section matcher's `end="expressions"` matched the "constant expressions"
  banner first, so types.c needed a follow-up commit; harmless, gates re-ran.
- The plan missed updating the `check_toolchain.SELFHOST` cc ENTRY (Task 1
  only made the loop list-capable); it now globs `compiler/*.c`.
- `check_ref`'s count of prototypes varies 264/265 with whether `main` is
  counted; the file list itself is what matters.
- Deferred minors (final review): cc.h staging guard in check_toolchain is
  vestigial (could be unconditional); NOTES/prototype count wording; a
  keep-in-sync comment between `Makefile` CC_SRC, `Makefile.board`'s cc line
  and the check_make fixture (drift is caught, but at the expensive gate).

## What it looks like now

`cc.h` (455 lines: shared types/macros, 141 `extern` globals, 265 prototypes —
extracted mechanically from cc.c itself; no include guard, included exactly once
per file) plus ten logic files and a 312-line driver `cc.c` (cfile->asm, as/ld
runner, `main`). `make`'s `CC_SRC = $(wildcard compiler/*.c)` makes adding a
file build-invisible; `Makefile.board` and the two checkers list the sources
(`check_toolchain.SELFHOST` cc entry globs them, staged at `/src/<repo-rel>`).

## Facts worth keeping

- Subset pre-probes (before starting): duplicate prototypes are tolerated;
  `extern`-before-definition in the defining TU works (`int x;` after
  `extern int x;`, arrays included). Cost of a wrong probe: none — `make`
  catches every mismatch.
- Shared enums must land in `cc.h`, not with the section that first used
  them: the `L_INS/L_LAB/L_BR/L_DEL` line-kind enum is read by all -O passes
  (the gcc build caught this; T2CC would have died earlier and worse).
- `static` in a column-0 prototype line = a PC-only helper (`shquote`): drop
  from `cc.h`, the file keeps it.
- Costs: `cc.bin` 207403 -> 226256 bytes (per-TU data segments and string
  pools, cross-TU symbol refs through `movw/movt`); board builds of cc 19 ->
  26-36 s. Both accepted (byte identity released by the owner).
- `check_ref` lost its `compiler/cc.c` entry: ref cc `e4fb427` has no
  `extern`. The other 39 corpus files still gate every code path; the split
  itself is gated by `check_make --board` + `--selfhost` byte identity.

## Gates at completion

`make check` ok; `bench.py --pc` 11/11; `check_make --pc` 13/13; tests 4-12
vs gcc under qemu; `check_make --board` 8/8 byte-identical + fail case;
`--selfhost` IDENTICAL x6 (board cc rebuilds the split cc, 26.2 s) + tests
4-12 built on-board PASS.
