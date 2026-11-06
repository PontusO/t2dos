# T2DOS: a DOS-class OS for the RP2350, with a self-hosting C compiler

One program at a time, flat memory, kernel in the Arduino sketch, everything
else is a T2CC program on the SD card (or, without a card, in on-board flash).

## Layout
    sys.h             syscall ABI: numbers, flags, structs. Shared by kernel, compiler, programs
    compiler/cc.c     the compiler and driver: C to assembly, then as and ld (gcc builds it as a
                      PC cross compiler, compiler/cc; it compiles itself on the board)
    compiler/tests/   language tests: test.c test3.c test4.c ...
    assembler/as.c    the assembler: GNU syntax Thumb-2 to T2DOS objects
    linker/ld.c       the linker: objects and archives to a .bin
    binutils/         ar (archives), nm (symbols), objdump (disassembler)
    lib/t2obj.h .c    the object and archive formats, shared by as, ld and the binutils
    tests/            asm/ (as), link/ (ld, ar), obj/ (nm, objdump), toolchain/ (cc driver)
    kernel/kernel.ino the kernel (arduino-pico; SDFS on SPI, set SD_CS; LittleFS fallback)
    host_linux.c      the same kernel contract on Linux/ARM, run binaries with qemu-arm
    userland/*.c      sh ls cat cp mv rm mkdir rmdir echo hexdump free put write edit blink
                      wifi get send rls nc (network)
    lib/net.c         client helpers for the file protocol, #include "net.c"
    tools/fileserver.py  Linux side: python3 fileserver.py dir [port]
    root/             filesystem image: /bin/*.bin  /lib/sys.h /lib/net.c /lib/t2obj.h /lib/t2obj.c  /src/
    Makefile          make all; make check; make ref; make host

## Building the kernel
Tested on the iLabs CPico 2350 with arduino-pico 6.2.0 (`rp2040:rp2040`).
The sketch includes `sys.h` from the repo root:

    FQBN=rp2040:rp2040:ilabs_cpico_2350:flash=8388608_7340032
    arduino-cli compile --fqbn $FQBN --build-property "compiler.cpp.extra_flags=-I$PWD" --output-dir build/kernel kernel
    arduino-cli upload  --fqbn $FQBN -p /dev/ttyACM0 --input-dir build/kernel

The `flash=` layout gives the kernel 1 MB (it uses about 184 KB) and the
filesystem 7 MB. Reflashing the kernel keeps the filesystem.

## Bootstrapping
1. `make` (needs gcc). Copy `root/` to the SD card.
2. Flash `kernel.ino`. It boots `/bin/sh.bin`; if that is missing you get a
   `recovery>` prompt with `put name size` (raw bytes follow), `ls`, `mkdir`, `run`.
   No SD card: the kernel falls back to LittleFS in on-board flash (banner says
   `fs flash`, formatted on first use). On the CPico 2350 build with the
   `flash=8388608_7340032` layout: 1 MB for the kernel, 7 MB filesystem. Fill it
   with `put` from the recovery prompt or the shell, one file at a time.
3. On the board: `cd /src`, `write hello.c` (type it, end with `.`),
   `cc -o hello.bin hello.c`, `hello`.
   `put` works from the shell too; from a PC:
   `(printf 'put cc.bin %d\n' $(stat -c%s cc.bin); cat cc.bin) > /dev/ttyACM0`
4. `/autoexec.sh` runs at shell start; `sh script` runs any script.

## Editor
`edit file` is a full-screen editor for an ANSI/VT100 terminal, e.g.
`screen /dev/ttyACM0 115200`. It reads the window size from the terminal
(falls back to 80x24) and uses the alternate screen, so the shell output
comes back when it quits.

    arrows Home End PgUp PgDn   move          Backspace Del Enter Tab   edit
    ^O (or ^S)                  save          ^X (or ^Q)                quit, twice if unsaved
    ^F                          find (Enter = next match, Esc cancels)
    ^B                          set the mark (again: clear it)
    ^K                          cut           ^C                        copy
    ^U                          paste the cut or copied text at the cursor

With a mark set, ^K and ^C take the text between the mark and the cursor
(shown in reverse video). Without one they take the whole current line, and
pressing them again right away adds the next line to the same block, so ^K^K^K
cuts three lines and ^C^C^C copies three. ^U pastes as often as you like.

Enter keeps the current line's indentation, except while text is pasted.
Use ^O to save under `screen`: it has flow control on for serial lines, so ^S
freezes its output until ^Q (or turn it off with `defflow off` in ~/.screenrc).
Closing the terminal window detaches `screen` but keeps the port open; quit
it with Ctrl-A k, or `screen -r` to get back in.
Long lines scroll horizontally; tabs are 8 columns. The edit buffer is the
file size plus 256 KB, so one session can add up to 256 KB of text.

## Network
    wifi ssid password        join (kernel: WiFi.begin, 15 s timeout);  wifi  -> status and IP
    rls  host[:port] [dir]    list files on the server
    get  host[:port] remote [local]
    send host[:port] local [remote]
    nc host port | nc -l port raw TCP terminal, Ctrl-] quits
Default port 7777. Protocol: one request per connection, GET name / PUT name
size + bytes / LS [dir], reply "OK [size]\n" + data or "ERR text\n".
Syscalls 80..88: net_join net_status net_ip connect send recv(fd,buf,n,timeout_ms)
listen accept(timeout_ms) sock_avail. Socket fds are 100+ and also work with
read/write/close; exec closes sockets a child leaked.

## Kernel contract
- Programs are position independent Thumb-2: `r7` frame, `r8` = kernel
  `sys()`, `r9` = data base, args on the stack (first arg deepest), result in r0.
- `sys(num, nargs, argv)`, `argv[nargs-1]` is the first argument. Numbers in sys.h,
  grouped: process 0+, console 16+, files 32+, memory/strings 64+, network 80+,
  board 96+, user 128+.
- `exec(path, argc, argv)` loads a binary into fresh code (SRAM) + data + 256 KB
  stack (PSRAM), runs it, frees everything, returns its exit code. Nesting up to 4.
  Files the child leaked are closed, memory it `malloc`ed is freed;
  `redirect(fd)` is restored. `malloc` returns zeroed memory.
- Syscalls run on a 16 KB kernel stack in SRAM, not the program's PSRAM stack
  (PSRAM is unreachable while flash is being written).
- fd 0/1 are the console; `redirect(fd)` routes fd 1 and printf to a file (the
  shell implements `> file` and `>> file` with it). cwd lives in the kernel.
- Console is a driver interface (`putc/getc/kbhit/ctl/width/height`); the serial
  driver is in; a display+keyboard driver is the same five methods.
- `printf`/`sprintf` are kernel-side: `%[-][0][width][.prec]d u x c s f e g %%`; `%f %e %g` take a float.

## Toolchain
`cc` works like gcc's driver: it compiles each `.c` to assembly itself, runs
`as` on every `.s`, then `ld` on all the objects. On the board the tools are
`/bin/as.bin` and `/bin/ld.bin` (run with `exec`); the PC build `compiler/cc`
runs `build/host/as` and `build/host/ld` (or the ones in the directory named by
`$T2_TOOLS`). Intermediate files go to `/tmp` (created when missing) and are
removed again, also after an error. Every tool is silent on success.

    cc [-o out] [-S|-c] [-mshort-data] [-I dir] [-D name[=v]] [-L dir] [-l name] file.c|file.s|file.o|file.a...

- Inputs by suffix: `.c` is compiled and assembled, `.s` assembled, `.o` and
  `.a` go to `ld` as they are, all in command-line order; `-l` archives come last.
- Without `-S`/`-c` the result is a `.bin`: `a.bin` unless `-o` names it.
- `-c` stops at objects, `-S` at assembly (`.c` inputs only). Each output is the
  input's base name with `.o` or `.s`, in the current directory; `-o` names it
  when there is exactly one input.
- `-mshort-data` makes `-S`/`-c` output use the short data references, as if
  the file's data came first in the program (see "Data references" below).
- `-l name` links `libname.a`, searched in each `-L dir` in order, then the
  current directory, then `/lib`. `-lname` and `-Ldir` work too.
- `-I dir` adds an include directory (`#include "file"` also searches the
  current directory and `/lib`); `-D NAME[=value]` defines a macro (value 1).
- When a step fails, its own message is what you see, and `cc` exits with that
  step's exit code (1 for a compile error).

The other tools:

    as [-o out.o] file.s             assemble (default output: file.o)
    ld [-o out.bin] [-e entry] file.o... lib.a...
                                     link; the entry is main unless -e names one; an
                                     archive member is linked only when it defines a
                                     symbol that is still undefined
    ar rc lib.a a.o b.o...           create an archive, or replace and append members
    ar t lib.a                       list the members
    ar x lib.a [member...]           extract (all members without names)
    nm file.o|lib.a...               symbols: T/t text, D/d data, A/a absolute (upper
                                     case global, lower case local), U undefined
    objdump file.o|lib.a|prog.bin... header, symbols, relocations, disassembly, data

Separate compilation: `extern int counter;`, `extern char name[];` and
`extern` prototypes declare what another file defines. Every global function
and variable is visible to the other files (there is no `static` yet). A call
of a function that no file defines is reported by `ld` (`ld: undefined
symbol: f (in x.o)`). Examples:

    cc -o prog.bin main.c util.c                      two C files in one step
    cc -c util.c; cc -o prog.bin main.c util.o        or one at a time
    cc -o mixed.bin main.c fast.s                     C and assembly together
    cc -c sq.c cube.c; ar rc libm2.a sq.o cube.o      a library ...
    cc -o p.bin p.c -L . -l m2                        ... and a program using it

Data references: code reaches a global as `r9` plus the global's offset in the
program's data, and `cc` picks the smallest instruction form that reaches it:
`addw r0, r9, #name` (4 bytes) below 4 KB, `movw.16` and `add.w` (8 bytes)
below 64 KB, `movw`, `movt` and `add.w` (12 bytes) above that. When `cc` builds
the whole program, it knows where `ld` will put each file's data (after the data
of the files before it; libraries come after all of them), so every file gets
the short form where it fits, and a single-file program is the same as before.
With `-S` or `-c`, `cc` cannot know what comes before the file in the link, so
it uses the 12-byte form for every data reference. `-mshort-data` compiles as
if the file's data came first, which is right for the first object of a
program. If that guess is wrong, `ld` stops instead of producing a broken
program:

    ld: data offset of counter (main.o) is 5000, too far for addw

(the global, and the object that refers to it). References to `extern`
variables always use the 12-byte form.

C and assembly call each other with cc's convention: the arguments on the
stack, the first one deepest, popped by the caller; the result in r0. Keep
`r7`, `r8` and `r9` (C functions save `r7` and `lr` and use `r0` to `r4`, saving
`r4` when they use it). tests/toolchain/ has a working example of each case.

## Assembly
`as` takes the subset of GNU's unified syntax that cc and hand-written code
need: the ARMv8-M mainline integer instructions with their narrow and wide
forms (`.n`, `.w`), IT blocks, and the single precision FPU instructions
(`vadd.f32`, `vldr`, `vmov`, `vcvt`, `vpush`, ...). The encodings equal
arm-none-eabi-as with `-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16`
(tools/check_asm.py compares them byte for byte). Directives: `.syntax
unified`, `.thumb`, `.thumb_func`, `.text`, `.data`, `.global`/`.globl`,
`.extern`, `.equ`/`.set`, `.byte`, `.hword`/`.short`, `.word`, `.ascii`,
`.asciz`/`.string`, `.space`/`.zero`, `.align`, `.balign`, `.t2init`.
Comments: `@`, `//`, `/* */`, and `#` at the start of a line.

