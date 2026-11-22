# Papercuts

Friction the repository itself can sand down. See the two-question test in the
papercuts skill before adding entries.

## Open

- [ ] `2026-11-22T17:54:35Z` — `claude` — AGENTS.md's triage table has no row for "added a userland program", so a new tool in `src/userland/` that uses a syscall builtin newer than the reference cc passes every gate except `check_ref`, which then reports it `different: 1` (happened with `date.c` on the clock round). Smallest fix: a triage row "new/changed `root/bin` program → `check_ref.py`".
- [ ] `2026-11-22T17:54:35Z` — `claude` — AGENTS.md's triage table says "their `--board` variants" for every checker, but not all checkers have one (`check_sh.py` has no `--board` mode), so following the table verbatim fails with an argparse error. Smallest fix: reword the column (e.g. "board variants where they exist").
