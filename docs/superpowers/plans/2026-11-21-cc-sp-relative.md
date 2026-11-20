# cc -O sp-relative frame slots (fp-free functions with locals) Implementation Plan

**Goal:** Let `cc -O` make functions that read their frame slots fp-free: rewrite every r7 slot access to `[sp, #off+sz+d]` at its tracked spill depth and strip the frame pointer, including multi-exit functions like fib.

**Architecture:** One new pass `sprel(fa, sz)` between `promote()` and `frameless()` in `compiler/cc.c`: an all-or-nothing screen/rewrite over one function's line buffer, plus a flag (`spconv`) that relaxes exactly one frameless() bail (the multiple-exit rule) for converted functions. trimframe runs after and is unaffected: it replaces the reservation trio with an equal-size `sub sp, #sz`, so rewritten offsets keep their meaning.

**Tech Stack:** C in the committed-cc bootstrap subset (single-pass, line buffer `lpool/lkind/ltext`, helpers `r7ref/opinfo/nx/pset/lend/pdel`); Python checkers; qemu for behavior.

**Spec:** `docs/superpowers/specs/2026-11-21-cc-sp-relative-design.md` - read it before Task 1; the rulings (A2 parked, lea bails, multi-exit relaxation) live there.

## Global Constraints

- Without `-O`, output stays byte-identical: `python3 tools/check_ref.py` prints `different: 0` after every task (`make ref` once if `build/ref/cc` is missing).
- `compiler/cc.c` stays inside the bootstrap subset: run `./build/ref/cc -o /tmp/x /dev/null 2>&1; ./compiler/cc -O -S compiler/cc.c -o /dev/null -I lib` style sanity (the committed cc must still compile cc.c; `check_toolchain.py --selfhost` later covers it).
- opinfo's globals (`opkind/oprd/opmask`) do NOT survive helper calls - every test reads them immediately after its own `opinfo()` call. This is the recurring bug class of this codebase.
- No em dashes in commit messages. Never `git add -A`. Commit only the files each task names.
- `make` before every check; `python3 tools/check_frame.py` → `ok: frame`; `python3 tools/check_prolog.py` → `ok: prolog`; `make check` green.
- Board runs need the user's explicit go-ahead; this plan is PC-only (board pass happens at merge time, outside it).

## Key facts (verified against the code - do not re-derive)

