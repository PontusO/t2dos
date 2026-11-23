# make GNU-ish dialect round — variables and command targets

Date: 2026-11-23. Status: implemented (branch `make-vars-recipes`; board run pending owner go-ahead).
Supersedes nothing: the make round spec (`2026-11-21-make-utility-design.md`)
stays binding for everything this doc does not extend. The `make -v` round
(merged 2026-11-23) already shipped verbose echo; this doc builds on it.

## Why

The board's `make` speaks a dialect that is one concept short of useful
everyday builds: variables (every list is retyped, hence the four-copy
source-list drift the repo's own comments warn about) and command targets
(`clean:` is unwritable — make can only ever run the fixed `/bin/cc.bin`).
The owner's benchmark for "like Linux make" is a conventional embedded
GNU makefile (TARGET/SRCS/BUILD=debug|release/`clean`/`size`/`help`); this
round makes that shape writable on the board except its per-file-object
rules, which are deliberately parked.

## Goals

1. **Variables.** Definition lines `NAME := value`, `NAME = value`,
   `NAME ?= value` (only if still undefined), `NAME += more` (space
   joined). Command-line assignment `make BUILD=release all`. Expansion
   of `$(NAME)` inside target names, flags, prereqs and recipe words,
   including nesting: `$(OPT.$(BUILD))` must resolve (this is what makes
   `BUILD=debug|release` configuration work). Undefined name expands to
   empty, silently, as GNU does.
   **Expansion model (builder-check ruling):** values are stored
   verbatim; every `$(NAME)` reference resolves at USE time against the
   fully loaded table (file parsed first, then commands planned/run).
   Consequences, all GNU-compatible: forward references work in
   prereqs and recipes alike (a benchmark makefile ordered the GNU way
   just works), and a command-line value may itself reference file
   variables (`make CFLAGS='$(OPT.release)'`). The four operators differ
   only in *whether/when* they assign (always / always / only-if-undefined
   / append); none expands early. Recursion (`A := $(A)`) is caught by
   the nesting cap (8) as an error, like GNU's recursion error. The one
   README divergence line: GNU distinguishes recursive `=` from simple
   `:=`; board make has one behavior for both (expand-at-use).
2. **Command-line overrides win**, as GNU: a file assignment to a name
   that was set on the command line is ignored (any operator). `?=`
   naturally loses to it.
3. **Command targets via recipes.** Lines indented with a literal TAB
   following a target line are that target's recipe: each line is
   comment-stripped, expanded, tokenized on whitespace into argv, and
   executed through the existing `exec()` builtin. A target WITH a
   recipe gets NO implicit cc call — recipe only. A `.bin` target without
   a recipe keeps today's implicit single-cc-call build, byte-identical.
   Program resolution for word 0: contains a `/` → used as given;
   otherwise `/bin/<word>.bin` is tried (so `rm -rf build` runs
   `/bin/rm.bin`). A leading `@` on a recipe line suppresses the `-v`
   echo for that line (GNU convention).
4. **Automatic variables** in recipe words of the rule being run: `$@`
   (the target), `$<` (first prereq), `$^` (all prereqs, space separated).
   **Plan form (owner decision, 2026-11-23):** `-n` prints recipe lines
   RESOLVED — word 0 mapped (`rm -rf build` shows as `/bin/rm.bin -rf
   build`), variables expanded, `@` stripped — the exact argv that would
   exec, identical to the line `-v` echoes. One cmd[] array feeds both
   printers, so plan and echo can never disagree; consistent with the
   existing plan, which already prints `/bin/cc.bin`.
5. **`.PHONY: name ...`** — declared names are never treated as files:
   always "built" when reached, exempt from the prerequisite-exists
   check, never handed to cc as input. **`.DEFAULT_GOAL := name`**
   overrides the default (which stays "first target defined"). Both
   dot-names are reserved: normal targets may not use them.
6. **`V=1` on the command line** turns on the verbose echo, exactly like
   `-v` (it is just a variable assignment; make checks the variable `V`
   after parsing argv). GNU convention, one line of code.
7. **Tools the recipes assume** (today they are bare):
   `rm` gains `-r` (recurse into directories: readdir loop, unlink
   files, rmdir on the way out) and `-f` (missing path is not an error);
   tokens may combine (`-rf`). `mkdir` gains `-p` (create each parent
   prefix; an existing directory is not an error under `-p`).
8. **`Makefile.board` is rewritten to use variables** (one source list
   per tool, no retyped paths). This is safe *because* the checker pins
   the EXPANDED plan: the existing plan-board case must keep passing
   with zero change to the pinned command strings, which is exactly the
   proof that expansion is faithful. The four-copy warning shrinks to
   three copies (root `Makefile`, `check_make` fixture, this file).
9. **Docs**: README gains a proper board-make section (there is none
   today), `make.c` usage comment and `Makefile.board` header updated,
   HANDOVER entry, this spec's Results at the close.

## Non-goals

- Timestamps / skip-if-up-to-date — owner ruling 2026-11-23, again:
  always builds. Everything below stays consistent with it.
- Pattern rules (`%.o: %.c`), per-file objects, order-only prereqs `|`.
  `cc` already has `-c` and `ld.bin` links objects, so this is a future
  round's parser problem, not a toolchain gap — parked deliberately
  (owner agreed: revisit only after living with this round).
- GNU functions (`$(addprefix ...)`, `$(if ...)`, `$(firstword ...)`,
  substitution references `$(SRCS:%.c=%.o)`), `include`, environment
  variables, shell metacharacters or quoting in recipes (arguments with
  spaces are not a board idiom; whitespace tokenization only), PATH
  search, `-j`, `.DELETE_ON_ERROR`, `$(@F)`-style basename forms,
  target-specific variables, `ifeq`/`ifneq`.
- `-MMD`/`-include $(DEPS)`: meaningless while always-build rules —
  dependency files exist to enable skipping.

## Anti-goals

- Any change to the bytes of shipped `root/bin/*.bin` or to what
  Makefile.board-shaped files (no variables, no recipes, target lines
  starting in column 0) mean — they must parse and produce identical
  plans and builds. Scoped deliberately (builder-check finding): the
  old parser tolerated a TAB-indented target line (`\ttools: a b`
  parsed as a target); under the new grammar a leading TAB is recipe,
  exclusively. No file in the repo indents a target line, so nothing
  in-tree changes meaning; the README states that target lines start at
  column 0 and TAB means recipe.
- make growing into a shell: exactly one exec per recipe line, no
  pipelines, no redirection, no `;`.
- Recursive variable semantics creeping in through the back door of
  nested expansion (`$(A)` values are already-expanded text, always).

## Constraints (binding, from AGENTS.md / architecture.md / make spec)

- `make.c` stays inside the committed cc's subset AND its documented
  string discipline (docs/NOTES-star-deref.md): no string in a `char *`
  variable; names/values are offsets into `buf` or fixed-stride table
  rows; comparisons via int-indexed helpers.
- Caps are explicit and reported on overflow, in the file's existing
  idiom: `MAXLINE 128`, `MAXTARGETS 64`, `MAXPRE 16`, `MAXARGS 32`,
  `MAXBUF 16384`. New: a variable table (`MAXVAR` rows; value rows
  wider than `MAXLINE` — a SRCS-style list must exceed 128 chars,
  geometry left to the builder) and an expansion nesting cap (8) with a
  `make:` error, not a hang.
- `check_make.py --pc` remains the plan authority (host_linux has no
  spawn); `-v`/recipe execution is board-verifiable only.
- rm/mkdir stay cc-subset one-file tools (patterns to copy: ls.c's
  readdir loop, rcp.c's recursion-free walking).
- Kernel untouched; no syscalls (`unlink`/`rmdir`/`mkdir`/`stat`/readdir
  already exist); no Makefile.board semantic change beyond variables
  (pinned plan proves it).

## Approaches considered (owner picked #2, 2026-11-23)

1. *Variables only* — cheap, but the owner's own benchmark sample
   shows `clean:`/`size:`/`help:` are half the want; underdelivers.
2. **Variables + command targets (CHOSEN)** — everything above; the
   sample becomes writable except per-file objects; biggest value per
   gram; the recipe machinery is additive to the parse (leading TAB,
   previously just skipped whitespace, becomes the recipe marker —
   see the scoped anti-goal).
3. *Full GNU-lite with pattern rules* — deferred round; needs stem
   matching plus a `cc -c ×N + ld` byte-identity study of the linker
   path, a toolchain question, not a make question.

## Grammar sketch (what will be legal that is not today)

```make
TARGET    := app
BUILD_DIR := build
BUILD     ?= debug
OPT.debug    := -Og -g3
OPT.release  := -O2 -DNDEBUG
SRCS := main.c uart.c spi.c
CFLAGS := $(OPT.$(BUILD)) -Wall

.PHONY: all clean
.DEFAULT_GOAL := all
all: $(TARGET).bin

$(TARGET).bin: $(SRCS)
	echo LD $@
	@rm -f old.log

clean:
	rm -rf $(BUILD_DIR)
```

Command line: `make V=1 BUILD=release clean`.

## Semantics decisions (controller calls, correctable)

- Expansion sites: target name, flags, prereqs, recipe words. Automatic
  variables (`$@ $< $^`) in recipe words only.
- `+=` on an undefined name behaves like `:=`. Redefinition is legal,
  last wins (command line excepted). No `.SECONDEXPANSION`-style extras.
- Recipe lines may appear only after a target line; a TAB line before
  any target is an error. Recipe lines of collection (non-`.bin`)
  targets are fine too — GNU-style `help:` works.
- An empty `$(NAME)` in prereq position yields no token (GNU behavior),
  so `all: $(MAYBE)` can't produce a phantom prerequisite.
- `.bin` target WITH a recipe: the checker's byte-compare does not
  apply (no implicit call); document that such targets are the author's
  responsibility.
- Recipe program not found: `exec` returns its failure; make prints
  `make: <target>: <program>: cannot exec` and exits with the child
  code (mirrors the existing child-failure contract).
- rm `-r` follows no symlinks (the board has none); depth is capped by
  the kernel's own dir-handle count, errors print and set exit 1.

## Tests

PC (`check_make --pc`, all plan-level through host_linux `-n`):
- `plan-assign`: four operators; command-line `VAR=v` beats all of them;
  `?=` loses to command line; `+=` space-joins; undefined expands empty.
- `plan-expand`: `$(NAME)` in name/flag/prereq positions; nesting
  `$(OPT.$(BUILD))` both BUILD sides; **forward references** — a
  prereq and a recipe word using a variable defined below their rule
  expand correctly (use-time model); unterminated `$(` errors; nesting
  past the cap errors (never hangs); `A := $(A)` errors via the cap.
- `plan-recipe`: dry-run prints recipe lines RESOLVED (word 0 mapped to
  `/bin/<name>.bin`), expanded, one per line, `@` stripped — byte-same
  text as the `-v` echo would give; implicit-cc targets unchanged
  byte-for-byte (existing cases guard this); mixed implicit+recipe file.
- `plan-phony`: phony prereq of a real target runs and is not existence
  checked; `.DEFAULT_GOAL` selects; dot-names rejected as targets.
- Plan-board fixture: Makefile.board rewritten with variables must
  reproduce the pinned expanded plan with ZERO fixture change.
- `make check` and `check_ref` clean (make.c/rm.c/mkdir.c are corpus).
Board:
- Scratch project: `make all` builds (implicit cc byte-identical to PC),
  recipe legs echo; `make V=1 clean` really removes a staged dir tree
  (`rm -rf` leg proves recursion); `mkdir -p` creates a two-level path;
  `BUILD=release` flips a flag visible in the printed plan; `@` line
  silent under `V=1`; old-style Makefile.board builds unchanged.
- `check_toolchain --selfhost` six IDENTICAL (make.bin is not in the
  six, but the toolchain rebuild must stay clean end to end).

## Left to the builder

Variable-table geometry and the expansion/tokenization plumbing (the
file already streams tokens into `cmd[]`/prereq arrays — expand as
tokens are taken, avoiding whole-line intermediates); whether `MAXLINE`
grows for recipe lines; exact cap values for `MAXVAR`/value width
(document in the file header); where the `.PHONY`/`.DEFAULT_GOAL`
tables live. None of these change what gets built.

## Results

PC (branch gates, all green):

- `check_make.py --pc`: 55 cases pass: the original 13 plan cases, the
  variables/recipes/`.PHONY`/`.DEFAULT_GOAL` plan cases of this round, four
  edge pins added at close-out (`.PHONY` before any target creates no
  target; command-line target beats `.DEFAULT_GOAL`; two `.PHONY` lines
  consume cleanly), final-review pins (a phony prereq with no rule is a
  no-op; `A#b := c` errors `no colon`; an empty command-line variable name
  is rejected; the recipe-line cap; `mkdir -p` with a trailing slash), and
  the `rm`/`mkdir` tool cases under qemu.
- The plan-board fixture passed with zero change against the rewritten
  `Makefile.board`: expansion is faithful (Goal 8's proof).
- `check_ref.py`: `different: 0`; full `make check` clean.
- Shipped caps (make.c header): `MAXVAR 32`, `MAXVAL 512`, `MAXEXPASS 8`,
  `MAXRECI 8`, `MAXPHONY 16`/`MAXPHNAME 64`; `MAXLINE` stayed 128.

board: PENDING (owner go-ahead round)

- `check_make.py --board`: the eight byte-compare legs through the rewritten
  `Makefile.board`, plus `/src/mkfeat` feature legs (`mkdir -p`; default-goal
  build byte-identical to a PC build; `V=1` resolved echo of the `rm` line
  with the `@` line running unechoed; `rm -rf` really removing the tree;
  old-dialect file byte-identical). One plan correction at implementation:
  `@` silences the `-v` echo, not the program's stdout, so the leg pins the
  echoed command line absent and the program output present.
- `check_toolchain.py --selfhost`: six IDENTICAL (make.bin is not among the
  six; the rebuild must stay clean).
