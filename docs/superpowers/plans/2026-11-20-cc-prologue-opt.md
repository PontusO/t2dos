# cc -O call-aware prologue/epilogue (csave + frameless) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Two -O optimizations in compiler/cc.c - calllive() (csave) stops promote() from parking frame slots in callee-saved registers that no call can preserve, and frameless() removes r7 bookkeeping from functions that never read r7 - so recursive and leaf code stops paying per-call fixed costs.

**Architecture:** Both optimizations work on the function line buffer inside parse_global's `if (optim)` block. csave lands as a weight filter *inside* promote()'s selection loop (`calllive(slot)`, evaluated before any rewriting, so nothing needs undoing); frameless() is a separate pass after trimframe() that deletes `mov r7, sp` / `mov sp, r7` and shrinks push/pop once nothing reads r7. No ABI, assembler, or non-`-O` change.

**Tech Stack:** C in the cc bootstrap subset (all functions return int, no initializers/static/floats in cc.c), Python checkers under tools/, qemu-arm + host_linux for behavior.

**Spec:** docs/superpowers/specs/2026-11-20-cc-prologue-opt-design.md

## Global Constraints

- Without `-O` output stays byte-identical: `python3 tools/check_ref.py` ends `different: 0` (needs `make ref` once).
- cc.c stays in the bootstrap subset; verify new code parses with the committed cc and build/ref/cc.
- Pass order after this plan: `prepro = 1; peep(); licm_loops(); prepro = 0; peep(); promote(); trimframe(frame_at, sz); frameless();` (csave lives inside promote()).
- `make` before any board/qemu tool run; each checker's flags are in its docstring.
- Never print whole .s files: `cc -O -S ... -o out.s` then grep the function.
- Board runs need the user's explicit go-ahead; disassemble new codegen before board.
- Commit messages: plain developer English, no em dashes.

## Review Focus

Failure modes no single task's happy-path test pins; each gets a pinning test in the named task:

1. A promoted register live across SOME calls but not others (loop-increment counter vs call argument): calllive must keep the first. Task 3.
2. A function with real locals that un-promotes its only register still reads r7 via `ldr rX, [r7, #N]`: frameless must NOT fire. Task 4.
3. vpush/vpop move sp: a float-promoted function keeps `mov sp, r7` and the frame pointer even when no core line reads r7. Task 4.
4. trimframe copies the epilogue pop into backward branches near the exit; frameless must rewrite those copies too or returns pop the wrong list. Task 4.
5. An unknown instruction form (opkind 4) in a function: calllive and frameless both bail conservatively. Tasks 2/4 pins.
6. A call-free function: calllive changes nothing at all (the has_calls gate); today's promoted shapes must survive byte-for-byte. Task 2's cleaf snapshot pin.

---

### Task 1: Test corpus for call liveness and frame-pointer use

**Files:**
- Create: `tests/opt/prolog.c`
- Modify: `tools/check_prolog.py` (new file)

**Interfaces:**
- Produces: `tools/check_prolog.py` runnable standalone (`python3 tools/check_prolog.py`, prints `ok: prolog` exit 0 when green); functions in tests/opt/prolog.c named csc, cleaf, cmixed, clocals, cfloat, cret used by Tasks 2-5 pins.

- [ ] **Step 1: Write tests/opt/prolog.c** with exactly these functions (bootstrap-subset C, floats allowed in test files):

```c
int g;
int ext(int a);                       // prototype only: forces real calls
int csc(int n) {                      // r promoted, value live across the call: keep
  int s;
  s = n;
  g = ext(s);
  return s + g;
}
int cleaf(int n) {                    // no calls, one local, one promoted param
  int t;
  t = n * 3;
  return t + 1;
}
int cmixed(int n) {                   // i dead across the call (arg), s live after it
  int i; int s;
  s = 0; i = 0;
  while (i < n) { g = ext(i); s = s + i; i = i + 1; }
  return s;
}
int clocals(int n) {                  // two locals, no calls: frame stays (r7 reads)
  int a; int b;
  a = n + 1; b = a * 2;
  return b - n;
}
float cfext(float x);
float cfloat(float x) {               // float local: vpush/vpop keep the frame pointer
  float y;
  y = x + 1.5;
  return y * 2.0;
}
int cret(int n) {                     // early return: trimframe copies the pop
  if (n > 0) return 7;
  return ext(n);
}
int main() {
  int a; int b; int c; int d; float e;
  a = csc(4); b = cleaf(5); c = cmixed(3); d = clocals(6); e = cfloat(1.5);
  printf("check %d %d %d %d %d\n", a, b, c, d, (int)e);   // gcc reference pins the numbers
  return 0;
}
```

