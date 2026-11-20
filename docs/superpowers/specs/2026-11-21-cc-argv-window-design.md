# cc -O 3d: the builtin argv window converts, without r7

Date: 2026-11-21. Status: short design, awaiting approval. Follows
3c (`2026-11-21-cc-lea-design.md`, merged as `7f4cf88`).

## Problem

The builtin syscall trampoline's argv window (`mov r2, sp`, the r4
save, the `bic` align, `movs`, `blx r8`, the restore, `pop {r4}`, then
the `add sp, #4n` cleanup) is the last big frame-keeping class: 221
framed functions across userland at 3c - sh 33 (incl. `dollar`,
`run_cmd`, `main`), get/rcd/rls/send/wifi 17 each, rcp 26, mount 20,
edit 22, every bench `main`. It is why `csys` keeps its frame.

Recon (instrumented `sprel` on `prolog.c`): the depth walk already
models the window. `sp_syscall_save` (cc.c ~1352) matches the whole
sequence line for line and returns 1 from its FIRST line only; the
prescan skip (~1593) and the conversion-walk skip (~1723) both land
past the `pop {r4}` and treat the span as net zero on sp. The gap is
the acceptance scan and `frameless`' range scan: `fl_frame`'s window
excuse (~1910) is `sp_syscall_save(i)`, which answers 0 for every line
but the first, so the scan bails on `mov r4, sp` (opinfo's `oprd == 13`
arm flags its SOURCE operand sp even though nothing moves; `mov sp, r3`
/ `mov sp, r4` are flagged through the write mask, and `mov r3, sp`
bails on the next line after `mov r4, sp` did).

## Design

- `sprel` records the spans it skips: at the prescan window arm, after
  the skip locates the ending `pop {r4}`, store the closed span
  `[start, end]` in a small per-function table (globals `wstart[]`,
  `wend[]`, `nwin`; reset where `spconv = 0` is set at `sprel` entry;
  if the table is full the span is simply not recorded - the acceptance
  scan then bails on the window's `mov r4, sp` line and the function
  keeps its frame, which is the correct fallback).
- New helper `in_window(i)`: i lies in a recorded span.
- `fl_frame`'s window excuse becomes
  `sprel_ok() && (sp_syscall_save(i) || in_window(i))`. The excuse is
  then true exactly on the lines the walk skipped - span membership,
  not a second text match, so the two cannot diverge if the shape
  matcher evolves.
- Nothing else changes. The excuse only matters when `sprel_ok()`; the
  conversion walk never touches window lines (they contain no r7 and
  `sp_syscall_save`'s text still matches after the walk rewrites other
  lines); `trimframe`'s `nor7` loop passes them; `frameless`' range
  scan uses the same `fl_frame`. The `add sp, #4n` cleanup sits outside
  the span and is claimed by the drain arm's na accounting, as today.

## Why it is safe (the argument, as pinned by csys)

The window never reads r7, and its net effect on sp is zero (push/pop
{r4} balance, `mov sp, r3` + `mov sp, r4` cancel; the kernel takes
argv by pointer in r2 and never touches the user's r4). Slot reads
between the argv pushes and the call map at their line's own d, which
the prescan's fl_argpush arm already counts - same model 3c hardened,
now exercised through windows. The kernel task switch saves the user
sp only (see `csys`' pin comment): a converted window caller was
already safe in principle; only the scan's line-level bail kept frames.

## Approaches

- **A (picked): span table recorded by the prescan + membership
  excuse.** One helper, one arm, one table; the excuse cannot diverge
  from the walk because it IS the walk's span.
- **B: re-match from `fl_frame` (scan back for a window start whose
  span covers i).** No table, but a second matching path over the same
  text - exactly the kind of drift the prescan/scan split has produced
  twice in this series (leaform, and this). Rejected.
- **C: rewrite the window during conversion (drop the align dance).**
  Edits the one shape the whole trampoline ABI rests on, for a smaller
  win (bytes, not frames). Rejected.

## Goals / non-goals

Goals: `csys` and the argv class convert; userland framed count drops
from 221 by ~everything except the pinned classes; behavior pins
unchanged (prolog's `check 9 16 3 8 6 16 19` incl. `csys(b, 0)`
returning 0 through a converted window); census + specs updated.
Non-goals: float `vpush`/`vstr` slots (3e), `sz > 4095`.

## Constraints (series rules, unchanged)

Non-`-O` byte-identical (`check_ref different: 0`); bootstrap subset
in `cc.c`; PC gates before board; board gates (`--selfhost`,
`bench --board`, `check_basic --board`) need the usual blanket go-ahead
(same as 3b-iii/3c: pre-approved, merge NOT pre-approved); TDD red
first; no em dashes.

## Acceptance

- `csys` flips to converted (pin: fp-free, `add`-free exit pop,
  drains the argv words; its run value `read(0, b, 0) = 0` keeps the
  prolog sum).
- The mixed-chain witness keeps a FRAMED callee across the object
  boundary: `tests/opt/prolog_stubframed.s`, a hand-written framed
  function `sframe` (push {r7, lr}, `mov r7, sp`, reads `[r7, #12]`,
  returns a + 1), assembled with `build/host/as` (the .s is plain
  enough for the PC side; cc itself keeps compiling `prolog_stub.c`).
  `cmix` adds `sframe(0) - 1` so the printed numbers stay; the NATIVE
  copy in `prolog.c` gets the C equivalent.
- `cfloat`/`fpint`/`fmany`/`huge` stay framed by their pinned reasons.
- bench `main` functions convert (their `-O` sizes drop); sh/get/
  userland census recorded before/after; every survivor named with
  its reason.
- All PC gates green; bootstrap build via `build/ref/cc`; whole-tree
  `OPT=-O` fixed point; board gates as in 3c.

## Left to the builder

- Table size and where the reset lands (next to `spconv = 0`).
- Exact pin wording and the census/spec bullets, incl. the superseding
  note in the 3c spec's Results (its "all survivors argv-window"
  finding is what 3d retires).
