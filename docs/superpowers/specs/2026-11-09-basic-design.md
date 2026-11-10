# T2DOS BASIC: design

## Intent

A BASIC for T2DOS that is easier to learn than the C toolchain. It follows
Microsoft's QBasic/QuickBASIC dialect: no line numbers, labels for jumps,
block-structured `IF`, loops, `SUB` and `FUNCTION`. Programs are plain text
files (`.bas`) written in the existing `edit` program and run with
`basic prog.bas`. Someone who knows QBasic should find their way at once, and
typical QBasic listings that stay inside the supported subset should run
unchanged.

Decided by the owner:
- An **interpreter** that runs the source file directly (no compiling to
  `.bin`).
- **QBasic-style** dialect, close to Microsoft BASIC.
- **No line numbers**; labels instead.
- Programs are **edited in `edit`**.
- The maths functions go into the **system** as library built-ins, usable
  from C as well, and BASIC uses them (part 1 below).

Everything marked *(my call)* below was decided by the designer and is open
to change.

The work has two parts, built and tested in this order:
1. **Maths functions in the system** (a small, separate step).
2. **The BASIC interpreter**, which uses them.

## Part 1: maths functions in the system

T2DOS has no maths library: a C program cannot call `sin` or `sqrt`. Add
them as library built-ins that run kernel-side, like `printf`, so they cost
a program no code, use the toolchain C library's single-precision functions
(`sinf`, `sqrtf`, ...) on the FPU, and serve C and BASIC alike.

- Functions *(my call)*: `sin cos tan asin acos atan atan2 sinh cosh tanh
  exp log log10 pow sqrt fabs floor ceil fmod`. In C they keep the standard
  names and work on T2DOS's 32-bit `float` (`double` is the same type).
- New syscall numbers in `sys.h`, from 112 upward in the order listed
  *(my call)*, added in all four places the ABI lives (`sys.h`, `cc.c`'s
  built-in table, `kernel/kernel.ino`, `host_linux.c`, which links `-lm`).
- `cc` types these built-ins as returning `float` and taking `float`
  arguments, so an int argument is converted (`sqrt(2)` passes 2.0) and the
  result is usable directly in float expressions. Today every built-in is
  int in, int out with arguments passed unchanged, so this is a small,
  careful change to `cc.c` (the bootstrap rule applies: `cc.c` itself must
  not use the new functions). Floats cross the syscall as their 32-bit
  pattern, as `printf` already does.
- `#include <math.h>` keeps working (system headers are ignored and the
  built-ins cover it). A small `lib/math.h`, installed to `/lib`, defines
  `M_PI`, `M_E` and friends for `#include "math.h"` *(my call)*.
- Tests: a C test program comparing results on the PC (qemu, `host_linux`)
  with gcc's `float` results, and the same on the board; the toolchain and
  self-hosting checks stay byte-identical except `cc.bin` (whose built-in
  table grows) and the files that use the new functions; `check_ref.py`'s
  "excluded (newer builtins)" class covers sources that use them.
- Docs: a maths section in the C chapter of `README.md` and the new numbers
  in the system reference.

## Part 2: the BASIC interpreter

## Goals

1. `basic prog.bas [args...]` loads, checks and runs a program. Errors name
   the file, the source line and the problem in QBasic's words, for example
   `game.bas:42: NEXT without FOR` and, at run time,
   `game.bas:57: Subscript out of range`.
2. The language subset listed below, with QBasic semantics wherever the
   subset covers them.
3. Works on the board and under `host_linux` (qemu), so it is testable on
   the PC.
4. Fast enough to be pleasant: a simple counting loop with arithmetic runs at
   least 100,000 statements per second on the board *(my call: a target, to
   be measured early and revised if it proves wrong)*. A statement is each
   statement executed, so `FOR i=1 TO n: s=s+i*2: NEXT` is two per pass
   (the assignment and `NEXT`). The target is measured and reported, not a
   hard gate, but the design should aim for it from the start (programs
   tokenized at load time, labels, blocks and variables resolved before the
   run), since a slow design is hard to fix later *(my call)*.
   *Measured:* `examples/basic/bench.bas` runs 200,000 statements in 1.06 s
   on the CPico, about 188,000 statements per second, so the target is met.
