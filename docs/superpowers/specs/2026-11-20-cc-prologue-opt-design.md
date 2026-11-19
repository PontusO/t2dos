# cc -O: call-aware prologue and epilogue optimization

Date: 2026-11-20. Status: implemented on branch cc-prologue (see Rulings below
and `.superpowers/sdd/2026-11-20-cc-prologue-opt/progress.md` for the task log).
Selfhost note: the board's plain cc reproduces the PC's PLAIN build byte for byte
(182157, md5 a5e626..); `check_toolchain.py --selfhost` against an `OPT=-O` root/bin
differs by design - run it with plain builds, or add `-O` to its cc command.

## Intent and why

`cc -O` is within 1.0-2.2x of gcc -O2 on loop-shaped benchmarks but 4.4x behind on
fib, which is deliberately a recursive-call benchmark. The remaining gap there is not
arithmetic: it is the per-call fixed cost. Per call, fib executes `push {r0} / bl /
add sp,#4` twice (the ABI, untouchable), plus three costs that are ours:

1. r4 is saved in the prologue purely because promote() parked `n` there, even though
   `bl` clobbers it - so both call sites reload r4 from the frame afterwards.
2. Frame-pointer bookkeeping (`mov r7, sp` at entry, `mov sp, r7` at exit) that fib
   itself never uses: no frame slots, no promoted references after the fix in 1.
3. A push/pop spill pair to hold the first result across the second call (partly a
   consequence of 1: with n in a frame slot and r0 free there is one fewer register
   for the partial sum; not targeted directly).

gcc wins by whole-function register allocation, which the single-pass buffer design
cannot do. This step takes the two pieces that need no allocation framework: a pass
that drops callee-saved registers no call can survive (csave), and one that removes
the frame-pointer dance from functions that provably never read r7 or sp between
entry and exit (frameless). Together they cut fib's fixed cost per call roughly in
half without touching the calling convention.

## Goals

- fib: measurable qemu and board speedup with -O; it is the headline metric.
- Every non-recursive function stops paying for `mov r7, sp` / `mov sp, r7` when it
  never reads r7 or moves sp between entry and exit (leaf elision).
- No ABI change: functions still receive args on the stack and return in r0; callers,
  libc-side builtins and hand-written .s files are unaffected.
- Without -O, output stays byte-identical (check_ref).

## Non-goals

- Register allocation across basic blocks (the redesign, explicitly postponed).
- Tail calls. Only one of fib's two calls could use it anyway.
- Cross-function or whole-program analysis. Everything below is per function buffer.
- Removing the push/pop spill pair around a second call (needs allocation).

## Anti-goals

- A miscompile that survives because every bench happens not to hit the case: the
  guards below are conservative by design, and check_frame pins the shapes.
- Slower code anywhere: csave trades a register for a frame load; it must only fire
  when the register is dead across every call anyway (then the trade is free).

## Constraints

- Bootstrap subset in cc.c (no new language features before their own change).
- Passes run on the function's line buffer after promote() and trimframe(), inside
  the existing `if (optim)` block; peep does not rerun afterwards.
- `dead()`'s call model is the source of truth for "survives a bl": at opkind == 1
  (bl/blx) only r0-r3 and r12 survive. syscalls (`blx r8`) are handled by dead() as
  reads of r0-r2, not clobbers - consistent with what promote() already assumes.
- check_frame's existing pins must keep passing: they pin the vpush/vpop exit shape,
  promoted parameter loads, bigprom's long frame and amem's un-promoted array pointer.

## Approaches considered

1. **csave + frameless as two small post-promote passes** (chosen). Reuses dead()
   and the existing reglist rewriting; each is under ~60 lines; independently
   testable; failures are local and shape-visible in check_frame.
2. Linear-scan-ish register allocation over the line buffer. The real fix for fib's
   spill pair, but it needs a CFG, live ranges and a rewrite of how promote() hands
   out registers: weeks, high miscompile risk. Postponed by agreement.
3. Tail-call optimization alone. Fires on one call site in one bench; nasty failure
   modes (shared frame, parameter shuffling). Rejected.

## Design

Pass order became: `prepro=1; peep(); licm_loops(); prepro=0; peep(); promote();
frameless(); trimframe()`. Both new passes are -O only. The implementation folded
csave into promote() as calllive(): a slot is only worth a register when its value
survives no call, checked with the existing dead() liveness instead of a second pass
that would have to undo promote's rewrites. frameless() runs after promote() and
before trimframe(), so it sees the exit pop before trimframe copies it to the early
returns (multi-exit functions keep their frame).

Rulings that changed the design while building:

  - ANY `[r7, #off]` read keeps the frame. A frameless entry saves one word less
    (`push {lr}` than `push {r7, lr}`), so once any function in a call chain drops
    r7, the caller's frame pointer and the callee no longer see the same words -
    an incoming parameter read names the wrong word. Witness: leaf + early return
    crashed under qemu (tests/opt/frame.c). The spec's "frameless with promoted
    slots" case therefore never fires; promote() parking a value in r4-r11 or an
    s register, or a vpush/vpop pair, also keeps the frame.
  - trimframe's copied pop of pc in the body (multi-exit) blocks frameless: the
    copy happens after frameless scans.

### csave(): drop callee-saved registers that never survive a call

Runs after promote(). For each core register r promote assigned (slotw <= -2, via
regnm: r4 r5 r6 r10 r11):

