# cc -O: sp-relative frame slots (fp-free functions with locals)

Date: 2026-11-21. Status: implemented on branch cc-prologue, board
gates green (selfhost byte-identical, bench and basic --board pass).
Follows
`2026-11-20-cc-prologue-opt-design.md` (branch cc-prologue); this spec removes
the restriction its Rulings left in place: "any `[r7, #off]` read keeps the
frame".

## Intent and why

After the prologue pass, `frameless()` strips the frame pointer only from
functions that never read r7 - so any function with a parameter or a local
still pays `push {r7, lr}` + `mov r7, sp` + `pop {r7, pc}` and one extra stack
word per call. Witness (current -O output):

    framed:                              // int framed(int n) { return n + 1; }
        push    {r7, lr}
        mov     r7, sp
        ldr     r0, [r7, #8]
        adds    r0, #1
        pop     {r7, pc}

The frame pointer is pure overhead for such functions: every slot address is a
constant offset from the stack pointer once the reservation is in. Making slots
sp-relative lets these functions become fully fp-free like `leaf()` already is:
`push {lr}` ... `pop {pc}`, two words less saved, one instruction less per use
(the `mov r7, sp` bookkeeping disappears).

## Goals

- A function whose frame uses are all plain slot accesses and whose body never
  moves sp becomes fp-free: entry `push {..., lr}`, exit `pop {..., pc}`, no
  `mov r7, sp`, slots addressed `[sp, #N]` (level A1).
- That includes functions that spill across calls and have more than one exit
  (fib's shape): sprel converts the slot reads, frameless() is relaxed to let
  such a function through (see Design), and fib's per-call entry/exit loses one
  save word and the `mov r7, sp` / `mov sp, r7` bookkeeping. Its promoted `n`
  stays in r4 with its hold load converted to `[sp, #N]`.
- dispatch/interp callees with parameters benefit too.
- No ABI change: callers still pass args on the stack at `[sp, #sz..]` before
  the `bl`; a callee's slot addressing is its own business. Hand-written `.s`
  functions are never rewritten and stay correct (see Constraints).
- Without `-O`, output stays byte-identical (`check_ref.py`: different: 0) -
  free: the pass order line already sits inside `if (optim)`.

## Non-goals

- Register allocation across calls (the fib `n` reload into r4 - that stays;
  only the frame bookkeeping around it goes).
- Tail calls (rejected before).
- Level **A2** (slot reads below spill pushes, depth-tracked rewrites): parked.
  A builder check of this spec found nothing to win with it: promote() hoists a
  promoted value's slot load into the hold area above every body push (fib's
  `ldr r4, [r7, #12]` sits at reservation depth), and un-promoted slot reads
  only happen when calllive proves no value crosses a call - so a read below a
  spill push is vanishingly rare. A1 plus the frameless relaxation below gets
  fib's shape. If a future corpus shows real functions reading slots inside
  spill windows, schedule A2 then.

## Constraints

- Conversion is only exact at a line's own stack depth: `[sp, #off+sz+d]` ==
  `[r7, #off]` where `d` is how far sp sits below the reservation at that line.
  cc moves sp in exactly two ways inside a body: value spills (`push {rX}` /
  `pop {rX}` / `add sp, #N`, emitted by push()/pop()/addsp()) and struct-arg
  space (a multi-register `push {...}`). Both are trackable with a running
  depth counter, so the pass allows them instead of bailing (see Design); it
  bails on any sp move it cannot attribute (`opmask & 8192 || oprd == 13`
  outside the tracked forms - a linked `.s`'s `add sp, r7, #4` - and on
  vpush/vpop). Read opinfo's globals immediately after its own call - they do
  not survive helper calls.
- Multiple exits (fib's early return) are handled, which means frameless()'s
  "more than one exit keeps the frame" bail must be relaxed for converted
  functions: see Design step 3. The relaxation is safe only because every
  `mov sp, r7` in the function was deleted while depth tracking proved each
  exit sits at reservation depth.
- A hand-written `.s` function reading `[r7, #off]` keeps its frame pointer and
  saves r7; it is called with the same stack layout callers always produced.
  Mixed chains stay correct because each function's frame base is private: the
  caller pops its own args, the callee indexes only its own frame, and an fp-free
  caller's entry saves lr (+ its builtins' r4) exactly as a framed one did -
  the only word that leaves the save list is r7, which no callee reads. The
  witness test goes in `tests/opt/prolog.c`/`prolog_stub.c`, which already link
  two objects (check_frame.py has no `.s` in its pipeline; extending it is not
  worth it).
- Bootstrap: new code stays inside the subset the committed cc compiles.
- The pass runs after `promote()` (which knows r7 offsets via r7ref/slotix) and
  before `frameless()`, which then sees no r7 uses left except bookkeeping.

## Approaches

A. Whole-function sp-relative (chosen). One new pass converts every r7 slot use
   in the function to `[sp, #(off+sz+d)]` at its own tracked depth d, then a
   relaxed `frameless()` strips the pointer. All-or-nothing bail keeps
   reasoning local and matches the existing passes' shape.
B. Split conversion (locals to sp, params stay on r7). Fires more often but
   still pays `push {r7, lr}` + `mov r7, sp` - half the win, and promote()/
   frameless() must then reason about two bases at once. Rejected: the extra
   interaction buys nothing when A already strips the pointer.
C. Per-call-site fp-free protocol (args through registers). Touches every call
   site and the de-facto contract with hand-written `.s`. Rejected: invasive,
   ABI risk, no measurable gain over A for the benchmarks at hand.

## Design

New pass `sprel(int fa, int sz)` between `promote()` and `frameless()`, all-or-
nothing in one buffer walk; offsets never leave the function, so a textual
rewrite inside one flush() buffer is sound. It marks success (a file-scope
flag sprel resets per function) for frameless()'s relaxed gate.

1. **Screen.** Walk `nx()` from `fa - 3` to the last live line. Bail (leave the
   function exactly as today) unless every line is one of:
   - the entry push at fa-3, the trio at fa..fa+2, `mov r7, sp` / `mov sp, r7`,
     or a pop of pc (fl_excused / fl_pc_end);
   - an r7 slot read in a convertible form: `r7ref(i) == 1` with a memory
     operand, `[reg, #off]` inside ldr/str (rfplain or another base register's
     slot load). Any other r7 use bails the whole function: r7ref kind 2
     (`subs/adds rX, r7, r1`, unknown forms) and kind 1 lea arithmetic
     (`subw/addw rX, r7, #N`) - an address-taken local needs `base + reg`
     addressing, which Thumb cannot express off sp once the frame pointer is
     gone. (Measured: this keeps `small`/`big`, whose locals are arrays, framed;
     the win set is parameter-and-scalar-local functions, which is what the
     bench corpus actually contains - no lea or lea-pair line in any of the 11
     benchmarks.)
   - an sp-tracking line: a single-register `push {rX}` (d += 4), single-
     register `pop {rX}` (d -= 4, bail if d goes negative), or `add sp, #N`
     after a call (d -= N, bail unless N <= d). These are exactly the spill
     windows; no slot line may sit at d != 0 with an offset that overflows the
     encodable range - bail rather than clamp.
   - any other line that does not move sp (`opmask & 8192 || oprd == 13` after
     its own opinfo) and is not vpush/vpop or opkind 4.
   Also bail when the last live line before each exit pop sits at d != 0 (the
   exit must return to reservation depth) and on any line in `[nlines tail]`
   past the final exit that reads r7 or sp (fl_outside's rule).
2. **Rewrite.** Second walk: every convertible r7 line becomes `sp` base with
   offset `rfoff + sz + d_at_that_line` (params land exactly where callers left
   them relative to sp after the reservation; locals become >= 0). Hold-area
   loads promote parked at phold convert too - they read parameters and sit at
   d == 0. Then delete every `mov sp, r7`: with the frame pointer gone it would
   set sp from a stale caller fp. Keep `mov r7, sp` for now (step 3 deletes it).
   Do not touch pop-of-pc lines: that is frameless()'s fl_list's job.
3. **Relax frameless().** With the flag set by sprel, frameless() skips its
   multiple-exit bail (earlier pops of pc are exits at reservation depth, and
   their `mov sp, r7` predecessors are gone) but keeps every other check: they
   re-verify what sprel assumed and catch each other's blind spots. Without the
   flag nothing changes, so all existing pins keep firing exactly as today.
   frameless() also deletes `mov r7, sp` in the converted case (its existing
   loop already does this once no line reads r7).

`slotix`-keyed metadata (slotw/slotf/slotk) is promote()-internal and read
before this pass - not consulted after.

## What it touches

- `compiler/cc.c`: new `sprel()` near frameless(); pass order line becomes
  `promote(); sprel(frame_at, sz); frameless(frame_at, sz); trimframe(...)`.
- `tools/check_frame.py` pins: `framed()` drops `push {r7, lr}` for `push {lr}`,
  gains `[sp, #N]` parameter reads; `early` (multi-exit, spills) goes fp-free;
  `small()`/`big()` keep their frames - their locals are arrays (lea lines),
  which bail by design; `plarge` converts (plain parameter loads, no promotion)
  and its pin updates to `[sp, #N]`; `bigprom` stays framed until the
  relaxation in step 3 proves it fires there too.
- `tests/opt/prolog.c` + `prolog_stub.c`: the mixed-chain witness - an `-O`
  fp-free C caller of a stub callee that reads its parameters.
- `tests/opt/frame.c`: expected-output string updates only if behavior changes
  (it must not).

## Left to the builder

- Whether the depth walk treats multi-register pushes as struct space (d +=
  4*n) or bails on them: start by bailing, let the corpus say.
- Whether `sub sp, #N` emitted by trimframe AFTER sprel changes depths: it
  does not - trimframe replaces the trio with an equal-size reservation, so
  slot offsets keep their meaning; pin this in a comment and a test.
- Marking: cite this spec from sprel()'s header, as frameless cites its own.

## Results (implementation, 2026-11-20)

Implemented as designed, with two model refinements that came out of the
depth-walk work (they supersede the per-line band model drafted above):

- Uniform mapping: frameless drops exactly ONE entry word (r7's), so
  EVERY converted literal - caller's band and own frame alike, at any
  spshift - maps `[r7, #k]` -> `[sp, #k+sz+d-4]` at the line's live
  depth d. No band/local aliasing case exists; the prescan and the
  nparams band bail were removed.
- `sz > 0` bails (the reservation's only unwind is `mov sp, r7`, which
  frameless deletes). Superseded the next round: reserved frames convert
  since 3b-iii (`2026-11-21-cc-sz-frameless-design.md`), the exits
  draining with `add sp, #sz`.

The argpush arm of the depth walk counts at the PUSH line (`spd()` words,
not the setup line): setup forms are unbounded (add.w's lea is not a
recognized setup), the push is where the word lands. This gap was a real
miscompile (userland/sh.c wdollar) caught by check_sh under `make OPT=-O`.

Corpus (functions keeping `mov r7, sp` under -O, before -> after):
fib 2->1, interp 3->2, strings 6->4, others unchanged; converted sp slot
reads: fib 1, interp 1, strings 3. On userland/sh.c the conversion
fires on 10 functions (has_prefix, closer, wdollar, wch, xput, lower,
isname, is_assign, skipws, opener) that HEAD kept framed.

bench.py --pc (qemu, single runs; timings carry ~10-30% noise): total
plain 264ms -> 263ms, -O 154ms both (1.71x both); fib line 26/23ms
1.13x -> 27/21ms 1.29x. Sizes are within 0.3% (a converted function
saves the 2-byte mov r7,sp, the r7 word in each pop and one live word
per call; alignment absorbs the rest). The gain this round is stack
words and prologue instructions, visible in the census above, not wall
time under qemu.

Gates at implementation: check_ref different: 0; make check green;
bench 11/11 equal to gcc; check_frame/check_prolog ok (pins updated to
the converted shapes, mixed-chain witness cmix->stublea added);
check_basic 139 + asan clean; full tree `make OPT=-O` + check_sh 5/5.

Board (merge gates, run 2026-11-20): `check_toolchain.py --selfhost`
IDENTICAL - the board's cc rebuilds cc/as/ld/ar/nm/objdump byte-for-byte
equal to the PC builds and its own cc2.bin builds test4..12 correctly;
`bench.py --board` 11/11 output equal to gcc on hardware, real-hardware
speedups: total 7281ms -> 3564ms (2.04x), fib 447 -> 240ms (1.86x),
mandel 3.05x, loop 2.60x; `check_basic.py --board` 139 cases (the
converted interpreter binary on hardware). `check_sh.py` has no --board
mode (the plan assumed one); shell coverage stays the PC suite. The
first check_basic --board run hit a latent board.py bug: the put of the
0-byte immediate*.bas cases raced (receiving and ok arrive in one
flush) - fixed, see the board.py commit.
