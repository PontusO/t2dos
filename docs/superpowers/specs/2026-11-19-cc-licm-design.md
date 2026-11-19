# cc -O: loop-invariant code motion (short design)

## Why

`-O` today rewrites inside one or two lines at a time (peep()) and moves whole variables
into registers (promote()), but it never moves work out of a loop. The hot loops in
tests/bench rebuild the same values every iteration:

- matmul's inner loop computes each array address as `movw/movt rX, A` + `add.w rX, r9, rX`
  + `muls` + `adds` + `lsls` inside the body; the base of a global is fixed for the whole
  function, so at least the first two lines are pure waste per element.
- crc reloads its constants (`movw r0, #1`, `movw r0, #255`) every iteration because the
  three-address forms want them in registers again and again.
- sieve, loop and list show the same shape: address math on globals and small constants
  rebuilt inside the body.

Measured with gcc 13 -O2 under qemu as reference (see the benchmark report of 2026-11-18):
cc -O totals about 1.5x slower, and the three-to-four-times cases (matmul, loop, sieve,
crc) are exactly the ones where a hoisted address or constant would remove most of the
per-iteration work. LICM also feeds what already exists: a hoisted base lands in a scratch
register that promote() can then give a callee-saved register, and a smaller body gives
peep()'s branch rules more to thread.

## Goal / non-goals

- Goal: with -O, move loop-invariant instructions out of loops so each runs once per
  function call instead of once per iteration. Measured on the board (bench.py matmul,
  crc, sieve, loop; bench.bas) and against gcc as today.
- Non-goals: strength reduction (`s += 4` instead of `i * n + k`, pointer bumps): it needs
  the hoisted expressions first, so it is a follow-up spec on top of this one. No new
  peephole rules for constant shifts here either; that fits in peep() and can land
  separately. No loop reordering, unrolling or vectorization (Cortex-M33 has no NEON).
- Without -O the output stays byte-identical (check_ref); plain and -O objects keep linking.

## Approaches

- Hoist inside the existing flat line buffer after peep(), as its own pass licm()
  (chosen): loops in that buffer have one recognizable shape (below), registers are r0-r3
  temporaries plus promoted registers, and every tool needed already exists: labat(),
  pdep[] loop depth (promote()), opinfo() read/write sets, dead(). No new data structure
  beyond a loop table.
- A real dominator/tree analysis on an IR: right for a compiler with an AST; cc has none,
  and building one is this project's biggest rewrite. Ruled out.
- Extending peep() with "if the cmp/add is before the label, don't redo it": peep() runs
  per line pair and cannot see loop bodies as units. Ruled out.

## Loop shape (what licm() may assume)

After peep(), every generator loop looks like:

    .Lc:                       // or the loop head is straight-line code before it
      cmp   ...                // flags for the exit test
      bge.w .Lx  /  ble.w .Lx  // conditional exit, one way out
      b.w   .Lb                // into the body (peep_br may have merged these)
    .Lb:                       // body start (a label a back branch targets or passes)
      ...                      // straight lines and nested loops
      b.w   .Lc                // back edge

licm() finds loops by scanning branches: a branch whose target line t is at or before the
branch's own line (labat(llab[i]) <= i, following peep_br's threading). The body is the
lines from the head label through the back branch; nested loops are inside it and get their
own entries, innermost first. promote() already trusts this same back-edge scan for its
loop weights, so licm() adds no new trust in the shape: a loop that does not match exactly
(one where the conditional exit is not directly after the head's compare, or with two back
edges into different labels) is skipped whole. peep_br threading is what makes the single
exit hold; if it did not, nothing matches and licm() does nothing (fails closed).

## Design (all with -O, in parse_global: peep(); licm(); promote(); trimframe())

licm() runs between the existing peep() and promote(): peep(); licm(); promote();
trimframe(). No second peep() is needed: licm() only inserts known lines before the head
and deletes body lines, so no new foldable pairs appear (the duplicate-fold idea was
rejected with the keep-both variant, see rule 3). promote() sees the final loop bodies and
can hand the hoisted values promoted registers as usual.

1. Loop table: for each matched loop record head line h, body range [h, b] (b = back
   branch), and depth from pdep[]-style counting; process innermost first so a hoisted
   line can be hoisted again by an outer loop's pass.
