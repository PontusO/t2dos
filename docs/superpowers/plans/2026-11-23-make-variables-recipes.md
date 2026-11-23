# make GNU-ish dialect (variables + command targets) Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Give the board's `make` build variables (`:= = ?= +=`, command-line overrides, `$(NAME)` expansion with nesting) and TAB-indented command recipes (`.PHONY`, `.DEFAULT_GOAL`, `$@ $< $^`, `@` silence, `V=1`), plus `rm -rf` and `mkdir -p`; always-build and byte-identity untouched.

**Architecture:** `make.c` becomes a two-pass loader (variables first, then rules, so use-time expansion sees the whole file), expands each target's flags/prereqs once into a persistent per-target epool row at plan time (recursion-safe argv storage), and executes recipe lines through the existing `exec()` path. `rm.c`/`mkdir.c` stay tiny one-file tools; `rm -r` uses a reopen-dir rescan loop so no mutation happens during readdir.

**Tech Stack:** T2DOS C subset (committed cc), `tools/check_make.py` (qemu+host_linux dry-run plans on PC; real builds on board), GNU `rm`/`mkdir` flag conventions.

**Spec:** `docs/superpowers/specs/2026-11-23-make-variables-recipes-design.md` (owner-approved 2026-11-23; its grammar sketch and tests section are binding). Prior dialect spec `2026-11-21-make-utility-design.md` stays binding where this does not extend.

## Global Constraints

- `make.c`, `rm.c`, `mkdir.c` stay inside the committed cc's subset AND the string discipline of `docs/NOTES-star-deref.md`: no `*p` rvalue deref, no strings held in `char *` variables (globals/params indexed by int are fine; `p[i]` indexing is proven correct; pointer-valued storages only in the sh.c sargv idiom — a stored address passed straight to a builtin). No 2-D char arrays; use flat arrays with `row * WIDTH + i`.
- Every cap is explicit and reported in the file's existing idiom (`make: %s: line %d: ... (cap %d)` + `exit(1)`). New caps, exact values: `MAXVAR 32`, `MAXVAL 512`, `MAXEXP 1024` (per-target expanded flags+prereqs row, and recipe working buffer), `MAXEXPASS 8` (expansion passes = nesting depth), `MAXRECI 8` (recipe lines per target), `MAXPHONY 16` names, phony-name rows `MAXPHNAME 64`. Existing: `MAXLINE 128`, `MAXTARGETS 64`, `MAXPRE 16`, `MAXARGS 32`, `MAXBUF 16384`.
- Expansion model (spec): values stored verbatim; `$(NAME)` resolves at USE time against the fully loaded table; forward references work everywhere; undefined expands empty; operators `:= = ?= +=` differ only in whether/when they assign; command-line rows are final (file definitions to them are ignored, any operator); recursion is caught by the 8-pass cap and errors, never hangs.
- `-n` plan form for recipes: RESOLVED argv (word 0 mapped), byte-same text as the `-v` echo, `@` stripped (owner decision in spec).
- No absolute addresses; no `char *` string locals; kernel untouched (no new syscalls); `make check`, `check_ref` (`make.c`/`rm.c`/`mkdir.c` are corpus — plain-path bytes must stay ref-identical) and the 13 existing `check_make --pc` cases must pass unchanged after EVERY task.
- Board runs need the owner's explicit go-ahead (Task 7). One serial holder; never kill the user's screen.

## File Structure

- `src/userland/rm.c` — rewrite: `-r`/`-f` (Task 1). `src/userland/mkdir.c` — `-p` (Task 2).
- `src/userland/make.c` — variables core: two-pass load, variable table, `expand()`, `prep()` (Task 3); recipes: TAB attach, execution, auto variables (Task 4); `.PHONY`/`.DEFAULT_GOAL`/reserved names (Task 5).
- `tools/check_make.py` — all new PC plan cases + PC tool cases (`tool_cases()`); board feature legs (Task 7).
- `Makefile.board` — rewritten with variables, zero fixture change (Task 6).
- `README.md` (new board-make section), `docs/HANDOVER.md`, `make.c` header/usage, `Makefile.board` header, `check_make.py` docstring, spec Results (Task 7).

## Interfaces

Task 3 produces in `make.c` (Tasks 4–5 consume these exact names):

