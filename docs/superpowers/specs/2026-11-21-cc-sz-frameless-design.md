# cc -O: frame reservations without r7 (3b-iii)

Date: 2026-11-21. Status: approved design. Follows
`2026-11-21-cc-sp-relative-design.md` (branch cc-prologue), whose Results
left one gate up: `sprel()` bails when the function reserves a frame
(`sz > 0`), because `mov sp, r7` at the exit is the reservation's only
unwind and `frameless()` deletes it.

## Intent and why

Functions with locals - `crc`'s `main`, `csc`, `dollar`/`tokenize` in
`sh.c`, most of real userland - still pay `push {r7, lr}`, `mov r7, sp`,
`mov sp, r7` and one extra stack word per call. The sprel mapping
`[r7, #k] -> [sp, #k+sz+d-4]` already handles `sz` in its formula; this
round only offers it to reserved frames. It is the round that empties
the framed column of the corpus.

## Goals
- A function with `sz > 0` converts: entry `sub sp, #N` (or the
  `movw/movt/sub.w` long form, untouched), no r7 anywhere, and every
  exit drains the reservation with `add sp, #sz` before its pop.
- Behavior identical; non-`-O` output untouched (check_ref).

## Non-goals
- Tail-call arg windows at exits (still a bail; cheap to take next,
  precisely because exits can now drain).
- Float functions whose `vpush`/`vpop` move sp (keep bailing).
- The `csys` builtin argv window shape (keeps its frame, as pinned).
- Shrinking the long reservation form (`movw r1; movt r1; sub.w sp,sp,r1`
  stays; a peep to `sub sp, #N` is separate work).

## Anti-goals
- Any change to when a reservation exists (promote/flush decide that).
- A framed-but-converted hybrid: a converted function never reads r7.

## Approaches
- A. `frameless()` rewrites every `mov sp, r7` in place into
  `add sp, #sz` (codegen emits one per exit, inline early returns
  included, so every path drains); `trimframe()` copies the drain line
  along with the pop when it inlines an early return; the `sz` gate in
  `sprel()` is lifted. Chosen: it touches each pass once and keeps the
  exit paths structurally as today.
- B. drain with `mov r1, #sz; add sp, sp, r1` for a uniform arbitrary
  range: one more instruction everywhere to cover `sz > 4095`, which is
  vanishingly rare. Rejected; instead `sz > 4095` keeps the frame (the
  `add.w` immediate window; `big`'s 1000 fits).
- C. restructure the reservation away (never reserve for converted
  functions, push locals instead): changes promote/flush, much larger
  blast radius. Rejected.

## Design (as approved in conversation)
- `sprel()`: drop `if (sz) return 0;` from both the screen and the
  frameless call site. The walk is already sz-agnostic: d counts words
  below the reservation; the exit arm requires `na==ps==d==0`, so the
  drain lands on a clean window; the new `add sp, #sz` is the constant
  sp form the walk and `fl_frame` already excuse once converted.
- `frameless()`: `if (sz)` no longer refuses; instead of deleting
  `mov sp, r7` it rewrites each one to `add sp, #sz` (as/ld pick
  `add.w` past the 8-bit form). `mov r7, sp` still deletes; r7 leaves
  both register lists as today. `sz > 4095` returns 0 (frame stays).
- `trimframe()`: where it copies an epilogue pop into a branching
  early return, copy an immediately preceding `add sp, #N` drain too
  (the inline copy must drain or the callee returns with sp below the
  return slot).
- Screen acceptance scan: unchanged in structure; it runs with the same
  gates (the sz refusal moves into frameless' `sz > 4095` test, which
  the scan mirrors).

## What it touches
- `compiler/cc.c`: sprel (one line), frameless (the sz gate + the
  `mov sp, r7` rewrite), trimframe (carry the drain line).
- `tools/check_frame.py` / `tools/check_prolog.py`: `small`, `big`,
  `csc`, `clocals`/`rec`-style pins move to the converted shapes;
  `csys` and `cfloat` keep theirs. `tests/opt/frame.c` may gain a
  `sz > 4095` witness.
- Nothing else; kernel and ABI untouched (a frameless function's stack
  at the call is already what callers see).

## Gates
Same suite as the sprel rounds: check_ref `different: 0`, make check,
bench --pc 11/11, frame/prolog pins, check_basic, full-tree
`make OPT=-O` + check_sh --pc, and the board gates (selfhost + bench
--board) at merge, with the user's go-ahead.

## Left to the builder
- Exact helper or inline loop frameless uses to find and rewrite the
  `mov sp, r7` lines (it already scans exits for the pop rewrite).
- Whether the acceptance scan mirrors the `sz > 4095` test as its own
  line or lets frameless' answer drive it; must stay one source of
  truth.
- Pin wording; corpus census update in the sprel spec's Results table.
