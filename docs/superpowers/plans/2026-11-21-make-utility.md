# make utility Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking. (Subagent dispatch is disabled in this session by harness constraints: single model instance drives the board.)

**Goal:** a `make.bin` that builds the board toolchain from a small target-line build file, always rebuilding, with a dry-run mode for PC testing and a byte-identity proof on the board.

**Architecture:** `userland/make.c` parses `Makefile.board` (target lines only: `name: [-flags] prereqs`), resolves collection targets recursively, and per `.bin` target runs one `cc` call over its `.c`/`.o` prerequisites (`as -o` for `.s`) via the synchronous `exec()` builtin, mirroring the PC Makefile's CCBIN recipe. `-n` prints the plan without running anything.

**Tech Stack:** C in the committed cc's subset, qemu `host_linux` for the dry-run tests, board serial runs for the executing tests, `tools/board.py` (`Remote`) for board IO.

**Spec:** `docs/superpowers/specs/2026-11-21-make-utility-design.md`

## Global Constraints

- `userland/make.c` stays inside the language subset the committed cc compiles (it is built BY that cc - it must compile).
- The PC `Makefile` needs no edit for `make.bin` (the `root/bin/%.bin: userland/%.c` wildcard rule picks it up); nothing that changes an existing tool's bytes (no `check_ref` drift: it must end `different: 0`).
- No new syscalls: only `open/read/close`, `printf`, `strlen/strcmp/strcpy`, `exec` (all kernel builtins, see `sys.h`).
- Board runs need the user's explicit go-ahead THIS round (no blanket pre-approval exists for it yet); one program on `/dev/ttyACM*`.
- `host_linux` returns `SPAWN_UNSUPPORTED`: the executing path is proven on the board only; under qemu only `-n` runs.
- No em dashes in code comments or commit messages. Gotchas: `make clean` deletes `./host_linux` (`make host` before `--pc` checks after a clean); grep for assembler tabs needs `$(printf '\t')`.

## Review Focus

- A malformed build-file line (no colon, `-I` as last token, unknown target named on the command line): make prints file, line number, and the offending text, exits nonzero, and never runs a partial build. Pinned by pc checker cases.
- A cycle between collection targets (`a: b.bin`, `b.bin: a`): error naming the target, no hang. Pinned by a pc checker case.
- A prerequisite that is not a defined target and not on disk: error naming both the target and the missing file. Pinned by a pc checker case.
- A child command failing (a source that makes cc exit nonzero): make stops at that command, prints the failed command, exits with the child's code. Pinned by a board checker case.
- A target whose command would exceed make's argv cap (more sources than `MAXARGS`): error before running, no truncation. Pinned by a pc checker case.

---

### Task 1: parser, planner, dry run; PC checker

**Files:**
- Create: `userland/make.c`
- Create: `Makefile.board` (repo root, version controlled)
- Create: `tools/check_make.py`
- Modify: `tools/check_make.py` carries both checkers; `--pc` in this task, `--board` in Task 2

**Interfaces:**
- Consumes: kernel builtins `open/read/close/printf/strlen/strcmp/strcpy`, `millis`; `exec(path, argc, argv)` from Task 2.
- Produces: `make.bin` CLI `make [-n] [-f file] [target ...]`; plan lines on stdout exactly `/bin/cc.bin <flags> -o <target> <sources...>` and `/bin/as.bin -o <s>.o <s>`, one per line, collections produce no line; exit 0 on success/`-n`, 1 on any error; constants `MAXLINE 128 MAXTARGETS 64 MAXPRE 16 MAXBUF 16384 MAXARGS 32`; target table `char *tname[MAXTARGETS]; char *tflags[MAXTARGETS][MAXARGS]; int tflagsn[MAXTARGETS]; char *tpre[MAXTARGETS][MAXPRE]; int tpren[MAXTARGETS];` plus `int tvisited[MAXTARGETS]` (0 fresh, 1 on the resolve stack, 2 done); `tools/check_make.py` runnable as `python3 tools/check_make.py --pc`.

- [ ] **Step 1: Write the failing checker**

