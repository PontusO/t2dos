# NOTES: sources moved into `src/` (round 2026-11-21/22) — DONE

Goal: one `src/` folder for all compiled-source trees, everything still
building and working. Spec: `docs/superpowers/specs/2026-11-21-src-move-design.md`
(Results there too); SDD ledger: `.superpowers/sdd/2026-11-21-src-move/`
until deleted at finish.

## Settled (don't re-investigate)

- Layout: `src/{compiler,assembler,linker,binutils,lib,basic,userland,kernel}`;
  `sys.h`, `host_linux.c`, `tools/`, `tests/`, `examples/`, `docs/`,
  `android/`, `third_party/` stay at the root (T2CC resolves `sys.h`
  against the repo root by design; kernel build injects `-I$PWD`).
- Board layout is a board-side convention and did NOT change: repo
  `src/X` stages at board `/src/X` (`check_toolchain`:
  `bp = '/src/' + os.path.relpath(src, 'src')`; `check_make.repo_path`
  maps `/src/X -> src/X`, REPO_OF holds the mirrored headers).
- `cc.c` PC branch `init_tools` walks **3** components up from
  `/proc/self/exe` (`src/compiler/cc` -> repo root -> `build/host`);
  the board branch is `#ifdef __T2CC__` and never sees it.
- `make ref` uses `git show $(cat build/ref/COMMIT):compiler/cc.c` — the
  HISTORIC tree's path; never "fix" it to `src/compiler/cc.c`.
- `check_asm --objdump-corpus` silently passed with an EMPTY corpus for
  one round (glob of the moved-away dir matched nothing) — a no-sources
  guard now exits 1. Same class: any tool globbing repo dirs after a
  move must guard against matching nothing.
- Scratch `root/bin/*.bin` leftovers (exe*/deep*/big*/fr/mim/ccdbg/three/
  cap.bin) are gitignored checker output; a forced clean legitimately
  removes them.

## Gates at completion

Full clean rebuild: every `root/bin/*.bin` byte-identical to the pre-move
build. `make check` ok; `check_ref` different: 0 (39 identical);
`check_make --pc` 13; `check_toolchain --pc` 9 cases + 7 errors;
`bench --pc` 11/11; `check_basic --pc` 139 cases; `check_jobs --build`,
`check_netfs --pc` (105 steps), `check_telnetd --pc`, licm/prolog/frame
ok; objdump corpus 59 files. Board: see the Task 5 commit message.

## Follow-ups

None owed by this round. The `static` adoption round (HANDOVER 0c) is
unrelated and still pending its own go.