- Pass order (cc.c ~3581): `prepro = 1; peep(); licm_loops(); prepro = 0; peep(); promote(); frameless(frame_at, sz); trimframe(frame_at, sz);` inside `if (optim)` - a `sprel()` call added there is `-O`-only for free.
- Prologue lines: entry `push {r7, lr}` at `fa-3` (`fa == frame_at`), trio `movw r1,#lo / movt r1,#hi / sub.w sp, sp, r1` at `fa..fa+2`, then 21 empty `L_DEL` hold slots from `phold` (promote fills some with `ldr r4, [r7, #N]` / `vldr sN, [r7, #N]` at d == 0), body, then `mov sp, r7`, the `pvpop` slot, final `pop {r7, pc}`. promote may have rewritten the entry push to `push {r4, r7, lr}` etc. via reglist().
- `r7ref(i)` (cc.c ~1150): 0 no r7 (excuses the four bookkeeping strings verbatim), 1 with `rfoff` for any `..r7, #off..` line (`rfplain` = strict `ldr/str r0-r3, [r7,#off]`; subw's value negated), 2 anything else. **Never trust `rfplain` alone**: `vldr s16, [r7, #12]` is kind 1 with rfplain 0 and must convert; `subw r0, r7, #20` is kind 1 with a sign-flipped value and must BAIL.
- The exact bail forms for kind-1 lines: mnemonic starts `vstr`, or `subw`/`addw` (lea), or the line has no memory operand at all (`cmp r1, r7`). A convertible kind-1 line contains `, [r7, #`.
- Spill shapes cc emits: single-register `push {r0}` (+4), `pop {r1}` (-4), `add sp, #N` after a call (-N). Struct args = multi-register pushes (BAIL for now, spec's builder note). Syscalls (`blx r8` + `mov sp, r3` + `bic.w`) move sp with registers - caught by the opinfo test below.
- The opinfo sp test is exactly fl_frame's: after its own `opinfo(i)`, `opmask & 8192 || oprd == 13` means "this line moves sp"; vpush/vpop set `oprd = 13`.
- frameless() bails today on: `phold < 0 || npro || nfpro`; entry push without lr; more than one pop of pc ("an earlier pop of pc is trimframe's copied return"); any line where fl_frame() says the frame stays. Its rewrite deletes `mov r7, sp` / `mov sp, r7` and rebuilds push/pop lists without r7 (fl_list).
- **fl_frame/fl_r7 need no relaxation** (verified): a converted buffer contains no `[r7, #off]`, no vpush/vpop, and no non-exit pop lines (sprel bails on every other pop), so fl_frame's slot-read and pop clauses never fire for it; the entry push is exempted by frameless itself.
- trimframe only rewrites when the trio at fa..fa+2 is still exactly `movw/movt/sub.w` - so **sprel must not delete any line**; it may only rewrite text in place (`pset`) or delete `mov sp, r7` lines... which would break trimframe. Do NOT delete anything in sprel: deleting `mov sp, r7` is frameless()'s existing job (its loop already does it), and it only runs when the screen passed.
- Existing pins that change shape (update them, keep behavior): check_frame.py's `framed`, `early`, `plarge`; check_prolog.py's `csc`, `cret`, and the CLEAF byte-identity pin (`cleaf` has a lea? no - cleaf reads `[r7, #12]` plain but is call-free; it CONVERTS, so its CLEAF pin updates).
- **The hold-load shift problem (verified, plan-level):** promote() rewrites un-promoted parameter loads with `r7shift(i, rfoff + sh)` where `sh = 4*(npro+nfpro)` accounts for the grown entry push. When frameless later strips those promoted registers from the list, the load must shift back by `4*npro` (floats included: they leave s16-s31 with the vpop) - today that is silently harmless because such functions keep r7; once sprel converts them it is a wrong-word read. sprel's rewrite walk therefore re-parses each kind-1 line's literal offset itself (NOT rfoff, which promote already shifted): memory operand -> `literal + sz + d`; `subw` -> `-literal + sz`; `addw` -> bail like any lea. This also covers the hold loads and needs no promote() change.
- Pins that must NOT change: every `vpush`/`vpop` function (fpm/fmany/fearly/cfloat - sprel bails on vpush), small/big/amem/bigprom (lea lines or long-frame interactions - verify big's `sub.w` stays), frame.bin output `1 18 7 5 3 11 14 0 94 29 18`, prolog.bin output `check 9 16 3 8 6 16`.

---

### Task 1: sprel() - screen, rewrite, wiring (A1 shape: bails on every body sp move)

**Files:**
- Modify: `compiler/cc.c` (new `spconv`/`sprel_ok()`/`sprel()` after r7shift(), before reglist(); pass-order line ~3581)
- Test: existing pins only - Task 2 adds the new-shape pins

**Interfaces:**
- Produces: `int spconv;` (file-scope, reset at top of every sprel call, set 1 on a successful conversion); `int sprel_ok();` (spconv == 1); `int sprel(int fa, int sz)` returning 0 always (all-or-nothing bail leaves the buffer untouched). Task 2's frameless relaxation calls `sprel_ok()`.

- [ ] **Step 1: Failing witness.** Compile and check the shape that must change:

```sh
make
./compiler/cc -O -S -I lib tests/opt/frame.c -o /tmp/fr.s
sed -n '/^framed:/,/global/p' /tmp/fr.s
```

Expected today: `push {r7, lr}` / `mov r7, sp` / `ldr r0, [r7, #8]`. This is the witness; no automated pin exists yet (Task 2 adds it).

- [ ] **Step 2: Implement.** Add to cc.c, with a header comment citing the spec:

```c
int spconv;                                // 1 when sprel() made this function's slots
int sprel_ok() { return spconv; }          // sp-relative: frameless() may allow >1 exit
```

`sprel(int fa, int sz)`: reset `spconv = 0`; bail (return 0 without touching a line) unless the trio at fa..fa+2 is still `movw r1,#/movt r1,#/sub.w sp, sp, r1` (use pnum/isins like trimframe's opening test). Then two walks with `nx()` over `[fa-3, last live line]`:

Screen walk, per L_INS line, in this order:
1. `r7ref(i) == 2` -> bail.
2. `r7ref(i) == 1` -> the line reads an r7 slot: convert it unless its mnemonic starts `vstr`, or starts `subw`/`addw` (lea), or the line contains no `, [r7, #` at all (bare-register r7 use). vldr lines convert.
3. otherwise (no r7): if `strncmp(t,"push\t{",6) || strncmp(t,"pop\t{",5) || strncmp(t,"add\tsp, #",9)` -> single-register form only (`{rX}` exactly, one digit between the braces; a second register means struct space: bail), and it moves sp - record it for the depth walk (push +4, pop -4 unless fl_pc_end(i), add sp -N via pnum). Any of these that is NOT cc's own single-register spill form -> bail.
4. then `opinfo(i)` and immediately read: `opmask & 8192 || oprd == 13` -> a line moved sp for another reason (vpush/vpop, syscall, linked .s) -> bail; `opkind == 4` -> bail.

For Task 1 keep it A1-simple: any recorded sp move at all -> bail (Task 3 lifts this to depth tracking). Also bail when the function has no r7 use outside the bookkeeping at all (`mov r7, sp`/`mov sp, r7` don't count - r7ref returns 0 for them): frameless already handles those and must keep firing exactly as today. The entry push at fa-3 is exempt from tests 2-4 (frameless checks lr itself).

Rewrite walk (only after a clean screen), per kind-1 line: **parse the line's own literal offset** (the text after `r7, #`, negated when the mnemonic is `subw`) - not `rfoff`, which promote may have shifted for the grown save list (see Key facts). New offset = `literal + sz` (Task 1: d is always 0). Bail the whole function if any converted line's new offset leaves `[0, 1020]` (the ldr/str/vldr immediate window) - check in the screen walk so the rewrite never starts on a doomed function. Then `t = lpool + ltext[i]; pset(i);` copy the text up to the first `r7`, `ls("sp, #")`, `ls(dec(new))`, rest of the line, `lend()`. Do not delete or insert any line: `mov sp, r7` stays for frameless()'s loop to delete; deleting here would break trimframe's trio test if the screen later fails. Set `spconv = 1`.

Wire it: `promote(); sprel(frame_at, sz); frameless(frame_at, sz); trimframe(frame_at, sz);`

- [ ] **Step 3: Witness passes.** Rerun Step 1's command: framed is now `push {r7, lr}` / `mov r7, sp` / `ldr r0, [sp, #8]` (the slot read converted; frameless still keeps the pointer - Task 2 strips it). framed must NOT yet show `push\t{lr}`.

- [ ] **Step 4: Nothing broke.**

```sh
python3 tools/check_ref.py | tail -1        # different: 0
python3 tools/check_frame.py                # ok: frame (behavior unchanged; no pin fires yet)
python3 tools/check_prolog.py               # ok: prolog
```

If a pin fires, read it against the Key facts: which function changed shape and why that is correct or a bug.

- [ ] **Step 5: Commit**

```sh
git add compiler/cc.c
git commit -m "cc -O: sprel() converts plain r7 slot reads to sp-relative"
```

---

### Task 2: frameless() relaxation, pin updates, syscall-shape witness

**Files:**
- Modify: `compiler/cc.c` (frameless()'s multiple-exit bail ~1334)
- Modify: `tools/check_frame.py`, `tools/check_prolog.py`, `tests/opt/prolog.c`
- Test: the updated pins themselves

**Interfaces:**
- Consumes: `sprel_ok()` from Task 1.
- Produces: fp-free functions that read slots (`push {lr}` ... `pop {pc}`, `[sp, #N]` reads); converted multi-exit functions may now have several inline pops.

- [ ] **Step 1: Failing pins.** Update check_frame.py first (they must fail before the cc change):
  - `framed`: expect `fb[:1] == ['push\t{lr}']`, no `mov r7, sp`, a `pop\t{pc}` exit, and a parameter read matching `re.match(r'ldr\tr\d+, \[sp, #\d+\]$', l)`; keep the old text as a comment naming this spec (the crash witness it recorded is now covered by conversion instead of the frame staying).
  - `early`: still >= 2 pops of pc, and now none of them mentions r7 (`pop\t{pc}` form), plus at least one `[sp, #N]` read.
  - `plarge`: its promoted-parameter load becomes `ldr r4, [sp, #N]` - update the regex from `\[r7, #` to `\[sp, #`.
  Run: `python3 tools/check_frame.py` -> FAILs on framed (still `push {r7, lr}`).

- [ ] **Step 2: Relax frameless().** Two changes, one behavior:
  1. Replace the multiple-exit bail loop ("an earlier pop of pc is trimframe's copied return") with: skip it when `sprel_ok()`. Comment why the relaxation is safe only for converted functions (every exit pop sits at reservation depth; the `mov sp, r7` lines that used to make an early pop correct are deleted by the rewrite loop below).
  2. Split the gate: `if (phold < 0) return 0; if ((npro || nfpro) && !sprel_ok()) return 0;`. A converted function may hold values in r4-r11/s16-s31 - sprel rewrote the hold loads to `[sp, #N]` and fl_list drops those registers from the save list exactly as it drops r7 (floats included: with nfpro > 0 a vpush exists, which sprel's screen bails on, so nfpro is 0 in every converted function - the condition keeps that honest rather than relying on it).

- [ ] **Step 3: Pins pass.** `python3 tools/check_frame.py` -> `ok: frame`. If qemu output changed, something is wrong (behavior must be identical); do not touch frame.c's expected string.

- [ ] **Step 4: prolog pins + syscall witness.** In check_prolog.py:
  - Update the CLEAF pin to today's new exact emission for cleaf (run `./compiler/cc -O -S -I lib tests/opt/prolog.c -o /tmp/pl.s` and read it): call-free, plain slot read -> converts to fp-free (`push {lr}`... `pop {pc}`, `[sp, #N]`).
  - Update the `csc` pin: the promoted-parameter load becomes `ldr r4, [sp, #N]`; the push list keeps r4 (still live across ext()).
  - Update the `cret` pin block: entry `push\t{lr}`, both pops `pop\t{pc}`, no bookkeeping; rewrite the ruling comment - reading an incoming slot no longer forces the frame once sprel converts the read.
  - Add to tests/opt/prolog.c a function whose body contains a syscall, e.g.

```c
int csys(int n) { int r; r = open("/x", 0); if (r > 0) close(r); return n + r; }
```

  and in check_prolog.py pin that `csys` keeps its frame: entry `push\t{r7, lr}` and `mov\tr7, sp` present (the syscall's `mov sp, r3` / `blx r8` moves sp outside the tracked forms - sprel must bail). Extend prolog.c's printf with a value that exercises csys's return; rebuild the expected output string by running native gcc (`gcc -marm -static` reference or plain cc.bin under qemu) and pin exactly what both print.

- [ ] **Step 5: All green.**

```sh
python3 tools/check_prolog.py               # ok: prolog
python3 tools/check_frame.py                # ok: frame
python3 tools/check_ref.py | tail -1        # different: 0
make check 2>&1 | tail -5
```

- [ ] **Step 6: Commit**

```sh
git add compiler/cc.c tools/check_frame.py tools/check_prolog.py tests/opt/prolog.c
git commit -m "cc -O: frameless fires for sp-relative functions, multi-exit included"
```

---

### Task 3: spill-depth tracking (the fib shape)

**Files:**
- Modify: `compiler/cc.c` (sprel(): replace the A1 "any sp move bails" rule with a depth walk; rewrite uses per-line depth)
- Modify: `tools/check_frame.py`, `tests/opt/frame.c`

**Interfaces:**
- Consumes: Task 1's screen structure.
- Produces: fp-free functions whose only body sp moves are single-register spills (`push {r0}` / `pop {r1}` / post-call `add sp, #N`), with slot reads rewritten at their own depth.

- [ ] **Step 1: Failing witness + pin.** frame.c: extend `early` so its parameter slot is read below a spill push - change it to

```c
int early(int x) { if (x < 2) return x; return early(x - 1) + early(x - 2); }
```

(the second call pushes `x - 1`'s result while `early` still reads its parameter for the second argument). Update main's expected value for `d` in the pinned output string (compute with the plain build under qemu, verify against native gcc). check_frame.py: add to the `early` block that no line matches `\[r7, #` and every pop is `pop\t{pc}`. Run: `python3 tools/check_frame.py` -> FAILs (spill pushes still bail sprel, frame stays).

- [ ] **Step 2: Implement depth tracking.** In sprel's screen walk keep a running `d` (0 at fa-3): single-register push `d += 4`; single-register value pop (`pop {rX}`, not fl_pc_end) `d -= 4`, bail if `d < 0`; `add sp, #N` `d -= N`, bail unless `N <= d`. These lines no longer bail per se; the opinfo test still catches every other sp mover. The convertibility offset window check now uses `literal + sz + d` per line (still in the screen walk, before any rewrite). The final live line must leave `d == 0`. The rewrite walk uses `literal + sz + d_at_line`, so both walks must track depth identically - one shared helper if it reads cleaner.

- [ ] **Step 3: Witness passes.** fib now compiles fp-free:

```sh
./compiler/cc -O -S -I lib tests/bench/fib.c -o /tmp/fib.s
sed -n '/^fib:/,/global/p' /tmp/fib.s | head -8
```

Expected: `push {r4, lr}` (or `{r4, r7...}` NO - frameless strips r7), no `mov r7, sp`, the hold load as `ldr r4, [sp, #N]`, both exits `pop {..., pc}`.

- [ ] **Step 4: Behavior + suites.**

```sh
python3 tools/check_frame.py                # ok: frame (new pinned output)
python3 tools/check_prolog.py               # ok: prolog
python3 tools/check_ref.py | tail -1        # different: 0
python3 tools/bench.py --pc 2>&1 | tail -13 # every check line equal to gcc; note the totals
```

- [ ] **Step 5: Commit**

```sh
git add compiler/cc.c tools/check_frame.py tests/opt/frame.c
git commit -m "cc -O: sprel tracks spill depth, fib goes frameless"
```

---

### Task 4: mixed-chain witness (.s callee of an fp-free caller) and full checks

**Files:**
- Modify: `tests/opt/prolog_stub.c` (add a hand-written-frame callee compiled from C whose -O body keeps r7 - see Step 1 for why it must keep one)
- Modify: `tools/check_prolog.py`, `tests/opt/prolog.c`

**Interfaces:**
- Consumes: the existing two-object build in check_prolog.py (`cc -c` stub -> `.o`, linked into prolog.bin).
- Produces: a regression witness that an fp-free `-O` caller and a framed callee share the stack correctly.

- [ ] **Step 1: The witness callee.** A true `.s` file is not needed (and check_prolog.py has no `as` step): what must be proven is a callee cc cannot convert, linked against converted callers. In prolog_stub.c add

```c
int stublea(int a, int b) { int v[4]; int i; v[0] = a; for (i = 1; i < 4; i++) v[i] = v[i - 1] + b; return v[3]; }
```

Compiled with `-O` as its own object it keeps its frame (lea lines bail sprel) exactly like a hand-written `.s` that reads `[r7, #off]`. Verify before pinning: `./compiler/cc -O -c -I lib tests/opt/prolog_stub.c -o /tmp/st.o && ./compiler/cc -O -S ...` is not possible for an object - instead compile the stub source with `-S` separately and confirm `stublea` contains `[r7, #` and `mov r7, sp`.

- [ ] **Step 2: Failing witness.** prolog.c: call it from a converted caller (e.g. inside `csc`'s successor or a new `int cmix(int n) { return csys(n) + stublea(n, 1); }`) and extend the pinned output. Run check_prolog.py -> FAIL on the output string (expected value not yet updated - it fails as an unverified witness first; compute the expected value with the PLAIN build under qemu AND a native gcc build of prolog.c+prolog_stub.c and pin only when both agree).

- [ ] **Step 3: It passes** once Task 1's conversion is in place (it already is): `python3 tools/check_prolog.py` -> `ok: prolog`. If the values disagree between plain/-O, debug now - this task exists to catch exactly that.

- [ ] **Step 4: Whole-tree check.**

```sh
make check 2>&1 | tail -8
python3 tools/check_ref.py | tail -1        # different: 0
python3 tools/check_basic.py --pc 2>&1 | tail -1    # ok: 139 cases
python3 tools/bench.py --pc 2>&1 | tail -13
```

Rebuild the toolchain objects with `-O` and confirm self-recompilation identity on the PC side if quick (`cc -O` building cc.c twice to identical bytes); board selfhost stays a merge-time step needing the user's go-ahead.

- [ ] **Step 5: Commit**

```sh
git add tests/opt/prolog.c tests/opt/prolog_stub.c tools/check_prolog.py
git commit -m "cc -O: witness an fp-free caller against a framed stub callee"
```

---

### Task 5: ledger and spec bookkeeping

**Files:**
- Modify: `docs/superpowers/specs/2026-11-21-cc-sp-relative-design.md` (Status line: implemented on branch cc-prologue, Results section with bench deltas)
- Modify: `.superpowers/sdd/2026-11-21-cc-sp-relative/progress.md` (create; the ledger per AGENTS.md/HANDOVER convention)

- [ ] **Step 1:** Record per-task rulings in the ledger as work proceeds (this task is written first so each earlier task's surprises land somewhere); at the end, add a Results section to the spec: `bench.py --pc` before/after totals and the fib line, plus which corpus functions converted (grep `[sp, #` across tests/bench -S output).

- [ ] **Step 2: Commit**

```sh
git add docs/superpowers/specs/2026-11-21-cc-sp-relative-design.md
git commit -m "docs: sprel spec status and results"
```

---

## Review Focus

- **A line that reads r7 without `#off`** (`cmp r1, r7`, a linked `.s`'s `mov r2, r7`): must bail the whole function. Pinned implicitly (r7ref kind 2 / no-memory-operand rule) - Task 1 Step 2's screen ordering is what makes it bail; a reviewer should grep sprel for any path that converts a line lacking `, [r7, #`.
- **A syscall inside a converted-looking body** (`printf` is a builtin call; `open()` is not): the syscall sequence moves sp with registers and must keep the frame. Pinned by Task 2 Step 4's `csys`.
- **Multi-register struct pushes** (call_ind's address-under-args, struct args): bail, frame stays - Task 3 Step 2 keeps them bailed; reviewer check: no code path adds a multi-register push to the depth walk.
- **vpush/vpop functions** (any float promotion): sprel bails, existing pins keep their exact shapes. Pinned by the untouched fpm/fmany/fearly/cfloat blocks in both checkers - a reviewer should see zero edits there.
- **trimframe interaction**: sprel must never delete a line (its trio test); reviewer check: `git diff` of sprel shows only pset/lend rewrites, no pdel calls.
