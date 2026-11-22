# NOTES: cc -O sp-relative frames (branch cc-prologue, Task 3b-ii)

Live notes for `compiler/cc.c` sprel() work. Read this first after a
compaction; do not re-investigate what is marked SETTLED. Last update:
3b-ii complete in the working tree (HEAD e83c618 + uncommitted edits),
all PC gates green including full-tree `make OPT=-O` + check_sh.

## Goal
`cc -O`: make functions that read frame slots fp-free: convert
`[r7, #k]` reads to `[sp, #...]` (sprel) so frameless() can delete
`mov r7, sp` and drop r7 from the push/pop lists. Tasks 1, 2, commit A
and 3b-i are committed (26037cf, 34bf8fe, 52a717a, e83c618). 3b-ii is
done in the working tree, uncommitted (cc.c, check_frame.py,
check_prolog.py).

## Final model (3b-ii) - supersedes the band model in git history
- Uniform mapping: EVERY r7-relative literal converts to
  `[sp, #k + sz + d - 4]` (hold loads at phold included, same formula).
  The frameless entry list drops exactly ONE word - r7's - so every
  literal shifts by 4 down: caller's band words and our own frame slots
  alike, at any spshift and any live spill depth d. Band aliasing and
  the band/local conflict are non-issues: prescan, spbandok, spfbandok
  and the nparams band bail are DELETED.
- `sz > 0` bails (frameless + screen): the reservation unwinds at the
  exit through `mov sp, r7`, which frameless deletes. Future extension
  (3b-iii): inject `add sp, #sz` at exits to admit sz > 0.
- Screen order: depth walk -> `nconv`/`d` checks -> frameless
  ACCEPTANCE scan (phold, sz, entry push + lr test, extra-pops loop,
  full fl_frame scan with spconv tentatively set, fl_outside) BEFORE
  the rewrite walk touches any line. The converted offsets bake in the
  dropped word, so the frame MUST actually go; every bail in the scan
  resets spconv = 0.
- Depth walk (both walks, same arms, same order):
  - d/ps/na: d = live words below the frame; ps = pending result-save
    flag; na = counted window words not yet drained.
  - setup line counts its push only when its OWN next line is the push
    ... EXCEPT the argpush arm counts AT THE PUSH LINE (`mv = spd(i)`,
    na += mv/4, d += mv): the setup forms are unbounded (add.w lea is
    not in fl_argsetup - that gap miscompiled wdollar in sh.c: a param
    read after a `push {r0}` spill converted with d=0), the push line
    is where the word lands. fl_argpush answers 1 for both, so either
    line enters the arm; only the push adds.
  - `bl`: no counter changes (the window waits for its `add sp, #N`).
  - `add sp, #N` drain arm: pops N/4 words newest first - counted args
    (na, each entered d at its push), then a pending save (ps), then
    plain words; `pop {..}` claim arm: same order, word count from spd.
  - exit pop of pc with na || ps || d open: bail (tail-call windows
    bail). `mov sp, r7` is excused (fl_excused).
  - sp_syscall_save window skip and strsave/fl_resultsave arms keep
    their committed places.
- fl_frame relaxation: when sprel_ok, a line that moves sp is excused
  if it is a converted slot read (split >= 0) or a CONSTANT
  `add sp, #N`/`sub sp, #N` (the cleanup the depth walk itself drained);
  linked-.s forms like `add sp, r7, #4` keep the frame.

## Kernel r7 ruling (SETTLED)
kernel.ino never reads the application's r7 (grep 0): task switch is
setjmp + saved sp/sys_sp. Converted functions containing syscalls are
safe in principle; `csys` in tests/opt/prolog.c still keeps its frame
because the builtin argv window is outside the walk's forms. The old
"r7 must survive syscalls" premise in the old pin comments was wrong.