- globals: `char varnam[MAXVAR * MAXLINE]`, `char varval[MAXVAR * MAXVAL]`, `int vused[MAXVAR]`, `int vcli[MAXVAR]`; `char epool[MAXTARGETS * MAXEXP]`; `char lbuf[MAXEXP]`, `char sbuf[MAXEXP]`; `int exowner` (recipe context: target index, or -1 outside recipes).
- `int findvar(int off, int len)` — buf token vs varnam rows, row index or -1 (int-indexed compare helper `xcmpvar(k, off)` beside `xcmpnam`).
- `int expandtok(int src)` — expand buf token at `src` into `lbuf`, returns length. Innermost-first iterative passes (a pass scans `lbuf`, inlines every `$(NAME)` whose body contains no `$(`, undefined → empty, `$@`/`$<`/`$^` substitute when `exowner >= 0`), max `MAXEXPASS` passes then error `make: %s: variable nesting too deep`. Unterminated `$(` → `make: %s: unterminated $(`. Auto variable with `exowner < 0` → `make: %s: $ variable outside a recipe`. Overflow of `MAXEXP` → `make: %s: expansion too long (cap %d)`.
- `void prep(int i)` — run once per target at the top of `resolve()`: expands each raw token in file order into epool row `i`, re-splits flags/prereqs with the old rule (leading `-` after expansion; `-I` takes the next expanded word; caps `MAXARGS`/`MAXPRE` counted on expanded words), classifies `tpk`, and AFTER writing the row sets `tpre[]`/`tflag[]` entries to epool offsets (`i * MAXEXP + k`, same int type — the discipline is honored by swapping the indexed array, never aliasing a `char *`). Writes an intact copy of the space-joined prereq list at the row end and sets `tphat[i]` (its epool offset, -1 if none) and `tfirst[i]` (first prereq word offset, -1 if none). Overflow → `expansion too long` error.
- `void setvar(...)` semantics table: `:=`/`=` overwrite verbatim; `?=` only if `!vused`; `+=` appends `" " + value` to the stored raw text; all four skip rows with `vcli[k] == 1`.

Task 4 produces: `int trcl[MAXTARGETS * MAXRECI]` (buf offsets of recipe lines), `int trcn[MAXTARGETS]`; `void run_recipe(int i)` called at the end of `resolve()` for any target with `trcn[i] > 0` (both the `.bin` and collection branches). Word-0 mapping inside `run_recipe`: word contains `/` → use as given; else append `/bin/<word>.bin` to the row after the last expanded word and point `cmd[0]` there.

Task 5 produces: `char phony[MAXPHONY * MAXPHNAME]`, `int nphony`; `char defgoal[MAXLINE]`. (The phony rows are recorded for reserved-name handling now and a future timestamp round; no matcher function yet — YAGNI until timestamps need it.)

## Review Focus

Failure modes no single task's happy-path tests cover; each gets a pin in the named task:

- **Expansion storage across recursive `resolve()`** — a parent building cc args from epool must not be clobbered by a child target's prep/run: words live in per-target rows, never shared scratch; pin: a collection of two variable-driven `.bin` targets plans both correctly in order (Task 3).
- **Empty expansion produces no token** — `all: $(UNDEFINED) real.c` must not yield a phantom prereq or empty flag; flags are recognized on the EXPANDED word (pin: `Z := -I` + `Z += /src/mk` produces the `-I /src/mk` flag pair, Task 3).
- **Command-line values are verbatim and may reference file vars** — `make FL=$(O.release) x.bin` expands at use time; a CLI row is never re-assigned by a file line of any operator (pin: Task 3 case pair).
- **TAB semantics change** — leading TAB (first non-blank char) is now recipe-only; TAB-indented comment/blank lines are silently skipped; a recipe before any target is an error (pins: Task 4). Old parser accepted a TAB-indented target line; accepted per spec's scoped anti-goal; README states it.
- **`rm -rf` loops forever or half-deletes** — a failing entry unlink must abort that directory (not rescan forever); a non-empty dir without `-r` errors; `-f` only excuses a missing path (pins: Task 1 PC cases).
- **word-0 mapping boundary** — a recipe word containing `/` runs as-is (no `/bin//src/x.bin`, no `.bin` suffix); missing program fails at run time with exit != 0, but `-n` prints it without an existence check, so PC plans never depend on board binaries (pin: Task 4).

---

### Task 1: `rm` gains `-r` and `-f`

**Files:**
- Modify: `src/userland/rm.c` (full rewrite, 6 lines today)
- Modify: `tools/check_make.py` (new `tool_cases()` + call it in `--pc`)

**Interfaces:**
- Consumes: nothing.
- Produces: `rm -r` (recurse into dirs, depth-first, reopen-rescan loop), `rm -f` (missing path → success), combinable `-rf`/`-fr`; unknown flag → usage line + exit 1. Board legs (Task 7) run `/bin/rm.bin -rf`.