`tools/check_make.py` (docstring explains modes, per repo convention). `--pc` stages a scratch tree under `root/` (host_linux maps `/` into `-r root`): `root/src/mk/a.c`, `root/src/mk/b.c`, `root/src/mk/util.s`, `root/src/mk/hdr.h`, each a one-line stub; a staged `root/tmp/make/Makefile` exercising the grammar; then per case runs `qemu-arm ./host_linux -r root /bin/make.bin -n -f /tmp/make/Makefile <target>` (pattern copied from `tools/check_sh.py:21`) and compares stdout/exit:

1. `bin: -I /src/mk /src/mk/a.c /src/mk/hdr.h` then target `bin` -> exactly `/bin/cc.bin -I /src/mk -o /src/mk/bin /src/mk/a.c`, exit 0.
2. flags order and a `.s` prereq: `asm: /src/mk/util.s` -> `/bin/as.bin -o /src/mk/util.o /src/mk/util.s` then the cc line for the `.o` (no flags: `/bin/cc.bin -o /src/mk/asm /src/mk/util.o`).
3. collection: `tools: bin asm` and `all: tools` -> running `all` prints bin's cc line, then asm's as line, then asm's cc line; a target named twice (once via collection, once explicitly) plans once.
4. error: line `bad /src/mk/a.c` (no colon) -> nonzero exit, output names line number 1-ish and `bad`.
5. error: `x: -I` (`-I` last token) -> nonzero, names the line.
6. cycle: `c1: c2` / `c2: c1` -> nonzero, says cycle, no hang (subprocess timeout 30).
7. missing file: `m: /src/mk/nope.c` -> nonzero, names `nope.c` and target `m`.
8. missing target: command line names `nosuch` -> nonzero, says unknown target.
9. flag order as in the file: `ip: -I /src/mk -O /src/mk/a.c` -> `/bin/cc.bin -I /src/mk -O -o /src/mk/ip /src/mk/a.c`.
10. argv cap: generate a line with `MAXPRE 16` sources and flags to reach `MAXARGS 32` exactly (passes) and 33 (fails naming the target).

The staged Makefile cases write to `root/tmp/make/Makefile` per case (one file rewritten per case is fine; run serially).

- [ ] **Step 2: Run it to confirm red**

Run: `make 2>&1 | grep -ciE '\berror\b'; python3 tools/check_make.py --pc 2>&1 | tail -3`
Expected: FAIL (root/bin/make.bin missing or not built).

- [ ] **Step 3: Implement `userland/make.c`**

