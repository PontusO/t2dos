# NOTES: `static` keyword in cc (round 2026-11-21)

Goal: full `static` (Approach A, owner-approved) — internal linkage for
file-scope vars/functions + static locals, global-init semantics. Spec:
`docs/superpowers/specs/2026-11-21-static-keyword-design.md`. Tests only
this round (bootstrap: the committed cc can't parse `static`, so the
toolchain sources can't use it yet).

## Settled (don't re-investigate)

- `as`/`ld` need zero changes: ld resolves cross-file only against global
  definitions and dup-checks only globals (ld.c:11-18, 186-201); `ldref`'s
  `s = 0` form (`.Ldata+n`, emit.c:118-121) gives static data without labels.
- Vars: new classes `CSta = 10` / `CStaArr = 11` (CLoc loops then skip them).
  Functions: `CFun` + `statik` field (`hstatik` saved in newlocal so a
  shadowing block symbol can't resurrect the flag).
- `init_addr` with `isym = 0` routes static-local inits to `.Linit` with the
  `.Ldata+off` target form.
- Token `Static` goes after `Extern` (operators' order load-bearing);
  registration next to `p = "extern"` at cc.c:55.
- Restore loops stmt.c:317/:346 must grow (`CSta`/`CStaArr` + `statik`);
  :323/:324 must NOT (no slots).
- check_ref: exclude test15.c by file name (ref cc error text too generic).

## Status

- [ ] spec approved
- [ ] plan (writing-plans)
- [ ] implemented, gated, (push only on owner's word)
