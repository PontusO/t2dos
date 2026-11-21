# `static` keyword Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Full `static` in cc — file-scope internal linkage for variables/functions plus static local variables, initializers on the global-init path. Tests only: `compiler/*.c` do not *use* `static` this round (bootstrap).

**Architecture:** Internal linkage is emitting no `.global` directive (`ld` resolves cross-file against global definitions only and dup-checks only globals — linker/ld.c:11-18, 186-201). Static variables get new symbol classes and no data label: references use `ldref`'s existing `s = 0` form `.Ldata+off` (compiler/emit.c:118-121), so same-named statics in different functions cannot collide. Initializers reuse the global mechanism: constants in the data image, computed init as `.Linit` code with `init_addr`'s `isym = 0` target form. `as` and `ld` need zero changes.

**Tech Stack:** C (cc's own subset) built with gcc on the PC; ARM Thumb asm output; python3 checkers; qemu-arm + `host_linux` for behaviour tests; RP2350 board gates.

**Spec:** `docs/superpowers/specs/2026-11-21-static-keyword-design.md` (settled facts and site list also in `docs/NOTES-static-keyword.md` — read that first after any compaction).

## Global Constraints

- Toolchain sources stay inside the committed cc's subset — this round *implements* the keyword; no toolchain source may *use* `static` (the committed board cc parses these sources to rebuild itself).
- Without `-O`, non-static paths must stay byte-identical: `python3 tools/check_ref.py` keeps `different: 0` on its corpus.
- `static` + `extern` on one declaration: die. `static main`: die.
- New token after `Extern` (operators' order is load-bearing for `expr()`); struct field for functions is `statik` (+ `hstatik` save slot).
- Classes: `CSta = 10` (scalar), `CStaArr = 11` (array, name is the address, mirrors class 6). `CLoc`-keyed loops at stmt.c:323/324 stay `CLoc`-only; restore loops 317/346 must also restore the two new classes.
- Board runs allowed without asking (standing permission); one program on `/dev/ttyACM*` at a time.
- Commit per task; merge/push only on the owner's word.

## Review Focus

Each line is pinned by a named test in the task that owns the code:

1. Two functions in **one file** each declaring `static int i;` — must compile (no duplicate assembly label) and hold separate values → `test15.c`.
2. The **same internal name defined in two translation units** — must link, each file using its own → `tests/toolchain/static_files` case (Task 4).
3. A computed static initializer runs **exactly once, before main** (`.Linit`), for both file-scope and function-scope statics → `test15.c` (init-time side effect visible in main).
4. `-O` must not break statics (they have no frame slots; refs use the literal `.Ldata+off` form) → `test15.c` compiled plain and `-O`, same output.
5. A block-local `int x;` inside a function where the file has `static int x;` — inside the block the local wins, after it the static is restored → `test15.c`.

---

### Task 1: Failing test + check_ref exclusion

**Files:**
- Create: `compiler/tests/test15.c`
- Modify: `tools/check_ref.py` (exclusion lists and report line)

**Interfaces:**
- Produces: `compiler/tests/test15.c` — standard C (gcc `-m32` reference), print/check lines like test4; used by Tasks 2-4 and by `check_toolchain --selfhost`. `check_ref.py` gains `STATIC_TESTS = ('compiler/tests/test15.c',)` (name-excluded before the ref run, reported as `excluded (static syntax): N`).

- [ ] **Step 1: Write `compiler/tests/test15.c`** — header comment in test4's style ("static variables and functions. Standard C: gcc builds it natively for reference output."). Must cover, each with printed lines compared via a `check(cond)` helper or direct expected output:
  - static global int + static function used from two callers;
  - same-named `static int i` locals in two different functions, incremented and printed (Review Focus 1);
  - static int array with constant init `{1,2,3}`, summed in a loop;
  - computed initializers: `static int g = 3 * 7;` at file scope and a static local `static int c = base() + 1;` whose function has a side-effect counter (`gside++`) that `main` prints — proving init ran once before main (Review Focus 3);
  - shadowing: `static int v = 1;` at file scope, a function with a block `{ int v = 2; ... }` printing 2 inside and `v` (1) after the block (Review Focus 5);
  - static function address taken: `fp = helper; fp(4)` and passed as an argument (fnaddr path).
  Integer and pointer arithmetic only — no `%e`, no floats needed (the board runner for Task 4 compares gcc `-m32 -msse2 -mfpmath=sse` stdout verbatim).

- [ ] **Step 2: Reference behaviour** — `gcc -m32 -msse2 -mfpmath=sse -w -o /tmp/opencode/t15 compiler/tests/test15.c && /tmp/opencode/t15` succeeds; record the exact expected output; then the current cc must fail: `./compiler/cc -o /tmp/opencode/t15.bin compiler/tests/test15.c` → nonzero with a parse error at the first `static`.

- [ ] **Step 3: check_ref exclusion** — add `STATIC_TESTS = ('compiler/tests/test15.c',)` (docstring: sources using `static`, unknown to the pinned ref cc `e4fb427`, which fails with a generic parse error so exclusion is by file name). In the per-source loop, skip before running the ref cc, like `NEWER_BUILTINS`. Extend the summary line with `excluded (static syntax): X` and print the names as the other lists do. Update the module docstring.

- [ ] **Step 4: Verify** — `python3 tools/check_ref.py` → `different: 0`, `excluded (static syntax): 1`. (cc still fails test15; that is expected here and checked per test in Task 2.)

- [ ] **Step 5: Commit** `git add compiler/tests/test15.c tools/check_ref.py && git commit -m "static: test15 (static vars, fns, init) + check_ref name exclusion"`

### Task 2: File-scope `static` (linkage for vars and functions)

**Files:**
- Modify: `compiler/cc.h` (token enum ~21-27, class enum ~29, `struct Sym` ~42-45)
- Modify: `compiler/cc.c` (keyword registration ~55)
- Modify: `compiler/stmt.c` (`parse_global` ~280-346, function label ~326, `init_addr` ~25, restore loops 317/346)
- Modify: `compiler/expr.c` (primary dispatch ~148-169)

**Interfaces:**
- Consumes: Task 1's test15.c (file-scope cases must pass by task end).
- Produces: token `Static` (last in enum, after `Extern`); classes `CSta = 10`, `CStaArr = 11`; `struct Sym` fields `int statik;` and `int hstatik;`; the `gsym` list holds only label-bearing globals (statics are never registered there).

- [ ] **Step 1: cc.h** — append `Static` after `Extern` in the token enum; append `, CSta = 10, CStaArr = 11` to the class enum (extend its trailing comment: `10/11 = static var/array (no label, .Ldata refs)`); add `int statik;` to `struct Sym` after `hval` and `int hstatik;` after `mdef` (keep the existing comment style).
- [ ] **Step 2: Keyword** — cc.c: replace `p = "extern"; next(); id->tk = Extern;` with the two-word loop `p = "extern static"; i = Extern; while (i <= Static) { next(); id->tk = i; i = i + 1; }` (pattern of the first keyword line above it).
- [ ] **Step 3: Parse global declarations** — in `parse_global`, make the storage flags order-insensitive: read `static` and `extern` (either order) into `st`/`ext`; `if (st && ext) die("static and extern");`. Function definition path: `if (s->statik && !st) die("static mismatch"); if (st) s->statik = 1;` **before** the label output at ~326, and emit `.global` only when `!s->statik` (the `NAME:` label stays). Function forward declaration (`;`-terminated): `if (st) s->statik = 1;` leaving class 0. Variable definition branch: `if (st && (s->class == 7 || s->class == 9)) die("static after extern");` then allocate exactly as the existing branch (align dpos, val = dpos, dpos advance, array sizing) but set class `CStaArr`/`CSta` instead of `6`/`CGlo` and **do not** append to `gsym`/`goff` (write_data then needs no change — storage lives in the image, no label); initializer block: `isym = 0;` (was `s`). Existing `die("duplicate global")` checks already reject mixing static and non-static definitions — verify, don't duplicate.
- [ ] **Step 4: `static main`** — where `parse_global` finishes binding a function name: `if (s->statik && strcmp(s->name, "main") == 0) die("static main");` (strcmp is a builtin; compare with `s->len == 4` too if the codebase style prefers).
- [ ] **Step 5: References + init target** — expr.c primary: next to `class == 6` (~160) add `else if (d->class == CStaArr) { lea_data(0, 0, d->val); ty = d->type; }`; in the scalar block (~163-166) add `else if (d->class == CSta) { lea_data(0, 0, d->val); ty = d->type; }` before the `die("undefined variable")`. stmt.c `init_addr`: `else if (isym) lea_data(isym, off, ibase + off); else lea_data(0, ibase + off, ibase + off);`.
- [ ] **Step 6: Restore loops** — 317 and 346: condition becomes `class == CLoc || class == CSta || class == CStaArr`, body also restores `s->statik = s->hstatik;`; `newlocal` (131) saves `s->hstatik = s->statik;` alongside the other `h` fields. 323/324 unchanged.
- [ ] **Step 7: Build + gates** — `make` (gcc builds the new cc). Scratch file `/tmp/opencode/st2.c`: static global + static function used from two places, `main` printing them; compile with `./compiler/cc`, run under `qemu-arm ./host_linux -r root ...`, expect the recorded values. Then `python3 tools/check_ref.py` → `different: 0` (test15 excluded; static *locals* don't parse yet — the whole test still fails to compile, that is fine this task).
- [ ] **Step 8: Commit** `git commit -am "static: file-scope linkage - no .global, CSta classes, .Ldata refs"`

### Task 3: Static local variables + full test15

**Files:**
- Modify: `compiler/stmt.c` (local-declaration branch ~227-250; new `newstatic()` next to `newlocal` ~131)

**Interfaces:**
- Consumes: Task 2's `CSta`/`CStaArr`, `newstatic` follows `newlocal`'s save pattern incl. `hstatik`.
- Produces: `newstatic(struct Sym *s, int t, int size)` — file-scope storage, block-visible name; no other file changes interfaces.

- [ ] **Step 1: `newstatic`** — mirror `newlocal`: save `hclass/htype/hval/hstatik`, `dpos = (dpos + 3) & -4;`, set class `CSta` (scalar) or `CStaArr` (arrays: `type = t + PTR`, `val = dpos` as the address, mirroring the class-6 global-array treatment — no frame slot, no array pointer cell), `dpos = dpos + size`, do not register in `gsym`.
- [ ] **Step 2: Local branch** — at ~229: `else if (tk == Static || is_type())`; inside, `st = 0; if (tk == Static) { st = 1; next(); if (!is_type()) die("type expected after static"); }`, then the existing `parse_type`/`declarator` loop. When `st`: call `newstatic` instead of `newlocal` (arrays: size product like the global path, skip the slot-pointer lines), and for initializers set `ikind = 0; isym = 0; ibase = d->val;` and call the same `init_array`/`init_scalar`/`init_struct` calls the global branch uses (scalar/array/struct cases as in stmt.c ~245-249 and ~379-381).
- [ ] **Step 3: Full test15 green (PC)** — `./compiler/cc -o root/tmp/t15.bin compiler/tests/test15.c && qemu-arm ./host_linux -r root /tmp/t15.bin` output equals the gcc reference from Task 1 Step 2; repeat with `-O` → same output (Review Focus 4).
- [ ] **Step 4: Gates** — `make check` ; `python3 tools/check_ref.py` → `different: 0`; the existing tests still match gcc: rerun the split-round loop for test4..test12 under qemu (plain); `python3 tools/bench.py --pc` → `ok: 11 benchmarks` (statics exercise no bench path, but the -O passes saw the restore-loop edit).
- [ ] **Step 5: Commit** `git commit -am "static: static locals via data-image allocation (.Ldata refs, .Linit init)"`

### Task 4: Error cases + cross-file independence case

**Files:**
- Create: `tests/toolchain/pipeline_errors/statext.c` + `.expected`, `statmain.c` + `.expected`, `statdup.c` + `.expected`
- Modify: `tests/toolchain/` case list + `tools/check_toolchain.py` (`CASES`, and selfhost `tests` list line ~524)
- Modify: `compiler/tests/test15.c` (only if a pinned case exposes a gap)

**Interfaces:**
- Produces: three error cases + one multi-file case run by `check_toolchain --pc` and `--board`-side error runners; selfhost runs `test15` on the board.

- [ ] **Step 1: Error cases** — `statext.c`: `extern static int x;` → expect die "static and extern". `statmain.c`: `static int main(void) { return 0; }` → "static main". `statdup.c`: `static int x; int x;` → existing "duplicate global". Each `.expected` holds the real first error line (`NAME.c:LINE: error: msg`) + `exit 1` — produce from the PC run, convention as in `syntax.expected`.
- [ ] **Step 2: Cross-file case** — two sources defining the *same* internal names with *different* bodies (`static int dup(void)` and `static int v` in each), one `main` calling its own `dup` and a non-static `int other(void)` from the second file that returns that file's `v`: links with no "duplicate symbol", output = each file's own values (Review Focus 2). Add to `tools/check_toolchain.py` `CASES` as a two-source case compiled in one `cc` command; `.expected` output per the runner's convention.
- [ ] **Step 3: Selfhost list** — line ~524: `tests = [... range(4, 13)] + [('test15', '')]` (docstring line ~23: `test4..12` → `test4..12 and test15`).
- [ ] **Step 4: Verify** — `python3 tools/check_toolchain.py --pc` → `ok: ... 6 error cases ...` (three new + three old) and the new CASES entry passes.
- [ ] **Step 5: Commit** `git commit -am "static: error cases (static+extern, static main, redefinition) + two-file internal-linkage case"`

### Task 5: Board gates + docs

**Files:**
- Modify: `docs/HANDOVER.md` (§5 state), `README.md` (language subset gains `static`), spec file (append a `## Results (landed ...)` section like the split spec), `docs/NOTES-static-keyword.md` (final state)

**Interfaces:**
- Consumes: everything above; board via `tools/board.py` wrappers only, one program per port.

- [ ] **Step 1: Board suite** — `python3 tools/check_toolchain.py --selfhost` (the *committed* board cc must rebuild the new cc byte-identically — it parses `static` in sources but no source *uses* it — then runs test4..12 + test15 built on-board; if test13's `%e` tempts anyone: out of scope, leave `range(4, 13)`). Then `python3 tools/check_make.py --board`. Both may take many minutes; run sequentially (shared port).
- [ ] **Step 2: Docs** — HANDOVER §5: a new numbered entry (static keyword, spec path, semantics: globals-init for statics, toolchain itself still static-free pending adoption round). README: the subset list gains `static` with the constant-or-.Linit init constraint noted. Spec: Results section (what changed vs design: statics never enter `gsym`, so `write_data` untouched — cleaner than the spec's "skip the label"; net effect identical — and note it in NOTES as settled). NOTES file: status ticks + anything learned.
- [ ] **Step 3: Final full PC pass on the exact tree** — `make && make check` tail; `check_ref` → `different: 0`; `check_make --pc`; `bench.py --pc`; `check_toolchain --pc`. Commit docs.
- [ ] **Step 4: Report to the owner** — summary + gates table + rulings; merge/push **only** on their word (they have historically said so per round).