- [ ] **Step 1: Write the failing PC cases** in `tools/check_make.py`: a `tool_cases()` function (run from `pc_cases()` after the plan cases) that uses `stage()` + `os.makedirs` to build `root/tmp/rmt/` with files `a.txt`, `sub/b.txt`, `sub/deep/c.txt`, then via `run_tool = ['qemu-arm','./host_linux','-r','root','/bin/rm.bin'] + args` (same runner style as `run_make`):
  - `rm-tree`: `rm -rf /tmp/rmt` → exit 0, and `os.path.isdir(root/tmp/rmt)` is False on the host.
  - `rm-dir-norec`: recreate tree; `rm -rf` NO — `rm /tmp/rmt/sub` → exit != 0, message names `sub`.
  - `rm-missing-fail`: `rm /tmp/rmt/nope` → exit != 0.
  - `rm-missing-force`: `rm -f /tmp/rmt/nope` → exit 0, no output.
  - `rm-file`: `rm /tmp/rmt/a.txt` → exit 0, file gone, tree else intact.
  - `rm-badflag`: `rm -q /tmp` → exit != 0 with `usage` in output.
  Add `import os` helpers already present; extend `cleanup()` to `shutil.rmtree(root/tmp/rmt)` (already covered by `shutil.rmtree(MKTMP...)`? no — rmt is `root/tmp/rmt`, add explicitly).