5. Plays well with background jobs: the interpreter calls `yield()` every few
   hundred statements, so a BASIC program can never freeze the board, even
   as a background job (`basic server.bas &`) *(my call)*. Named pipes
   work through the ordinary file statements: `OPEN "/pipe/name" FOR INPUT
   AS #1` and `LINE INPUT #1` / `INPUT #1` read a request (waiting for one
   yields to other tasks in the kernel), `EOF(1)` is true at the end of a
   message, `OPEN ... FOR OUTPUT` and `PRINT #` write a reply that ends at
   `CLOSE`. A background program's `PRINT` goes to the console, or to the
   file given with `> file &` *(my call)*. One test drives a small BASIC
   pipe server like `demosrv`.
6. Ctrl-C stops the running program with `Break at prog.bas:LINE`, like
   Ctrl-Break in QBasic. Programs already receive Ctrl-C as byte 3 from
   `getchar`. The interpreter polls `kbhit()` between statements (every few
   hundred, together with the `yield()` of goal 5) and moves waiting keys
   into its own key queue; a 3 stops the program, other keys stay queued
   for `INKEY$`, `INPUT` and `SLEEP` *(my call)*.
7. An `edit` shortcut runs the file being edited *(my call)*: in `edit`, a
   key (suggested: Ctrl-R) saves the file (unsaved changes included) and, if
   its name ends in `.bas`, runs `basic` on it. Afterwards `edit` resets the
   colours, shows `Press any key` (with the exit code if it is not 0), waits
   for a key and redraws. For other file names the key shows a short message
   and does nothing.
8. Immediate mode *(my call)*: `basic` with no file shows a prompt where
   single lines run at once (`PRINT 2^10`, `FOR i=1 TO 5: PRINT i: NEXT`),
   variables persist between lines, `RUN "file.bas"` runs a program, and
   `SYSTEM` returns to the shell. Multi-line blocks are not supported in
   immediate mode.
9. A user's-manual chapter in `README.md` with examples copied from real
   runs, and a few example programs installed to `/src/basic/` (or similar).

## Non-goals (this version)

- Line numbers (not even as optional labels), `AUTO`, `LIST`, `RENUM` and an
  in-interpreter line editor.
- Compiling BASIC to `.bin`.
- `TYPE ... END TYPE` user types, `STATIC` variables, `COMMON`, `CHAIN`,
  `DEF FN`, `ON ... GOTO/GOSUB`, `ON ERROR` / `RESUME`, `PRINT USING`,
  the `MID$` statement form, graphics (`SCREEN`, `LINE`, `CIRCLE`, `PSET`),
  `SOUND`/`PLAY`, `PEEK`/`POKE`, `DATE$`/`TIME$` (there is no real-time
  clock), random-access and binary files (`GET`/`PUT`, `FIELD`).
- Networking statements. (Possible later; the interpreter's structure should
  not make them hard to add.)
- 16-bit `INTEGER` overflow emulation and real 64-bit `DOUBLE`.

## Anti-goals