The code model (what the kernel and ld expect): code is position independent,
`r9` holds the data base and `r8` the kernel's syscall entry. The value of a
data symbol is its offset in the data, so data lives at `r9 + offset`:

    addw    r0, r9, #counter          @ offsets 0 to 4095
    movw.16 r1, #counter              @ offsets up to 65535
    add.w   r0, r9, r1
    movw    r1, #:lower16:counter     @ any offset
    movt    r1, #:upper16:counter
    add.w   r0, r9, r1

`.word label` in `.data` stores the label's section offset (the value ld
gives the symbol), not an address: a function pointer table in `.data` does
not work, since calling through it jumps to an offset. Take function
addresses at run time with `fnaddr`. `.align`/`.balign` go up to 4 bytes
(ld aligns each section to 4). As in GNU as, `sp` and `pc` are refused where
ARMv8-M makes them unpredictable (`r13 not allowed here`, `r15 not allowed
here`), and `add rd, pc, #imm` is `adr`.

`b`, `b<cond>` and `bl` to another file's functions are relocated by ld.
T2DOS pseudo-instructions (GNU as has no such forms):

- `fnaddr rd, sym`: `rd` = the Thumb address of the function `sym`, PC
  relative (10 bytes: `movw rd, #lo; movt rd, #hi; add rd, pc`).