Compute the expected `check` line from a native gcc build (`gcc -w tests/opt/prolog.c` needs a stub for ext/cfext: add `int ext(int a) { return a + 1; } float cfext(float x) { return x; }` behind `#ifdef NATIVE` and keep the prototypes otherwise, or link a tiny second .c file - builder's choice, pin whatever gcc prints).

- [ ] **Step 2: Write tools/check_prolog.py skeleton** modeled on tools/check_frame.py: build `cc -O -S -I lib tests/opt/prolog.c -o root/tmp/prolog.s`, parse functions into line lists (copy check_frame.py's `funcs()`), build and run the `.bin` under qemu host_linux, compare stdout to the pinned check line. No shape assertions yet.

- [ ] **Step 3: Run it; it must pass before any cc.c change** (`python3 tools/check_prolog.py` prints `ok: prolog`). This is the behavior baseline.

- [ ] **Step 4: Commit** `tests/opt/prolog.c tools/check_prolog.py`: "check_prolog: corpus for call liveness and frame-pointer shapes".

### Task 2: calllive() - promote() skips registers no call can preserve

**Files:**
- Modify: `compiler/cc.c` (new `int calllive(int slot)` near promote(); called from promote()'s selection loop; a function-level `has_calls` scan computed at promote() entry)

**Interfaces:**
- Consumes: the still-unrewritten frame references (`ldr/str rX, [r7, #off]`), `opinfo()` results (`opkind`: 1 for bl/blx-syscall-exempt, 4 unknown), `nx()`, `labat()`, licm-style backward-branch detection.
- Produces: `calllive(slot)` returns 1 when the slot's value is live across some call (or the function has no calls at all, or anything is uncertain); promote()'s selection loop treats weight 0 slots as unpromotable. frameless() (Task 4) benefits because fewer promoted slots means fewer r7 rewrites... it reads npro/nfpro after promote.

- [ ] **Step 1: Write the failing pins** in tools/check_prolog.py: for `csc`, the push line still contains a callee-saved register and an `ldr r4, [r7, #` exists; for `cmixed`, exactly one callee-saved register is saved (s stays, i goes): pin the push list has exactly one of {r4,r5} and today it has two. Run: cmixed pin fails today (both registers saved).

- [ ] **Step 2: Implement calllive() in cc.c** (bootstrap subset throughout):
  - At promote() entry compute `has_calls`: scan lines once, 1 when any line's opinfo gives a real call. Careful: `blx r8` (syscall) is opkind 1 too but clobbers nothing past r2 - count only `mnis("bl")` and `blx` of a register other than r8 as a call. If !has_calls, calllive returns 1 for everything (call-free functions promote exactly as today; this keeps check_frame's plarge/bigprom pins intact).
  - `int calllive(int slot)`: the frame references are still unrewritten `ldr/str rX, [r7, #off]` at selection time (promote selects first, rewrites after), so walk lines with nx(); let u be a line with r7ref(i) == 1 and rfoff == (slot - 256) * 4. Between consecutive references u1 -> u2, and from the last reference to the exit pop: run opinfo on every line in between; if none is a call, continue. If one IS a call: the value must survive it only when a later reference reads (not just writes) - so return 1 (keep) unless every following reference up to the next store-only... simplest sound rule: **return 1 if any call lies between a *reading* reference of the slot and any later reference at all**, where "reading" means the ldr form or an str whose value operand is not provably fresh - treat both ldr and str as reading unless the very next mention is a store (conservative: count both as reads; over-keeping only loses speed). Any L_LAB/L_BR between the two references, or opkind 4 anywhere in the function, returns 1.
  - In the selection loop, right where `if (i >= 258 && w > 0) w = w - 1;` sits, add: `if (w > 0 && !calllive(i)) w = 0;`. Weight-0 slots are never picked, so promote emits no register, no parameter load, and reglist shrinks on its own.
- [ ] **Step 3: Rebuild host cc (`make`), run check_prolog**: cmixed now saves one register not two; csc still saves r4 with its `ldr r4, [r7, #N]`... after folding into promote, csc's slot stays promoted (live across call), cmixed's loop counter slot loses weight. Pins pass. Behavior line unchanged.

- [ ] **Step 4: Run check_frame** (`python3 tools/check_frame.py` -> `ok: frame`) and check_ref (`different: 0`). The plarge/bigprom pins must survive via the has_calls gate (both are call-free). Pin the same property in check_prolog: cleaf's shape is byte-identical to today's output (snapshot its function body from prolog.s into the checker as a literal list, so any future change there needs a deliberate pin update).
- [ ] **Step 5: Quick fib measurement**: build `cc -O -I lib -o root/tmp/fib_o.bin tests/bench/fib.c`, run under qemu host_linux, expect time dropping from ~22 to roughly 15-18 ms (csave alone removes the per-call r4 reload). Check line unchanged.
- [ ] **Step 6: Commit** compiler/cc.c + tools/check_prolog.py: "cc -O: calllive drops promoted registers no call can preserve".

### Task 3: Mixed-liveness and recursion pins

**Files:**
- Modify: `tools/check_prolog.py`
- Create: nothing (corpus from Task 1)

**Interfaces:**
- Consumes: Task 2's calllive behavior.
- Produces: pinned shapes Review Focus #1 names.

- [ ] **Step 1: Add pins**: in cmixed, the register holding `s` (live across `bl ext`: used after it) is still saved; a second promoted slot would be the loop counter - pin exactly one callee-saved reg in cmixed's push. In csc pin r4 present. Add a self-recursive function `int rec(int n)` to prolog.c (rec calls rec: its promoted param must stay saved) and pin it.
- [ ] **Step 2: Run check_prolog, fix cc.c until green** (if Task 2's dead() rule over-drops csc/rec, tighten calllive: a reference AFTER a bl on the linear path proves liveness directly).
- [ ] **Step 3: make check** (`| tail -20`) and bench.py --pc (`| tail -15`): all check lines equal gcc; record fib time.
- [ ] **Step 4: Commit.**

### Task 4: frameless() - elide r7 setup/teardown when nothing reads r7

**Files:**
- Modify: `compiler/cc.c` (new `int frameless()` after promote/trimframe; pass-order line)
- Modify: `tools/check_prolog.py`

**Interfaces:**
- Consumes: final buffer state (post trimframe): prologue `push {..., r7, lr}` or rewritten list, `mov\tr7, sp`, exit `pop {..., r7, pc}`, `mov\tsp, r7`, trimframe's copied pops. Globals npro/nfpro, r7ref(), opinfo().
- Produces: for eligible functions: no `mov r7, sp`, no `mov sp, r7` (unless nfpro), push/pop without r7; returns nothing meaningful.

- [ ] **Step 1: Failing pins** in check_prolog.py: cleaf and cret contain no `mov\tr7, sp`, no `mov\tsp, r7`; their push is `push\t{lr}` (or the shrunk list without r7) and every pop ending pc lacks r7; cret's trimframe-copied pop(s) also lack r7. cfloat still has `mov\tr7, sp`, `mov\tsp, r7` and r7 in push/pop (vpush/vpop rule). clocals still reads r7 via its frame slots and keeps the bookkeeping. Run: fails.
- [ ] **Step 2: Implement frameless():**
  - Eligibility scan over all lines except the prologue push, `mov\tr7, sp`, the exit region (`mov\tsp, r7`, vpop, final pop) and trimframe's copied pops: any L_INS line where opinfo shows a read or write of r7 (opmask bit 7 or oprd == 7 - note push/pop mention r7 in braces; those lines are the prologue/exit/copied pops themselves, exclude by identity `preins(l,"push\t{")` / `preins(l,"pop\t{")` and handle them separately) -> not eligible. Any opkind == 4 line -> not eligible (Review Focus #5). nfpro > 0 -> not eligible (Review Focus #3).
  - If eligible: pdel the `mov\tr7, sp` line(s); pdel every `mov\tsp, r7`; for EVERY line starting `push\t{` or ending a return path (`pop\t{...pc}`) including trimframe copies: rewrite without r7 via pset + reglist-style emit (write a small helper that re-renders the list minus r7; when the push becomes empty of saved regs it is exactly `push\t{lr}`, pop is `pop\t{pc}`). Thumb encodes both; verify in Task 5.
  - A function whose frame sz > 0 but whose locals never got promoted reads r7 through slots -> ineligible by the scan, no special case needed.
- [ ] **Step 3: Rebuild, check_prolog green; check_frame green (fpm/fmany vpop pins unchanged; leaf/noret already had no r7 - make sure frameless is a no-op there); check_ref `different: 0`; compiler/tests behavior under qemu.**
- [ ] **Step 4: fib measurement again** (expect toward ~12 ms total with both passes). Commit: "cc -O: frameless entry and exit when nothing reads r7".

### Task 5: Bench, assembler and self-host gates

**Files:**
- Modify: `tools/check_prolog.py` only if pins need the final shapes.

**Interfaces:**
- Consumes: everything above. Produces: green board-ready branch.

- [ ] **Step 1: `make check 2>&1 | tail -20`** exit 0.
- [ ] **Step 2: Assembler sanity for the new encodings**: extract one `push\t{lr}` and `pop\t{pc}` line from prolog.s, assemble with build/host/as and the GNU oracle (`~/.arduino15/packages/rp2040/tools/pqt-gcc/*/bin/arm-none-eabi-as`), compare bytes; or show `check_asm.py --diff N` already covers them. Disassemble fib_o.s with build/host/objdump and eyeball the entry/exit (never the whole file).
- [ ] **Step 3: `bench.py --pc 2>&1 | tail -15`**: all check lines match gcc; table shows fib improved, nothing regressed more than noise. Record the table in the commit message.
- [ ] **Step 4: OPT self-rebuild**: `make OPT=-O` rebuilds the toolchain; the freshly built cc reproduces prolog.s byte-identically (run check_prolog again after).
- [ ] **Step 5: Commit** any remaining test pins: "check_prolog: final shapes for csave and frameless".

### Task 6: Board verification (needs user go-ahead, run last)

- [ ] **Step 1:** Ask; on yes: `make` then `T2DOS_PORT=... python3 tools/bench.py --board 2>&1 | tail -15` and `python3 tools/check_toolchain.py --selfhost 2>&1 | tail -10`. All check lines correct, selfhost six tools IDENTICAL.
- [ ] **Step 2:** Fold spec + plan + final numbers into docs (spec status line: implemented), commit on the branch, report table to user for merge decision.
