# Move all source directories into `src/` (design, 2026-11-21)

## Intent and why

The repo root carries eight compiled-source directories side by side with
tooling, fixtures and build output. Folding the sources into one `src/`
folder cleans the root and makes the two-path story (repo layout vs board
layout, which already mirrors repo dirs under `/src`) explicit. Owner
request, 2026-11-21.

## Scope

Move (contents unchanged, directory names kept): `compiler/`, `assembler/`,
`linker/`, `binutils/`, `lib/`, `basic/`, `userland/`, `kernel/` → under
`src/`.

Stay where they are: `tools/` (PC checkers), `tests/` (fixtures),
`examples/`, `docs/`, `android/`, `third_party/`, `root/`, `build/`,
`.superpowers/`. Root files `sys.h` and `host_linux.c` stay at the root:
T2CC resolves `sys.h` against the current directory by design (the Makefile
and kernel build inject the repo root as include dir), and the board/PC
runtime trees stage it as `/lib/sys.h` and `root/lib/sys.h`.

## Goals / non-goals / anti-goals

- Goal: `make`, `make check`, `make host`, `make ref`, every checker, the
  board build and `--selfhost` work unchanged in behavior after the move.
- Goal: the selfhost fixed point survives — the board's committed cc (built
  from the old paths, binary already on the SD image) rebuilds the moved
  sources byte-identically. Sources are moved with `git mv`; contents of
  compiler sources must not change at all in this round.
- Non-goals: renaming anything, adopting `static`, touching board-side paths.
- Anti-goal: any change to emitted binaries. The only diffs are paths in
  Makefiles, checkers and docs.

## Board layout: no change (my call)

The board root already mirrors repo dirs under `/src` (`/src/compiler/cc.c`,
headers in `/lib`). That layout stays exactly as is — `Makefile.board`, the
`check_make` plan-board command fixture, and every `REMOTE = /src/...` path
in the checkers are untouched. Only the repo side of the mapping changes:

- `check_toolchain.SELFHOST` entries gain the `src/` prefix; the board path
  becomes `/src/` + path relative to `src/` (so it stays `/src/compiler/cc.c`).
- `check_make.REPO_OF`: `basic.h`→`src/basic/basic.h`, `cc.h`→`src/compiler/cc.h`
  (`sys.h` unchanged); `repo_path()` maps `/src/X` → `src/X`.
- `check_toolchain` selfhost tests stage `src/compiler/tests/NAME.c` at
  `/src/NAME.c` — the board path is name-only, unaffected.

## PC build: path rewrites only

`Makefile`: `CC_HOST = src/compiler/cc`, PROGS/Globs, `CC_SRC`, `BASIC_SRC`,
per-tool rules, `root/lib` staging sources, `host` target's `kernel/*` prereqs,
`-I compiler`→`-I src/compiler`, `-I lib`→`-I src/lib`, `-I basic`→
`-I src/basic`. The `ref` target keeps `git show $(cat
build/ref/COMMIT):compiler/cc.c` — it extracts a historic blob whose path
predates the move and must stay.

`.gitignore`: `/compiler/cc` → `/src/compiler/cc`.

`make ref`'s gcc build and every PC-side `-I .` stay (sys.h is still at the root).

## Checkers: prefix the repo paths

- `check_ref.py`: corpus globs and the existence guards gain `src/`
  (`userland/*.c`→`src/userland/*.c`, etc.); `STATIC_TESTS` entry becomes
  `src/compiler/tests/test15.c`; `CC = ROOT/src/compiler/cc`; docstring.
- `check_toolchain.py`: `TOOL['cc']`, `SELFHOST` (with the mapping above),
  the `/lib` staging sources (`lib/t2obj.h`→`src/lib/t2obj.h`, `compiler/cc.h`
  →`src/compiler/cc.h`), selfhost test sources, docstring.