- `movw.16 rd, #sym`: a `movw` of a data offset that must fit 16 bits (no
  `movt` follows).
- `.t2init sym`: `sym` is a run-before-main routine; ld's entry stub calls
  each one in link order before `main`. cc uses it for global initializers
  that need code.

Symbol operands: `#sym` and `#sym+n` (with `addw` and `movw.16`),
`#:lower16:sym` and `#:upper16:sym` (with `movw` and `movt`), plain `sym` for
branches and `fnaddr`. Labels starting with `.L` stay inside the file. Not
supported: literal pools (`ldr r0, =x`, `ldr r0, label`), writeback and
post-indexed addressing (`[r0, #4]!`, `[r0], #4`), double precision, FPU
instructions inside IT blocks, and assembler macros (`.macro`, `.rept`).

The object format (`T2OB`: header, text, data, symbols, relocations
`R_BL R_BW R_BCOND R_FNADDR R_DATA_ADDW R_DATA_MOVW R_DATA_MOVT R_DATA_MOVW16
R_WORD`, string table) and the archive format (`T2AR`: header, symbol index,
member table, string table, the member objects) are defined in `lib/t2obj.h`. The `.bin` that ld writes is the kernel's program
format: a 20-byte header (`T2CC`, code size, data size, 0, entry), then the
code and the data.