2. A line in the body is a candidate when it is L_INS, its mnemonic is one licm() knows
   (mov, movw, movt, add, add.w, addw, sub, subw, mul/muls, lsls, lsrs, ands with an
   immediate: pure ALU, no memory, no flags that later lines read before this line's own
   result), it does not write sp/lr/pc/r7/r8/r9, and its inputs are invariant:
   - a register written nowhere inside [h, b] (opinfo() scan of the body), or written only
     by an earlier candidate in the same loop that is also being hoisted (the movw/movt/add
     triple for one global base is exactly this chain); and
   - not live-modified between uses: since nothing in the body writes it, any use reads the
     same value. A bl inside the body clobbers r0-r3 and r12: a candidate whose result or
     inputs sit in caller-saved scratch registers is only hoisted when the loop body has no
     call at all; promoted registers (r4-r6, r10, r11) and s16-s31 are callee-saved, so a
     value in them survives calls and needs no such restriction.
3. The destination must survive to the head: dead() is not enough (it proves "never read
   again", we need "the value at the head equals the value after the hoisted line"). Rule:
   licm() deletes the original and inserts one copy before .Lc, which is sound when the
   destination is written nowhere else in the body and every read of it inside the body
   reads the hoisted value (no other candidate for the same register runs after a write to
   it). When that fails licm() leaves the line alone: keep-both was rejected because two
   definitions of one register across the head is exactly the kind of shape later passes
   misread, and a half-hoist buys little.
4. Where to insert: directly before the loop head label .Lc (or before the straight-line
   block that starts the outermost loop when several hoists stack). Registers: licm() keeps
   each hoisted line in its own destination register; promote() is what can give it a
   callee-saved one. One known cost: peep_push has already turned many call arguments into
   `push {r0}` pairs whose value was computed just above, so a hoistable constant that sits
   in such an argument chain is invisible to licm(). Restoring the push-immediate form for
   constants inside loops is a small follow-up rule (it needs only opinfo's "written once
   right before the push" check) and is part of this spec's second step, not its first.
5. Safety rules that make it fail closed:
   - any candidate whose inputs include a register loaded from memory inside the body is
     rejected (no alias analysis exists: a store through a pointer may change what an ldr
     would return, so loads are never candidates and expressions over freshly loaded values
     stay put);
   - loops containing bl only hoist into callee-saved-eligible forms (rule 2);
   - IT blocks straddle nothing: peep_ite has already turned the loop's ifs into branches;
     a candidate inside an it/ite block is rejected outright;
   - a loop whose body contains vpush/vpop or touches s registers is skipped (float code is
     rare in hot integer loops; mandel already ties with gcc).

## Checks

- check_ref: plain output unchanged (licm() only runs under -O).
- A new tools/check_licm.py (or a licm case in check_frame.py's style): pins the shapes on
  tests/opt/licm.c, e.g. the global-base triple appears once per function and not inside
  .L; the loop body of matmul-style code contains no movt; runs under qemu and pins output.
- The -O suite: compiler/tests under qemu vs gcc, OPT=-O check_basic.py --pc,
  OPT=-O check_sh.py --pc, check_asm.py --corpus on the -O build.
- Behavior equality by construction: every existing userland binary built plain and with
  -O must still print identical output (bench.py --pc already checks gcc's checksums).
- The self-hosting fixed point: check_toolchain.py --selfhost on the board (the -O cc
  rebuilds itself byte-identically; licm() must not make cc's own build drift).
- Benchmarks before/after, board and qemu: matmul, crc, sieve, loop first; then bench.bas.
  Board numbers averaged over several runs (alignment noise is 5-6%).

## Risks

- The register-survives-the-head argument (rule 3) is the one place a bug miscompiles
  quietly. Mitigation: delete-only with no other writer of the destination in the body, and
  pin it with check_licm.py plus fuzzing -O vs plain on random loops (extend the 2026-11
  audit's generator with nested loops over globals).
- Compile time stays flat: licm() is one scan per loop over lines peep() already walked;
  cc compiling cc.c is the budget check (about 27 s on the board today, most of it flash
  writes).
