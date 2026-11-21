# `static` in cc — design (2026-11-21)

Approach A approved by the owner: full `static` — file-scope internal linkage
(globals and functions) plus static local variables, with global-init semantics
for initializers (constant data or computed code in `.Linit`, like globals).
Tests only this round: `compiler/*.c` cannot *use* `static` yet (the committed
cc must first parse it — bootstrap), adoption in the toolchain is a separate
future round after the `--selfhost` fixed point holds.

## Why this is cheap here (verified)

- **Internal linkage = omitting `.global`.** `ld` resolves cross-file references
  using *only global definitions* and hashes only globals for its
  duplicate-symbol check (linker/ld.c:11-18, 186-201). In-file relocations
  resolve to the defining object's own symbols. So `as` and `ld` need **zero
  changes**; cc just stops emitting `\t.global\tNAME` at stmt.c:326 (functions)
  and stmt.c:397 (data labels).
- **Static data needs no label at all.** `ldref` (emit.c:118-121) already has
  the dual form: `NAME+n` for a named global, `.Ldata+n` for `s = 0` (the
  string-literal path). A static variable takes the `.Ldata+n` form, so two
  functions in one file can each have `static int i;` without any label
  collision. `init_addr` (stmt.c:25) reaches the same path with `isym = 0`,
  which routes static-local initializers into `.Linit` — correct C semantics
  (init once, before main) using the mechanism globals already have.

## Semantics

- `static` file-scope variable/function: internal linkage. A reference from
  another file is an undefined-symbol link error; another file may define the
  same name freely.
- `static` local: global storage, name visible only in its function
  (shadowed/restored like any block symbol), initialized once before main
  with the same initializer subset as globals (constant or `.Linit` code).
- Initializers follow the **global-init path** (`.Linit` computed init
  allowed) — consistent with globals, zero extra code.
- `static` + `extern` on the same declaration: `die()`. `static main`: `die()`
  (the entry stub needs a global `main`). Redefinition checks extended: a
  name defined `static` may not later be defined non-static (and vice versa).

## Symbol representation