## Language subset (compiler/cc.c)
int char short void, unsigned and signed (`unsigned char`, `unsigned short`,
`unsigned`), long (= int, 32 bits), float (and double, which is the same
32-bit float: see below), pointers, structs (named or anonymous,
array members),
typedef, global/local arrays of any dimension, enum, `#include "file"` (also
searched in /lib), macros (see below), all operators incl. ?: ++/--,
compound assignment (+= -= *= /= %= &= |= ^= <<= >>=) and the comma operator
(`for (i = 0, j = n; i < j; i++, j--)`; in arguments and initializers only
inside parentheses, as in C), sizeof(type), casts,
if/else while for switch/case/default break continue return, prototypes,
`extern` declarations of globals defined in other files (see Toolchain).
String and char escapes: `\n \t \r \b \a \f \v \e \\ \' \"`, octal `\033`, hex `\x1b`.
Integer rules follow C: char and short promote to int; with an unsigned int
operand, comparisons, `/`, `%` and `>>` are unsigned. Casts, stores and
`return` truncate to the target type. Literals take `u`/`l` suffixes; hex
constants above 0x7FFFFFFF are unsigned. `sizeof` gives an int.
Function pointers: `int (*fp)(int)`, arrays `int (*tbl[4])(int)`, struct
members, parameters, typedefs (`typedef int (*op)(int, int);`), casts and
sizeof `(int (*)(int))`, `fp = f` / `fp = &f`, calls `fp(x)` / `(*fp)(x)` /
`s->fn(x)` / `tbl[i](x)`, functions returning function pointers. Parameter
types of a function pointer are not checked. Builtins (printf, ...) are
syscalls and cannot be taken as function pointers.
Initializers, global and local: `int x = 5, y = x * 2;`, `char *s = "hi";`,
`char buf[] = "abc";`, `char b[8] = "hi";`, `int a[5] = {1, 2};` (rest zero),
`int t[] = {1, 2, 3,};` (size from the list), structs and arrays of structs
`{ {1, 2}, {3, 4} }`, function pointer tables `{ add, sub }`, `&var` and
other expressions. Nested aggregates need their own braces; no designators.
Constant global initializers go straight into the data image; the others run
as code before `main`. Array sizes may be `#define`/enum names.
Multi-dimensional arrays: `int m[3][4]`, `int cube[2][3][4]`, struct members
`int m[2][3]`, `char names[][8] = {"a", "b"}`, `int t[][2] = {{1, 2}, {3, 4}}`,
parameters `int m[][4]`, pointers to arrays `int (*p)[4] = m;`, `m[i]` as an
`int *` to row i, `sizeof(int[3][4])`. Only the first dimension may be empty.
Macros: `#define NAME text`, function-like `#define MAX(a, b) ((a) > (b) ? (a) : (b))`,
multi-line bodies with `\` at the line ends, `#x` (string), `a ## b` (paste),
variadic `#define LOG(fmt, ...) printf(fmt, __VA_ARGS__)`, `#undef`. As in C,
arguments are macro-expanded before substitution except next to `#` and `##`,
results are rescanned, and a macro is not expanded inside its own expansion. Adjacent string literals join
(`"T2DOS " VERSION`). Array sizes, case labels and enum values take constant
expressions (`#define SIZE (4 * 2)`, `sizeof(type)`, `?:`).
Conditional compilation: `#if`, `#ifdef`, `#ifndef`, `#elif`, `#else`, `#endif`
(nested), `defined(X)` / `defined X`, `#error`. `#if` expressions are
macro-expanded constant expressions; unknown names are 0. `__T2CC__` is
predefined, and `cc -D NAME[=value]` defines a macro (value 1 by default).
Float: single precision on the RP2350's FPU. Literals `1.5`, `.5`, `1e-3`,
`2.5f` are decoded at compile time, correctly rounded. C's conversions apply on
assignment, casts, `return`, initializers and arguments (also through function
pointers); arithmetic mixing int and float is float; comparisons follow IEEE
(NaN compares unequal), and `-0.0` is false. `double` is accepted but is the
same 32-bit float, so unsuffixed literals are float too (in C they are double).
Not yet: 64-bit integers or doubles, `long long`.
See compiler/tests/ for the covered subset (test4.c: function pointers,
test5.c: initializers, test6.c: multi-dimensional arrays, test7.c:
unsigned/short, test8.c: the comma operator, test9.c: macros, test10.c:
conditional compilation, test11.c: float).

