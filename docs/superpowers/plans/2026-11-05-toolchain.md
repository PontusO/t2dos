# T2DOS Toolchain Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** An assembler, linker, archiver and inspection tools for T2DOS, and a `cc` that emits assembly and runs them, so `cc hello.c` still builds `hello.bin` in one step.

**Architecture:** `as` reads GNU ARM unified-syntax assembly and writes T2DOS object files (`lib/t2obj.h`). `ld` links objects and archive members into today's `.bin` (no kernel change). `cc` keeps its parser and code generator but writes assembly lines instead of machine code, then `exec`s `as` and `ld`. The instruction encodings leave `cc.c` for `assembler/as.c`.

**Tech Stack:** The T2CC C subset (every tool must build with the committed `cc` and with gcc on the PC). Test oracles on the PC: `arm-none-eabi-as`, `arm-none-eabi-objdump`, `arm-none-eabi-objcopy` from `~/.arduino15/packages/rp2040/tools/pqt-gcc/5.0.0-9576866/bin/`, gcc, Python 3. Run target: the CPico 2350 board over `/dev/ttyACM*`.

**Spec:** `docs/superpowers/specs/2026-11-05-toolchain-design.md`

## Global Constraints

- The `.bin` format is unchanged: `"T2CC"`, code size, data size, reserved 0, entry offset (32-bit LE), code, data. No kernel change.
- ABI unchanged: args on the stack left to right (first deepest), caller pops, result in `r0`, `r7` frame, `r8` = kernel `sys()`, `r9` = data base, code PC relative.
- **Code model rule:** a data symbol's value is its offset in the data segment (the address minus `r9`). A text symbol's value is its offset in the code segment. There are no absolute addresses.
- Bootstrap rule: tool sources use only what the committed `cc` compiles; a new language feature may be used only after a commit that supports it. `cc.c` itself never uses a feature added in the same change.
- Every encoding `as` produces must equal `arm-none-eabi-as` output for the same source (except T2DOS pseudo-instructions, which have no GNU form).
- `as`, `ld`, `ar` are separate programs; `cc` runs them with `exec` on the board (owner's decision).
- Intermediate files are acceptable (normal use is an SD card); on the board they go to `/tmp`, created if missing.
- New code generation is disassembled with `arm-none-eabi-objdump` before it runs on the board (a bad encoding hangs the board until the owner resets it).
- No em dashes anywhere (owner's global rule): code, comments, docs, commit messages.
- Commit messages end with the attribution lines given by the harness; commit only what the task touched (never `git add -A` with unrelated changes).

## Review Focus

1. **C names that are also register or mnemonic names** (`sp`, `pc`, `r1`, `b`, `add`, `push`) used as functions or globals must link and run: `as` decides register vs symbol by operand position, never by spelling. Test: Task 9 `names.c`.
2. **Data beyond 4 KB across files:** an `addw` data reference that ends up above 4095 after linking must stop `ld` with an error naming the symbol and file, not produce a wrong offset. Test: Task 5 `ld_addw_overflow`.
3. **Errors in hand-written assembly** (unknown mnemonic, immediate that does not fit, branch out of range, undefined local label) must report `file:line: error: ...` and exit 1, never write a wrong object. Test: Task 3 `as_errors`.
4. **Library chains:** a member pulled in for one symbol that itself needs a symbol from another member (in the same or another archive, either order) must be resolved. Test: Task 6 `ar_chain`.
5. **A failed step in `cc`'s pipeline** (assembler error, link error) must stop `cc` with a non-zero exit, show the error, and remove the temporary files. Test: Task 9 `pipeline_errors`.

---

## File Structure

| File | Responsibility |
|---|---|
| `lib/t2obj.h` | Object and archive format: structs, constants, relocation types. Included by `as`, `ld`, `ar`, `nm`, `objdump`. Copied to `/lib` by the Makefile. |
| `lib/t2obj.c` | Reading and writing objects and archives (`#include`d like `net.c`). |
| `assembler/as.c` | Assembler: lexer, directives, symbols, instruction encoder, object writer. |
| `linker/ld.c` | Linker: load objects and archives, resolve, lay out, relocate, write `.bin`. |
| `binutils/ar.c`, `binutils/nm.c`, `binutils/objdump.c` | Archiver, symbol lister, dumper and disassembler. |
| `compiler/cc.c` | Compiler: now emits assembly and drives `as`/`ld`. |
| `tests/asm/*.s`, `tests/link/*`, `tests/toolchain/*` | Test inputs and expected outputs. |
| `tools/board.py` | Serial harness for the board: upload, run, read back, compare. |
| `tools/check_asm.py`, `tools/check_ref.py` | PC-side checks against the GNU tools and the reference `cc`. |
| `Makefile` | Builds all tools for the PC and `root/bin/`; `make check` runs the PC-side checks. |

---

### Task 1: Test harness and reference compiler

**Files:**
- Create: `tools/board.py`
- Modify: `Makefile` (targets `ref`, `check`), `.gitignore` (`/build/` already ignored)

**Interfaces:**
- Produces: `tools/board.py` as a module and CLI. Class `Board(port)` with `put(local, remote)`, `run(cmd, wait) -> list[str]` (output lines without echo and prompt), `fetch(remote) -> bytes` (via `hexdump`), `expect_gcc(test_c, flags='')` (compile `test_c` with gcc natively, run the T2CC build on the board, compare stdout). CLI: `python3 tools/board.py put|run|fetch ...`. Port from `$T2DOS_PORT` or the first `/dev/serial/by-id/*CPico*`.
- Produces: `make ref` builds `build/ref/cc` (gcc build of `compiler/cc.c` at the commit recorded in `build/ref/COMMIT`, default `040b630`) via `git show <commit>:compiler/cc.c`. This is the byte-identity reference for Task 8.

- [ ] **Step 1:** Write `tools/board.py` (the session's scratchpad `term.py` and hexdump read-back are the starting point; pyte is optional and only for `edit` tests). Opening the port with `write_timeout=5` so a hung board raises instead of blocking.
- [ ] **Step 2:** Verify: `python3 tools/board.py run 'free'` prints the `sram free` line; `fetch /bin/echo.bin` equals `root/bin/echo.bin`.
- [ ] **Step 3:** Add `make ref`; verify `build/ref/cc -o /tmp/x.bin userland/echo.c` equals `root/bin/echo.bin`.
- [ ] **Step 4:** Commit: `tools: board harness and reference compiler`.

---

### Task 2: Object format and `nm`

**Files:**
- Create: `lib/t2obj.h`, `lib/t2obj.c`, `binutils/nm.c`, `tests/obj/mkobj.py`, `tests/obj/sample.expected`
- Modify: `Makefile` (build `binutils/*` for the PC as `build/host/<tool>` and as `root/bin/<tool>.bin`; copy `lib/t2obj.h` and `lib/t2obj.c` to `root/lib/`)

**Interfaces:**
- Produces, in `lib/t2obj.h` (all fields 32-bit little endian, plain `int`, file order as listed):

```c
// object file: header, text bytes, data bytes, symbols, relocations, string table
struct t2obj_hdr { int magic; int version; int text_size; int data_size; int nsyms; int nrels; int str_size; };
enum { T2OBJ_MAGIC = 0x424F3254, T2OBJ_VERSION = 1 };          // "T2OB"
struct t2sym { int name; int sect; int value; int flags; };      // name: offset in the string table
enum { SEC_UNDEF = 0, SEC_TEXT = 1, SEC_DATA = 2, SEC_ABS = 3 };
enum { SYM_GLOBAL = 1, SYM_INIT = 2 };                           // SYM_INIT: a run-before-main routine
struct t2rel { int sect; int off; int sym; int type; int addend; };   // sym: index into the symbol table
enum {
  R_BL = 1,          // bl (T1, 24-bit) at off: target = S + A
  R_BW = 2,          // b.w (T4, 24-bit)
  R_BCOND = 3,       // b<cond>.w (T3, 20-bit)
  R_DATA_ADDW = 4,   // addw rd, rn, #imm12: S + A, must be 0..4095
  R_DATA_MOVW = 5,   // movw rd, #imm16: low 16 bits of S + A
  R_DATA_MOVT = 6,   // movt rd, #imm16: high 16 bits of S + A
  R_DATA_MOVW16 = 7, // movw without a movt: S + A must be 0..65535
  R_FNADDR = 8,      // movw/movt/add rd,pc (10 bytes at off): S + 1 - (off + 12), the PC-relative Thumb address
  R_WORD = 9         // 32-bit word: S + A (an offset, since there are no absolute addresses; e.g. jump tables)
};
// archive: header, index (symbol name offset, member number), member table, string table, member objects
struct t2ar_hdr { int magic; int version; int nmembers; int nindex; int str_size; };
enum { T2AR_MAGIC = 0x52413254 };                                 // "T2AR"
struct t2ar_index { int name; int member; };
struct t2ar_member { int name; int offset; int size; };          // offset from the start of the archive file
```

- Produces, in `lib/t2obj.c`: `struct t2o { struct t2obj_hdr h; char *text; char *data; struct t2sym *syms; struct t2rel *rels; char *str; };` with `int t2o_read(char *buf, int size, struct t2o *o)` (0 ok, -1 bad magic or truncated), `int t2o_write(int fd, struct t2o *o)`, `char *t2o_name(struct t2o *o, int sym)`, `char *t2_readfile(char *path, int *size)`. Task 6 adds `struct t2ar { struct t2ar_hdr h; struct t2ar_index *index; struct t2ar_member *members; char *str; char *base; };` with `int t2ar_read(char *buf, int size, struct t2ar *a)` (0 ok, -1 bad) and `int t2ar_member(struct t2ar *a, int i, struct t2o *o)`.
- Produces: `nm file.o|file.a...` prints `%08x %c name` per symbol, sorted by name, where the letter is `T`/`D`/`A`/`U` for text/data/abs/undefined (lower case for local symbols); for an archive, a `member.o:` heading per member.

- [ ] **Step 1: Write the failing test** `tests/obj/mkobj.py`: writes `tests/obj/sample.o` with two text symbols (`main` global at 0, `helper` local at 8), one data symbol `counter` global at 4, one undefined `puts`, one `R_BL` relocation; `tests/obj/sample.expected` holds the exact `nm` output:

```
00000004 D counter
00000008 t helper
00000000 T main
         U puts
```

- [ ] **Step 2:** Run `make build/host/nm && build/host/nm tests/obj/sample.o | diff - tests/obj/sample.expected`; expected: fails (no `nm`).
- [ ] **Step 3:** Implement `lib/t2obj.h`, `lib/t2obj.c` and `binutils/nm.c` (undefined symbols print 8 spaces for the value).
- [ ] **Step 4:** Rerun Step 2; expected: no diff. Also `make` builds `root/bin/nm.bin` with `compiler/cc`.
- [ ] **Step 5:** Add the check to `make check`. Commit: `binutils: T2DOS object format and nm`.

---

### Task 3: Assembler core

**Files:**
- Create: `assembler/as.c`, `tests/asm/core.s`, `tests/asm/errors/*.s` (+ `.expected`), `tools/check_asm.py`
- Modify: `Makefile` (`build/host/as`, `root/bin/as.bin`; `make check` runs `tools/check_asm.py`)

**Interfaces:**
- Consumes: `lib/t2obj.h`, `lib/t2obj.c` (Task 2).
- Produces: `as [-o out.o] file.s` writes an object; errors print `file:line: error: message` and exit 1 without writing the output.
- Produces: `tools/check_asm.py file.s...`: assembles each with `build/host/as` and with `arm-none-eabi-as -mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16` (plus `objcopy -O binary -j .text`/`-j .data`), and compares the text and data bytes. For files with relocations it compares after linking at offset 0 only once Task 5 exists; in this task, test files resolve everything locally.

Scope of this task (all must match GNU byte for byte):
- Lines: `label:`, instruction, directive, comments `@` and `//` and `/* */`; `.syntax unified`, `.thumb`, `.text`, `.data`, `.global`/`.globl`, `.extern`, `.word`, `.short`/`.hword`, `.byte`, `.ascii`, `.asciz`/`.string`, `.space`/`.zero n[,fill]`, `.align n` (power of two, pads text with `nop`/zero), `.balign n`, `.thumb_func` (no-op), `.equ`/`.set name, expr`.
- Expressions: decimal, hex, binary, char literals, symbol, `sym + const`, `sym - const`, `.` (current location), unary `-`.
- Instructions: `mov`/`movs` (register and immediate, including modified-immediate `mov.w` and `movw`/`movt` with plain immediates), `add`/`adds`/`addw`/`sub`/`subs`/`subw` (register, immediate, SP forms `add sp, #n`, `sub sp, #n`), `cmp`, `cmn`, `and`/`orr`/`eor`/`bic`/`mvn`/`tst` (with and without `s`, `.w`), `lsl`/`lsr`/`asr`/`ror` (immediate and register, narrow and `.w`), `mul`/`muls`/`mls`/`sdiv`/`udiv`, `neg`/`negs`, `sxtb`/`uxtb`/`sxth`/`uxth`, `ldr`/`str`/`ldrb`/`strb`/`ldrh`/`strh`/`ldrsb`/`ldrsh` with `[rn]`, `[rn, #imm]`, `[rn, rm]`, `[sp, #imm]`, and `.w`, `push`/`pop` (narrow lists and `.w`), `b`/`b.w`/`bl`/`b<cond>`/`b<cond>.w` to local labels, `bx`/`blx` register, `nop`, `it`/`ite`/`itt`/`itet`... with conditional instructions inside the block (`moveq r0, #1`).
- Narrow vs wide: like GNU, choose the 16-bit encoding when the operands allow it unless `.w` is given; `addw`/`subw` always T4.
- Operand parsing: register names (`r0`..`r15`, `sp`, `lr`, `pc`, `ip`, `fp`) only where the syntax expects a register; anywhere a symbol is expected (branch targets, `#sym`, `.word sym`) any identifier is a symbol, even `sp` or `add`.

- [ ] **Step 1: Write the failing tests:** `tests/asm/core.s` with at least one line per instruction form above (both register-number extremes, immediate edge values such as `#0`, `#255`, `#256`, `#4095`, `#65535`, negative offsets), and `tests/asm/errors/` cases: unknown mnemonic, `movs r0, #256`, `ldr r0, [r1, #3]` (misaligned), branch to an undefined local label, `b` out of range, duplicate label, each with its expected `file:line: error:` first line.
- [ ] **Step 2:** Run `make check`; expected: fails (no `as`).
- [ ] **Step 3:** Implement `assembler/as.c`. Approach: one pass collecting instructions into a list with their size chosen at parse time (narrow if the operands allow it and no `.w`; branches to labels not yet defined use the wide form unless written narrow), then a second pass that resolves labels, encodes and range-checks. The `I_*` constants in `compiler/cc.c` are verified encodings to start from.
- [ ] **Step 4:** Run `make check`; expected: `core.s` bytes equal GNU's, and every error case prints its expected line and exits 1.
- [ ] **Step 5:** **Corpus check:** `tools/check_asm.py --corpus` disassembles every `root/bin/*.bin` and `build/ref` test binary with `arm-none-eabi-objdump`, turns it into assembly (branch targets become labels, data becomes `.byte`), and checks that `as` reproduces the original code bytes. Expected at this task: passes for all instructions except FPU, `movw`/`movt` relocation operators and the PC-relative sequence (Task 4); the script reports those lines as "deferred".
- [ ] **Step 6:** Commit: `assembler: core instructions and directives`.

---

### Task 4: Assembler: FPU, relocations and T2DOS pseudo-instructions

**Files:**
- Modify: `assembler/as.c`
- Create: `tests/asm/fpu.s`, `tests/asm/reloc.s`, `tests/asm/reloc.expected` (the `nm`-style symbol list plus a relocation list printed by `tools/check_asm.py --relocs`)

**Interfaces:**
- Produces the operand forms `cc` will emit (Task 8 relies on these exact spellings):
  - `addw rd, r9, #sym[+n]`: data offset, `R_DATA_ADDW` (GNU syntax, T2DOS meaning: the value of a data symbol is its data offset).
  - `movw rd, #:lower16:sym[+n]`, `movt rd, #:upper16:sym[+n]`: `R_DATA_MOVW`/`R_DATA_MOVT`. `movw rd, #:lower16:sym` with no `movt` following for the same register is `R_DATA_MOVW16` when written as `movw.16 rd, #sym` (pseudo, see below).
  - Pseudo-instructions (no GNU form, documented in the README):
    - `fnaddr rd, sym`: 10 bytes `movw rd, #lo; movt rd, #hi; add rd, pc` with `R_FNADDR` (resolved by `as` when `sym` is local, else by `ld`).
    - `movw.16 rd, #sym[+n]`: a `movw` whose value must fit 16 bits (`R_DATA_MOVW16`); `cc` uses it for data offsets 4096..65535, matching today's `imm()` that omits `movt` when the high half is 0.
    - `.t2init sym`: marks `sym` as this object's run-before-main routine (`SYM_INIT`).
  - Branches and `bl` to symbols not defined in this file's text emit `R_BL`/`R_BW`/`R_BCOND`; local ones are resolved by `as`.
  - FPU: `vmov sN, rM`, `vmov rM, sN`, `vadd/vsub/vmul/vdiv.f32`, `vneg.f32`, `vsqrt.f32`, `vcmp.f32`, `vcmp.f32 sN, #0`, `vmrs APSR_nzcv, fpscr`, `vcvt.f32.s32/.f32.u32/.s32.f32/.u32.f32`, `vldr`/`vstr sN, [rM, #imm]`, `vpush`/`vpop`.

- [ ] **Step 1: Write the failing tests:** `fpu.s` (every form above, s0..s31), `reloc.s` (each operand form against a local and an undefined symbol), and the expected relocation list.
- [ ] **Step 2:** Run `make check`; expected: fails on the new files.
- [ ] **Step 3:** Implement. Relocated fields are written as zero in the object (the linker fills them); for GNU comparison, `check_asm.py` links `reloc.s` at offset 0 once Task 5 exists and compares against GNU with the data symbols' offsets substituted as constants.
- [ ] **Step 4:** Run `make check`; expected: all pass, and `--corpus` no longer reports deferred lines.
- [ ] **Step 5:** Commit: `assembler: FPU, relocations and T2DOS pseudo-instructions`.

---

### Task 5: Linker

**Files:**
- Create: `linker/ld.c`, `tests/link/*.s` and `tests/link/*.expected`
- Modify: `Makefile` (`build/host/ld`, `root/bin/ld.bin`)

**Interfaces:**
- Consumes: `lib/t2obj.*` (Task 2), objects from `as` (Tasks 3, 4).
- Produces: `ld [-o out.bin] [-e entry] file.o... lib.a...` writes the `.bin`; entry defaults to `main`. Errors `ld: undefined symbol: name (in file.o)`, `ld: duplicate symbol: name (file1.o and file2.o)`, `ld: data offset of name (file.o) is N, too far for addw` and exit 1.
- Layout: text sections in input order, then archive members in pull order; data likewise, each object's data aligned to 4. If any object has a `SYM_INIT` symbol, `ld` appends an entry stub and makes it the entry: `push {r4, lr}`, `bl init` per object in link order, `pop.w {r4, lr}`, `b.w main`; otherwise entry is `main`'s offset. (Same as today's stub around the init chunks; the stub is emitted by `ld` as bytes, not assembled.)

- [ ] **Step 1: Write the failing tests:** `tests/link/hello.s` (a complete program: `main` calls `printf` through the syscall sequence and returns 0), `two_a.s` + `two_b.s` (call across files, a global in one used by the other), `undef.s`, `dup_a.s` + `dup_b.s`, `ld_addw_overflow` (an object with 5000 bytes of data and an `addw` reference to its last byte from another object), `init_a.s` + `init_b.s` (each with a `.t2init` routine incrementing a shared global; `main` returns it).
- [ ] **Step 2:** Run `make check`; expected: fails (no `ld`).
- [ ] **Step 3:** Implement `linker/ld.c`.
- [ ] **Step 4:** PC checks: the error cases print exactly their expected line; `hello.bin` has the expected header; `check_asm.py` link-and-compare of `reloc.s` (Task 4) passes. Disassemble every produced `.bin`.
- [ ] **Step 5:** Board checks (`tools/board.py`): upload `as.bin`, `ld.bin` and the `tests/link` sources; on the board assemble and link `hello.s` and `two_*.s` and `init_*.s`, run them, compare output and exit codes with `tests/link/*.expected`.
- [ ] **Step 6:** Commit: `linker: link objects into .bin`.

---

### Task 6: Archives (`ar`) and selective linking

**Files:**
- Create: `binutils/ar.c`, `tests/link/lib_*.s`, `tests/link/ar_chain.expected`
- Modify: `linker/ld.c`, `binutils/nm.c` (archives), `lib/t2obj.c`, `Makefile`

**Interfaces:**
- Produces: `ar rc lib.a a.o b.o...` (create or replace members), `ar t lib.a` (list), `ar x lib.a [member]` (extract). The index lists every global defined symbol of every member.
- Produces in `ld`: object files are always linked; archives are searched for members defining a still-undefined global, those are added, and the search repeats over all archives until nothing changes (order of archives on the command line does not matter).

- [ ] **Step 1: Write the failing tests** (`ar_chain`): `lib_a.s` defines `fa` calling `fb`; `lib_b.s` defines `fb` and `fz`; `lib_c.s` defines `fc` (unused). `main.s` calls `fa`. Cases: one archive with all three; `fa` and `fb` in separate archives given in either order. Expected: links, runs, and `objdump`/`nm` of the result shows `fa`, `fb` linked and `fc` absent (check by text size equal to main + lib_a + lib_b).
- [ ] **Step 2:** Run `make check`; expected: fails.
- [ ] **Step 3:** Implement `ar`, archive reading in `lib/t2obj.c`, the search loop in `ld`, archives in `nm`.
- [ ] **Step 4:** Run `make check`; expected: passes. Board check: build and run the `ar_chain` case on the board.
- [ ] **Step 5:** Commit: `binutils: archives; ld links only needed members`.

---

### Task 7: `objdump`

**Files:**
- Create: `binutils/objdump.c`, `tests/obj/objdump_*.expected`
- Modify: `Makefile`

**Interfaces:**
- Produces: `objdump file.o|file.bin|file.a`: header and section sizes, symbols (as `nm`), relocations (`offset type symbol+addend`), and a disassembly of the text section, one instruction per line as `offset: hex   mnemonic operands` in the same unified syntax `as` accepts, with branch targets shown as `symbol+off` when known.

- [ ] **Step 1: Write the failing test:** round trip. For `tests/asm/core.s`, `fpu.s` and the corpus, `objdump -d` output (instruction columns only) fed back into `as` must reproduce the same bytes; plus one exact expected dump of `tests/obj/sample.o`.
- [ ] **Step 2:** Run `make check`; expected: fails.
- [ ] **Step 3:** Implement. Instructions `as` does not know print as `.short 0xNNNN`.
- [ ] **Step 4:** Run `make check`; expected: passes.
- [ ] **Step 5:** Commit: `binutils: objdump with disassembler`.

---

### Task 8: `cc` emits assembly (byte-identical single-file builds)

**Files:**
- Modify: `compiler/cc.c`, `Makefile` (build userland through `cc -S` + `as` + `ld` once this task passes)
- Create: `tools/check_ref.py`

**Interfaces:**
- Consumes: the operand forms and pseudo-instructions of Task 4; `ld`'s init stub (Task 5).
- Produces: `cc -S [-o out.s] file.c` writes assembly. Line format: tab-indented instructions in the Task 3/4 syntax, labels `.L<n>:` for branch targets, functions as `name:` with `.global name`, `.text`/`.data` sections, the data segment as `.byte`/`.word` lines with a local label `.Ldata` at its start and global labels at each global variable. String literals and file-local data are referenced as `.Ldata+off`.

Approach (decided):
- Replace the code buffer with a buffer of assembly lines for the current function; `emit16/emit32` call sites become functions that append one instruction line (`sprintf` into a line, since T2CC has no variadic user functions). `lastload` becomes "number of lines to drop". Branch fixups (`patch_b`, `chain_add`, `chain_patch`, `ret_fix`, `brk_fix`) become label numbers: a branch line refers to a label id, and `patch_b(at, cpos)` attaches a label to the next line. The buffer is written out when the function ends.
- Data references keep today's size choice from the file-local offset: `addw rd, r9, #.Ldata+off` or `#name` when below 4096, `movw.16` + `add.w rd, r9, r1` up to 65535, `movw`/`movt` above. References to data defined in another file (Task 9) always use `movw`/`movt`.
- Function addresses: `fnaddr r0, name`. Calls: `bl name`. Syscalls and everything else: the instructions they are today, spelled so that GNU picks the same encoding (`addw` for T4 adds, `.w` where today's encoding is 32-bit, `moveq`/`movne` inside `ite`).
- Run-before-main initializers: the init chunks become one function per file, `.Linit:` ... `bx lr`-style return, marked `.t2init .Linit`; `ld` builds the stub (Task 5). Programs with such initializers are checked by behaviour, not bytes.
- The `I_*` encoding table and `emit16`/`emit32`/`put16`/`patch_b` machine-code helpers are removed from `cc.c` at the end of this task.

- [ ] **Step 1: Write the failing check** `tools/check_ref.py`: for every `userland/*.c`, `compiler/cc.c` and `compiler/tests/test.c`, `test3.c`, `test4.c`, `test6.c`..`test9.c` (the ones without run-before-main initializers): `build/ref/cc -o ref.bin X` vs `compiler/cc -S X && as && ld`; the `.bin` files must be byte-identical.
- [ ] **Step 2:** Run it; expected: fails (no `-S`).
- [ ] **Step 3:** Implement as described. Keep both back ends working in the middle of the task only if it helps; the task ends with only the assembly back end.
- [ ] **Step 4:** Run `tools/check_ref.py`; expected: every file identical. For `test5.c`, `test10.c`, `test11.c`: disassemble, then `tools/board.py` runs them on the board and compares with gcc (`expect_gcc`).
- [ ] **Step 5:** Board self-hosting: upload `cc.bin`, `as.bin`, `ld.bin`; on the board, `cc -S cc.c`, `as`, `ld`; the result equals `root/bin/cc.bin`. Record the build time and the size of `cc.s` in the commit message.
- [ ] **Step 6:** Commit: `cc: emit assembly; encodings move to the assembler`.

---

### Task 9: `cc` driver, `extern` and multi-file programs

**Files:**
- Modify: `compiler/cc.c`, `Makefile` (userland built with plain `cc`), `README.md`, `CLAUDE.md`
- Create: `tests/toolchain/` cases: `c_calls_asm.c` + `asm_fn.s`, `asm_calls_c.s` + `c_fn.c`, `globals_a.c` + `globals_b.c`, `names.c` + `names_lib.c` (functions and globals named `sp`, `pc`, `r1`, `b`, `add`, `push`), `lib_use.c` + an archive, `pipeline_errors/*`, each with `.expected` output

**Interfaces:**
- Consumes: `as`, `ld`, `ar` (Tasks 3 to 6).
- Produces in `cc`:
  - `cc [-o out] [-S|-c] [-I dir] [-D name[=v]] [-L dir] [-l name] file.c|file.s|file.o|file.a...`. Without `-S`/`-c`: each `.c` is compiled to `/tmp/ccN.s` and assembled to `/tmp/ccN.o`, each `.s` assembled, then everything linked with `ld` into `out` (default `a.bin`). `-c` stops at `.o` (output next to the source, `.c` replaced by `.o`, or `-o` for one input); `-S` stops at `.s`. `-l name` means `lib<name>.a` searched in `-L` dirs, the current directory and `/lib`.
  - On the board `cc` runs `/bin/as.bin` and `/bin/ld.bin` with `exec`; on the PC (gcc build, `#ifndef __T2CC__`) with `system()` on `build/host/as` and `build/host/ld` next to its own path. A tool's non-zero exit stops `cc` with the same exit code after removing its temporary files.
  - `extern` declarations for variables (no storage, references use `movw`/`movt`) and calls to functions not defined in the file (left to the linker instead of today's "undefined function" error, which `ld` now reports).
- [ ] **Step 1: Write the failing tests** listed above. `pipeline_errors`: a `.c` with a syntax error, a `.s` with a bad instruction, and an undefined function: `cc` exits non-zero, prints the tool's error, and leaves no `/tmp/cc*` files.
- [ ] **Step 2:** Run them on the PC (build and link with host tools, compare symbol tables and sizes) and expect failures.
- [ ] **Step 3:** Implement.
- [ ] **Step 4:** PC: all link correctly; `tools/check_ref.py` still passes. Board: build and run every `tests/toolchain` case with `cc` on the board, outputs match `.expected`; all `compiler/tests` still match gcc; `edit`, `sh` and the other tools work after `make` rebuilt them through the new pipeline.
- [ ] **Step 5:** Self-hosting fixed point on the board: `cc` built by the toolchain rebuilds `cc`, `as`, `ld`, `ar`, `nm`, `objdump` byte-identically to the PC builds.
- [ ] **Step 6:** Docs: README (toolchain usage, assembly syntax and the T2DOS pseudo-instructions, object/archive formats pointer to `lib/t2obj.h`, verified list), CLAUDE.md (new build and check commands). Commit: `cc: driver for as and ld; extern and multi-file programs`.

---

## Self-review notes

- Spec coverage: assembler (Tasks 3, 4), linker (5), libraries with archive semantics (6), `nm` (2), `objdump` (7), `cc` emitting assembly (8), one-step driver and gcc-style options (9), separate compilation (9), self-hosting (8, 9), unchanged `.bin` and kernel (Global Constraints, 5), byte-identity stage check (8), behaviour checks for initializer programs (8), multi-file and library tests (5, 6, 9), SD/flash risk (Global Constraints: `/tmp` on whatever filesystem is mounted). No default library (spec: none yet).
- `static` linkage: not in scope (spec left it to the builder; deferred).