- `check_make.py`: `REPO_OF` + `repo_path()` (above). Plan-board fixture unchanged.
- `check_jobs.py`: `SRCS`/sh source → `src/userland/…`; `-I lib` runs with
  `cwd=ROOT` → `-I src/lib`.
- `check_telnetd.py`: `MTAB` and the three `cc -I lib` builds → `src/lib`, `src/…`.
- `check_netfs.py`: `userland/cp.c`, `sys.h` copy source → `src/userland/cp.c`
  (sys.h stays).
- `check_basic.py`: `PC_CFLAGS` `-I basic`→`-I src/basic`, `-include basic/pcsys.h`
  →`src/basic/pcsys.h`; `build_pc` glob → `src/basic/*.c`. (Board side `/src/basic/...`
  runtime paths are unrelated and stay.)
- `check_android.py`: `lib/mtab.c` check → `src/lib/mtab.c` (message too).
- `bench.py`: `CC` → `src/compiler/cc`; `-I ROOT/lib` → `src/lib`.
- `board.py`: `expect_gcc` cc path → `src/compiler/cc`.

## Docs

`AGENTS.md` (build commands, kernel sketch path `src/kernel`, triage file
paths), `README.md` (build/kernel/flashing paths, the `make ref` note if any),
`docs/HANDOVER.md` (new §5 entry when done), `docs/architecture.md` path
mentions. The `Makefile.board` keep-in-sync comment's `CC_SRC (compiler/*.c)`
reference → `src/compiler/*.c` (comment only, no board line changes).

## Approach considered

One `git mv` batch + mechanical path updates (chosen): minimal diff, review
by inspection, gates are the existing ones. Rejected: symlinks from old paths
(defeats the point, and `git show`/board staging get subtle); two rounds,
build first then docs (docs-only drift is caught by the same review — one
round is cheaper).

## Verification (all on this tree)

`make` · `make check` · `python3 tools/check_ref.py` (different: 0) ·
`check_make --pc` then `--board` (byte-identical) · `check_toolchain --pc`
then `--selfhost` (IDENTICAL ×6 + tests) · `bench --pc` · `check_basic --pc`
· `check_jobs --build`-style PC path if quick · `make host` + one qemu run.

## Results (landed 2026-11-22)

Executed as designed; five deviations, all ledgered:

- One deliberate content change: cc.c's PC-only `init_tools` walked two
  path components up to find `build/host`; the moved host cc sits three
  deep, so the walk is now three (`#ifndef __T2CC__` branch - the board
  build never sees it, selfhost confirms).
- The `root/lib` staging recipes hardcoded their source paths (the plan
  assumed `$<`); they now use `$<`, which cannot drift again.
- The baseline checksum set included gitignored scratch `root/bin/*.bin`
  from earlier checker runs; the forced clean removed them. No hash
  mismatch on any built binary.
- `check_asm --objdump-corpus` silently ran an EMPTY compiler-tests corpus
  for one round (the glob of the moved-away dir matched nothing). Fixed
  and guarded (`sys.exit 1`); corpus back to 59 files.
- `check_make --board` staging (only reachable on the board) dead-ended on
  the `/lib/t2obj.c|net.c|mtab.c` and `/lib`-directory tokens, whose
  `lstrip('/')` fallback no longer resolved; `REPO_OF` maps them now.

Gates on the final tree: full clean rebuild byte-identical; `make check`
ok; `check_ref` different: 0 (39 identical); `check_make --pc` 13/13 and
`--board` 8/8 byte-identical + fail case; `check_toolchain --pc` 9 cases +
7 errors; `--selfhost` IDENTICAL ×6 (cc 230240 B, 32.4 s) + tests 4-12 and
15 on-board; `bench --pc` 11/11; `check_basic --pc` 139 cases;
`check_jobs --build`, `check_netfs --pc` (105 steps), `check_telnetd --pc`,
licm/prolog/frame all ok.
