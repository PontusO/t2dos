# T2DOS: a DOS-class OS for the RP2350, with a self-hosting C compiler

One program at a time, flat memory, kernel in the Arduino sketch, everything
else is a T2CC program on the SD card (or, without a card, in on-board flash).

## Layout
    sys.h             syscall ABI: numbers, flags, structs. Shared by kernel, compiler, programs
    compiler/cc.c     the compiler (gcc builds it as a PC cross compiler, compiler/cc; compiles itself on the board)
    compiler/tests/   language tests: test.c test3.c test4.c ...
    kernel/kernel.ino the kernel (arduino-pico; SDFS on SPI, set SD_CS; LittleFS fallback)
    host_linux.c      the same kernel contract on Linux/ARM, run binaries with qemu-arm
    userland/*.c      sh ls cat cp mv rm mkdir rmdir echo hexdump free put write edit blink
                      wifi get send rls nc (network)
    lib/net.c         client helpers for the file protocol, #include "net.c"
    tools/fileserver.py  Linux side: python3 fileserver.py dir [port]
    root/             filesystem image: /bin/*.bin  /lib/sys.h /lib/net.c  /src/
    Makefile          make all; make host

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
    ^O (or ^S)                  save          ^X (or ^C, ^Q)            quit, twice if unsaved
    ^F                          find (Enter = next match, Esc cancels)

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
- `cc -I dir` adds an include directory (the Makefile uses `-I lib`).
- `printf`/`sprintf` are kernel-side: `%[-][0][width]d u x c s %%`.

## Language subset (compiler/cc.c)
int char void, pointers, structs (named or anonymous, array members),
typedef, global/local arrays of any dimension, enum, `#define NAME number`,
`#include "file"` (also searched in /lib), all operators incl. ?: ++/-- and
compound assignment (+= -= *= /= %= &= |= ^= <<= >>=), sizeof(type), casts,
if/else while for switch/case/default break continue return, prototypes.
String and char escapes: `\n \t \r \b \a \f \v \e \\ \' \"`, octal `\033`, hex `\x1b`.
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
Not yet: float, unsigned/short, multi-line macros.
See compiler/tests/ for the covered subset (test4.c: function pointers,
test5.c: initializers, test6.c: multi-dimensional arrays).

## Verified
On a CPico 2350 (no SD card, flash filesystem): boot into `/bin/sh.bin` from
flash; files survive reset and reflash; `ls`, `free`, `echo`, `> file`,
`>> file`, `cat`, `cp`, `hexdump`, `put`; `cc` compiles and runs test.c and an
`#include "sys.h"` program; `cc` rebuilds `cc.bin` byte-identical to the PC
build, and that `cc` rebuilds it again identically. PSRAM is fully returned
after every program. `edit` works through `screen` (tested with its default
flow control on) and builds on the board byte-identical to the PC build.
Backspace erases at the shell and `write` prompts. test4.c (function
pointers), test5.c (initializers) and test6.c (multi-dimensional arrays)
print the same as a native gcc build, whether compiled on the PC or on the
board.
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
  unlike the board kernel.
- Uploading a whole `root/` tree to the flash filesystem is manual (`put` per file).
- compiler/tests/ has no automatic runner: compare their output by hand
  (test4.c to test6.c against `gcc compiler/tests/testN.c && ./a.out`;
  test4's pointer `sizeof` differs on a 64-bit PC).
- Under qemu/host_linux the function pointer support has not been run yet
  (qemu-arm is not installed on the development PC).