- A BASIC that looks like QBasic but behaves differently in the common cases
  (`PRINT` spacing of numbers, `,` zones, `INPUT` re-prompting, `FOR` loop
  bounds evaluated once, integer division `\`, string comparison). Surprising
  a QBasic user is worse than a missing feature: missing features must give
  a clear error, not silently do something else.
- Memory that grows while a program runs in a loop (strings must be freed);
  free PSRAM after a program ends must equal free PSRAM before it.
- Kernel or syscall changes for BASIC beyond part 1. BASIC is a userland
  program on top of the library.

## The language

### Source form

- Case-insensitive keywords and names; the interpreter keeps the program's
  spelling for messages.
- One statement per line, or several separated by `:`.
- Comments: `REM ...` and `' ...` (to end of line).
- A label is an identifier followed by `:` at the start of a line
  (`loop:`). Labels are global to the main program; labels inside a `SUB` or
  `FUNCTION` are local to it.
- Lines may end with `_` to continue on the next line *(my call; QuickBASIC
  4.5 style)*.

### Types and variables

| Suffix | `AS` name | Meaning |
|---|---|---|
| `%` | `INTEGER` | 32-bit integer *(QBasic's is 16-bit; 32-bit here)* |
| `&` | `LONG` | 32-bit integer (same as `INTEGER`) |
| `!` | `SINGLE` | 32-bit float |
| `#` | `DOUBLE` | 32-bit float (the board has no 64-bit float) |
| `$` | `STRING` | string, length 0 to 32767 |

- Default type is `SINGLE`, as in QBasic. `DEFINT`, `DEFLNG`, `DEFSNG`,
  `DEFDBL`, `DEFSTR` with letter ranges change it.
- `x` and `x$` are different variables; `x` and `x()` (array) are different.
- Variables need no declaration and start at 0 or `""`.
- `DIM name AS type`, `DIM a(10)`, `DIM b(1 TO 5, 0 TO 3) AS INTEGER`,
  `DIM SHARED ...` at module level makes a variable visible inside
  `SUB`/`FUNCTION`. Arrays used without `DIM` get bounds 0 to 10 as in
  QBasic. `REDIM` re-dimensions (contents cleared), `ERASE` frees. Arrays
  have up to 8 dimensions. `LBOUND`/`UBOUND`.
- `CONST name = expr`.
- Numeric conversion follows QBasic: float to integer rounds half to even
  when stored in an integer variable; `INT` floors, `FIX` truncates. Integer
  overflow beyond 32 bits is not detected *(my call)*.

Load-time checks *(my call)*: before running, `basic` reports block
mismatches (`NEXT without FOR`, `END IF` without `IF` and the rest),
duplicate or undefined labels, calls to undefined `SUB`s/`FUNCTION`s or with
the wrong number of arguments, and type mismatches. Types are known before
the run in QBasic (suffixes, `AS`, `DEF`*type*), so `a$ = 1` is a load-time
`Type mismatch`. Run-time errors are those that depend on values
(`Subscript out of range`, `Division by zero`, `Illegal function call`,
`Out of DATA`, `RETURN without GOSUB`, file errors).

### Expressions

- Precedence (high to low): `^`; unary `-`; `*` `/`; `\` (integer
  division); `MOD`; `+` `-`; `=` `<>` `<` `>` `<=` `>=`; `NOT`; `AND`;
  `OR`; `XOR`; `EQV`; `IMP`.
- Comparisons give -1 for true, 0 for false. `AND`/`OR`/`XOR`/`NOT` are
  bitwise on integers, as in QBasic.
- `+` also joins strings; strings compare by character codes.
- Number literals: `12`, `1.5`, `1E3`, `&HFF`, `&O17`, and type suffixes on
  literals.

### Statements

- Assignment, with optional `LET`. `SWAP a, b`.
- `IF cond THEN stmts [ELSE stmts]` on one line; block `IF cond THEN` /
  `ELSEIF cond THEN` / `ELSE` / `END IF`. `IF cond THEN label` jumps (as in
  QBasic).
- `FOR v = a TO b [STEP s]` ... `NEXT [v[, v...]]`, bounds and step
  evaluated once.
- `WHILE cond` ... `WEND`.
- `DO [WHILE|UNTIL cond]` ... `LOOP [WHILE|UNTIL cond]`.
- `SELECT CASE expr` / `CASE value, value` / `CASE a TO b` /
  `CASE IS > x` / `CASE ELSE` / `END SELECT`.
- `EXIT FOR`, `EXIT DO`, `EXIT SUB`, `EXIT FUNCTION`.
- `GOTO label`, `GOSUB label` / `RETURN`.
- `END`, `STOP` (same as `END` here), `SYSTEM`.
- `SUB name (params)` ... `END SUB`, `FUNCTION name[suffix] (params)` ...
  `END FUNCTION`, return value by assigning to the function name. Calls:
  `name args`, `CALL name(args)`. Parameters pass by reference when the
  argument is a variable or array element, by value otherwise (QBasic
  rules); `a()` passes a whole array. Locals and recursion work. `DECLARE`
  lines are accepted and ignored.
- `DATA`, `READ`, `RESTORE [label]`.
- `RANDOMIZE [seed]` (`RANDOMIZE TIMER` works).

### Console

- `PRINT` with `;`, `,` (14-column zones), a trailing `;` or `,` to stay on
  the line, `?` as a short form, and `TAB(n)`, `SPC(n)`. Numbers print as in
  QBasic's `SINGLE` rule: a leading space for positive numbers (a `-` for
  negative ones), a trailing space, at most 7 significant digits with
  trailing zeros and a leading `0` dropped, and the `E` form when the
  exponent is outside QBasic's range (`PRINT 1/3` gives ` .3333333 `,
  `PRINT 1E7` gives ` 1E+07 `, `PRINT .000015` gives ` 1.5E-05 `).
  Integers print all their digits. `#` (`DOUBLE`) values print like
  `SINGLE`, since they are the same 32-bit float. `STR$` uses the same form
  without the trailing space.
- `INPUT ["prompt"{;|,}] v1, v2...` with QBasic's `?` and `Redo from start`.
  `LINE INPUT ["prompt";] s$`.
- `INKEY$` (non-blocking; `""` when no key), `CLS`, `LOCATE row, col`
  (1-based), `COLOR fg [, bg]` (colours 0 to 7 map onto `conctl`'s colours;
  8 to 15 map to the same as 0 to 7 *(my call)*), `BEEP` (sends a bell),
  `SLEEP [seconds]` (no argument: wait for a key), `WIDTH` is accepted and
  ignored.
- `INKEY$` returns QBasic's two-character codes for the arrow and other
  special keys (`CHR$(0) + "H"` for Up, `"P"` Down, `"K"` Left, `"M"` Right,
  and Home, End, PgUp, PgDn, Ins, Del), decoded from the terminal's escape
  sequences, so QBasic games work *(my call)*.

### Files and system

- `OPEN f$ FOR INPUT|OUTPUT|APPEND AS #n`, `CLOSE [#n...]`,
  `PRINT #n, ...`, `WRITE #n, ...` (and `WRITE` to the screen),
  `INPUT #n, ...`, `LINE INPUT #n, s$`, `EOF(n)`, `FREEFILE`. Up to the
  kernel's open-file limit.
- Paths are T2DOS paths, relative to the current directory; `\` in a path
  is taken as `/`, so QBasic-style paths work *(my call)*.
- `KILL f$`, `NAME old$ AS new$`, `MKDIR`, `RMDIR`, `CHDIR`, `FILES [dir$]`.
- `SHELL cmd$` runs a T2DOS program through `exec` (first word is the
  program, found like the shell finds it), `COMMAND$` returns the program's
  arguments.

### Functions

`ABS SGN INT FIX SQR SIN COS TAN ATN EXP LOG RND TIMER CINT CLNG CSNG CDBL
LEN LEFT$ RIGHT$ MID$ INSTR UCASE$ LCASE$ LTRIM$ RTRIM$ STR$ VAL CHR$ ASC
STRING$ SPACE$ HEX$ OCT$ INKEY$ EOF FREEFILE LBOUND UBOUND COMMAND$`.
`TIMER` is seconds since the board started (from `millis()`), as a float.
The maths functions use part 1's built-ins (`SQR` is `sqrt`, `ATN` is
`atan`, `LOG` is the natural logarithm, `^` uses `pow` for non-integer
exponents).

### T2DOS extras *(my call)*

Board access, because this is a microcontroller and blinking an LED should
be a five-line program: `PINMODE pin, mode`, `PINWRITE pin, value`,
`PINREAD(pin)`, `ANALOGREAD(pin)`, `PWM pin, value`, `PAUSE ms` (sleep in
milliseconds). They map one-to-one onto the library's `gpio_mode`,
`gpio_write`, `gpio_read`, `analog_read`, `pwm_write` and `sleep`.

## Constraints

- Written in the T2DOS C subset and built by the project's `cc` (no
  `static`, `const`, `do`/`while`, `goto`, struct assignment, user variadic
  functions, doubles or 64-bit ints). Floats and function pointers are
  available. Built by the Makefile like other userland programs into
  `root/bin/basic.bin`.
- Program code runs from SRAM (about 370 KB free); data and `malloc` live in
  PSRAM (8 MB). The interpreter's code should stay well below 100 KB.
  *Measured:* `basic.bin` is 175,228 bytes, above that soft target; it fits
  the free SRAM and has not been trimmed.
- Uses only library calls (the existing ones plus part 1's maths
  functions); no other kernel or `sys.h` changes.
- `make check` must stay green; if `tools/check_ref.py` cannot compare
  `basic.c` against the reference compiler, extend its exclusion rules the
  way earlier work did ("excluded (newer builtins)").
- No em dashes in code, comments or docs (the owner's rule).

## Testing

- `tests/basic/*.bas`, each with an expected-output file; a
  `tools/check_basic.py --pc` runner (qemu with `host_linux`) and `--board`.
  Cases cover each statement and function group, the error messages (load
  and run time), QBasic number formatting, and a memory check (free PSRAM
  equal before and after a string-heavy loop on the board).
- Where possible, expected outputs come from real QBasic behaviour (the
  builder may use QB64 or FreeBASIC `-lang qb` on the PC as an oracle if
  available, and must note where T2DOS BASIC differs on purpose, such as
  32-bit `INTEGER`).
- The speed target (goal 4) is measured on the board and reported.

## Left to the builder

- How the interpreter is structured (tokenizing at load time, pre-resolving
  labels and block matches, how values and strings are stored), as long as
  the error messages point at source lines and the speed target is met.
- Whether `basic.c` is one file or several (`cc` can build multi-file
  programs; the Makefile rule may need a line).
- The exact float-printing algorithm, as long as it matches QBasic's output
  for the tested cases.
- Where the example programs live and what they are (suggested: a guessing
  game, a blinking LED, a text-file word counter, a recursive `SUB` such as
  Towers of Hanoi).
