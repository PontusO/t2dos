# cc -O 3c: frame-address (lea) arithmetic without r7

Date: 2026-11-21. Status: approved design, corrected by pre-plan
recon (the idle-reservation class was disproved; 3c is the lea arm
alone). Follows 3b-iii
(`2026-11-21-cc-sz-frameless-design.md`, merged to main as of
`2eba3af`).

## Problem

One bail reason keeps most of the corpus's remaining `-O` frames:

**lea forms.** `subw rD, r7, #N` / `addw rD, r7, #N` - taking the
address of a local array (or a caller band word) - answer -1 from
`split()` and the screen bails on the line. Census over bench +
sh.c + get.c + the two pin files: 20 functions, e.g. sh's
`dollar`/`glob_word`/`run_cmd`, get's `net_*` cluster, and every
array witness in `frame.c` (`small`, `big`, `bigprom`, `amem`).
Every local declaration emits one: cc's local array keeps a pointer
cell in its slot 0 and initializes it with exactly this shape, so
`int f(int n) { char b[40]; return n; }` - a fully dead array -
still reads r7 at its descriptor init.

An earlier draft also claimed an "idle reservation" class (bodies
with `sz > 0` that read r7 nowhere, blocked by frameless'
`sz && !sprel_ok()` gate). Recon disproved it: the descriptor init
above means no such body exists, and `find_var`, the suspected
witness, converts fine standalone - in sh.c it is the BUILTIN argv
window (a non-goal class) that bails. The census script had also
mislabelled wide-form lines (`str.w r0, [r7, #-N]`). The gate stays
as the cheap invariant guard it is; 3c is the lea arm alone.

## Key invariant (why lea needs no liveness analysis)