## Verified
On a CPico 2350 (no SD card, flash filesystem): boot into `/bin/sh.bin` from
flash; files survive reset and reflash; `ls`, `free`, `echo`, `> file`,
`>> file`, `cat`, `cp`, `hexdump`, `put`; `cc` compiles and runs test.c and an
`#include "sys.h"` program; `cc` rebuilds `cc.bin` byte-identical to the PC
build, and that `cc` rebuilds it again identically. PSRAM is fully returned
after every program. `edit` works through `screen` (tested with its default
flow control on) and builds on the board byte-identical to the PC build.
Backspace erases at the shell and `write` prompts. test4.c (function
pointers), test5.c (initializers), test6.c (multi-dimensional arrays),
test7.c (unsigned/short), test8.c (comma operator), test9.c (macros),
test10.c (conditional compilation, built with -D FROM_CMDLINE=7) and test11.c
(float) print the same as a native gcc build, whether compiled on the PC or on
the board. Float literals decode to the same bits as gcc's for 2841 test values
(with an f suffix; without one gcc rounds through double).
The toolchain (tools/check_toolchain.py --board): every tests/toolchain/ program
(C calling assembly, assembly calling C, globals shared through `extern`,
functions and globals named `sp`, `pc`, `r1`, `b`, `add`, `push`, a library
linked with `-l` of which only the needed member is linked) is built with `cc`
on the board, prints what it should, and its `.bin` equals the PC build. A
compile error, an `as` error and an undefined function stop `cc` with the
tool's message and exit code and leave no files in `/tmp`. On the board `cc`
rebuilds `cc`, `as`, `ld`, `ar`, `nm` and `objdump` byte-identical to the PC
builds (cc.c takes about 22 s), and that `cc` compiles test4.c to test12.c
with the same output as gcc (`--selfhost`). The userland programs and the
tools built through `cc`, `as` and `ld` are byte-identical to the output of the
reference compiler from before the assembler existed (`make ref`,
tools/check_ref.py), and work on the board after `make` rebuilt them.
Under qemu with fileserver.py: rls/get/send against a real TCP server, get cc.c
then compile it on the "board" and send the binary back, identical to the PC
build; nc -l accepts a connection.
Under qemu-arm with host_linux: the shell runs every tool, `cc` compiles and
runs test.c, redirection works, and `cc` on the "board" rebuilds `cc.bin`
byte-identical to the PC cross build.

## Not yet verified / known gaps
- SD card on the CPico 2350: not tested yet (no card was mounted; the kernel
  expects it on SPI0 with CS on GP17, `SD_CS SS`).
- Network tools on real hardware: only tested under qemu so far.
- `host_linux.c` does not yet free a program's `malloc`s on exit or zero them,
  unlike the board kernel. Its new `%f %e %g` formatting is untested (no qemu here).
- Uploading a whole `root/` tree to the flash filesystem is manual (`put` per file).
- compiler/tests/ runs only on the board, through `tools/check_toolchain.py
  --selfhost` (test4.c to test12.c against `gcc -m32 -msse2 -mfpmath=sse`, which
  matches cc's 32-bit pointers and single precision floats); there is no PC runner.
- No `static` (file-local) functions or variables: all globals of a program
  share one namespace at link time.
- Under qemu/host_linux the function pointer support has not been run yet
  (qemu-arm is not installed on the development PC).