- [ ] **Step 2: Run to verify failure** — `make 2>&1 | tail -2 && python3 tools/check_make.py --pc 2>&1 | tail -3`: expect FAIL on `rm-tree` (current rm treats `-rf` as a filename).
- [ ] **Step 3: Rewrite `src/userland/rm.c`.** Structure (keep the file's terse comment idiom; document the rescan):
  - `int rm1(char *path, int force, int rec)` returns 0/1: `stat(path,&st)` fails → `force ? 0 : (printf("rm: %s failed\n", path), 1)`. `!st.isdir` → `unlink`. `isdir && !rec` → `printf("rm: %s is a directory\n", path); return 1`.
  - Directory with `-r`: rescan loop — `{ open dir; take FIRST entry name; close dir; if none: break; join(child, path, name) [rcp.c join idiom: dir + '/' + name into local char[200]]; if rm1(child,...) { printf and return 1 } }` then `closedir` done, `rmdir(path)`. Reopening per entry means readdir is never iterated while the directory mutates, and there is no entry-count cap. Any child error aborts (prevents infinite rescans on an entry that will not die).
  - `main`: argv loop; token starting `-`: parse chars `r`/`f` into flags, unknown char → `printf("usage: rm [-rf] file...\n")` exit 1. Non-flag → `rm1`, OR the return into `r`. `return r;`
- [ ] **Step 4: Gates** — `make 2>&1 | tail -1 && python3 tools/check_make.py --pc 2>&1 | tail -2 && python3 tools/check_ref.py 2>&1 | head -1` (expect `different: 0`; rm.c is corpus and plain-path must stay ref-identical) `&& make check 2>&1 | tail -1`.
- [ ] **Step 5: Commit** — `git add src/userland/rm.c tools/check_make.py && git commit -m "rm: -r recurses (reopen-rescan), -f tolerates missing"` (plus tool-case runner note in the body).

### Task 2: `mkdir` gains `-p`

**Files:**
- Modify: `src/userland/mkdir.c`
- Modify: `tools/check_make.py` (extend `tool_cases()`)

**Interfaces:** Produces `mkdir -p` (create every parent; existing dir is not an error). Task 7 board legs use `mkdir -p a/b/c`.

- [ ] **Step 1: Failing cases** — `mk-p`: `mkdir -p /tmp/mkt/sub/deep` → exit 0, both exist on host; `mk-p-exists`: same again → exit 0 (idempotent); `mk-exists-fail`: `mkdir /tmp/mkt` (exists) → exit != 0; `mk-plain`: `mkdir /tmp/mkt/x` → exit 0; `mk-nop-parent-fail`: `mkdir /tmp/mkt/ghost/x` → exit != 0. Cleanup: `shutil.rmtree(root/tmp/mkt)` in `cleanup()`.
- [ ] **Step 2: Verify failure** (`make && check_make --pc` → FAIL `mk-p`).
- [ ] **Step 3: Implement** — copy arg into a local `char p[200]`; under `-p`, walk index `k` over the copy: at each `/` (skip leading-duplicate and trailing) NUL-terminate, `stat(p,&st)` → `isdir` ? continue : `mkdir(p)`; restore the byte and continue; final component last; under `-p` an existing dir is fine, a failed mkdir (not-exist-parent is impossible under -p since parents are made first — real errors like EEXIST-on-file) prints `mkdir: %s failed` and returns 1. Without `-p`: unchanged behavior, one `mkdir(arg)` per argument, first failure exits 1. Flag parse like rm's, unknown → `usage: mkdir [-p] dir...`.
- [ ] **Step 4: Gates** (same four commands as Task 1 Step 4).
- [ ] **Step 5: Commit** — `mkdir: -p creates parents, tolerates existing directories`.

### Task 3: make variables core (two-pass load, expansion, `prep`, `V=1`)

**Files:**
- Modify: `src/userland/make.c`
- Modify: `tools/check_make.py`

**Interfaces:** Consumes nothing from other tasks. Produces the Interfaces-block names (`findvar`, `expandtok`, `prep`, `exowner`, `epool`, `tphat`, `tfirst`, variable table) for Tasks 4–5.

- [ ] **Step 1: Failing PC cases** in `check_make.py` (`pc_cases()`, after case 11 keep numbering as comments). Stage `/src/mk/c.c` alongside the existing stage list. Use `run_make(['-n','-f','/tmp/make/Makefile'] + args)` via `expect_ok`/`expect_err`:
  - `plan-assign` / `plan-assign-cli` (the CLI values prove the file's `:=`, `+=` (of `Z`) and `?=` (of `W`) all lost to it; `-O` is a plain cc flag, so the flag/prereq split stays readable):
    ```python
    amk = ('T := /src/mk/a.c\nU = /src/mk/b.c\nW ?= /src/mk/c.c\nZ := -I\nZ += /src/mk\n'
           '/src/mk/p.bin: $(Z) $(T) $(U) $(W)\n')
    expect_ok('plan-assign', amk, [],
              ['/bin/cc.bin -I /src/mk -o /src/mk/p.bin /src/mk/a.c /src/mk/b.c /src/mk/c.c'])
    expect_ok('plan-assign-cli', amk, ['W=/src/mk/b.c', 'Z=-O'],
              ['/bin/cc.bin -O -o /src/mk/p.bin /src/mk/a.c /src/mk/b.c /src/mk/b.c'])
    ```
  - `plan-expand` (nesting + `?=` + forward ref + CLI value referencing a file var + target-name expansion), three asserts on one file:
    ```python
    exp = ('O.debug := -O\nO.release := -DREL\nBLD ?= debug\nFL := $(O.$(BLD))\n'
           '/src/mk/e.bin: $(FL) $(S) /src/mk/hdr.h\nS := /src/mk/a.c\n')
    expect_ok('plan-expand', exp, [], ['/bin/cc.bin -O -o /src/mk/e.bin /src/mk/a.c'])
    expect_ok('plan-expand-release', exp, ['BLD=release'],
              ['/bin/cc.bin -DREL -o /src/mk/e.bin /src/mk/a.c'])
    expect_ok('plan-expand-cli-var', exp, ['FL=$(O.release)'],
              ['/bin/cc.bin -DREL -o /src/mk/e.bin /src/mk/a.c'])
    expect_ok('plan-expand-tname', 'T := /src/mk/t1\n$(T).bin: /src/mk/a.c\n', ['/src/mk/t1.bin'],
              ['/bin/cc.bin -o /src/mk/t1.bin /src/mk/a.c'])
    expect_ok('plan-expand-empty', '/src/mk/g.bin: $(NOPE) /src/mk/a.c\n', ['/src/mk/g.bin'],
              ['/bin/cc.bin -o /src/mk/g.bin /src/mk/a.c'])
    expect_err('err-unterminated', '/src/mk/u.bin: $(A\n', ['/src/mk/u.bin'], 'unterminated')
    expect_err('err-recursion', 'A := a $(A)\n/src/mk/r.bin: $(A) /src/mk/a.c\n',
               ['/src/mk/r.bin'], 'nesting')
    # Review Focus pin: per-target epool rows survive nested resolve()
    expect_ok('plan-expand-coll',
              ('A := /src/mk/a.c\nB := /src/mk/b.c\n/src/mk/x.bin: $(A)\n/src/mk/y.bin: $(B)\n'
               'coll: /src/mk/x.bin /src/mk/y.bin\n'), ['coll'],
              ['/bin/cc.bin -o /src/mk/x.bin /src/mk/a.c',
               '/bin/cc.bin -o /src/mk/y.bin /src/mk/b.c'])
    ```
    (`-DREL` is legal cc input: cc usage has `-D name[=v]`. `hdr.h` already staged.)
- [ ] **Step 2: Verify failure** — `make 2>&1 | tail -2 && python3 tools/check_make.py --pc 2>&1 | tail -3` (FAIL at `plan-assign`).
- [ ] **Step 3: make.c — variable table and pass A.** Add globals per the Interfaces block. `int isdef(int st, int e)`: skip spaces (not tabs); empty → 0; name = run of chars not in `" \t:?=+"` (tab included in the stop set, so `  \tcmd` has an empty name → not a definition); empty name → 0; skip spaces; then `':' && next '='` → 1, `'?' '='` → 1, `'+' '='` → 1, `'='` → 1, else 0. `void scanvar(int st, int e, int lineno)`: comment-strip, extract name/value/operator per `isdef`, trim value; name `.DEFAULT_GOAL` → copy value to `defgoal[MAXLINE]` (Task 5 uses it; store now, honor in Task 5) and return; `setvar`: find row by name (append row if new, cap `MAXVAR` error `too many variables (cap %d)`); `vcli` rows win (return silently); copy value verbatim (`> MAXVAL-1` → `value too long (cap %d)`); `?=` skip if `vused`; `+=` append `" "` + value to the stored text. `loadfile` gains pass A: after reading, line-loop calling `if (buf[st] != '\t' && isdef(st, e)) scanvar(st, e, lineno);` (a second, cheap line scan — no line-index array).
- [ ] **Step 4: make.c — `expandtok`.** Innermost-first passes exactly as in the Interfaces block. `$(` scan for a token body not containing `$(`; `findvar` by int-length compare; undefined → remove; `$@`/`$<`/`$^` (a `$` not followed by `(`): `exowner >= 0` substitutes namstr row / `buf + tfirst[exowner]`... use `epool + tfirst[exowner]`/`epool + tphat[exowner]` (valid in recipes: `prep` has run); `exowner < 0` → error. Any other `$x` → `make: %s: bad $ reference`. Passes ping-pong lbuf/sbuf; final result ends in `lbuf`; caps per Interfaces. Keep all copies via `strcpy`-free int-indexed loops or `strcpy` with `array + offset` arguments (the shipped opool pattern).
- [ ] **Step 5: make.c — pass B and `prep`.** Pass B (the old line loop): line whose first non-blank char is TAB → store as recipe for Task 4 but for now count `trcn` and keep `trcl` offsets, attached to `lastt` (error `recipe before any target` if none — implement the storage now, execution comes in Task 4; `-n` must NOT print recipes yet, so Task 3's tests never see them). `isdef` lines → skip. Else `parse_line`, changed: strip comment; after the name is trimmed, if it contains `$(`: `expandtok` (lbuf) then copy to the namstr row (cap error `target name too long`); `.PHONY` handling arrives Task 5 (leave a named gap comment). Flags/prereqs: keep the old ntok split into `tflag`/`tpre`, but NO cap/type checks at parse (tokens may hold `$(...)`): store raw tokens in file order in `tpre` alone, `tpn` = raw count, `tfn` = 0; delete the parse-time flagsdone logic. New `prep(i)` (Interfaces block) does flag/prereq re-splitting, `tpk` classification, `-I needs an argument` (`make: %s: -I needs an argument (target %s)`), caps `too many flags/prereqs`, and sets `tfirst`/`tphat`; `exowner = -1` during prep (auto variables there error). Call `prep(i)` first thing in `resolve()`. `resolve()` otherwise unchanged (its `tpre`/`tflag` reads now hit epool offsets, transparently).
- [ ] **Step 6: make.c — argv variables and `V`.** In `main`'s argv loop, before the request-target fallback: a token containing `=` → split in place (write NUL into argv, the sh.c writable-argv pattern; name = before, value = after), `setvar(..., from_cli = 1)`; if the name is `V` and the value is non-empty → `verbose = 1`. Insert CLI rows BEFORE `loadfile` so the file cannot override them.
- [ ] **Step 7: Gates + no-regression of the 13 existing cases.** Same four commands as Task 1 Step 4. The old `cap-exact`/`cap-over` cases must still pass — they count words after (identity) expansion, so moving the check into `prep` is behavior-preserving. `err-flags-eol` message keeps containing the target name.
- [ ] **Step 8: Commit** — `make: build variables (=, :=, ?=, +=, command line, $(NAME) use-time expansion)` (note in body: use-time model, 8-pass nesting cap, per-target epool rows).

### Task 4: recipes — TAB lines, execution, `$@ $< $^`, `@`, word-0 mapping

**Files:**
- Modify: `src/userland/make.c`
- Modify: `tools/check_make.py`

**Interfaces:** Consumes `prep`/`tfirst`/`tphat`/`exowner`/`lbuf` from Task 3. Produces `trcl`/`trcn`/`run_recipe(i)`.

- [ ] **Step 1: Failing cases.**
  ```python
  rec = ('T := /src/mk/r\nall: $(T).bin\n\t# a tabbed comment\n'
         '\trm -f gone.txt\n\techo built $(T).bin $@ $^\n'
         '$(T).bin: /src/mk/a.c\n\techo cc $< into $@\n'
         '\t/bin/echo.bin direct\n')
  expect_ok('plan-recipe', rec, ['all'],
            ['/bin/echo.bin cc /src/mk/a.c into /src/mk/r.bin',
             '/bin/echo.bin direct',
             '/bin/rm.bin -f gone.txt',
             '/bin/echo.bin built /src/mk/r.bin all /src/mk/r.bin'])
  # (plan touch-up, Task 4 ruling: the first draft's want interleaved the
  # prereq's recipe block around all's and evaluated $@/$^ in the wrong
  # context; this is the GNU-consistent order: r.bin's whole block runs
  # during the prereq's resolve, then all's, with $@=all $^=/src/mk/r.bin)
  expect_ok('plan-recipe-quiet', 'q:\n\t@echo hushed\n', ['q'], ['/bin/echo.bin hushed'])
  expect_err('err-recipe-first', '\techo orphan\nx: /src/mk/a.c\n', ['x'], 'recipe')
  ```
  Semantics pinned here: a `.bin` target WITH a recipe gets NO implicit cc call (the `all` plan contains no `/bin/cc.bin` line); a recipe-less `.bin` prereq keeps it; recipe of the PREREQ runs during the prereq's resolve (order in `want`); `$^` = expanded prereqs; word-0 mapping `echo` → `/bin/echo.bin`, `/bin/echo.bin` as-is; `@` stripped in the resolved plan.
- [ ] **Step 2: Verify failure** — FAIL (`no colon` on the first TAB line).
- [ ] **Step 3: make.c — attach.** Pass B: first non-blank char is TAB → strip comment from the trimmed start; blank/comment-only → skip silently; else store `buf` offset in `trcl[lastt * MAXRECI + trcn[lastt]]` (cap `too many recipe lines (cap %d)`); error `make: %s: line %d: recipe before any target` when `lastt < 0`. `lastt` = index of the last parsed target. (Task 3 stubbed the error path; now it is the real attach.)
- [ ] **Step 4: make.c — `run_recipe(int i)`.** For each stored line: `exowner = i`; `expandtok(lineoff)` → lbuf; strip a leading `@` from the expansion result into `quiet` (GNU: `-n` prints the line, `-v` does not echo a quiet line); tokenize lbuf with an `ntokl()` clone over lbuf (mirror of `ntok`, same globals shape); word 0: contains `/` → `cmd[0] = lbuf + off`; else append `/bin/<word>.bin` at the row end after the last token (a small epool tail region or just after lbuf content, both outlive `emit_cmd`) and point `cmd[0]` there; `nw` from `addw` as elsewhere. If not `quiet`: `emit_cmd()` unchanged handles `-n` print, `-v` echo (`quiet` suppresses the `-v` echo: pass a flag or clear `verbose` around the call — do it with a save/restore, not a global clobber). `exowner = -1` after. Call `run_recipe(i)` as the last action in `resolve()` before `tvis[i] = 2`, in BOTH branches (isbin and collection). No word-0 existence check (GNU `-n` plans blindly); a real run gets `exec` failure → the existing `emit_cmd` failed path prints and exits with the child code.
- [ ] **Step 5: make.c — implicit cc now conditional.** In `resolve()`'s isbin branch, wrap the existing as/cc build in `if (trcn[i] == 0)`; prereq walking/exists checks and `.s` assembly pass 1 stay OUTSIDE the guard only if they are part of the implicit build — they are: move the whole two-pass block inside the guard (a recipe'd `.bin` target still resolves target prereqs and exists-checks file prereqs first: keep pass 1's *prereq recursion/exists* loop, skip only the `as`/`cc` emissions). Re-read the shipped `resolve()` before restructuring; the rule: recipe'd = prereqs honored, zero toolchain calls.
- [ ] **Step 6: Gates** (Task 1 Step 4's four commands).
- [ ] **Step 7: Commit** — `make: TAB-indented recipes run through exec; $@ $< $^; @ silence; word0 maps to /bin/<name>.bin`.

### Task 5: `.PHONY`, `.DEFAULT_GOAL`, reserved dot-names

**Files:**
- Modify: `src/userland/make.c`
- Modify: `tools/check_make.py`

- [ ] **Step 1: Failing cases.**
  ```python
  ph = ('.PHONY: gen\nall: gen\n\techo after\ngen:\n\techo gen\n')
  expect_ok('plan-phony', ph, ['all'], ['/bin/echo.bin gen', '/bin/echo.bin after'])
  expect_ok('plan-default-goal',
            '/src/mk/q.bin: /src/mk/a.c\n.DEFAULT_GOAL := all\nall: /src/mk/q.bin\n\techo all\n',
            [], ['/bin/cc.bin -o /src/mk/q.bin /src/mk/a.c', '/bin/echo.bin all'])
  expect_err('err-reserved-name', '.SUFFIXES: x\n', [], '.SUFFIXES', 'reserved')
  expect_err('err-unknown-defgoal', '.DEFAULT_GOAL := nope\nall:\n\techo a\n', [], 'nope')
  ```
  (`plan-default-goal` proves the default is no longer the first target; `q.bin` exists so the implicit-cc plan still precedes `all`'s recipe.)
- [ ] **Step 2: Verify failure** (FAIL: `.PHONY` line currently becomes a target).
- [ ] **Step 3: make.c.** In `parse_line`: expanded target name equal to `.PHONY` → each following token `expandtok`'d and copied into `phony` rows (cap `MAXPHONY`), do NOT create a target, `lastt` unchanged. Any other name starting with `.` → `make: %s: line %d: reserved target name %s` + exit. (`.DEFAULT_GOAL` never reaches parse_line — pass A consumed it.) In `main`: `nreq == 0` → if `defgoal[0]`, `find_req(defgoal)`, error `make: unknown target %s` when absent (matches `err-unknown-defgoal`), else `resolve(0)` as today. A phony prereq of a real target resolves through the normal target path (no exists check — a prereq naming a defined target already skips it); is_phony exists for Task 7's board semantics checks and future timestamps.
- [ ] **Step 4: Gates + commit** — `make: .PHONY, .DEFAULT_GOAL, reserved dot-names`.

### Task 6: `Makefile.board` rewritten with variables (zero fixture change)

**Files:**
- Modify: `Makefile.board`
- Read-only authority: `tools/check_make.py` `plan-board` (its pinned `want` strings and tokenizer MUST NOT change)

**Interfaces:** Consumes Task 3 expansion. Produces the dialect reference file README points at.

- [ ] **Step 1: Rewrite `Makefile.board`.** Keep the header comment (update: variables note, sync note now names root `Makefile` CC_SRC, the check_make `plan-board` fixture, and `src/compiler/Makefile`). Definitions (each path stays LITERAL on a definition line — the checker's staging tokenizer scans raw text and only stages tokens starting with `/`; `$(...)` tokens are invisible to it and must remain so):
  ```make
  CCFLAGS := -I /lib
  CC_SRC := /src/compiler/cc.c /src/compiler/emit.c /src/compiler/expr.c /src/compiler/fp.c /src/compiler/lex.c /src/compiler/licm.c /src/compiler/opt.c /src/compiler/regs.c /src/compiler/state.c /src/compiler/stmt.c /src/compiler/types.c /lib/cc.h /lib/sys.h
  AS_SRC := /src/assembler/as.c /lib/sys.h /lib/t2obj.h
  LD_SRC := /src/linker/ld.c /lib/sys.h /lib/t2obj.h
  AR_SRC := /src/binutils/ar.c /lib/sys.h /lib/t2obj.h
  NM_SRC := /src/binutils/nm.c /lib/sys.h /lib/t2obj.h
  OD_SRC := /src/binutils/objdump.c /lib/sys.h /lib/t2obj.h
  BASIC_SRC := /src/basic/comp.c /src/basic/fmt.c /src/basic/funcs.c /src/basic/io.c /src/basic/lex.c /src/basic/main.c /src/basic/vm.c /lib/basic.h
  SH_SRC := /src/userland/sh.c /lib/sys.h
  ```
  Target lines `/src/cc.bin: $(CCFLAGS) $(CC_SRC)` etc. for all eight; `tools:` and `all:` keep their LITERAL target-name prereq lists (a `/src/x.bin` token on a definition line is staged as a source file by the checker tokenizer — keep those names only after colons). No `.PHONY`/recipes here (byte-compare targets must stay implicit-cc).
- [ ] **Step 2: Prove faithfulness** — `make 2>&1 | tail -1 && git diff --stat tools/check_make.py` (EMPTY — no checker change) `&& python3 tools/check_make.py --pc 2>&1 | tail -2` (all cases incl. `plan-board` pass: the pinned expanded plan is byte-identical, which IS the expansion proof).
- [ ] **Step 3: Gates** (check_ref, make check) + commit — `Makefile.board: one source list per tool via variables (plan-board fixture unchanged)`.

### Task 7: board feature legs + docs

**Files:**
- Modify: `tools/check_make.py` (`board_main`: feature checks after the existing byte-compare legs)
- Modify: `README.md` (new `## Board make` section near the tools/build docs), `docs/HANDOVER.md`, `tools/check_make.py` docstring (dialect summary), spec file Results block
- Board (owner go-ahead REQUIRED — one session, fresh round): everything below in order.

- [ ] **Step 1: Board legs in `board_main`** (all under a `/src/mkfeat` scratch dir, `r.run('mkdir ...')`-tolerant like the existing legs). First `r.put` the NEW `root/bin/rm.bin` and `root/bin/mkdir.bin` to `/bin/` (the board's current copies predate `-rf`/`-p`), next to the existing `make.bin`/`cc.bin`/`as.bin` puts:
  1. `mkdir -p /src/mkfeat/t/a/b` via `/bin/mkdir.bin` exits 0 and `ls /src/mkfeat/t/a` shows `b` (tool leg); `/bin/mkdir.bin /src/mkfeat/t` (exists, no -p) exits != 0.
  2. Stage `/src/mkfeat/Makefile` (raw bytes via `r.put(b'...')`): `CCFLAGS := -I /lib`, `/src/mkfeat/echo.bin: $(CCFLAGS) /src/userland/echo.c /lib/sys.h` (sources already staged by the existing loop), a `.PHONY: clean` + `clean:` with a TAB recipe `rm -rf /src/mkfeat/t` + `@echo hushed`, and `.DEFAULT_GOAL := /src/mkfeat/echo.bin`. Run `/bin/make.bin -f /src/mkfeat/Makefile` (no target arg → default goal leg).
  3. Fetch `/src/mkfeat/echo.bin`, byte-compare against a PC build made the same way: `src/compiler/cc -I src/lib -o /tmp/opencode/mkfeat-echo.bin src/userland/echo.c` (expect identical; this is the implicit-cc-under-variables board proof).
  4. `V=1` leg: `/bin/make.bin V=1 -f /src/mkfeat/Makefile clean` → output CONTAINS `/bin/rm.bin -rf /src/mkfeat/t` (resolved-form echo — the `-v`/`V=1` equivalence and the resolved plan form, board side) and does NOT contain `hushed` (`@` silence); after it `ls /src/mkfeat/t` reports not found (`rm -rf` recursion over the two-level tree really deleted).
  5. Old-dialect leg: stage `/src/mkfeat/Makefile.old` with one literal line `/src/mkfeat/echo2.bin: -I /lib /src/userland/echo.c /lib/sys.h` (no variables, no recipes); build it and byte-compare to the same PC build (old files mean exactly what they meant).
  6. The existing eight-tool byte-compare legs now exercise the rewritten `Makefile.board` end to end; finish with the usual `board-fail` leg.
- [ ] **Step 2: Owner go-ahead, then run** `python3 tools/check_make.py --board 2>&1 | tail -15` and `python3 tools/check_toolchain.py --selfhost 2>&1 | tail -8` (six IDENTICAL; make.bin is not among them but the rebuild must stay clean). Free the port first (owner's ruling; `fuser` check; stop and ask if busy).
- [ ] **Step 3: README** — a `## Board make` section: usage line (`make [-n] [-v] [-f file] [VAR=value] [target ...]`), the dialect in short (target lines at column 0, TAB means recipe — note the reinterpretation of TAB-indented target lines; implicit cc rule; variables and use-time expansion with the one GNU divergence line: board make has ONE expansion behavior, GNU's `=` vs `:=` distinction collapses; `$(NAME)`, `$@ $< $^`, `@`, `.PHONY`, `.DEFAULT_GOAL`, `V=1`; the resolved-plan form; caps list). README is the owner-facing surface — no slop, concrete examples over prose.
- [ ] **Step 4: Close the paper trail** — `check_make.py` docstring (dialect + new case list, `-v` note unchanged); `docs/HANDOVER.md` round entry (state: what shipped, gates, board results); spec file: flip Status to `implemented`, add `## Results` with the verification facts.
- [ ] **Step 5: Commit** — `make round E: board feature legs + README board-make section + close-out`.

## After the plan

Whole-branch gates (SDD close): full `make check`, `check_ref`, `check_make --pc` (all old 13 + ~20 new), board session per Task 7, then the usual merge menu — merge/push only on the owner's word.
