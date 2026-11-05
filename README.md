# T2DOS: a DOS-class OS for the RP2350, with a self-hosting C compiler

One program at a time, flat memory, kernel in the Arduino sketch, everything
else is a T2CC program on the SD card.

## Layout
    sys.h             syscall ABI: numbers, flags, structs. Shared by kernel, cc.c, programs
    cc.c              the compiler (gcc builds it as a PC cross compiler; compiles itself on the board)
    kernel/kernel.ino the kernel (arduino-pico, SDFS on SPI, set SD_CS)
    host_linux.c      the same kernel contract on Linux/ARM, run binaries with qemu-arm
    userland/*.c      sh ls cat cp mv rm mkdir rmdir echo hexdump free put write blink
                      wifi get send rls nc (network)
    lib/net.c         client helpers for the file protocol, #include "net.c"
    tools/fileserver.py  Linux side: python3 fileserver.py dir [port]
    root/             SD card image: /bin/*.bin  /lib/sys.h /lib/net.c  /src/
    Makefile          make all; make host

## Bootstrapping the card
1. `make` (needs gcc). Copy `root/` to the SD card.
2. Flash `kernel.ino`. It boots `/bin/sh.bin`; if that is missing you get a
   `recovery>` prompt with `put name size` (raw bytes follow), `ls`, `mkdir`, `run`.
3. On the board: `cd /src`, `write hello.c` (type it, end with `.`),
   `cc -o hello.bin hello.c`, `hello`.
   `put` works from the shell too; from a PC:
   `(printf 'put cc.bin %d\n' $(stat -c%s cc.bin); cat cc.bin) > /dev/ttyACM0`
4. `/autoexec.sh` runs at shell start; `sh script` runs any script.

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
  grouped: process 0+, console 16+, files 32+, memory/strings 64+, network 80+
  (reserved), board 96+, user 128+.
- `exec(path, argc, argv)` loads a binary into fresh code (SRAM) + data + 256 KB
  stack (PSRAM), runs it, frees everything, returns its exit code. Nesting up to 4.
  Files the child leaked are closed; `redirect(fd)` is restored.
- fd 0/1 are the console; `redirect(fd)` routes fd 1 and printf to a file (the
  shell implements `> file` and `>> file` with it). cwd lives in the kernel.
- Console is a driver interface (`putc/getc/kbhit/ctl/width/height`); the serial
  driver is in; a display+keyboard driver is the same five methods.
- `cc -I dir` adds an include directory (the Makefile uses `-I lib`).
- `printf`/`sprintf` are kernel-side: `%[-][0][width]d u x c s %%`.

## Language subset (cc.c)
int char void, pointers, structs (named or anonymous, array members),
typedef, global/local 1-D arrays, enum, `#define NAME number`,
`#include "file"` (also searched in /lib), all operators incl. ?: ++/-- and
compound assignment (+= -= *= /= %= &= |= ^= <<= >>=), sizeof(type), casts,
if/else while for switch/case/default break continue return, prototypes.
Not yet: float, initializers, function pointers, unsigned/short, 2-D arrays,
multi-line macros. See test.c and test3.c for the covered subset.

## Verified
Under qemu with fileserver.py: rls/get/send against a real TCP server, get cc.c
then compile it on the "board" and send the binary back, identical to the PC
build; nc -l accepts a connection.
Under qemu-arm with host_linux: the shell runs every tool, `cc` compiles and
runs test.c, redirection works, and `cc` on the "board" rebuilds `cc.bin`
byte-identical to the PC cross build.