A converted literal maps `[r7, #k] -> [sp, #o]` at the read line's
live depth d, where `o = spmap(k, sz, d) = k + sz + d - (k >= 0 ? 4 :
0)`: dropping r7's word from the entry list shifts the CALLER's band
(k >= 0) down by one, but our own-frame slots (k < 0) live inside the
reservation, which hangs off the moved sp and keeps the offset it had
framed. (The first draft of this spec claimed one uniform `-4` for
both; the `bigprom` witness caught it: a local slot parked 4 low let
`b[599]` store over the saved lr.) A lea line materialises the SAME
word's ADDRESS, so the identical formula gives the sp-relative
constant: `subw rD, r7, #N` (word k = -N) becomes `addw rD, sp, #o`
with `o = spmap(-N, sz, d)`; `addw` enters with k = +N. That is it -
no constraint on how long `rD` lives. `rD` holds an absolute address
and the word's absolute address does not move during the function:
pushes grow DOWNWARD from sp, so everything inside the reservation
(and the caller's band) sits at a fixed address until the exit drains.
The earlier worry (an address register read after sp moves) does not
exist; the screen already knows d at every line.

Encodings: `as` already assembles `addw/subw rD, sp, #N` - verified
bit-identical to GNU `arm-none-eabi-as` (`f20d000c` etc.), including
the wide immediate (subw r1, sp, #1012 -> f2ad31f4) and hi registers
(addw sl, sp, #4).

## Design

- **sprel screen.** The `k == -1` bail splits: `vstr` and bare-register
  r7 uses keep bailing; a line matching `subw rD, r7, #N` or
  `addw rD, r7, #N` (base operand exactly r7) is rewritten in place
  with the formula above and counts toward `nconv`. The existing
  mapped-offset window check (0..1020, the ldr/str reach) bounds it:
  the lea's `o` must satisfy the same test as a literal read of the
  same word. The depth walk is unchanged - lea lines move nothing.
  Both walks need the arm: the prescan (test, window, bail) and the
  conversion walk (the `pset` rewrite). The rewritten line
  (`addw rD, sp, #N'`) loses its r7 token, so it passes the
  conversion walk's `r7ref(i) == 1` gate, `fl_r7` (whose `r7ref`
  scan finds no r7) and `trimframe`'s `nor7` loop with NO changes
  to any of them.
- **frameless, trimframe, fl_r7, split, sprewrite**: no change. The
  `sz && !sprel_ok()` gate stays (no body can reach frameless with
  `sz > 0` and zero r7 reads, see Problem).

## Approaches considered

- **A (picked): the lea arm, one round.** One function touched
  (`sprel`), witnesses already exist, no model change to the depth
  walk. An earlier A also removed frameless' `sz && !sprel_ok()`
  gate; recon (Problem) showed nothing needs that, and the gate is
  kept as a cheap invariant guard.
- **B: add the pending-save walk fix** (the `rec` class, 23
  functions). Rejected for this round: it edits the walk's claim
  logic, the one place a modeling slip means silent corruption; it
  deserves its own round and board campaign.
- **C: rewrite at the lea's uses instead of the lea line** (fold the
  frame offset into each `ldr rX, [rD]`). Rejected: it duplicates
  the depth model at every use site, where the screen already walks
  it once at the line.

## Goals / non-goals

Goals: the lea class converts; frame survivors are only argv
windows, pending saves, float slots and over-window shapes; the
usual suites prove behavior; census and specs updated.
Non-goals: pending saves, argv windows, `vstr`/`vldr` slots (future
rounds 3d/3e).

## Constraints (unchanged series rules)

Bootstrap subset in `cc.c`; non-`-O` byte-identical (`check_ref`
`different: 0`); the drain/rewrite are relative-only (no absolute
addresses); PC gates before board; board gates (`--selfhost`,
`bench --board`, `check_basic --board`) at merge; disassemble new
codegen before running it on the board.

## Acceptance

- `frame.c` pins flip: `small`, `big`, `bigprom`, `amem` lose r7 and
  drain `add sp, #sz` (`as` picks the wide `add.w` for 1008); `huge`
  stays framed (sz gate); `fpint` stays framed if its float slot
  survives peep, else converts - the builder pins whichever holds.
  The `frame.bin` run pin keeps printing the same numbers.
- `stublea`, `cfloat`, `csys`, `rec`, `fmany` and friends keep their
  frames by the reasons already pinned.
- census: remaining framed functions carry only the non-goal reasons;
  bench and sh.bin sizes recorded; all gates green.

## Results (as built, commit ce44426)

- Formula as built: `spmap(k, sz, d) = k + sz + d - (k >= 0 ? 4 : 0)`,
  applied at every mapping site (prescan window, hold window,
  conversion walk, `leaoff`). The lea arm lives in `leaoff` (test +
  window), `leawrite` (rewrite), `leaform` (fl_r7 excuse on shape) and
  `leasp` (fl_frame excuse for the rewritten `addw rD, sp, #o`, which
  opinfo flags via its SOURCE operand sp although nothing moves).
  `fl_r7`'s converted-line excuse widened from `split(i) >= 0` to
  `split(i) != -1`: own-frame slot reads carry negative offsets and
  3b-iii never reached that path (every negative reader also had leas
  and kept its frame).
- Census (`mov r7, sp` lines in -O output, 3b-iii `2eba3af` -> 3c):
  sh.c 43 -> 33, bench total 18 -> 10 (crc/interp/matmul/sieve/sort -1
  each, strings -3), get.c 17 -> 17. Every survivor carries the argv
  window class (the builtin `push {r4}` window and its `mov r2, sp`
  friends; all 33 sh survivors and all 17 get survivors show it), so
  the lea-only scope is complete: no framed function is lea-blocked
  anymore.
- Witness flips: `small`, `big`, `bigprom`, `amem` (frame.c) and `rec`,
  `stublea` (prolog) convert; run pins unchanged (frame.bin prints the
  same numbers, prolog.bin `check 9 16 3 8 6 16 19`). `rec`'s
  previously pinned "pending result save" reason was really the
  negative-slot bug: its `str r0, [sp, #4]` sits at drained depth and
  traces clean. `fpint` keeps its frame (float slot survives). The
  mixed-chain witness moved its framed half: `cmix` now also calls
  framed `csys` (`read(0, b, 0)` adds 0), keeping a converted caller ->
  framed callee on the run; `stublea` stays as the cross-object
  converted callee.
- Sizes: sh.bin -O 71791 -> 71621 (-170 B); bench -O total 93764 ->
  93678 (-86 B). (Board totals follow in the board section below.)
- Fixed point: whole-tree `make OPT=-O` then `make` both rebuild and
  pass `check_sh --pc` (5 cases). PC gate at commit: `check_ref`
  different: 0, `make check` ok, bench --pc 11/11, basic 139, sh 5.

- Board gates (all on hardware): `check_toolchain --selfhost` ok (the
  board cc rebuilds the six tools IDENTICAL, tests PASS);
  `bench --board` ok, 11/11 equal to gcc, -O total 3430 ms (3b-iii:
  3534) and 6690 B (3b-iii: 6740; -50 B, the movers matching the
  flipped functions), speedup 2.12x overall; `check_basic --board` ok,
  139 cases.
