# NOTES: splitting compiler/cc.c into files (round 2026-11-21)

Goal: pure code move of `compiler/cc.c` into `compiler/{cc.h,state,emit,opt,fp,
regs,licm,lex,types,expr,stmt}.c` — plan `docs/superpowers/plans/2026-11-21-cc-split.md`,
spec `docs/superpowers/specs/2026-11-21-cc-split-design.md`. SDD ledger:
`.superpowers/sdd/2026-11-21-cc-split/progress.md` (rulings live there).

## Status

- Task 1 done: Makefile `CC_SRC = $(wildcard compiler/*.c)` (new .c files need no
  Makefile edit), `compiler/cc` gcc rule and `cc.bin` rule take `$(CC_SRC)` with
  `-I compiler`; `check_toolchain.SELFHOST` entries carry source lists, staged at
  `/src/<repo-rel path>` (matches check_make's `repo_path()`), `compiler/cc.h`
  auto-stages to `/lib/cc.h` once it exists. Rebuild byte-identical.

## Gates per move (the tests of this round)

`make` (link/duplicate-symbol oracle), `check_ref.py` different: 0 (codegen),
PC selfhost proxy (qemu: root/bin/cc.bin rebuilds the split byte-identically —
subset + codegen), board at Task 12 (`check_make --board`, `check_toolchain --selfhost`).
