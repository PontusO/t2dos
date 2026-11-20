# cc -O 3c: frame-address arithmetic and idle reservations, without r7

Date: 2026-11-21. Status: approved design (approach A, user pick from
the recon report). Follows 3b-iii
(`2026-11-21-cc-sz-frameless-design.md`, merged to main as of
`2eba3af`).

## Problem

Two bail reasons keep most of the corpus's remaining `-O` frames:

1. **lea forms.** `subw rD, r7, #N` / `addw rD, r7, #N` - taking the
   address of a local array (or a caller band word) - answer -1 from
   `split()` and the screen bails on the line. Census over bench +
   sh.c + get.c + the two pin files: 20 functions, e.g. sh's
   `dollar`/`glob_word`/`run_cmd`, get's `net_*` cluster, and every
   array witness in `frame.c` (`small`, `big`, `bigprom`, `amem`).
2. **Idle reservations.** A function with `sz > 0` whose body reads
   r7 NOWHERE (locals fully dead-coded, only the reservation left):
   sprel bails with "no slot reads" (`nconv == 0`), then frameless'
   `sz && !sprel_ok()` gate refuses the drain it could perform
   unaided. sh's `find_var` is the witness.

## Key invariant (why lea needs no liveness analysis)

A converted literal maps `[r7, #k] -> [sp, #k+sz+d-4]` at the read
line's live depth d. A lea line materialises the SAME word's
ADDRESS, so the identical formula gives the sp-relative constant:
`subw rD, r7, #N` (word k = -N) becomes `addw rD, sp, #o` with
`o = -N+sz+d-4` (or `subw rD, sp, #-o` when o < 0); `addw` enters
with k = +N. That is it - no constraint on how long `rD` lives.
`rD` holds an absolute address and the word's absolute address does
not move during the function: pushes grow DOWNWARD from sp, so
everything inside the reservation (and the caller's band) sits at a
fixed address until the exit drains. The earlier worry (an address
register read after sp moves) does not exist; the screen already
knows d at every line.

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
  the lea's `|o|` must satisfy the same test as a literal read of the
  same word. The depth walk is unchanged - lea lines move nothing.
- **frameless gate.** Drop `sz && !sprel_ok()`. `fl_frame`'s scan
  already bails on every line that touches r7 beyond the excused
  entry/exit forms, so an unconverted body with real r7 uses (float
  slots, `vstr`, bare uses, un-windowed literals) still keeps its
  frame; only bodies that read nothing get the free drain. Keep
  `sz > 4095` (screen and frameless test it identically, as today),
  and keep the `fl_pc_end && !sprel_ok` multi-exit restriction: an
  unconverted body's exits are only trusted at reservation depth
  once the screen has walked them.
- **trimframe**: no change (same argument as 3b-iii: the `sz > 0`
  branch returns before the pop-copy code, so early returns branch to
  the drained exit label).

## Approaches considered

- **A (picked): lea arm + gate removal, one round.** Two functions
  touched, witnesses already exist, no model change to the depth
  walk.
- **B: add the pending-save walk fix** (the `rec` class, 23
  functions). Rejected for this round: it edits the walk's claim
  logic, the one place a modeling slip means silent corruption; it
  deserves its own round and board campaign.
- **C: lea only, keep the redundant gate.** Rejected: `find_var`
  keeps a frame for no reason and the spec must document a known
  redundant test.

## Goals / non-goals

Goals: the lea class and the idle-reservation class convert; frame
survivors are only argv windows, pending saves, float slots and
over-window shapes; the usual suites prove behavior; census and specs
updated.
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
- A new idle-reservation witness (locals fully dead across a call,
  `sz > 0`, zero r7 reads) converts; `find_var`-shaped code is the
  model.
- `stublea`, `cfloat`, `csys`, `rec`, `fmany` and friends keep their
  frames by the reasons already pinned.
- census: remaining framed functions carry only the non-goal reasons;
  bench and sh.bin sizes recorded; all gates green.

## Left to the builder

- Whether the lea arm parses inline or via a small helper next to
  `sprewrite` (which already skips the `r7, #` prefix).
- Exact witness source for the idle-reservation class (an unused
  array local plus a call should leave a live reservation and no
  reads; verify maxloc counts it).
- Pin wording, census table, sprel/3b-iii spec superseding bullets.
