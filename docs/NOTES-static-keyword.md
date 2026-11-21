# NOTES: `static` keyword in cc (round 2026-11-21) — DONE

Goal: full `static` (Approach A, owner-approved) — internal linkage for
file-scope vars/functions + static locals, global-init semantics. Spec:
`docs/superpowers/specs/2026-11-21-static-keyword-design.md` (Results there
too). SDD ledger (all rulings): `.superpowers/sdd/2026-11-21-static-keyword/progress.md`
until deleted at finish — the rulings are also in the commit messages.

## Settled (don't re-investigate)

- `as`/`ld` needed zero changes: ld resolves cross-file only against global
  definitions and dup-checks only globals (ld.c:11-18, 186-201); `ldref`'s
  `s = 0` form (`.Ldata+n`, emit.c) gives static data without labels — and
  with `s = 0` the printed `n` is the whole `.Ldata` offset (call
  `lea_data(0, val, val)`).
- Vars: classes `CSta = 10` / `CStaArr = 11` (CLoc-keyed -O/param loops skip
  them); functions: `CFun` + `Sym.statik` (saved/restored as `hstatik` in
  newlocal, newstatic and the param loop). File-scope static declarations
  seed `h*` = self so the function-end restore loops no-op on them —
  without the seed, the first function end clobbers every file-scope static.
- Statics never enter `gsym` — `write_data` untouched.
- Coded static-local inits use `init_splice(from, lpf)`: only the lines
  added since the save move to `.Linit`; `init_close` would flush the whole
  function body (segfault). Constants need nothing (image writes).
- C forbids non-constant static initializers; cc accepts them (the existing
  global `.Linit` extension). gcc-diffed tests must use constant inits.
- cc has no block-scope restore (pre-existing, ref cc reproduces with plain
  locals) — shadow tests pin function-end restoration.
- Names are not NUL-terminated (`oname` copies `len` bytes) — compare with
  `strncmp(s->name, "main", 4)`, not `strcmp`.
- Token `Static` after `Extern`; registered in the `p = "extern static"`
  loop; storage flags are order-insensitive with a `duplicate static` guard.

## Gates at completion

`make check` ok; `check_ref` different: 0 (39 identical, 1 static-syntax
exclusion); `bench --pc` 11/11; `check_make --pc` 13/13; `check_toolchain
--pc` 8 cases + 6 errors (3 new); test15 vs gcc plain + `-O`; board: see
commit message / final report (selfhost incl. test15 on-board, check_make
board).

## Follow-up round (not this one)

Adopt `static` in `compiler/*.c`: give each file's private globals/statics
internal linkage (retires cc.h's unique-name rule), after the new cc is the
committed board cc and the selfhost fixed point is re-proven. Its diff
should be binaries-only in effect.
