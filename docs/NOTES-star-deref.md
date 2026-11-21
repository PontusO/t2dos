# NOTES: the `*p` rvalue dereference is N+1 loads (cc latent bug)

Found 2026-11-21 while landing `userland/make.c` (make is the first tool in
the tree to dereference a `char **`). Do not re-investigate; the facts below
were measured on binaries (objdump + qemu runs), not read off listings.

## Fact

An rvalue dereference `*p` where `p` has pointer depth N emits **N+1** loads;
the extra load is the variable-reference handler's own `load(ty)`
(`expr()` `tk == Id` fallback: `lea_loc/lea_var` then `load(ty)`, cc.c
~3784-3789), which is not rolled back when the value is then dereferenced.
Verified: `*p` on `char *`/`unsigned char *` = `ldr`+`ldrsb` (works by
accident: the word load fetches the address, the byte load reads the char);
`*pp` on `char **` = 3 loads; `**pp` = 4 loads; `*p` on `int *` = 2 loads
(returns `**p` at runtime, proved with distinct probe values).
`*p = v` (lvalue) is CORRECT: `lvalue()` deletes the last load.
`p[i]`, `p->m` are correct (different path). Data layout is byte-packed;
kernel builtins (strlen/strcpy/printf %s) are byte-based; a byte walk via
`p = p + 1` with `*p` is correct for depth-1 pointers.
`build/ref/cc` (e4fb427) emits it identically - pre-existing, not a 3d
regression. No committed tool hits depth >= 2 rvalue derefs, and depth-1
rvalue derefs work by accident, which is why the tree is fine.

## Consequences

- `char **pp` params with `*pp` reads are MISCOMPILED. Avoid until fixed:
  make.c was rewritten to the working idiom (global cursor + `buf[i]`
  indexing, like sh.c's `ap`).
- Pointer arithmetic on `char *` (`t - b1`) scales as words: unreliable in
  EITHER direction. Index with ints instead.
- Fixing the compiler changes plain-path bytes -> breaks `check_ref` by
  design (the ref compiler is pinned at e4fb427); a cc-fix round must decide
  how to move `build/ref/COMMIT` and re-baseline the byte-identity gate.
  Full triage then: check_ref, make check, tests vs gcc, selfhost on board.