Token: `Static` appended after `Extern` in the token enum (cc.h:21-27 —
operators' order is load-bearing, new keywords go last). Registered in
cc.c's driver next to `extern`: `p = "extern static"; i = Extern;
while (i <= Static) { next(); id->tk = i; i = i + 1; }`.

Variables (file-scope and local alike) — **new classes** so the existing
`CLoc` loops (`-O` slot passes regs/fp, param fixup stmt.c:323/324) skip them
naturally:

- `CSta = 10` — scalar static, `val` = data-image offset.
- `CStaArr = 11` — static array: name is the address, exactly like class 6
  (global array), no frame slot.

Functions — `CFun` stays, plus a new `int statik;` field in `struct Sym`
(checked only at the label-output site; `newlocal` saves/restores `hstatik`
alongside `hclass`/`htype`/`hval` so a block symbol shadowing a static
function cannot resurrect the flag — 3 one-line edits).

## Sites (all verified this round)

1. **cc.h** — `Static` in the token enum; `CSta`/`CStaArr` in the class enum
   (cc.h:29, comment lists 6=global array, 8=typedef, 7/9=extern); `statik`
   and `hstatik` in `struct Sym`.
2. **cc.c ~55** — keyword registration (pattern above).
3. **types.c:27 `is_type()`** — unchanged (static is not a type); the two
   declaration entry points accept it before the type and `die("type expected
   after static")` if none follows: **stmt.c parse_global** (top-level, next
   to the `ext` flag at :285) and the **local-declaration branch**
   (stmt.c:229). A `static` in a parameter list fails naturally there.
4. **parse_global** — variable branch: allocate at `dpos` as today but set
   class `CSta`/`CStaArr`, register in `gsym`/`goff` (write_data's offset
   walk), initializer with `isym = 0`. Function branch: set `statik`; the
   `.global` at :326 becomes conditional. `static`+`ext` die; `static main`
   die; duplicate-global checks (:292, :344, :354) extended to the static
   classes.
5. **write_data (stmt.c:389-397)** — skip `.global` and the label for
   `CSta`/`CStaArr` entries (storage is still in the data image).
6. **local declarations (stmt.c:229-250)** — with the static flag: allocate
   data like parse_global (helper `newstatic()` mirroring `newlocal()`:
   save `h*`, set class, `val = dpos`), no frame slot, no array pointer
   cell; initializer `ikind = 0; isym = 0; ibase = s->val` (arrays:
   `init_array` as globals).
7. **references** — `init_addr` (stmt.c:25): `isym == 0` →
   `lea_data(0, ibase + off, ibase + off)`. expr.c primary (148-169):
   `CStaArr` → address `lea_data(0, 0, val)` (like class 6); `CSta` → same
   + `load`. Static functions need nothing: they stay `CFun`, keep their
   plain `NAME:` label (no `.global`), so `call_fn`/`fn_addr` are untouched.
8. **restore loops (stmt.c:317, :346)** — restore also
   `class == CSta || class == CStaArr` (a static global shadowed inside a
   function must come back at `}`; local statics leave with their function).
   `newlocal` also restores `statik` (see above). Loops :323/:324 stay
   `CLoc`-only (correct: statics have no slots; the `-O` passes never see
   them — `.Ldata+off` is a form they already optimize for literals).

## Tests (this round's deliverable)

`compiler/tests/test15.c` — standard C (gcc builds it natively for reference
output, like test4+): static global var/function used across several
functions, same-named static locals in two functions (collision test), static
arrays, constant *and* computed initializers (`.Linit` path) for both static
globals and static locals, shadowing (a local named like a static global),
static function called through a function pointer (`fnaddr`). Plain and `-O`
builds compared under qemu; error cases (`static`+`extern`, `static main`,
redefinition) as checker error cases.

**check_ref**: the ref cc (`e4fb427`) cannot parse `static` (it dies with the
generic "type expected near token N"), so the new test file is excluded by
explicit **file name** — extend the checker's exclusion mechanism with a
third class beside the init-code and newer-builtins ones. The other 39
corpus files keep gating byte identity, and every non-static path must stay
byte-identical anyway.

## Gates (AGENTS triage: cc codegen + new syntax)

PC: `make check`; `check_ref` different: 0; test15 vs gcc under qemu, plain
and `-O`; `bench.py --pc`; `check_make --pc`.
Board (standing permission): `check_toolchain.py --selfhost` (the committed
board cc must still rebuild the new cc byte-identically — it only *parses*
`static` in the sources, never compiles with it), `check_make --board`,
test15 built and run on the board.

## Out of scope

Using `static` inside `compiler/*.c` (retiring the globally-unique-name rule
in `cc.h`) — needs the new cc committed first; its own round with only
binaries as the diff. `static` in BASIC — different language, no. Inline
storage-class keywords in parameters — invalid C, unhandled.

## Results (landed 2026-11-21)

Implemented as designed with four deviations, all ledgered:

- The new test is `test15.c`, not `test14.c` — test14 was taken (float
  s-register test); `check_ref.STATIC_TESTS` excludes it by name.
- Statics never register in `gsym` at all, so `write_data` needed no change
  (cleaner than the spec's "skip the label"; same net effect).
- `lea_data(0, n, off)` prints `n` literally, so static references pass
  `(0, val, val)` — not `(0, 0, val)` as the plan text had it.
- A static local's *coded* initializer could not call `init_close` (it
  flushes the function's whole line list into `.Linit` — crash mid-body);
  `init_splice` moves only the initializer's trailing lines. Constants
  write the image directly and need nothing. Computed initializers for
  statics extend C the way globals already do (gcc rejects them; the
  gcc-diffed `test15.c` therefore uses constant inits, and the once-before-
  main property is pinned by a qemu case).
- Block scoping (restoring shadowed names at `}`) is a pre-existing cc
  limitation, reproduced with the reference cc on plain locals — the
  shadowing test pins restoration at function end instead.

Gates: `make check` ok; `check_ref` different: 0 (39 files); `bench --pc`
11/11; `check_make --pc` 13/13; `check_toolchain --pc` 8 cases + 6 errors;
test4–12 still match gcc; test15 matches gcc plain **and** `-O`;
`--selfhost` and `check_make --board` as reported in the ledger.
