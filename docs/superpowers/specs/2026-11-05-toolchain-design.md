# T2DOS toolchain: assembler, linker, and a compiler that emits assembly

Date: 2026-11-05. Status: approved by the owner.

## Intent

T2DOS should have a complete development toolchain that runs on the board
itself: compiler, assembler, linker, and tools to inspect what they produce.
Today `cc` turns C straight into a finished `.bin`. The goal is the classic
pipeline instead:

    C source --cc--> assembly (.s) --as--> object file (.o) --ld--> program (.bin)

The owner's main reason is a cleaner, complete toolchain (the compiler hands
off to the assembler rather than encoding instructions itself). Hand-written
assembly is a second use: routines in `.s` files that C code calls, and whole
programs in assembly.

## Goals

- **Assembler `as`**: GNU ARM assembler syntax (unified syntax, Thumb-2, as
  accepted by `arm-none-eabi-as`), producing T2DOS object files. It must cover
  everything `cc` emits, plus what hand-written code commonly needs.
- **Linker `ld`**: combines one or more object files and libraries into a
  runnable `.bin` in the existing format, resolving symbols across files.
  Libraries work like real linkers' archives: `ld` pulls in only the library
  members that define a symbol the program still needs, repeating until
  nothing more is needed. (Owner's choice.)
- **Archiver `ar`**: builds a library file from object files (and lists or
  extracts its members), in a T2DOS archive format with an index of the global
  symbols each member defines.
- **`cc` emits assembly**: the compiler's code generator writes assembly text
  instead of machine code. The instruction encodings move out of `cc.c` into
  the assembler.
- **One-step use stays**: `cc hello.c` still produces a runnable `hello.bin`;
  `cc` runs `as` and `ld` behind the scenes (gcc style) and removes the
  intermediate files. gcc-style options to stop early or mix inputs:
  `cc -S` (stop at `.s`), `cc -c` (stop at `.o`), and
  `cc main.c util.s lib.o -o prog.bin` (compile/assemble each, then link).
- **Separate compilation**: a C program can call functions and use globals
  defined in other C or assembly files (`extern` declarations, prototypes).
- **Inspection tools**: `nm` (list an object's or library's symbols) and
  `objdump` (show an object's sections, relocations and a disassembly).
- **Self-hosting continues**: all tools are written in the C subset `cc`
  compiles, run on the board, and also build on the PC (with gcc) as cross
  tools, like `cc` today.

## Non-goals (for this project)

- ELF or any format the GNU tools can read. The object format is T2DOS's own,
  deliberately simple. (Owner's choice.)
- Full GNU assembler compatibility: macros (`.macro`), conditional assembly,
  expressions beyond what is listed below, debug info, CFI directives.
- Optimization. The generated code stays what `cc` generates today.
- Dynamic linking, shared libraries, position-dependent code.

## Constraints

- **The binary format and kernel stay as they are.** `ld` produces the current
  T2CC `.bin`: header (`"T2CC"`, code size, data size, reserved 0, entry
  offset), code bytes, data bytes. Programs stay position independent with no
  run-time relocations: `r9` = data base, `r8` = kernel `sys()`, code
  addresses are PC relative. No kernel change is needed.
- **Calling convention and ABI unchanged** (see README "Kernel contract"):
  args on the stack left to right (first deepest), result in `r0`, `r7` frame
  pointer, syscalls through `r8`.
- **The bootstrap rule**: every tool's source must stay inside the subset the
  *previous* `cc` compiles (see CLAUDE.md). A tool may use a language feature
  only after a committed `cc` supports it.
- **Separate programs**: `as`, `ld` and `ar` are separate programs, and `cc`
  runs them with `exec` (no assembler or linker built into `cc`). Intermediate
  files go through the filesystem. (Owner's choice.)
- **Storage**: in normal use the system runs from an SD card, which tolerates
  the intermediate-file writes well; the 7 MB on-board flash filesystem is the
  fallback when no card is present. 520 KB SRAM, 8 MB PSRAM.
- **Testing**: the target is the CPico 2350 board. A bad encoding hangs it until
  the owner resets it by hand, so new code generation is disassembled
  (`arm-none-eabi-objdump`) before it runs on the board, as today. qemu-arm is
  not installed on the development PC.
- **Encodings**: instruction encodings must match `arm-none-eabi-as` exactly
  (the existing `I_*` constants in `cc.c` are a verified starting point and
  move to the assembler).

## Decided design (owner's decisions)

- GNU ARM assembler syntax.
- A simple object format of our own, and our own tools to inspect it.
- A linker that handles multiple object files and libraries, with archive
  semantics: only the members that are needed get linked.
- `cc` runs the assembler and linker itself, so one command still builds a
  program; `as`, `ld` (and `ar`) are separate programs, not built into `cc`.
- The system normally runs from an SD card, so writing intermediate files is
  acceptable.

## Design calls made by the assistant (open to change)

These were not discussed in detail; they are the assistant's proposals.

### Staging

Build it in stages, each checked before the next:

1. **Object format** header shared by all tools (`lib/t2obj.h`, copied to
   `/lib` on the board like `sys.h`), plus `nm` (small, proves the format).
2. **Assembler** `as`, tested against `arm-none-eabi-as` on the PC (assemble
   the same source with both, compare the machine code bytes) and with a
   linker-free mode for testing (see below).
3. **Linker** `ld`, the archiver `ar`, and `objdump`.
4. **`cc` emits assembly** and drives `as` and `ld`.

### Verification strategy

- **Assembler vs GNU**: every instruction form `as` supports has a test that
  assembles with both `as` and `arm-none-eabi-as` and compares the encoded
  bytes. (The GNU tools are only a test oracle on the PC; nothing depends on
  them on the board.)
- **The big check (stage 4)**: for single-file programs without run-before-main
  initializers, `cc prog.c` through the new pipeline must produce a `.bin`
  **byte-identical** to today's direct `cc`. This covers all 20 userland
  programs and `cc.bin` itself. Keep the current direct-emission `cc` around
  (e.g. built from the last pre-switch commit) as the reference during stage 4.
- Programs with run-before-main initializers (test5 and friends) may differ in
  layout; they are checked by behaviour: the existing tests match gcc on the
  board, as today.
- **Self-hosting fixed point**: on the board, the toolchain built by the
  toolchain rebuilds `cc`, `as` and `ld` byte-identically.
- New tests for multi-file programs: C calling assembly, assembly calling C,
  shared globals across files, undefined and duplicate symbol errors, and
  libraries: only needed members linked, members needing other members, a
  member defining a symbol that is never used is left out.

### Assembler scope

- Syntax: `.syntax unified`, `.thumb` implied; labels `name:`, local numeric
  labels are optional; comments `@` and `//`; `#` before immediates.
- Instructions: everything `cc` emits today (the `I_*` list, including FPU
  `vmov/vadd/vsub/vmul/vdiv/vneg/vcmp/vmrs/vcvt`), the branches `b`, `bl`,
  `b<cond>` (wide forms), `blx`, plus common hand-coding forms: `mov/movs`,
  `add/sub` (register and immediate), `ldr/str/ldrb/strb/ldrh/strh` with
  immediate offsets, `push/pop`, `cmp`, logical ops, `it` blocks, `bx`, `nop`.
  Exact list is the builder's call; anything accepted must encode exactly as
  `arm-none-eabi-as` does.
- Directives: `.text`, `.data`, `.global`/`.globl`, `.extern` (optional, as
  undefined symbols are external anyway), `.word`, `.short`, `.byte`,
  `.ascii`, `.asciz`/`.string`, `.space`/`.zero`, `.align`/`.balign`,
  `.thumb_func` (accepted, may be a no-op).
- Expressions: numbers (decimal, hex, char), symbols, `symbol + constant`.
- T2DOS-specific operand forms the code model needs (data offsets from `r9`,
  PC-relative function addresses) are the builder's design, documented in the
  README; GNU-style relocation operators such as `#:lower16:sym` /
  `#:upper16:sym` are a good fit for `movw`/`movt`.

### Object format (`.o`)

A small header, then sections and tables:

- **Sections**: `text` (code) and `data` (initialized data; zero-filled data
  may simply be stored as zeros, as today).
- **Symbols**: name, section (text, data, or undefined), value (offset),
  global or local.
- **Relocations**: section, offset, symbol, type, addend. Types needed at least:
  - `bl`/`b.w` 24-bit branch to a symbol (calls across files),
  - conditional `b<cond>.w` 20-bit (if used across files),
  - data offset of a symbol from `r9` in a `movw`/`movt` pair or in an
    `addw rd, r9, #imm12` (globals and string literals),
  - PC-relative function address in a `movw`/`movt` + `add rd, pc` sequence
    (function pointers),
  - 32-bit data word holding a data offset or function address (only if
    initializers need it; see below).
- Exact layout, field widths and type numbers are the builder's call; keep it
  simple and document it in `lib/t2obj.h`.

### Linker behaviour

- Object files on the command line are always linked. Libraries are searched
  for members that define a still-undefined global symbol; such members are
  added, and the search repeats until no library member resolves anything more
  (so the order of libraries does not matter, unlike classic Unix `ld`).
- Concatenates the linked `text` sections into the code segment and the `data`
  sections into the data segment, objects in command-line order, then library
  members in the order they were pulled in; applies relocations; reports
  undefined and duplicate global symbols by name.
- Entry point: `main`. Run-before-main initializer code (today's init chunks)
  becomes per-object init routines that `ld` calls before `main`, through an
  entry stub like today's. The mechanism (e.g. a well-known per-file symbol or
  a list section) is the builder's call.
- If a relocated value does not fit its instruction (for example a data offset
  above 4095 in an `addw`), `ld` stops with a clear error naming the symbol.
  `cc` keeps today's rule of using the short `addw` form only when it knows
  the offset is small; for references to other files' data it uses
  `movw`/`movt`.

### `cc` changes

- The code generator writes assembly lines instead of calling `emit16/emit32`;
  branches use labels instead of patched offsets; the `I_*` encodings leave
  `cc.c`.
- Globals and functions are emitted as symbols; `extern` and non-static
  globals are visible across files. (`static` linkage: builder's call whether
  to support it now.)
- Builtins (syscalls) stay inline sequences through `r8`; no linking needed.
- Option handling as in Goals; intermediate files go to a temporary directory
  and are removed unless `-S`/`-c` asked for them.
- Libraries are given like objects (`cc main.c libfoo.a`) or with `-l`/`-L` as
  in gcc (builder's call). There is no default library yet (nothing to put in
  one: the C "library" is the kernel's builtins). When one exists, `cc` links
  it by default, with an option to leave it out.

### Archive format (`.a`)

T2DOS's own: a header, an index of global symbol name to member, then the
members (each a complete object file with its name). Layout is the builder's
call; define it in `lib/t2obj.h` next to the object format.

### Inspection tools

- `nm file.o` / `nm lib.a`: symbols with section and value, sorted (per member
  for a library).
- `objdump file.o` (and ideally `file.bin`): headers, sections, relocations,
  and a disassembly of the code (at least the instruction forms `as` knows).

### Where things live

`assembler/as.c`, `linker/ld.c`, `binutils/ar.c`, `binutils/nm.c`,
`binutils/objdump.c` (or similar; builder's call), all built by the Makefile into `root/bin/` for the
board and as PC cross tools, like `compiler/`.

## Risks and open points for the builder

- **Intermediate files**: every build writes `.s` and `.o` files; `cc.c`'s
  assembly will be roughly a megabyte. On the SD card (normal use) that is
  acceptable; on the flash fallback it costs time and wear. Keep the assembly
  reasonably terse and measure in stage 4.
- **The SD card has not been tested on the CPico 2350 yet** (the kernel expects
  it on SPI0 with CS on GP17). The toolchain must work on the flash fallback
  too, so this does not block any stage, but SD support should be tried on the
  real hardware during the project.
- **Build time on the board**: three passes instead of one. Measure; today
  `cc` compiles `cc.c` in a few seconds.
- **The 4 KB `addw` data-offset limit** across files (see Linker behaviour).
- **Existing tests stay valid**: `compiler/tests/test*.c` must keep passing at
  every stage.
