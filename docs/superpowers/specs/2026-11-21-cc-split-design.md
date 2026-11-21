# Design: splitting `compiler/cc.c` into files

Date: 2026-11-21. Status: approved approaches — true multi-TU split,
byte identity of the `cc.c` compile released by the owner ("good in the
beginning, not required at this point").

## Goal

`compiler/cc.c` (4,641 lines) becomes a set of functional logic files
that each compile on their own and link like every other multi-file
binary in the tree (the model is `basic/`: one header of shared types,
`extern` globals and prototypes; no `static` exists in the subset and
none is needed - names are already unique within the file today).

The split is a pure code move: no behavior, no codegen change. Each
moved file must stay inside the committed cc's subset (bootstrap), and
the board proves the result: the board cc rebuilds the split cc
byte-identically (`--selfhost`), which is the strongest gate this repo
has.

## Files (cc.c's section map becomes the module map)

| file | from cc.c | ~lines | holds |
|---|---|---|---|
| `compiler/cc.h` | new | ~400 | system includes (T2CC skips angle includes), `#include "sys.h"`, token enum, `struct Sym`/type decls, macros, `extern` for every global, prototypes for every cross-file function |
| `state.c` | state + helpers | 150 | global definitions (the file that `#define`s nothing - the definitions), `readfile`, small helpers |
| `emit.c` | assembly output | 275 | `lnew`/`ls`/`lend`/`oflush`, the `.Linit` buffer, short-data/ref emission |
| `opt.c` | peephole (-O) | 430 | `peep` and its rewrite passes |
| `fp.c` | floats in s regs (-O) | 280 | the s-register allocator |
| `regs.c` | locals in registers (-O) | 1130 | promote/sprel/frameless/sh - the -O core |
| `licm.c` | licm (-O) | 545 | loop-invariant code motion |
| `lex.c` | lexer + float literals | 590 | `next`, preprocessor (`#include`, macros, `#if`), `num_scan` |
| `types.c` | types + constexpr | 245 | declarators, `sizeof`, constant folding |
| `expr.c` | expressions | 300 | `expr`, assignment, calls, lvalue |
| `stmt.c` | initializers + statements + toplevel | 405 | block/statement parsers, `parse_global`, globals & `.t2init` |
| `cc.c` | file->asm + as/ld runner + driver | 290 | `cfile`/`write_data`, `run_tool` (+`#include <sys/wait.h>` stays here), `main` |

Names follow the existing lowercase style. `regs.c` at ~1130 is the
biggest file; splitting the register allocator further would cut across
one algorithm, so it stays.

## Build and checker changes

- `Makefile`: a `CC_SRC` list; `compiler/cc` (gcc host build) and
  `root/bin/cc.bin` compile it, with `-I compiler` so `cc.h` resolves
  (sys.h resolves from the repo root cwd, as basic's does).
- `Makefile.board`: `/src/cc.bin:` lists all sources, `-I /lib`,
  `/lib/cc.h` as a header prereq (staged like `/lib/basic.h`).
- `tools/check_toolchain.py`: `SELFHOST` entries carry source lists;
  the cc entry stages every `compiler/cc*.{c,h}` (`cc.h` into `/lib`);
  the board compiles `cc -I /lib -o /src/cc2.bin /src/compiler/*.c` and
  byte-compares as before.
- `tools/check_make.py`: `REPO_OF` gains `'/lib/cc.h': 'compiler/cc.h'`.
- `tools/check_ref.py`: drop the `compiler/cc.c` entry from the corpus
  (the ref cc at `e4fb427` predates `extern` and cannot parse the split)
  and say so in the docstring. The other ~60 corpus files still gate
  byte identity of every code path; the split cc itself is gated by
  behaviour (tests, bench) and on-board byte identity (check_make,
  selfhost). `build/ref` stays pinned where it is.

## Gates (AGENTS.md triage: "anything in cc")

PC: `make`; `make check`; `check_ref.py` (different: 0 on the reduced
corpus); `bench.py --pc` 11/11; tests 4-12 vs gcc under qemu.
Board (standing permission this round): `check_make.py --board` - all 8
tools byte-identical, cc now built from its files - and
`check_toolchain.py --selfhost`: the board cc rebuilds the split cc and
all other tools byte-identically, then builds tests 4-12.

## Working order (risk control)

1. Checker/Makefile prep with cc.c still single (all gates still pass).
2. Move sections out bottom-up - each step: create the file + extend
   `cc.h`, `make` (the whole tree rebuilds with the changed cc, so every
   step is integration-tested), fix fallout, one commit per moved file.
3. Drop the check_ref entry once cc.c is multi-TU; run the PC suite.
4. Board: check_make --board, then --selfhost.

A `docs/NOTES-cc-split.md` ledger tracks what moved and what is ruled
out, per the context-window rules.
