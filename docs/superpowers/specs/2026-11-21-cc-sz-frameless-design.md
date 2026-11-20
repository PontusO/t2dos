# cc -O: frame reservations without r7 (3b-iii)

Date: 2026-11-21. Status: implemented on branch cc-prologue (commits
6954959, e6774ee); board gates below. Follows
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
- `trimframe()`: needs NO change. Its `sz > 0` branch rewrites the entry
  trio to `sub sp, #sz` and returns before the pop-copying code, so a
  converted reserved-frame function never gets an inline copied pop -
  every early return branches to the exit label, which now drains. (The
  drain-carry drafted above is therefore moot; the copy loop must NOT
  gain one.)
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

## Results (implementation, 2026-11-21)

Built as designed, and the round turned out to be about the two latent
bugs it exposed rather than about the gates.

- `frameless()` rewrites each `mov sp, r7` into `add sp, #sz` with `sz`
  the reservation size, and deletes it when `sz == 0`. `sprel()`'s gate
  became `sz > 4095`, and the acceptance scan drives off `frameless`'
  own answer: the screen tests `sz && !sprel_ok()` and `sz > 4095`
  itself (one source of truth).
- `fl_list()` copied a register name ONE digit at a time, so a promoted
  `r10` or `r11` came out as `r1`. `as` merged the duplicate and the
  save list silently lost those words. Harmless until now because the
  functions that converted saved at most three core registers; matmul's
  `mul()` promotes five and it printed `check 0`. The rewrite copies the
  whole token.
- The screen rejected `k < -8` on the belief that a real slot offset is
  never below -8. It was: `small`'s `char buf[16]` sits at `[r7, #-20]`.
  The test is now `k == -1`, which is `split()`'s answer for exactly the
  forms sprel cannot move (`subw`/`addw`/`vstr` and the bare-register
  r7 uses); a real slot below r7 maps through `k + sz + d - 4` like any
  other and the existing 1020-word ldr/str window bounds it. `trimframe`
  needed no change, as predicted.
- `frameless()` no longer zeroes `npro`. `flush()` runs one final
  `peep()`, and `peep_hsrc`/`peep_tohi` gate on `npro`: zeroing it
  disabled the promoted-register merges for exactly the functions that
  had just converted, which is why `csc`'s hold load came out as
  `ldr r0, [sp, #16]` + `mov r4, r0` instead of one load. `trimframe`'s
  `nor7` is the only reader left and it only decides deletion of the two
  `mov` lines frameless has already deleted; the next function resets
  `npro`.
- What still keeps a frame, all pinned: array-addressed locals (`small`,
  `big`, `amem`, `bigprom`, `huge` - their `&a` is a `subw rD, r7, #N`),
  reservations over 4095 bytes (`huge`), float slots in memory (`fpm`,
  `fpint`, `fmany`, `fearly`, `cfloat`), the builtin argv window
  (`csys`, `cscreen`), and `rec`'s `str r0, [r7, #-N]` result save,
  whose pending word the walk cannot see claimed.
- `stublea` gained a float local: under the lifted gate a locals-only
  body now converts, so the mixed-chain witness's framed half is held by
  the `vstr`/`vldr` slot instead.

Superseded by 3c (`2026-11-21-cc-lea-design.md`): the lea forms and the
negative-offset own-frame slot reads this list blamed for `small`, `big`,
`bigprom`, `amem` and `rec` convert since 3c (and `stublea` with them,
the mixed chain's framed half moving to `csys`); what still keeps a
frame is the argv window, `vpush`-bracketed float state (`cfloat`,
`fpint`, `fmany`), `sz > 4095` and un-windowed shapes.

Corpus census, functions still keeping `mov r7, sp` under `-O`
(this round: before -> after): crc 2->1, matmul 2->1, sieve 2->1,
strings 4->2, sh.c 43->41; the rest unchanged, and their remaining
frames are the shapes listed above. Bench total -O size 6776 -> 6740
bytes; `frame.c` pins the drain with its run (`1 18 7 5 3 11 14 0 94 29
18 7`).

Gates: check_ref `different: 0`; `make check` green; bench --pc 11/11
equal to gcc; frame and prolog pins ok; check_basic 139; check_sh 5/5;
full tree `make OPT=-O` then `make` both pass check_sh with their own
`host_linux` (the two `root/bin` sets differ by the `-O` flag, as they
should). Board: `check_toolchain --selfhost` rebuilt all six tools
byte-identically and test4..12 passed; `bench --board` 11/11 equal to
gcc on hardware, total 7227 -> 3534 ms (2.04x); `check_basic --board`
139 cases. Moved on hardware since the 3b-ii run: strings 226 -> 201
ms, fib 240 -> 229, matmul 232 -> 220 — matmul's `mul()` is the
five-promotion function whose save-list rebuild the `fl_list` fix
rescues, and its `check 4294967213` is correct on the board.