- Scan the buffer; for every line that *reads or writes* that register (opinfo's
  opmask/oprd), ask `dead(regno, line + 1, 0)`. That is exactly "the value here is
  never read again on any path", which covers "is dead at every following call"
  because dead() returns false at a bl for registers past r3.
- If *every* use is dead immediately after it (or is overwritten before any read),
  the register earns nothing from being saved: un-promote its slot (slotw = -1, so
  references fall back to their frame slots through the existing r7shift path) and
  remove it from the prologue push and epilogue pop lists.

The simple form (what to implement): a register is dropped when *no use of it is
followed by another use that crosses a call*. Concretely, walk uses in order; track
whether any two consecutive uses have a `bl` between them with the value still live
(dead() false at the first). If no such pair exists, drop.

Float slots (s16-s31) are not touched: vpush/vpop cost one instruction each for up to
16 registers, and float-heavy benches are not where the gap is.

Rationale: after csave, fib keeps `n` in its frame slot; the `ldr r4, [r7, #12]` per
call disappears along with the push {r4}; the epilogue pop shrinks by one register.
Where a register genuinely lives across calls (qsort_int's recursion), every use is
live across that call and csave changes nothing - that is the safety property.

### frameless(): elide r7 bookkeeping when r7 and sp are untouched between entry and exit

Runs after csave(). A function is frameless when, scanning lines strictly between the
prologue push and the exit pop:

- no line reads or writes r7 (r7ref() == 0 for all; the existing prologue/epilogue
  exemptions in r7ref are not in the scan range), and
- no line writes sp other than `push`/`pop` of r0-r3/r12 values and the exit pop
  (i.e. no `sub sp`, no `add sp, #N` beyond call cleanup... call cleanup is `add sp`
  which does move sp: allow it only when balanced by a push at the same nesting - in
  practice: allow `add sp, #N` lines that peep's call pattern produced; they restore
  what the matching pushes took), and
- no vpush/vpop (they move sp too; if promote() used s registers, keep r7), and
- not a function whose exit relies on `mov sp, r7` to unwind a variable frame - with
  sz fixed at compile time and every path ending at the shared exit or trimframe's
  copied pop, this holds whenever the two rules above hold.

When frameless: delete `mov r7, sp`, delete `mov sp, r7` before the vpop/exit, and
rewrite the exit `pop {.., r7, pc}` to drop r7 (and the prologue push likewise). If
trimframe already removed them (nor7 path), this pass is a no-op; it fires exactly on
the case trimframe skips: functions that promote registers (npro > 0 blocks trimframe's
nor7) but whose promoted uses are all dead across calls - which is precisely what
csave just created.

The two passes compose: csave first shrinks the save list, then frameless can see a
function with no r7 readers left.

### What each bench should do (expected shapes, for check_frame extensions)

- fib: prologue `push {r7, lr}` -> after both passes `push {lr}`... note push {lr}
  alone is fine; exit `pop {pc}`... Thumb allows pop {pc}; keep `push {lr}` /
  `pop {pc}` as the minimal frameless leaf-with-calls shape. If as/ld encode these
  (STMDB/POP do, they are standard), no assembler change is needed - verify with
  check_asm --diff before board runs.
- rnd (leaf, calls nothing... it does not call): `push {r7, lr}` / ... / `pop {r7, pc}`
  becomes `push {lr}` / `pop {pc}`, mov r7, sp and mov sp, r7 gone.
- qsort_int: unchanged (all five registers live across the recursive bl).
- main in fib.c: keeps its frame (locals), may lose nothing; that is fine.

### Verification

1. check_ref: different: 0 (no -O path touched).
2. check_frame: existing pins pass; extend with pinned shapes for fib-style csave +
   frameless (push {lr} / pop {pc}, no r7 lines), qsort-style recursion unchanged,
   and a function where one of two promoted registers is call-dead and the other is
   not (only the first drops).
3. compiler/tests/*.c under qemu plain vs -O; BASIC cases; bench.py --pc: all check
   lines equal gcc native; expect fib ~2x better, nothing slower than -1%.
4. make OPT=-O self-rebuild identity on PC; then (with go-ahead) board: bench.py
   --board and check_toolchain.py --selfhost.

## Left to the builder

- Exact line-scan helpers for "line reads or writes reg" (opinfo already gives
  opmask/oprd; reuse rather than new parsers).
- Whether frameless also fires when npro > 0 but all promoted slots use r7 (it should:
  after csave those ldr/str [r7, #off] lines are gone or rewritten to sp-relative...
  they are NOT: un-promoted references keep using [r7, #off]). Builder note: an
  un-promoted frame reference is `ldr r0, [r7, #N]`, which reads r7 and therefore
  blocks frameless. So for fib, frameless fires only because fib has NO frame refs
  after un-promotion (its only slot was the parameter). Functions with locals keep
  r7 - correct, their slots need a base; making them sp-relative is future work.
- Marking in code comments: cite this spec from csave()/frameless() headers, as licm
  cites its own.

## Results (branch cc-prologue)

`bench.py --pc`: total 271 -> 155 ms (1.75x), fib 28 -> 23 ms, every check line
equal to the native gcc build. `bench.py --board`: total 7190 -> 3546 ms (2.03x),
fib 446 -> 262 ms, "output equal to gcc" for all 11 benchmarks.

fib keeps its frame: `n` is live across both calls so calllive() leaves it in r4,
and the parameter load reads r7. The remaining fib gap is the spill pair and needs
the register allocation of approach 2.
