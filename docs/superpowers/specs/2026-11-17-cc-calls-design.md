# cc -O: cheaper function entry and exit (short design)

## Why
BASIC runs each bytecode as a call through a function-pointer table (basic/vm.c optab), and
most handlers are small: 65 of the VM's 112 functions have no locals. Every function still
pays the full entry and exit, so call overhead is a large share of BASIC's time (and the
reason call-heavy benchmarks gain least from -O: fib 1.42x, dispatch 1.67x).

## Goal / non-goals
- Goal: with -O, trim each function's entry and exit. Measured on the board (bench.py
  fib, dispatch, interp; bench.bas).
- Non-goals: changing the calling convention (arguments in registers), inlining. Without -O
  the output stays byte-identical (check_ref); -O and plain objects keep linking together.

## Approaches
- Trim entry/exit in the -O pass (chosen): local to cc.c, no ABI change.
- Arguments in registers: changes the convention for every object, plain included; ruled out.
- Inlining: BASIC calls its handlers through pointers, so it would not reach them; ruled out.

## Design (all with -O, after promote())
1. The frame reservation (movw r1, #N / movt r1, #0 / sub.w sp, sp, r1): N = 0 drops all
   three; N <= 508 becomes `sub sp, #N`.
2. A function whose body uses r7 nowhere else (no parameters, no stack locals, nothing
   promoted) drops `mov r7, sp` and `mov sp, r7`. It keeps push {r7, lr} / pop {r7, pc} so
   the stack stays 8-byte aligned.
3. An unconditional branch to the function's exit, when the exit is only `mov sp, r7` (if
   present) and the `pop {.., pc}` (no vpop), is replaced by a copy of those lines.

## Checks
A codegen check of the three shapes (tools/check_frame.py, under qemu: the snippets also run
and print the right values); the existing -O checks (compiler tests and every BASIC case
built with OPT=-O, check_asm --corpus on the -O build, the -O toolchain rebuilding itself
identically); check_ref (plain output unchanged); bench.py --pc; then bench.py --board and
bench.bas on the board before and after.