## Gates - all green at this state (2026-11-20)
- check_ref: `different: 0`. bench.py --pc: 11/11 (fib -O == gcc).
- check_frame ok, check_prolog ok (pins updated: `framed`, `cret`,
  `early` now pin the CONVERTED shapes - push {lr}, [sp,#4] reads,
  pop {pc}; csys pin kept with corrected comment).
- check_basic --pc: 139/139 + ASan clean.
- `make clean && make OPT=-O && make host && check_sh --pc`: 5/5
  (the -O-built sh, cmp and friends run under qemu/host_linux).
- Plain tree restored afterwards (`make clean && make`).

## Crash triage recipe (proven this round)
- `./compiler/cc -O -I lib -o root/bin/sh.bin userland/sh.c`, repro via
  `printf '...' | qemu-arm ./host_linux -r root /bin/sh.bin`.
- gdb: `(sleep 1; gdb-multiarch -batch -ex 'set confirm off' -ex
  'target remote localhost:1235' -ex 'continue' -ex 'info registers pc
  lr sp ...' -ex 'x/6i $pc' </dev/null) & printf ... | qemu-arm -g 1235
  ./host_linux -r root /bin/sh.bin`. Map pc with
  `./build/host/objdump -d root/bin/sh.bin` (guest text base is low,
  host mappings live at 0x408...; the .bin offsets are what objdump
  prints).
- Per-line walk traces: probe copy of cc.c as `compiler/dbg_sprel.c`
  (+ gcc -O1 -I . -w -o compiler/dbg_cc compiler/dbg_sprel.c). It MUST
  live in compiler/ (cc resolves build/host/as from argv[0]'s dir).
  fprintf(stderr, "S i=%d t=[%s] d=%d ps=%d na=%d") at the top of each
  walk's L_INS branch (S = screen, R = rewrite); SPREL_BAIL="name1,
  name2," env forces a per-function bail - match name+comma through
  s->len (s->name is NOT NUL-terminated, strstr on it silently fails).
  Compare S vs R line by line to find walk divergences.
- Probe traps: rebuild probes from fresh source (a stale /tmp copy
  silently compiled an old model once); check_sh fails ALL cases when
  ./host_linux is missing (make clean deletes it - run `make host`,
  every ad-hoc .bin run then "crashes" as exit 1); write test .c files
  with a heredoc, NOT shell printf - the outer printf eats `%d` and
  bakes literals like "check 0 0" into the source.

## NEXT (3b-iii round, 2026-11-21)
0. 3b-iii lives in .superpowers/sdd/2026-11-21-cc-sz-frameless/progress.md;
   spec docs/superpowers/specs/2026-11-21-cc-sz-frameless-design.md.
   Commits 6954959 e6774ee fb2f4fa on cc-prologue. PC gates green;
   board gates ALL GREEN (selfhost IDENTICAL x6, bench board
   2.04x 11/11, basic 139). Gotchas this round: fl_list single-digit reg
   names (fixed); frameless npro zeroing killed flush-peep hold merges
   (removed); make clean deletes host_linux BITES AGAIN.
## OLD NEXT
1. Merge branch cc-prologue at the user's word. All PC and board gates
   are GREEN (2026-11-20): check_toolchain --selfhost byte-identical +
   test4..12; bench --board 11/11 2.04x on hardware; check_basic
   --board 139. Commits ce4daa4/e54c9fd/d9cd3f9/2980981/177dbcc on
   cc-prologue. check_sh has no --board mode (PC suite only).
2. Board gotchas learned: /src flash full makes put fail SILENTLY
   (clear leftover X2.bin/cc3-5.bin/sources first; a flash `df` does
   not exist); board.py put now handles 0-byte files (fixed +
   committed 2980981).
3. Possible follow-up rounds (not in this plan): 3b-iii inject
   `add sp, #sz` at exits so sz > 0 functions convert too; the
   flash-space papercut (board.py could show the kernel's error line on
   a failed put).

## Operational lessons (this repo, this session)
- Apply cc.c changes ONE hunk at a time with the edit tool; scripted
  multi-hunk python assert+replace repeatedly left mixed states.
- Committed cc constraints hit so far: duplicate `int n` in one decl
  list; `{ }` empty block; `lpool + ltext[i][5]` precedence.
- Do NOT dispatch subagents (user rule: hardware runs one model).
- Commit/push/merge only when asked. No em dashes in messages/comments.
- /tmp/t1.c (fib witness), /tmp/t2.c (nested fib(fib(n-1))) and
  /tmp/in.txt (sh for-loop repro) are throwaway repros; recreate from
  tests/bench/fib.c and the check_sh cases if lost.
