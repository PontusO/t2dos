# cc -O: frame reservations without r7 (3b-iii) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Let `-O` make functions with a frame reservation (`sz > 0`) fp-free: `frameless()` rewrites each exit `mov sp, r7` into `add sp, #sz`, and `sprel()` stops bailing on `sz`.

**Architecture:** Extends the committed sprel pipeline (screen walk + acceptance scan + rewrite walk in `sprel()`, then `frameless()`, then `trimframe()`). The slot mapping `[r7, #k] -> [sp, #k+sz+d-4]` already carries `sz`; only the gates refuse it. No ABI, kernel, or non-`-O` change.

**Tech Stack:** C (the cc subset the committed compiler accepts), python3 checkers, qemu/host_linux, RP2350 board.

**Spec:** `docs/superpowers/specs/2026-11-21-cc-sz-frameless-design.md`

## Global Constraints

- Non-`-O` output stays byte-identical: `python3 tools/check_ref.py` -> `different: 0`.
- `cc.c` stays inside the committed cc's C subset (bootstrap: builds with `./compiler/cc` and `build/ref/cc`).
- No em dashes in code comments or commit messages. `git add` explicit paths only; commit only per the steps below (the user has pre-approved this plan's commits).
- Every change round: `touch compiler/cc.c && make` first. `make clean` deletes `./host_linux` - run `make host` before any `--pc` check after a clean.
- Board runs need the user's explicit go-ahead (pre-approved for the Task 3 board gates of this plan).
- Do not dispatch subagents (user rule). Ledger: append to `.superpowers/sdd/2026-11-21-cc-sz-frameless/progress.md` after each task; working notes in `docs/NOTES-sp-relative.md` (untracked).

## Key facts (verified against the code - do not re-derive)

- `sprel()` screen: `if (sz) return 0;` at ~l.1569 and a mirror inside the acceptance scan (the `if (sz) return 0;` block before the frameless-mirror tests). The walk treats `mov sp, r7` as bookkeeping (screen ~l.1495 and 1609 skip it; `fl_excused` ~l.1791 excuses both sp moves) - no walk change is needed.
- The per-line window check `if (k < 0 || k > 1020) return 0;` already bounds `k + sz + d` to ldr/str's 12-bit form; a function whose literals + reservation exceed it keeps its frame without any new gate.
- `frameless()` ~l.1824: gate `if (sz) return 0;` at 1827; the final rewrite loop at 1874-1884 deletes `mov r7, sp` / `mov sp, r7` (`pdel`) and rebuilds the entry/exit lists (`fl_list`).
- `trimframe()` ~l.1887 needs NO change: for `sz > 0` it rewrites the entry trio to `sub sp, #sz` (sz <= 508; long form kept above that) and returns before the pop-copy code, so a converted reserved-frame function never gets an inline copied pop - every early return branches to the exit label, which now drains. Verify this stays true; do not add a drain-copy to the copy loop.
- `mov sp, r7` is emitted once per exit path by flush()/early-return codegen before this pass (single `ins("mov\tsp, r7")` at ~l.4102 plus copies from the same shape); rewrite them all, wherever they sit.
- Exit-arm condition `if (na || ps || d) return 0;` already guarantees the drain lands at a clean window; tail-call windows keep bailing (non-goal).
- `main` and the kernel `run()` launch need nothing: the argv band reads are ordinary `r7` literals and map through the same formula.

## Review Focus

- Every path to a pop of pc drains exactly once (early returns branch to the drained exit; `noret` falls off the end into the same exit) - a missed or doubled drain corrupts the caller's sp. Pinned by Task 1's behavior run and Task 2's `bigprom`.
- A fully promoted function (reservation kept, zero surviving slot reads, `nconv == 0` so `sprel_ok()` is false) must NOT be made frameless by deleting its unwind: frameless only rewrites the unwind when the screen converted, or the frame stays. Pinned by Task 1's `clocals`-style check.
- The drain's immediate form: `add sp, #sz` must assemble for every sz that passes the gates (8-bit `add`, 12-bit `add.w`; sz > 4095 keeps the frame). Pinned by Task 2's `big`-range and `huge` witnesses.
- `trimframe`'s entry test (`pnum(fa, "movw r1, #") == sz&65535` + `sub.w`) must still match after `frameless()` has rewritten lists and drained - run order is sprel -> frameless -> trimframe, none of them touches the trio. Pinned by Task 1's `small` shape.
- Converted `main` reading argv/argc from the band under the kernel on hardware. Pinned by Task 3's board bench + check_sh on PC.

---

### Task 1: lift the gates, rewrite the unwind, flip the pins

**Files:**
- Modify: `compiler/cc.c` (sprel ~l.1569 + acceptance-scan mirror; frameless ~l.1824-1885)
- Modify: `tools/check_frame.py` (`small` pin flips), `tools/check_prolog.py` (`csc`, `clocals` pins flip)
- Test: existing witnesses `tests/opt/frame.c` (`small`), `tests/opt/prolog.c` (`csc`, `clocals`, `rec`)

**Interfaces:**
- Consumes: sprel's committed mapping and acceptance scan unchanged; `frameless(int fa, int sz)` keeps its signature.
- Produces: converted reserved-frame functions: entry `sub sp, #N` (or long trio), no r7 anywhere, each exit drains `add sp, #sz` before its pop; `frameless` still returns 0 (the return value is a formality today - do not change callers).

- [ ] **Step 1: Flip the pins to the converted expectation (failing).** In `tools/check_frame.py`, the `small` block: instead of requiring `mov r7, sp`, require no `r7` in `small` at all, `sub\tsp, #16` as entry reservation form, and some exit of the shape `add\tsp, #16` immediately followed (later in the body) by the `pop\t{` line ending `pc}`; keep the "no empty reservation" tests. In `tools/check_prolog.py`, `csc` and `clocals` (and `rec` if it is pinned framed): require no `r7` in the body; `csc` still loads its parameter, now as `ldr\tr4, [sp, #16]` (k=12, sz=8); `clocals` keeps its drain `add\tsp, #8` before the pop. Run `python3 tools/check_frame.py` and `python3 tools/check_prolog.py` -> both FAIL (frames still there).

- [ ] **Step 2: Implement.** In `sprel()`: delete the screen's `if (sz) return 0;` and change the acceptance-scan mirror to `if (sz > 4095) return 0;` (comment: the drain's add.w immediate window; the 1020 slot window does the rest). In `frameless()`: replace `if (sz) return 0;` with `if (sz && !sprel_ok()) return 0; if (sz > 4095) return 0;` (comment: a converted body drains the reservation with add sp, #sz where mov sp, r7 stood; unconverted reservations keep the frame; sz > 4095 outlives the add.w immediate). In the rewrite loop at ~l.1879: `mov r7, sp` still `pdel`; `mov sp, r7` -> `if (sz) { pset(i); ls("add\tsp, #"); ls(dec(sz)); lend(); } else pdel(i);`. Update the stale comments (the 1827-1832 block, the frameless header, sprel's header citing sz). No trimframe change (see Key facts).

- [ ] **Step 3: Verify shapes and behavior.** `python3 tools/check_frame.py` -> `ok: frame`; `python3 tools/check_prolog.py` -> `ok: prolog`. Witness by eye: `./compiler/cc -O -S -I lib tests/opt/frame.c -o /tmp/f.s` - `small` shows `push {.., lr}`, `sub sp, #16`, `[sp, #N]` reads, `add sp, #16` before each pop path, no `r7` line.

- [ ] **Step 4: Behavior + suites.** `python3 tools/check_ref.py | head -1` (different: 0), `python3 tools/bench.py --pc 2>&1 | tail -2` (11/11), `python3 tools/check_basic.py --pc 2>&1 | tail -1`.

- [ ] **Step 5: Commit**

```sh
git add compiler/cc.c tools/check_frame.py tools/check_prolog.py
git commit -m "cc -O: reserved frames convert too, exits drain with add sp, #sz"
```

---

### Task 2: witnesses at the boundaries and corpus census

**Files:**
- Modify: `tests/opt/frame.c` (add `huge`, adjust `main`), `tools/check_frame.py` (new pins)
- Read-only census: `tests/bench/*.c`, `userland/sh.c` under `-O -S`

**Interfaces:**
- Consumes: Task 1's converted shapes.
- Produces: pinned evidence at the frame-size boundaries and an updated census.

- [ ] **Step 1: Failing boundary witnesses.** Add to `tests/opt/frame.c`:

```c
int huge(void) { char b[5000]; b[4999] = 7; return b[4999]; }   // sz > the add.w drain window: keeps its frame
```

and fold `h = huge();` into main's pinned print (extend the format string and output check consistently - `huge()` returns 7). In `check_frame.py`: `huge` must keep `mov\tr7, sp` and a `pop` of pc containing r7 (the drain window refused it), the entry keeps the `movw r1`/`movt r1`/`sub.w` long form. Run `python3 tools/check_frame.py` -> FAIL until the output string and pins are coherent, PASS with the shape above. Also pin the existing `big` (1000 B): its slot reads exceed the 1020 window so it keeps its frame today - assert the current (framed) shape so a future relaxation shows up as a pin change, not a silent conversion; pin `bigprom` (promoted locals + early return + sz > 0): no `r7` once Task 1 lands, every pop of pc preceded in its block by `add\tsp, #`, and the early return branches to the drained exit (no copied pop).

- [ ] **Step 2: Run the frame suite** -> `ok: frame` (value check: the pinned program output equals the plain build's, so every drain ran exactly once on every path).

- [ ] **Step 3: Whole-tree check.** `make check 2>&1 | tail -6`; `python3 tools/check_sh.py --pc 2>&1 | tail -1`; then the fixed point: `make clean && make OPT=-O && make host && python3 tools/check_sh.py --pc` -> `ok: 5 cases`; restore with `make clean && make`.

- [ ] **Step 4: Census.** For each `tests/bench/*.c` and `userland/sh.c`: `-O -S`, count `mov r7, sp` (real tab: `printf 'mov\tr7, sp'`) before (from the sprel spec's Results table) and after; print the two tables side by side in the ledger entry.

- [ ] **Step 5: Commit**

```sh
git add tests/opt/frame.c tools/check_frame.py
git commit -m "cc -O: witnesses for reserved-frame drains and the drain window"
```

---

### Task 3: docs, board gates, hand-off

**Files:**
- Modify: `docs/superpowers/specs/2026-11-21-cc-sp-relative-design.md` (Results: census + "sz > 0 converts since 3b-iii"), `docs/superpowers/specs/2026-11-21-cc-sz-frameless-design.md` (Status line), `.superpowers/sdd/2026-11-21-cc-sz-frameless/progress.md`

**Interfaces:**
- Consumes: Task 2's census.
- Produces: merge-ready branch `cc-prologue` with board evidence (user pre-approved the board runs for this plan).

- [ ] **Step 1: Ledger + spec updates** per Task 2's numbers; spec Status -> implemented, board pending until Step 2 lands. In the 3b-iii spec, correct the trimframe paragraph to what the code does: `trimframe()` never copies a pop when `sz > 0` (it rewrites the entry to `sub sp, #sz` and returns early), so the drain at the drained exit label is already on every path and no drain-carry was needed.

- [ ] **Step 2: Board gates (user go-ahead on file for this plan).** `python3 tools/check_toolchain.py --selfhost` -> `ok: selfhost` (byte-identical fixed point is THE correctness gate for the new codegen); `python3 tools/bench.py --board 2>&1 | tail -13` -> 11/11 equal to gcc on hardware; record the speedup line in the spec Results. If `put` fails with `''`: the board flash is full of leftover artifacts - clear `/src` junk, not a code bug.

- [ ] **Step 3: Commit**

```sh
git add docs/superpowers/specs/2026-11-21-cc-sp-relative-design.md docs/superpowers/specs/2026-11-21-cc-sz-frameless-design.md
git commit -m "docs: 3b-iii status and board results"
```

- [ ] **Step 4: Report** to the user: census table, board lines, merge/push awaits their word.

---

## Post-merge ideas (not in this plan)

Tail-call windows at exits (the drained-exit form makes `add sp,#N; pop {regs}; b f` the natural next shape), `csys` argv window, literal/`add.w` relaxation in `ld`, register-arg ABI.