`main(argc, argv)` parses `-n`, `-f <file>` (default `Makefile`), remaining names as requested targets (none: the file's first target). Read the file whole into a `char buf[MAXBUF]` (`open`/`read`, size via `stat`), reject with an error if truncated. Parse line by line: strip `#` comments and blank lines; split on the first `:` (target = text before, trimmed); tokens split on spaces/tabs; leading tokens starting with `-` are flags, `-I` consumes the next token (a `-I` with no next token is the line error); the rest are prerequisites; a second `:` on a line or a duplicate target name is an error naming the line. Resolve(mark/cycle via `tvisited` states 0/1/2) each requested target; a name matching a defined target recurses (state 1 on entry = cycle), an undefined name that is not on disk (`open` probe, closed immediately) is the missing-file error only when the owning target is a `.bin`. File prereqs are checked for existence at plan time. For a `.bin` target emit commands (into a growing list of argv vectors; in `-n` print them immediately instead): for each `.s` prereq an as line + an implicit `.o` source, then one cc line: `/bin/cc.bin`, target's flags, `-o`, target name, all `.c`/`.o` sources (flags + fixed words counted against `MAXARGS`). Print plan lines as single-spaced argv joins. All errors: `printf` one line naming file, line number where applicable, and the offending text; `exit(1)`-equivalent (return 1 from main is fine). No `exec()` yet: running is Task 2 (keep the argv vectors built either way, `-n` just skips the run call).

- [ ] **Step 4: Green the checker**

Run: `make 2>&1 | grep -ciE '\berror\b'; python3 tools/check_make.py --pc 2>&1 | tail -3`
Expected: `ok: make plan` (or the checker's wording).

- [ ] **Step 5: Sanity-check Makefile.board parses (dry, PC)**

Checker case 11: copy the repo's real `Makefile.board` into the staged tree, write stub files for every source/lib/header path it names (under the staged `/src/...` and `/lib/...` absolute paths; remove them in cleanup), run `make -n -f <staged> all`, and expect the 6 tool `cc` lines, then `basic.bin`, then `sh.bin`, in file order, collections contributing no line.

Run: `python3 tools/check_make.py --pc 2>&1 | tail -1`
Expected: `ok`.

- [ ] **Step 6: PC gates**

Run: `python3 tools/check_ref.py 2>&1 | grep different; make check 2>&1 | tail -1; python3 tools/check_sh.py --pc 2>&1 | tail -1`
Expected: `different: 0`; ok line; `ok: 5 cases` (make.bin joined the tree; nothing else moved).

- [ ] **Step 7: Commit**

```sh
git add userland/make.c Makefile.board tools/check_make.py
git commit -m "make: target-line build executor with dry-run plan and PC checker"
```

---

### Task 2: execute on the board; byte identity

**Files:**
- Modify: `userland/make.c` (run the built argv vectors with `exec`)
- Modify: `tools/check_make.py` (`--board` mode)

**Interfaces:**
- Consumes: Task 1's argv vectors and plan-line printer.
- Produces: `make` (no `-n`) executes each command via `exec("/bin/cc.bin" or "/bin/as.bin", argc, argv)`; child failure stops make with the child's exit code after printing the failed command; `check_make.py --board` prints per-tool timings and a final `ok` line.

- [ ] **Step 1: Wire execution**

For each resolved target's command list (same order as `-n`): `r = exec(path, ac, av)`; nonzero `r`: print `make: <cmd...> failed (<r>)`, stop the whole run, `return r`. Ask the user for the board go-ahead before Step 3 runs.

- [ ] **Step 2: Write the board checker**

`--board` in `tools/check_make.py` using `Remote` from `tools/board.py`: push `make.bin` to `/bin/make.bin`; for every path token in the repo `Makefile.board` push the mapped repo file (`/src/compiler/cc.c` <- `compiler/cc.c`, `/lib/t2obj.c` <- `lib/t2obj.c`, `/sys.h` <- `sys.h`, ...) and `Makefile.board` to `/src/Makefile.board`; also push the current `root/bin/{cc,as,ld}.bin` so make itself runs with a working toolchain. One tool per command for timings: `make -f /src/Makefile.board <tool>.bin` for each of `cc as ld ar nm objdump sh` plus `basic`, fetch each built file, byte-compare against `root/bin/<tool>.bin`; nonzero everything on the first mismatch (print sizes and first differing offset). Add a failure-mode case: push a `Makefile.fail` whose only target compiles a source containing `bogus syntax here;` -> expect make to exit nonzero and say the command failed.

- [ ] **Step 3: Run it (after the user's go-ahead)**

Run: `python3 tools/check_make.py --board 2>&1 | tail -10`
Expected: per-tool `PASS ... byte-identical (Ns)` lines, `ok: make board`.

- [ ] **Step 4: PC re-gate and commit**

Run: `python3 tools/check_make.py --pc 2>&1 | tail -1; python3 tools/check_ref.py 2>&1 | grep different`
Expected: `ok`; `different: 0`.

```sh
git add userland/make.c tools/check_make.py
git commit -m "make: execute the plan with exec, prove board byte identity"
```

---

### Task 3: results and handover

**Files:**
- Modify: `docs/superpowers/specs/2026-11-21-make-utility-design.md` (Results section)
- Modify: `docs/HANDOVER.md` (state line: make exists, cc.c split is the next round)

- [ ] **Step 1: Record Results**: what was built vs spec (grammar as implemented incl. any deviations), checker output lines, board timings per tool, the argv cap hit or not.
- [ ] **Step 2: HANDOVER**: one or two lines in the current-state area: `make.bin` builds the board tree from `Makefile.board`; splitting `compiler/cc.c` is now an open next round that uses it; `check_make.py` modes.
- [ ] **Step 3: Commit**: `docs: make utility results; the cc.c split is unblocked`.
- [ ] **Step 4: Final review**: whole-branch self-review (no subagent tool), report; merge/push awaits the user's explicit word.

## Ledger

Track at `.superpowers/sdd/2026-11-21-make-utility/progress.md` (header = plan path; one entry per task: gates run + results + rulings).
