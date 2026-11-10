# T2DOS User's Manual

T2DOS is a small DOS-class operating system for the RP2350 (tested on the
iLabs CPico 2350). It runs one program at a time in flat memory, with a shell,
file tools, a full-screen editor, network tools and a complete C development
toolchain: a C compiler (`cc`), an assembler (`as`), a linker with libraries
(`ld`, `ar`) and the inspection tools `nm` and `objdump`. The toolchain runs
on the board itself and rebuilds itself there, byte-identical to the PC build.
For quick programs there is also a QBasic-style BASIC interpreter (`basic`),
which runs a `.bas` file straight from the editor.

The kernel is an Arduino sketch. Everything else is a program on the SD card,
or, when no card is found, in the board's flash.

Contents

1. [Getting started](#1-getting-started)
2. [The shell](#2-the-shell)
3. [Command reference](#3-command-reference)
4. [The editor](#4-the-editor)
5. [Writing programs in C](#5-writing-programs-in-c)
6. [Programming in BASIC](#6-programming-in-basic)
7. [The toolchain](#7-the-toolchain)
8. [Assembly language](#8-assembly-language)
9. [System reference](#9-system-reference)
10. [Building T2DOS from source](#10-building-t2dos-from-source)
11. [Status](#11-status)

---

## 1. Getting started

### What you need

- An RP2350 board with PSRAM: the iLabs CPico 2350 (8 MB PSRAM) is the tested one.
- Optional: an SD card on SPI0 with CS on GP17. Without one, T2DOS uses a
  7 MB filesystem in the board's flash.
- A Linux PC with gcc, `arduino-cli` and the arduino-pico core
  (`rp2040:rp2040`, tested with 6.2.0), and a terminal program such as
  `screen`.

### Build and install

On the PC, in the repository:

```sh
make                                   # the PC cross compiler and every program, into root/
FQBN=rp2040:rp2040:ilabs_cpico_2350:flash=8388608_7340032
arduino-cli compile --fqbn $FQBN --build-property "compiler.cpp.extra_flags=-I$PWD" --output-dir build/kernel kernel
arduino-cli upload  --fqbn $FQBN -p /dev/ttyACM0 --input-dir build/kernel
```

The `flash=` layout gives the kernel 1 MB and the flash filesystem 7 MB.
Reflashing the kernel keeps the filesystem.

Then put the files on the board:

- **SD card:** copy the contents of `root/` to the card.
- **Flash filesystem:** upload each file over the serial line with `put`
  (see [put](#put)). For example, from the PC:

  ```sh
  f=root/bin/sh.bin; (printf 'put /bin/sh.bin %d\n' $(stat -c%s $f); cat $f) > /dev/ttyACM0
  ```

  The board needs everything in `root/bin/` in `/bin`, and `root/lib/*` in
  `/lib` (the headers and sources the compiler includes).

### Connect

```sh
screen /dev/ttyACM0 115200
```

After a reset the kernel prints a banner and starts the shell (the numbers
vary):

```
T2DOS kernel  psram 8388608  sram free 412345  fs flash
T2DOS shell, type help
/>
```

`fs flash` or `fs sd` says which filesystem is in use. To leave `screen`,
press Ctrl-A then k. Closing the terminal window only detaches `screen` and
keeps the port busy; `screen -r` gets you back in.

`screen` has flow control on for serial lines, so Ctrl-S freezes its output
until Ctrl-Q. Turn it off with `defflow off` in `~/.screenrc` if that gets in
the way.

### A first session

```
/> cd /src
/src> edit hello.c
```

Type the program, press Ctrl-O to save and Ctrl-X to quit:

```c
#include "sys.h"
int main(int argc, char **argv) {
  printf("hello from T2DOS, %d args\n", argc);
  return 0;
}
```

Compile and run it:

```
/src> cc -o hello.bin hello.c
/src> hello a b
hello from T2DOS, 3 args
```

`cc` prints nothing when it succeeds. `hello` runs `hello.bin` from the
current directory.

### The recovery prompt

If `/bin/sh.bin` is missing, or the shell exits, the kernel shows a minimal
prompt that can still load files and run programs:

```
recovery> put /bin/sh.bin 5025         (then send the raw bytes)
recovery> ls /bin
recovery> mkdir /src
recovery> run /bin/ls.bin /src
recovery> sh                           (start the shell again)
```

---

## 2. The shell

The shell prints the current directory as its prompt and runs one command
line at a time. A command can also be started in the background, see
[Background jobs](#background-jobs).

### Running programs

A command is a program name followed by arguments separated by spaces:

```
/src> ls /bin
/src> cp hello.c hello.bak
```

The shell looks for `name.bin` in the current directory first, then in
`/bin`. A name that contains a dot is used as given (`tool.bin`,
`/bin/ls.bin`). Arguments are split at spaces; there is no quoting. A line
holds up to 255 characters and 15 arguments.

When a program ends with a non-zero exit code, the shell reports it with the
run time:

```
/src> cat nosuch.txt
cat: nosuch.txt: not found
[exit 1, 12 ms]
```

### Built-in commands

```
cd [dir]        change directory (no argument: /)
pwd             print the current directory
exit [code]     leave the shell (the kernel's recovery prompt takes over)
jobs            list the background jobs
kill N          end background job N
help            a one-line summary
```

### Output redirection

`> file` writes a program's output to a file, `>> file` appends to it:

```
/src> ls /bin > files.txt
/src> echo one more line >> files.txt
```

Only the output is redirected; there are no shell pipes (`a | b`). Programs
can talk through [named pipes](#named-pipes) instead.

### Background jobs

A command line that ends in `&` starts the program as a background job and
gives the prompt back at once. The shell prints the job's number:

```
/src> beat &
[1] beat
/src> beat 1
beat 2
beat 3
```

`beat` is the small program from
[Writing a background program](#writing-a-background-program): it prints a
line every second, three times, then ends. Its output appears on the console
while you keep working, mixed with whatever you type. When a job ends, the
shell reports it at the next prompt:

```
/src> jobs
[1] done beat (exit 0)
```

`jobs` lists the running jobs and the ended ones that have not been reported
yet:

```
/src> beat &
[1] beat
/src> beat &
[2] beat
/src> jobs
[1] running beat
[2] running beat
```

`kill N` ends job N. The job's exit code is -9, and its pipes and memory are
freed:

```
/> demosrv &
[1] demosrv
/> kill 1
[1] done demosrv (exit -9)
/> kill 9
kill: no job 9
```

Rules for jobs:

- At most 4 jobs run at once. A fifth `&` prints `too many jobs`:

  ```
  /src> beat &
  too many jobs
  ```
- After `&` the shell lets the new job run until it first waits (a `sleep`,
  a read of an empty pipe, ...) and only then reads the next command. So
  whatever the job creates before that, such as a server's pipes, is there
  for the next line, also in a script.
- A job inherits the current directory; a `cd` inside the job changes only
  the job's. `cmd > file &` and `cmd >> file &`
  send the job's output to the file, which stays open until the job ends:

  ```
  /src> beat >> /tmp/b.txt &
  [1] beat
  /src> cat /tmp/b.txt
  beat 1
  beat 2
  beat 3
  [1] done beat (exit 0)
  ```

- Jobs keep running when the shell exits. The exit reports are printed by
  the shell at its next prompt.
- Keys always go to the foreground program. A job that calls `getchar` just
  waits, and never gets a key.
- Tasks switch only when a program waits (see
  [Writing a background program](#writing-a-background-program)). One effect:
  while `cc` runs, the jobs wait, because `cc` compiles without waiting.
  They continue when `cc` ends.

### Line editing and history

- Backspace erases the last character, Ctrl-C discards the line.
- Up and Down scroll through the last 16 command lines. A recalled line can
  be edited before pressing Enter. Down past the newest line brings back what
  you had started typing.

### Scripts

A script is a text file with one command per line; empty lines and lines
starting with `#` are skipped.

```sh
# build.sh: build and run the example
cc -o hello.bin hello.c
hello
```

```
/src> sh build.sh
hello from T2DOS, 1 args
```

`/autoexec.sh`, if it exists, runs every time the shell starts.

---

## 3. Command reference

Example outputs show the format; sizes and numbers vary.

### Files and directories

#### ls

```
ls [file|dir...]
```

Lists a directory (default: the current one) with sizes, or shows one file.

```
/> ls /src
     112  hello.c
    5025  hello.bin
   <dir>  old/
2 files, 5137 bytes
```

#### cat

```
cat file...
```

Prints files to the console, one after another.

```
/src> cat hello.c
```

#### cp

```
cp src dst
```

Copies a file.

```
/src> cp hello.c /src/old/hello.c
```

#### mv

```
mv old new
```

Renames or moves a file.

```
/src> mv hello.bak hello.old
```

#### rm

```
rm file...
```

Deletes files.

```
/src> rm hello.old hello.bin
```

#### mkdir, rmdir

```
mkdir dir...
rmdir dir...
```

Create or remove directories. `rmdir` only removes empty directories.

```
/> mkdir /src/lib
/> rmdir /src/lib
```

#### hexdump

```
hexdump file
```

Shows a file as hex bytes and characters, 16 bytes a line.

```
/src> hexdump hello.c
000000  23 69 6e 63 6c 75 64 65 20 22 73 79 73 2e 68 22  #include "sys.h"
000010  0a 69 6e 74 20 6d 61 69 6e 28 69 6e 74 20 61 72  .int main(int ar
```

#### write

```
write file
```

Types lines into a file from the console, a quick alternative to the editor.
A line with a single `.` ends the input.

```
/src> write notes.txt
enter text, end with a single . on a line
first line
second line
.
```

#### put

```
put file size
```

Receives exactly `size` raw bytes from the console and stores them in
`file`. This is how files get onto the board without an SD card reader or a
network. Send the command and the bytes from the PC in one go (with no
`screen` session open on the port):

```sh
f=hello.c; (printf 'put /src/hello.c %d\n' $(stat -c%s $f); cat $f) > /dev/ttyACM0
```

The same `put` works at the recovery prompt.

### System

#### free

```
free
```

Shows free memory and disk space.

```
/> free
sram free     412345 bytes
psram        8388608 bytes, 8126464 free
disk            7168 KB, 1234 KB used
```

#### echo

```
echo [word...]
```

Prints its arguments, separated by single spaces. Handy with `>`:

```
/> echo cc -o hello.bin hello.c > /src/build.sh
```

#### blink

```
blink [pin] [count]
```

Blinks a GPIO pin `count` times (defaults: pin 25, 10 times), as a board I/O
demo.

```
/> blink 25 3
```

### Background

`jobs`, `kill` and `&` are part of the shell, see
[Background jobs](#background-jobs).

#### demosrv

```
demosrv [name] [-p port]
```

A small server that answers requests sent through a named pipe. It is the
reference for writing your own background program (its source is
`userland/demosrv.c`, and every step is commented). Run it as a job:

```
/> demosrv &
[1] demosrv
```

It makes two pipes, `/pipe/NAME` for requests and `/pipe/NAME.out` for replies
(`NAME` is `demosrv` unless you give another name). One request is one
message: write it with `echo`, then read the reply with `cat`. The requests are
`status`, `echo TEXT` and `quit`:

```
/> echo status > /pipe/demosrv
/> demosrv: status
cat /pipe/demosrv.out
demosrv: up 0 s, 1 requests
/> echo echo hello there > /pipe/demosrv
/> demosrv: echo
cat /pipe/demosrv.out
hello there
/> echo foo > /pipe/demosrv
/> demosrv: foo
cat /pipe/demosrv.out
error: unknown command 'foo'
/> echo quit > /pipe/demosrv
/> demosrv: quit
cat /pipe/demosrv.out
bye
/> jobs
[1] done demosrv (exit 0)
```

The `demosrv: ...` lines are the server's log, printed on the console when it
handles a request (the shell does not redraw its prompt after them, so just
type the next command). The pipes exist once `[1] demosrv` is printed, so the
first request can follow at once, also on the next line of a script. `cat` on
the reply pipe waits until the reply exists; if you read before sending a
request, press Ctrl-C to give up. `quit` ends the server; it lets the client read the reply first.
`kill N` also works, but then any unread reply is lost.

With `-p port` the server also answers each TCP connection on that port with
its status line (this needs the WiFi up, see [wifi](#wifi)):

```
/> demosrv -p 5000 &
```

A second server needs another name: `demosrv two &` uses `/pipe/two` and
`/pipe/two.out`.

### Text editor

```
edit file
```

A full-screen editor. See [The editor](#4-the-editor).

### Network

The network tools use the board's WiFi and a small file server on the PC.

#### wifi

```
wifi ssid password
wifi
```

Joins a network (15 s timeout), or shows the status.

```
/> wifi homenet secret123
joining homenet...
connected, ip 192.168.1.42
/> wifi
connected, ip 192.168.1.42
```

#### The PC file server

`rls`, `get` and `send` talk to `tools/fileserver.py` on the PC, which serves
one directory (default port 7777):

```sh
python3 tools/fileserver.py ~/t2files 7777
```

#### rls

```
rls host[:port] [dir]
```

Lists files on the server.

```
/> rls 192.168.1.10
/> rls 192.168.1.10:7777 projects
```

#### get

```
get host[:port] remote [local]
```

Downloads a file from the server. The default local name is the remote
file's name without its directory.

```
/src> get 192.168.1.10 game.c
/src> get 192.168.1.10 projects/game.c game2.c
```

#### send

```
send host[:port] local [remote]
```

Uploads a file to the server. The default remote name is the local file's
name without its directory.

```
/src> send 192.168.1.10 hello.c
/src> send 192.168.1.10 hello.bin builds/hello.bin
```

#### nc

```
nc host port
nc -l port
```

A raw TCP terminal: what you type is sent, what arrives is printed. `-l`
waits for one incoming connection first. Ctrl-] quits.

```
/> nc 192.168.1.10 23
/> nc -l 5000
```

### Development

`cc`, `as`, `ld`, `ar`, `nm` and `objdump` are described in
[The toolchain](#7-the-toolchain), with examples.

#### basic

```
basic [file.bas [args...]]
```

Runs a BASIC program, or with no file name starts the immediate prompt
(`Ok`), where single lines run at once. Errors print as
`file.bas:LINE: message`; Ctrl-C stops a program with `Break at file.bas:LINE`.
The exit code is 0, or 1 after an error or a break. See
[Programming in BASIC](#6-programming-in-basic) for the language and the
examples in `/src/basic/`.

```
/src> basic basic/words.bas basic/guess.bas
 15 lines, 77 words, 385 characters
```

---

## 4. The editor

```
edit file
```

`edit` is a full-screen editor for an ANSI/VT100 terminal such as `screen`.
It reads the window size from the terminal (80x24 if it cannot), and uses
the alternate screen, so the shell's output comes back when it quits. A new
file is created when you save.

### Keys

```
arrows, Home, End, PgUp, PgDn     move
Backspace, Del, Enter, Tab        edit
^O (or ^S)                        save
^R                                save and run a .bas file with basic
^X (or ^Q)                        quit (press twice to leave without saving)
^F                                find (^F again or Enter: next match, Esc: cancel)
^B                                set the mark (again: clear it)
^K                                cut
^C                                copy
^U                                paste the cut or copied text at the cursor
```

The bottom line shows the file name, `[modified]` when there are unsaved
changes, the line and column, and the keys.

### Cut, copy and paste

Without a mark, `^K` and `^C` take the whole current line. Pressing them
again right away adds the next line to the same block:

```
^K^K^K        cut three lines
^C^C          copy two lines (the cursor moves down as it copies)
^U            paste them, as often as you like
```

To work on any part of the text, set the mark with `^B` at one end, move the
cursor to the other end (the block shows in reverse video), then press `^K`
or `^C`. The block includes the characters at both ends. Typing or deleting
clears the mark.

Example: to duplicate a function, put the cursor on its first character,
press `^B`, move to its closing `}`, press `^C`, move to where the copy goes,
and press `^U`.

### Running BASIC programs

`^R` saves the file (including changes you have not saved yet) and, if its
name ends in `.bas`, runs it with [`basic`](#6-programming-in-basic). When the
program ends, `edit` shows `Press any key` (with the exit code if it is not
0), waits for a key and redraws the screen. For other files `^R` only shows
`^R runs .bas files`.

### Notes

- Enter keeps the current line's indentation, except while text is being
  pasted from the terminal (so pasted code keeps its own layout).
- Use `^O` to save under `screen`: with `screen`'s flow control on, `^S`
  freezes the terminal until `^Q`.
- Long lines scroll horizontally; tabs are 8 columns.
- One session can add up to 256 KB of text to a file.

---

## 5. Writing programs in C

### Hello, world

```c
#include "sys.h"
int main(int argc, char **argv) {
  int i;
  for (i = 0; i < argc; i++) printf("argument %d: %s\n", i, argv[i]);
  return 0;
}
```

```
/src> cc -o args.bin args.c
/src> args one two
argument 0: args
argument 1: one
argument 2: two
```

`main`'s return value is the program's exit code. `#include "sys.h"` gives
the constants (`OPEN_READ`, `PIN_OUTPUT`, ...) and the structs (`stat_t`,
`dirent_t`). `#include <...>` lines are accepted and ignored, so standard C
code with `#include <stdio.h>` compiles as it is.

### The library

The library functions are built into the compiler and run in the kernel, so
they cost no code in the program. All arguments and results are ints or
pointers, except for the maths functions, which take and return `float`
(see [Maths](#maths)).

| Group | Functions |
|---|---|
| Process | `exit(code)`, `exec(path, argc, argv)` (runs a program, returns its exit code), `millis()`, `sleep(ms)`, `sysinfo(kind)`, `spawn(path, argc, argv)` (starts a background job, returns its number), `yield()`, `jobs(buf, max)`, `kill(job)` |
| Console | `putchar(c)`, `getchar()` (waits for a key), `kbhit()` (1 if a key is waiting), `conctl(cmd, a, b)`, `redirect(fd)` |
| Files | `open(path, mode)` (mode `OPEN_READ`, `OPEN_WRITE`, `OPEN_APPEND`; -1 on error), `mkpipe(name)`, `close(fd)`, `read(fd, buf, n)`, `write(fd, buf, n)`, `seek(fd, offset, whence)`, `stat(path, &st)`, `unlink(path)`, `rename(old, new)`, `mkdir(path)`, `rmdir(path)`, `chdir(path)`, `getcwd(buf, n)` |
| Directories | `opendir(path)`, `readdir(d, &entry)` (1 while there are entries), `closedir(d)` |
| Memory | `malloc(n)` (zeroed memory), `free(p)`, `memset`, `memcmp`, `memcpy` |
| Strings | `strlen`, `strcmp`, `strncmp`, `strcpy`, `strchr`, `atoi`, `printf(fmt, ...)`, `sprintf(buf, fmt, ...)` |
| Network | `net_join(ssid, password)`, `net_status()`, `net_ip()`, `connect(host, port)`, `send(fd, buf, n)`, `recv(fd, buf, n, timeout_ms)`, `listen(port)`, `accept(timeout_ms)`, `sock_avail(fd)` |
| Board | `gpio_mode(pin, mode)`, `gpio_write(pin, v)`, `gpio_read(pin)`, `analog_read(pin)`, `pwm_write(pin, v)`, `i2c_write(addr, buf, n)`, `i2c_read(addr, buf, n)`, `spi_transfer(buf, n)`, `delay_us(us)` |
| Maths | `sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2(y, x)`, `sinh`, `cosh`, `tanh`, `exp`, `log`, `log10`, `pow(x, y)`, `sqrt`, `fabs`, `floor`, `ceil`, `fmod(x, y)` |

`printf` and `sprintf` support `%d %u %x %c %s %f %e %g %%` with `-`, `0`,
a width and a precision (`%-8s`, `%04x`, `%.3f`). fd 0 and 1 are the console;
`redirect(fd)` sends fd 1 and `printf` to a file. `conctl` takes `CON_CLEAR`,
`CON_GOTO` (column, row) and `CON_COLOR` (foreground, background, 0 to 7).
`sysinfo` takes `INFO_SRAM_FREE`, `INFO_PSRAM_TOTAL`, `INFO_PSRAM_FREE`,
`INFO_DISK_TOTAL_KB` and `INFO_DISK_USED_KB`. Socket fds also work with
`read`, `write` and `close`; `recv` returns 0 on a timeout and -1 when the
connection is closed.

A program that counts the lines of a file:

```c
#include "sys.h"
char buf[512];
int main(int argc, char **argv) {
  int fd; int n; int lines; int i;
  if (argc < 2) { printf("usage: lines file\n"); return 1; }
  fd = open(argv[1], OPEN_READ);
  if (fd < 0) { printf("cannot open %s\n", argv[1]); return 1; }
  lines = 0;
  while ((n = read(fd, buf, 512)) > 0)
    for (i = 0; i < n; i++) if (buf[i] == '\n') lines++;
  close(fd);
  printf("%d lines\n", lines);
  return 0;
}
```

A program that drives a pin:

```c
#include "sys.h"
int main() {
  int i;
  gpio_mode(25, PIN_OUTPUT);
  for (i = 0; i < 5; i++) { gpio_write(25, 1); sleep(200); gpio_write(25, 0); sleep(200); }
  return 0;
}
```

### Writing a background program

Any program can run as a job (`prog &`). Nothing changes in how you write it,
with one rule that matters a great deal: **T2DOS does not preempt.** The
foreground program, the shell and every job take turns, and a task gives up
the processor only when it calls something that waits:

- `getchar`, `kbhit`, `sleep`, `recv`, `accept` (with a timeout),
- `read` of an empty named pipe, `write` to a full one,
- `yield()`, which lets another task run and returns 1 if one did, else 0.

A job that loops without ever calling one of these freezes the whole board,
the keyboard included. There is no way out except cutting the power, so a
polling loop must call `yield()` (or `sleep`) in every round. Anything else
the program does, such as reading a file or printing, runs to the end without
interruption.

A job that only waits is cheap. This one prints a line every second:

```c
#include "sys.h"
int main(int argc, char **argv) {
  int i;
  for (i = 1; i <= 3; i++) {
    sleep(1000);
    printf("beat %d\n", i);
  }
  return 0;
}
```

```
/src> cc -o beat.bin beat.c
/src> beat &
[1] beat
/src> beat 1
beat 2
beat 3
```

(While `cc` itself runs, the jobs are paused, because the compiler never
waits. They continue when it ends.) A job reaches the console with `printf`
like any program; with `> file &` its output goes to the file instead.

A server follows three steps, as `userland/demosrv.c` shows:

1. Create the resources first, before the first call that waits: the pipes
   (`mkpipe`), a `listen` socket, and open what you will read. The shell runs
   a new job until its first wait before it takes the next command, so what
   exists by then is there for the next line of a script. If one fails, print
   why and return.
2. Loop: wait for work, handle it, reply. When the program waits on two
   things at once (a pipe and a socket), no single call can wait for both, so
   check each without waiting (`accept(0)`, `stat` on the pipe) and call
   `yield()` when neither had anything. `accept` returns a socket fd (100 and
   up) for a connection, 0 when nobody connected in time (always for
   `accept(0)` with no one waiting) and -1 without a listener, so test
   `sock > 0`.
3. Leave cleanly. A program's pipes disappear the moment it exits, with any
   unread data, so wait until the client has read the last reply.

`spawn(path, argc, argv)` starts a job from a program: it returns the job
number (1 to 4) or a negative `SPAWN_` code (see
[System calls](#system-calls)). `jobs(buf, max)` fills an array of
`struct job_t` and `kill(job)` ends a job.

### Named pipes

A named pipe is a file name under `/pipe/` that programs use to send each
other messages. A program makes one with `mkpipe("name")` (the name without
the `/pipe/` prefix, at most 31 characters, no `/`), which creates
`/pipe/name`. After that the ordinary `open`, `read`, `write` and `close`
work on it, from C or from the shell (`echo hi > /pipe/name`, `cat /pipe/name`).

```c
mkpipe("work");                          /* creates /pipe/work, owned by this program */
fd = open("/pipe/work", OPEN_READ);
n = read(fd, buf, 512);                  /* waits until a message arrives */
```

How pipes behave:

- **Messages.** The bytes a writer sends are one message, which ends when the
  writer closes the pipe. A reader gets the bytes, and then `read` returns 0
  once at the end of the message. The next `read` starts the next message.
  So `echo status > /pipe/x` is exactly one message. To read a whole
  message, call `read` until it returns 0.
- **Waiting.** `read` of an empty pipe waits for a writer, and `write` to a
  full pipe waits for a reader. Both are switch points, so other tasks run
  meanwhile. In a foreground program, Ctrl-C ends the wait: `read` returns 0
  and `write` returns -1. This works also when other keys were typed before
  the Ctrl-C; those keys are kept for the program (up to 64 of them).
- **Size.** A pipe buffers 512 bytes. A longer message goes through in pieces
  as the reader consumes it. At most 8 pipes exist at a time. Two writers
  that write at the same time interleave their bytes, so give each pipe one
  writer at a time.
- **Owner.** The program that made the pipe owns it. When the owner exits or
  is killed, the pipe disappears at once with any unread data. Readers that
  are waiting get 0.
- **stat.** `stat("/pipe/x", &st)` gives `st.size`, the bytes buffered, and
  `st.mtime`, the number of complete messages waiting. Pipes have no time, so
  `mtime` is reused. When `st.mtime > 0`, reading one message will not wait.
  The byte count alone does not say that: the writer may have sent half a
  request and not closed yet.
- **Not listed.** `/pipe` has no directory listing; `ls /pipe` says not found.

A reply is a second pipe: the server reads `/pipe/NAME`, writes one message
to `/pipe/NAME.out` per request, and the client reads that. A client that
reads `NAME.out` before the reply exists just waits for it.

### The C language subset

`cc` compiles a large subset of C. It is written in that subset itself, which
is what lets it compile itself on the board.

**Types:** `int`, `char`, `short`, `void`, `unsigned` and `signed` forms,
`long` (32 bits, same as `int`), `float` (and `double`, which is the same
32-bit float), pointers, structs (named or anonymous, with array members),
`typedef`, `enum`, arrays of any dimension.

**Statements and operators:** `if`/`else`, `while`, `for`, `switch`/`case`/
`default`, `break`, `continue`, `return`; all operators including `?:`,
`++`/`--`, the compound assignments (`+= -= *= /= %= &= |= ^= <<= >>=`) and
the comma operator; `sizeof(type)`; casts; prototypes; `extern`.

**Integer rules** follow C: `char` and `short` promote to `int`; with an
`unsigned int` operand, comparisons, `/`, `%` and `>>` are unsigned; casts,
stores and `return` truncate to the target type. Literals take `u` and `l`
suffixes; hex constants above `0x7FFFFFFF` are unsigned.

**Strings and characters:** escapes `\n \t \r \b \a \f \v \e \\ \' \"`, octal
`\033` and hex `\x1b`. Adjacent string literals join (`"T2DOS " VERSION`).

**Initializers**, global and local:

```c
int x = 5, y = x * 2;
char *s = "hi";
char buf[] = "abc";
int a[5] = {1, 2};                 /* the rest is zero */
int t[] = {1, 2, 3,};              /* size from the list */
struct pt { int x; int y; } pts[] = { {1, 2}, {3, 4} };
int (*ops[])(int, int) = { add, sub };
```

Nested aggregates need their own braces; there are no designators. Constant
global initializers go straight into the data; the others run as code before
`main`.

**Multi-dimensional arrays:** `int m[3][4]`, `int cube[2][3][4]`,
`char names[][8] = {"a", "b"}`, parameters `int m[][4]`, pointers to arrays
`int (*p)[4] = m;`. Only the first dimension may be left empty.

**Function pointers:**

```c
typedef int (*op)(int, int);
int add(int a, int b) { return a + b; }
int apply(op f, int x, int y) { return f(x, y); }
int main() { op f = add; printf("%d\n", apply(f, 2, 3)); return 0; }
```

Also arrays of function pointers, struct members (`s->fn(x)`), `(*fp)(x)`,
and functions returning function pointers. Builtins such as `printf` cannot
be taken as function pointers.

**Float:** single precision on the RP2350's FPU. Literals (`1.5`, `.5`,
`1e-3`, `2.5f`) are rounded correctly at compile time. Mixed int and float
arithmetic is float; comparisons follow IEEE.

```c
float r = 2.0;
printf("area %.3f\n", 3.14159 * r * r);        /* area 12.566 */
```

**Preprocessor:** `#include "file"` (searched in the current directory, the
`-I` directory and `/lib`), `#define` with object-like and function-like
macros, multi-line bodies with `\`, `#x`, `a ## b`, variadic macros with
`__VA_ARGS__`, `#undef`, and conditional compilation with `#if`, `#ifdef`,
`#ifndef`, `#elif`, `#else`, `#endif`, `defined`, `#error`. `__T2CC__` is
predefined; `cc -D NAME[=value]` defines a macro.

```c
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#define LOG(fmt, ...) printf("log: " fmt "\n", __VA_ARGS__)
#ifdef DEBUG
  LOG("x is %d", x);
#endif
```

**Not supported:** `static`, `const`, `do`/`while`, `goto`, `sizeof` of an
expression (only `sizeof(type)`), functions of your own with `...` (variadic
macros work), struct assignment (`a = b` with structs is an error; copy with
`memcpy`), 64-bit integers, `long long` and real doubles.

The tests in `compiler/tests/` show each feature in use (test4.c function
pointers, test5.c initializers, test6.c arrays, test7.c unsigned and short,
test8.c the comma operator, test9.c macros, test10.c conditional compilation,
test11.c float, test12.c addresses of struct members, test13.c the maths
functions).

### Maths

The standard maths functions are built in and run in the kernel on the
RP2350's FPU:

`sin(x)`, `cos(x)`, `tan(x)`, `asin(x)`, `acos(x)`, `atan(x)`, `atan2(y, x)`,
`sinh(x)`, `cosh(x)`, `tanh(x)`, `exp(x)`, `log(x)` (natural), `log10(x)`,
`pow(x, y)`, `sqrt(x)`, `fabs(x)`, `floor(x)`, `ceil(x)`, `fmod(x, y)`.

They are single precision only: every argument and result is a 32-bit
`float` (T2DOS's `double` is the same type). An int argument is converted
as in an assignment, so `sqrt(2)` is `sqrt(2.0)`. Angles are in radians.
`#include <math.h>` is accepted and ignored; `#include "math.h"` (found in
`/lib`) defines the constants `M_PI`, `M_E`, `M_SQRT2`, `M_LN2` and
`M_LN10`. A function of your own with one of these names replaces the
built-in.

```c
#include "sys.h"
#include "math.h"

int main() {
  printf("sqrt(2)  %f\n", sqrt(2));
  printf("sin 30   %.4f\n", sin(30 * M_PI / 180));
  printf("hypot    %.3f\n", sqrt(pow(3, 2) + pow(4, 2)));
  printf("angle    %.2f degrees\n", atan2(1, 1) * 180 / M_PI);
  printf("e^1      %.6f\n", exp(1));
  printf("round    %.1f %.1f\n", floor(-2.5), ceil(-2.5));
  return 0;
}
```

prints

```
sqrt(2)  1.414214
sin 30   0.5000
hypot    5.000
angle    45.00 degrees
e^1      2.718282
round    -3.0 -2.0
```

---

## 6. Programming in BASIC

`basic` is an interpreter for a BASIC that follows Microsoft's QBasic: no line
numbers, labels for jumps, block `IF`, `SUB` and `FUNCTION`. A program is a
plain text file, usually ending in `.bas`, that you write in `edit` and run
at once. There is nothing to compile. Someone who knows QBasic should feel at
home, and listings that stay inside the subset described here run as they
are.

### Running programs

```
basic file.bas [args...]
basic
```

With a file name `basic` loads the program, checks it and runs it. The
examples of this chapter are in `/src/basic/`:

```
/src> basic basic/hanoi.bas
Moving 3 disks from A to C:
Disk 1 from A to C
Disk 2 from A to B
Disk 1 from C to B
Disk 3 from A to C
Disk 1 from B to A
Disk 2 from B to C
Disk 1 from A to C
Moves needed: 7
```

The rest of the command line is the program's `COMMAND$`. `basic` ends with
exit code 0 when the program finishes, and 1 after an error.

Errors name the file, the source line and the problem, in QBasic's words.
Mistakes that can be seen without running the program (a `FOR` without its
`NEXT`, an undefined label, a call with the wrong number of arguments, a
string assigned to a number) are found before the first statement runs. This
`err2.bas`

```
PRINT "start"
FOR i = 1 TO 3
PRINT i
```

prints nothing but

```
/src> basic err2.bas
err2.bas:2: FOR without NEXT
```

Errors that depend on values stop the program where they happen, after the
output it has already printed. Here `err1.bas` uses the array `a` with the
indexes 0 to 5 and goes up to 10:

```
DIM a(5)
FOR i = 0 TO 10
  a(i) = i * i
NEXT
```

```
/src> basic err1.bas
err1.bas:3: Subscript out of range
```

Ctrl-C stops a running program, as Ctrl-Break does in QBasic:

```
/src> basic loop.bas
Break at loop.bas:2
[exit 1, 1497 ms]
```

(`loop.bas` is `DO`, `n = n + 1`, `LOOP`.)

The program's memory is freed at the end, also after an error or a break.
A program that runs out of memory stops with `Out of memory` at its line and
`basic` ends with exit code 1. Its variables live in PSRAM, and `basic`
stops while 64 KB of it are still free, so the system keeps its own memory.
Keys you type while a program runs wait in a queue (up to 64) for `INKEY$`,
`INPUT` and `SLEEP`. The interpreter gives other tasks a turn often enough
that a BASIC program never freezes the board, and `basic prog.bas &` runs it
as a [background job](#background-jobs). Every run of `basic` loads
`basic.bin` (about 176 KB) into the SRAM that programs run from, so two BASIC
jobs fit beside each other but a third does not start. For the same reason a
program started with `^R` in `edit` that runs `SHELL "cc ..."` can run out of
program memory: `edit`, `basic` and `cc` are then all loaded at once.

### Writing and running a program in edit

Open the file in [the editor](#4-the-editor) and press `^R`. The editor saves
the file, including changes you have not saved yet, runs `basic` on it, and
when the program ends it shows `Press any key` (with the exit code if it is
not 0). The next key brings back the editor with your text where you left it.
This is the whole write, run, fix loop:

```
/src> edit basic/hanoi.bas
```

For any file name that does not end in `.bas`, `^R` only shows
`^R runs .bas files`.

### Immediate mode

`basic` without a file name shows the prompt `Ok`. A line you type runs at
once; variables stay between lines:

```
/src> basic
T2DOS BASIC
Ok
PRINT 2^10
 1024 
Ok
x = 5
Ok
PRINT x * 3, SQR(x)
 15            2.236068 
Ok
FOR i = 1 TO 5: PRINT i;: NEXT
 1  2  3  4  5 
Ok
a$ = "hello"
Ok
PRINT UCASE$(a$); LEN(a$)
HELLO 5 
Ok
PRINT foo(
LINE 1: Syntax error
Ok
RUN "/src/basic/hanoi.bas"
Moving 3 disks from A to C:
...
Moves needed: 7 
Ok
SYSTEM
/src>
```

A line is one statement or several joined by `:`. Block statements across
lines are not possible at the prompt, and `SUB`, `FUNCTION` and `DECLARE`
give `Illegal in direct mode` (as in QBasic); put procedures in a file and
`RUN "file.bas"` it. `SYSTEM` returns to the shell. Ctrl-C breaks an `INPUT`
and a running line.

### The examples

The examples are installed in `/src/basic/` (they come from `examples/basic/`
in the source tree).

**guess.bas** picks a number with `RND` and asks with `INPUT`. The numbers
differ in every game because `RANDOMIZE TIMER` seeds the generator from the
clock:

```
/src> basic basic/guess.bas
I am thinking of a number from 1 to 100.
Your guess? 50
Too high.
Your guess? 25
Too low.
Your guess? 37
Too high.
Your guess? 31
Too high.
Your guess? 28
Too high.
Your guess? 26
Too low.
Your guess? 27
Right! You needed 7 guesses.
```

**blink.bas** blinks the board LED (pin 25) five times with `PINMODE`,
`PINWRITE` and `PAUSE`, then prints `Done.` It is the BASIC version of the
[`blink`](#blink) command:

```
CONST led = 25
PINMODE led, 1
FOR i = 1 TO 5
  PINWRITE led, 1
  PAUSE 200
  PINWRITE led, 0
  PAUSE 200
NEXT
PRINT "Done."
```

**words.bas** counts the lines, words and characters of the text file given
on the command line, with `COMMAND$`, `LINE INPUT #` and `INSTR`:

```
/src> basic basic/words.bas basic/guess.bas
 15 lines, 77 words, 385 characters
/src> basic basic/words.bas
usage: basic words.bas file
```

**hanoi.bas** moves the disks of the Towers of Hanoi with a recursive `SUB`
(its output is at the top of this chapter). Change `n = 3` to move more
disks; the number of moves is 2^n - 1.

**bench.bas** measures the speed of the interpreter, see
[Status](#11-status):

```
/src> basic basic/bench.bas
seconds: 1.063477
statements per second: 188062
```

### A BASIC server on a pipe

`PIPE` makes a [named pipe](#named-pipes), so a BASIC program can be a
server like `demosrv`. This one answers each request in capitals:

```
PIPE "shout"
PIPE "shout.out"
OPEN "/pipe/shout" FOR INPUT AS #1
DO
  LINE INPUT #1, a$
  IF a$ = "quit" THEN EXIT DO
  OPEN "/pipe/shout.out" FOR OUTPUT AS #2
  PRINT #2, UCASE$(a$)
  CLOSE #2
LOOP
CLOSE
```

Saved as `shout.bas`, it runs as a job and is used from the shell (`basic`
is the job's name; at most two BASIC jobs fit in program memory, see
[Running programs](#running-programs)):

```
/src> basic shout.bas &
[1] basic
/src> echo hello board > /pipe/shout
/src> cat /pipe/shout.out
HELLO BOARD
/src> echo quit > /pipe/shout
/src> jobs
[1] done basic (exit 0)
```

One request is one message: it ends when the writer closes the pipe, which
is what `CLOSE #2` does for the reply. `LINE INPUT #1` on a pipe waits for
the next message while other tasks run, and `EOF(1)` on an empty pipe also
waits for data. The pipes disappear when the program ends.

### The language

This is a reference, grouped by task. Where nothing special is said, a
statement works as in QBasic.

#### Source form

- Keywords are not case sensitive; names are (unlike QBasic): `Total`, `total`
  and `TOTAL` are three variables, and the same goes for arrays, labels,
  `SUB`s and `FUNCTION`s. A name that matches a keyword in any case is still
  the keyword, so `Name$` is the `NAME` statement.
- One statement per line, or several separated by `:`.
- Comments: `REM text` and `' text`, to the end of the line.
- A label is a name followed by `:` at the start of a line (`loop:`). Labels
  of the main program are global; labels in a `SUB` or `FUNCTION` belong to
  it.
- A line ending in `_` continues on the next line.
- Lines may end with LF or CRLF.

#### Types and variables

| Suffix | `AS` name | Meaning |
|---|---|---|
| `%` | `INTEGER` | 32-bit integer |
| `&` | `LONG` | 32-bit integer (same as `INTEGER`) |
| `!` | `SINGLE` | 32-bit float |
| `#` | `DOUBLE` | 32-bit float (same as `SINGLE`) |
| `$` | `STRING` | text, 0 to 32767 characters |

A name without a suffix is `SINGLE`; `DEFINT`, `DEFLNG`, `DEFSNG`, `DEFDBL`
and `DEFSTR` with letter ranges change that. Variables need no declaration
and start at 0 or `""`. `x`, `x$` and the array `x()` are different things.

- `DIM a(10)`, `DIM b(1 TO 5, 0 TO 3) AS INTEGER`, `DIM s AS STRING`:
  arrays have up to 8 dimensions. An array used without `DIM` gets the
  bounds 0 to 10. `DIM SHARED` makes a variable visible inside `SUB` and
  `FUNCTION`. `REDIM` dimensions an array again (the contents are cleared),
  `ERASE` frees it, `LBOUND` and `UBOUND` give the bounds.
- `CONST name = expression`.
- A float stored in an integer variable is rounded (halves to the even
  number: 2.5 gives 2, 3.5 gives 4); `INT` rounds down and `FIX` towards 0.

#### Expressions

From the highest precedence down: `^`, unary `-`, `*` `/`, `\` (integer
division), `MOD`, `+` `-`, the comparisons `=` `<>` `<` `>` `<=` `>=`, `NOT`,
`AND`, `OR`, `XOR`, `EQV`, `IMP`. Comparisons give -1 for true and 0 for
false, and `AND`, `OR`, `NOT` work on all the bits, as in QBasic. `+` joins
strings, and strings compare by character codes. Number literals are `12`,
`1.5`, `1E3`, `&HFF` and `&O17`.

#### Control flow

- `IF cond THEN statements [ELSE statements]` on one line, or a block with
  `ELSEIF`, `ELSE` and `END IF`. `IF cond THEN label` jumps to the label.
- `FOR v = a TO b [STEP s]` ... `NEXT [v]`: the bounds and the step are
  worked out once.
- `WHILE cond` ... `WEND`, and `DO [WHILE|UNTIL cond]` ... `LOOP
  [WHILE|UNTIL cond]`.
- `SELECT CASE expr` with `CASE 1, 2`, `CASE 5 TO 9`, `CASE IS > 10` and
  `CASE ELSE`, closed by `END SELECT`.
- `EXIT FOR`, `EXIT DO`, `EXIT SUB`, `EXIT FUNCTION`.
- `GOTO label`, `GOSUB label` and `RETURN`. `END` and `STOP` end the program.
  Jumping out of a `FOR` loop or a `SUB` with `GOTO` is allowed.
- `SUB name (params)` ... `END SUB` and `FUNCTION name (params)` ... `END
  FUNCTION`; a function returns its value by assigning to its name. A `SUB`
  is called as `name args` or `CALL name(args)`. Variables and array elements
  are passed by reference (the procedure can change them), other
  expressions by value, and `a()` passes a whole array. Locals and recursion
  work; a runaway recursion stops with `Out of stack space`. `DECLARE` lines
  are accepted and ignored.
- `DATA`, `READ` and `RESTORE [label]`.
- `SWAP a, b`. `RANDOMIZE [seed]`: without a seed the clock is used (there is
  no prompt for a number as in QBasic); `RANDOMIZE TIMER` also works.

#### Screen and keyboard

- `PRINT` (or `?`) with `;` and `,` (14-column zones), a trailing `;` or `,`
  that keeps the cursor on the line, `TAB(n)` and `SPC(n)`.
- `INPUT ["prompt";] v1, v2, ...` asks again with `?Redo from start` when
  the answer does not fit; `INPUT "prompt", v` leaves out the `?`.
  `LINE INPUT ["prompt";] s$` takes the whole line. A typed line can be
  up to 1023 characters long; a longer one is the error `Line buffer
  overflow`.
- `INKEY$` returns the next key or `""`. The arrow and other special keys
  come as QBasic's two characters: `CHR$(0) + "H"` (up), `"P"` (down), `"K"`
  (left), `"M"` (right) and the codes for Home, End, PgUp, PgDn, Ins, Del.
- `CLS`, `LOCATE [row] [, col]` (1-based; a row or column left out stays
  where the cursor is), `COLOR fg [, bg]` with QBasic's colour numbers
  (0 black, 1 blue, 2 green, 3 cyan, 4 red, 5 magenta, 6 brown, 7 white; 8 to
  15 look the same as 0 to 7), `BEEP`, `SLEEP [seconds]` (without a number it
  waits for a key; the key stays in the queue), `WIDTH` (accepted, no
  effect). QBasic's cursor arguments of `LOCATE` (`LOCATE row, col, cursor,
  start, stop`) are accepted and have no effect: the console cannot hide or
  shape the cursor. `basic` counts the cursor's row from what it prints, so
  `LOCATE , col` is right after a `CLS` or a `LOCATE` with a row; before
  either it takes the top row as the start.

How numbers print is the part of QBasic that is easiest to get wrong, and
`basic` follows it. A positive number has a space before it and every number
a space after it. A `SINGLE` has at most seven significant digits, without
trailing zeros or a leading 0, and uses the `E` form for very large and very
small values. Integers print all their digits. `STR$` gives the same text
without the trailing space.

```
a = 7: b = 2
PRINT a / b, a \ b, a MOD b, a ^ b
PRINT 1E7, .000015, 100000
x% = 2.5: y% = 3.5
PRINT x%; y%
PRINT 1/3
```

prints

```
 3.5           3             1             49
 1E+07         1.5E-05       100000
 2  4
 .3333333
```

#### Files and the system

- `OPEN f$ FOR INPUT|OUTPUT|APPEND AS #n`, `CLOSE [#n, ...]` (without a
  number all files), `PRINT #n, ...`, `WRITE #n, ...` (quoted strings and
  commas), `INPUT #n, ...`, `LINE INPUT #n, s$`, `EOF(n)` and `FREEFILE`.
  `WRITE` without `#` writes to the screen. `INPUT #` reads lines of up to
  1023 characters and `LINE INPUT #` lines of up to 32767; a longer line is
  the error `Line buffer overflow`.
- Paths are T2DOS paths, relative to the current directory. A `\` in a path
  counts as `/`, so QBasic-style names work.
- `KILL f$`, `NAME old$ AS new$`, `MKDIR`, `RMDIR`, `CHDIR` and
  `FILES [dir$]`.
- `SHELL cmd$` runs a T2DOS program (the first word is the program name, found
  as the shell finds it) and waits for it; the exit code is not returned.
  `COMMAND$` is the command line after the file name.
- `PIPE name$` creates the [named pipe](#named-pipes) `/pipe/name`. Open it
  like a file.
- `RUN "file.bas"` (at the immediate prompt) runs a program with fresh
  variables and comes back to the prompt, and `SYSTEM` ends `basic`.

#### Functions

Numbers: `ABS SGN INT FIX SQR SIN COS TAN ATN EXP LOG RND TIMER CINT CLNG CSNG
CDBL`. `SQR`, `ATN` and `LOG` (natural) are the [maths
built-ins](#maths) of the system; `^` with a non-integer exponent uses
`pow`. `RND` gives a number from 0 up to 1, and `TIMER` the seconds since the
board started (a float with about seven digits: milliseconds for the first
few hours, then coarser).

Strings: `LEN LEFT$ RIGHT$ MID$ INSTR UCASE$ LCASE$ LTRIM$ RTRIM$ STR$ VAL CHR$
ASC STRING$ SPACE$ HEX$ OCT$`. `INKEY$`, `EOF`, `FREEFILE`, `LBOUND`,
`UBOUND` and `COMMAND$` are described above.

Board access (T2DOS extras, so that a blinking LED is a five-line program):
`PINMODE pin, mode`, `PINWRITE pin, value`, `PINREAD(pin)`,
`ANALOGREAD(pin)`, `PWM pin, value` and `PAUSE ms`. They are the library's
`gpio_mode`, `gpio_write`, `gpio_read`, `analog_read`, `pwm_write` and
`sleep`; see [The library](#the-library).

### Differences from QBasic

- `INTEGER` and `LONG` are both 32-bit (QBasic's `INTEGER` has 16 bits), and
  overflow is not detected.
- `DOUBLE` is the same 32-bit `float` as `SINGLE`: the board has no 64-bit
  float. A number prints with at most seven digits whatever its type, and
  `VAL` of a number with more than about seven digits loses the rest.
- There are no line numbers (not even as labels), and no `AUTO`, `LIST`,
  `RENUM` or line editor in `basic`: you write programs in `edit`.
- `SUB`, `FUNCTION` and `DECLARE` are not allowed at the immediate prompt.
- `RANDOMIZE` without a number does not ask for one.
- `REDIM` or `ERASE` of an array while one of its elements is being used in
  the same statement gives `Array in use`, which QBasic does not have.
- A `NEXT` that belongs to a loop of the caller, written inside a `GOSUB`
  routine, is an error (`NEXT without FOR`).
- A `SHARED` statement inside a `SUB` gives `Advanced feature unavailable`
  (use `DIM SHARED` at the top), and passing `a()` of an array that was never
  used gives `Array not defined`.
- `RUN` works only at the immediate prompt; inside a program it gives
  `Advanced feature unavailable`.
- `DATE$` and `TIME$` do not exist: the board has no clock.
- Any QBasic statement or function that `basic` does not have stops the
  program, before it runs, with `Advanced feature unavailable`. The few
  things that are accepted without an effect are listed above: `WIDTH`,
  `DECLARE` lines and the cursor arguments of `LOCATE`. Two values are
  clamped instead of refused: `TAB(n)` beyond column 80 goes to column 80,
  and `SPC(n)` gives at most 255 spaces. These are not supported: line
  numbers; graphics and sound (`SCREEN`, `PSET`, `LINE`, `CIRCLE`, `PAINT`,
  `DRAW`, `SOUND`, `PLAY`, ...); `TYPE` and `STATIC`, `COMMON`, `CHAIN`;
  `DEF FN`; `ON ... GOTO`/`GOSUB`; `ON ERROR` and `RESUME`; `PRINT USING`;
  the `MID$` statement; `PEEK` and `POKE`; random-access and binary files
  (`GET`, `PUT`, `FIELD`, `LOF`, `SEEK`); `ENVIRON`, `FRE` and the rest of
  QBasic's hardware and event statements. Networking statements may come later.

---

## 7. The toolchain

### How it fits together

```
 file.c --cc--> file.s --as--> file.o --+
 file.s ------------as-------> file.o --+--ld--> program.bin
 lib.a (made by ar from .o files) ------+
```

`cc` drives the whole chain: it compiles each `.c` to assembly itself, runs
`as` on every assembly file, then `ld` on all the objects and libraries. You
normally only type `cc`. The tools are separate programs in `/bin`;
intermediate files go to `/tmp` and are removed afterwards, also after an
error. Every tool is silent when it succeeds.

### cc

```
cc [-o out] [-S|-c] [-mshort-data] [-I dir] [-D name[=v]] [-L dir] [-l name] file...
```

| Option | Meaning |
|---|---|
| `-o out` | the output name (default `a.bin`) |
| `-c` | stop at object files: `file.o` for each input |
| `-S` | stop at assembly: `file.s` for each `.c` input |
| `-I dir` | another directory for `#include "..."` |
| `-D name[=v]` | define a macro (value 1 by default) |
| `-L dir` | another directory to search for libraries |
| `-l name` | link the library `libname.a` (searched in the `-L` directories, the current directory, then `/lib`) |
| `-mshort-data` | with `-S`/`-c`: use the short data references (see [Data references](#data-references)) |

Inputs are recognized by suffix: `.c` (compiled), `.s` (assembled), `.o` and
`.a` (linked). With `-c` or `-S` and exactly one input, `-o` names the output;
otherwise each output takes the input's name with the new suffix, in the
current directory.

Examples:

```
/src> cc hello.c                               one file, output a.bin
/src> cc -o hello.bin hello.c                  one file, named output
/src> cc -o counter.bin main.c count.c         two C files in one step
/src> cc -c count.c                            compile only: count.o
/src> cc -o counter.bin main.c count.o         ... and link it later
/src> cc -o useadd.bin useadd.c add3.s         C and assembly together
/src> cc -S sq.c                               look at the generated assembly: sq.s
/src> cc -D DEBUG -o dbg.bin prog.c            with a macro defined
```

### Programs in several files

Every global function and variable is visible to the other files of a
program. Declare what another file defines with `extern` and a prototype:

```c
/* count.c */
int counter;
int bump(int n) { counter = counter + n; return counter; }
```

```c
/* main.c */
extern int counter;
int bump(int n);
int main() { bump(2); bump(3); printf("counter = %d\n", counter); return 0; }
```

```
/src> cc -o counter.bin main.c count.c
/src> counter
counter = 5
```

### Libraries

A library is an archive of object files. `ld` links only the members the
program needs: a member is linked when it defines a symbol that is still
undefined.

```c
/* sq.c */
int square(int x) { return x * x; }
```

```c
/* cube.c */
int cube(int x) { return x * x * x; }
```

```c
/* p.c */
int square(int x);
int main() { printf("%d\n", square(7)); return 0; }
```

```
/src> cc -c sq.c cube.c
/src> ar rc libm2.a sq.o cube.o
/src> cc -o p.bin p.c -L . -l m2
/src> p
49
```

Only `sq.o` ends up in `p.bin`. A member is linked whole, so put one
function (or a group that is always used together) in each source file of
a library. Libraries installed in `/lib` are found without `-L`.

### Mixing C and assembly

C and assembly call each other with one convention: arguments are pushed on
the stack from first to last (so the first one is deepest), the caller
removes them, and the result is in `r0`. Assembly code must keep `r7`, `r8`
and `r9`, and save `r4` when it uses it.

```asm
@ add3.s: int add3(int a, int b, int c)
        .syntax unified
        .thumb
        .text
        .global add3
add3:
        ldr     r0, [sp, #8]          @ a
        ldr     r1, [sp, #4]          @ b
        ldr     r2, [sp]              @ c
        add     r0, r0, r1
        add     r0, r0, r2
        bx      lr
```

```c
/* useadd.c */
int add3(int a, int b, int c);
int main() { printf("%d\n", add3(1, 20, 300)); return 0; }
```

```
/src> cc -o useadd.bin useadd.c add3.s
/src> useadd
321
```

`tests/toolchain/` in the repository has more examples: assembly calling C,
assembly reading C's globals, and a callback from assembly into C.

### Error messages

Each tool reports its own errors, with the file and line where there is one,
and `cc` stops with that tool's exit code:

```
/src> cc bad.c
bad.c:1: error: bad expression near token 59
/src> cc undef.c
ld: undefined symbol: nothere (in /tmp/cc29ae_1_undef.o)
/src> as bad.s
bad.s:1: error: immediate out of range
```

The temporary object names end with the source's name (`..._undef.o` came
from `undef.c`).

### as

```
as [-o out.o] file.s
```

Assembles one file (default output: the same name with `.o`). See
[Assembly language](#8-assembly-language).

```
/src> as -o add3.o add3.s
```

### ld

```
ld [-o out.bin] [-e entry] file.o... lib.a...
```

Links objects and libraries into a program (default `a.bin`). The entry
point is `main` unless `-e` names another function. Objects are always
linked; library members only when needed.

```
/src> ld -o useadd.bin useadd.o add3.o
/src> ld -o p.bin p.o libm2.a
```

### ar

```
ar rc lib.a file.o...       create a library, or replace and add members
ar t lib.a                  list the members
ar x lib.a [member...]      extract members (all when none are named)
```

```
/src> ar rc libm2.a sq.o cube.o
/src> ar t libm2.a
sq.o
cube.o
/src> ar x libm2.a cube.o
```

### nm

```
nm file.o|lib.a...
```

Lists symbols: `T` code, `D` data, `A` absolute values, `U` undefined (used
here, defined elsewhere). Lower case letters are local symbols.

```
/src> nm count.o
00000000 T bump
00000000 D counter
/src> nm libm2.a
libm2.a(sq.o):
00000000 T square
libm2.a(cube.o):
00000000 T cube
```

### objdump

```
objdump file.o|lib.a|program.bin...
```

Shows the header, symbols, relocations, a disassembly of the code and the
data. The disassembly is the same text GNU objdump prints.

```
/src> objdump add3.o
add3.o: T2DOS object, text 12 bytes, data 0 bytes, 1 symbols, 0 relocations
SYMBOLS:
00000000 T add3
RELOCATIONS:
DISASSEMBLY:
       0:	9802      	ldr	r0, [sp, #8]
       2:	9901      	ldr	r1, [sp, #4]
       4:	9a00      	ldr	r2, [sp, #0]
       6:	4408      	add	r0, r1
       8:	4410      	add	r0, r2
       a:	4770      	bx	lr
DATA:
```

### Data references

Programs reach their global variables through register `r9`, which holds the
start of the program's data, plus the variable's offset. `cc` picks the
shortest instruction form that reaches the offset: 4 bytes below 4 KB,
8 bytes below 64 KB, 12 bytes above that.

When `cc` builds the whole program in one step, it knows where every file's
data will end up and always picks the shortest form. With `-S` or `-c` it
cannot know what will come before the file in the link, so it uses the
12-byte form, which always works. `-mshort-data` tells it to compile as if
the file's data came first in the program, which gives the short forms and
is right for the first object of a program. If that turns out wrong, `ld`
stops with a clear message instead of making a broken program:

```
ld: data offset of counter (main.o) is 5000, too far for addw
```

References to `extern` variables always use the 12-byte form.

---

## 8. Assembly language

`as` takes the GNU unified syntax for Thumb-2, the subset that compiled C and
hand-written code need: the ARMv8-M mainline integer instructions with their
narrow and wide forms (`.n`, `.w`), IT blocks, and the single precision FPU
instructions (`vadd.f32`, `vldr`, `vmov`, `vcvt`, `vpush`, ...). The
encodings equal GNU `as` with `-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16`,
and `as` refuses what GNU refuses (for example `r13 not allowed here` where
ARMv8-M makes `sp` unpredictable). `add rd, pc, #imm` is `adr`.

### Directives

`.syntax unified`, `.thumb`, `.thumb_func`, `.text`, `.data`,
`.global`/`.globl`, `.extern`, `.equ`/`.set`, `.byte`, `.hword`/`.short`,
`.word`, `.ascii`, `.asciz`/`.string`, `.space`/`.zero`, `.align` and
`.balign` (up to 4 bytes), and `.t2init`. Comments: `@`, `//`, `/* */`, and
`#` at the start of a line. Labels starting with `.L` stay inside the file.

### The code model

Code is position independent: there are no absolute addresses. `r9` holds
the data base and `r8` the kernel's entry, and the value of a data symbol is
its offset in the data, so data lives at `r9 + offset`:

```asm
        addw    r0, r9, #counter          @ r0 = &counter, offsets 0 to 4095
        movw.16 r1, #counter              @ offsets up to 65535
        add.w   r0, r9, r1
        movw    r1, #:lower16:counter     @ any offset
        movt    r1, #:upper16:counter
        add.w   r0, r9, r1
        ldr     r2, [r0]                  @ r2 = counter
```

A complete example, a counter that assembly owns and C can call and read:

```asm
@ ids.s
        .syntax unified
        .thumb
        .global next_id, last_id
        .text
next_id:                                  @ int next_id(void)
        movw    r1, #:lower16:last_id
        movt    r1, #:upper16:last_id
        add.w   r1, r9, r1
        ldr     r0, [r1]
        adds    r0, r0, #1
        str     r0, [r1]
        bx      lr
        .data
        .align  2
last_id: .word  0
```

```c
/* useids.c */
int next_id();
extern int last_id;
int main() { next_id(); next_id(); printf("%d %d\n", next_id(), last_id); return 0; }
```

```
/src> cc -o useids.bin useids.c ids.s
/src> useids
3 3
```

`b`, `b<cond>` and `bl` to functions in other files are filled in by `ld`.
`.word label` in `.data` stores the label's offset, not an address, so a
table of function pointers in `.data` does not work; take function addresses
at run time with `fnaddr`.

### T2DOS pseudo-instructions

These exist only in T2DOS's `as`:

- `fnaddr rd, sym`: `rd` = the address of the function `sym`, computed PC
  relative (10 bytes: `movw`, `movt`, `add rd, pc`). Use it to pass a
  function as a callback.
- `movw.16 rd, #sym`: a `movw` of a data offset that must fit in 16 bits.
- `.t2init sym`: `sym` is a routine that runs before `main`. `ld` calls each
  one in link order; `cc` uses it for global initializers that need code.

### Symbol operands and limits

`#sym` and `#sym+n` (with `addw` and `movw.16`), `#:lower16:sym` and
`#:upper16:sym` (with `movw` and `movt`), plain `sym` for branches and
`fnaddr`.

Not supported: literal pools (`ldr r0, =x`, `ldr r0, label`), writeback and
post-indexed addressing (`[r0, #4]!`, `[r0], #4`), double precision, FPU
instructions inside IT blocks, and macros (`.macro`, `.rept`).

---

## 9. System reference

### Programs and memory

- A program's code is loaded into SRAM; its data, a 256 KB stack and its
  `malloc` memory are in PSRAM. When it exits, everything is freed and files
  it left open are closed. `malloc` returns zeroed memory.
- Programs can run other programs with `exec`, nested up to 4 deep (the shell
  counts as one).
- Registers: `r7` is the frame pointer, `r8` the kernel's entry and `r9` the
  data base. Arguments go on the stack, first argument deepest; the result
  is in `r0`.
- The current directory lives in the kernel and is shared by all programs.
- Background jobs: up to 4 programs run beside the foreground program, each
  with its own code, data, stack and kernel stack. Tasks switch cooperatively,
  only in the calls listed under
  [Writing a background program](#writing-a-background-program), and never in
  the middle of a file, flash or WiFi operation. A job inherits the current
  directory when it starts. Memory a job allocates is freed when it ends.
  A program that starts another with `exec` inside a job should keep the
  nesting shallow: each job has only an 8 KB kernel stack.
- System calls run on a 16 KB kernel stack in SRAM, because PSRAM cannot be
  reached while the flash is being written.

### System calls

The library functions in [The library](#the-library) are system calls. Each
has a fixed number in `sys.h`, grouped: process 0+, console 16+, files 32+,
memory and strings 64+, network 80+, board 96+, maths 112 to 130, and
131+ free for user extensions. The kernel sees `sys(num, nargs, argv)`, where `argv[nargs-1]`
is the first argument.

#### Maths

| Call | Number | Call | Number | Call | Number |
|---|---|---|---|---|---|
| `sin(x)` | 112 | `sinh(x)` | 119 | `sqrt(x)` | 126 |
| `cos(x)` | 113 | `cosh(x)` | 120 | `fabs(x)` | 127 |
| `tan(x)` | 114 | `tanh(x)` | 121 | `floor(x)` | 128 |
| `asin(x)` | 115 | `exp(x)` | 122 | `ceil(x)` | 129 |
| `acos(x)` | 116 | `log(x)` | 123 | `fmod(x, y)` | 130 |
| `atan(x)` | 117 | `log10(x)` | 124 | | |
| `atan2(y, x)` | 118 | `pow(x, y)` | 125 | | |

Each argument word and the result in `r0` is a `float`'s 32-bit pattern;
the kernel computes with `sinf`, `cosf`, ..., `fmodf`.

#### Jobs and pipes

| Call | Number | Meaning |
|---|---|---|
| `spawn(path, argc, argv)` | 5 | start `path` as a background job; returns its number 1 to `MAX_JOBS` (4), or a `SPAWN_` error |
| `yield()` | 6 | let another runnable task run; returns 1 if one did, else 0 |
| `jobs(buf, max)` | 7 | fill `buf` (an array of `struct job_t`) with the job table, return how many entries it wrote; the slots of jobs reported as done are freed |
| `kill(job)` | 8 | end a running job (its exit code becomes `JOB_KILLED`, -9); returns 0 once it has ended, -1 if there is no such running job |
| `mkpipe(name)` | 47 | create `/pipe/name` (the name without the prefix, up to 31 characters, no `/`), owned by the caller; returns 0, or -1 if it exists, the table is full or the name is invalid |

```c
struct job_t { int id; int state; int exit_code; char name[32]; };
```

`state` is `JOB_RUNNING` (1) or `JOB_DONE` (2); `exit_code` is the program's
exit code once it is done. `spawn` returns these errors:

| Code | Name | Cause |
|---|---|---|
| -1 | `SPAWN_NOTFOUND` | no such file |
| -2 | `SPAWN_BADPROG` | not a T2DOS program |
| -3 | `SPAWN_NOMEM` | not enough memory |
| -4 | `SPAWN_FULL` | 4 jobs already exist |
| -5 | `SPAWN_UNSUPPORTED` | this system cannot run jobs (the Linux test harness `host_linux`) |

`stat` on a `/pipe/` file fills `size` with the bytes buffered and `mtime`
with the number of complete messages waiting. The behaviour of pipes is
described in [Named pipes](#named-pipes).

### Network protocol

`fileserver.py` handles one request per connection: `GET name`,
`PUT name size` followed by the bytes, or `LS [dir]`. It answers
`OK [size]` and the data, or `ERR text`. Socket fds start at 100.

### File formats

- **Program (`.bin`):** a 20-byte header (`T2CC`, code size, data size, 0,
  entry offset, all 32-bit little endian), then the code, then the data.
- **Object (`.o`, `T2OB`) and library (`.a`, `T2AR`):** defined in
  `lib/t2obj.h`. An object holds code, data, symbols, relocations and a
  string table; a library holds a symbol index and the member objects.

---

## 10. Building T2DOS from source

### Repository layout

```
sys.h             the system call ABI, shared by the kernel, the compiler and programs
kernel/kernel.ino the kernel (arduino-pico; SD card on SPI, LittleFS fallback)
compiler/cc.c     the compiler and driver; compiler/tests/ has the language tests
assembler/as.c    the assembler
linker/ld.c       the linker
binutils/         ar, nm, objdump
lib/              t2obj.h and t2obj.c (object formats), net.c (network helpers)
basic/            the BASIC interpreter (several files; pcsys.c is the shim for the PC test build)
examples/basic/   example BASIC programs; make copies them to root/src/basic/
userland/         sh, ls, cat, cp, mv, rm, mkdir, rmdir, echo, hexdump, free, put,
                  write, edit, blink, wifi, get, send, rls, nc, demosrv
tests/            assembler, linker, object and toolchain tests; tests/jobs/ has the
                  programs for the background job checks
tools/            board.py (board access), fileserver.py, the check scripts
host_linux.c      the kernel's system calls on Linux, to run programs under qemu-arm
root/             the filesystem image that make fills: /bin, /lib, /src
```

### Make targets

```sh
make            # the PC cross compiler (compiler/cc), host tools (build/host/) and root/bin/*.bin
make check      # the PC test suite
make ref        # the reference compiler for the byte-identity check
make host       # host_linux (needs arm-linux-gnueabihf-gcc)
make clean
```

The PC build of `cc` runs `build/host/as` and `build/host/ld` (or those in
`$T2_TOOLS`), so the same commands work on the PC:

```sh
compiler/cc -I root/lib -o hello.bin hello.c
```

### Checks

`make check` runs these; each can also run on its own:

- `tools/check_asm.py`: `as` against GNU `as` (`--corpus` reassembles every
  program, `--diff N` compares N random instructions, `--objdump-corpus`
  compares `objdump` with GNU objdump).
- `tools/check_link.py --pc`: the linker tests.
- `tools/check_ref.py`: every single-file program built by today's toolchain
  is byte-identical to the reference compiler's output.
- `tools/check_toolchain.py --pc`: the multi-file, library and error cases.
- `tools/check_basic.py --pc`: the BASIC cases of `tests/basic/` (each
  `NAME.bas` with its expected `NAME.out`) on a native build of the
  interpreter, with a memory-leak check.

With the board connected (and no `screen` on the port):

```sh
python3 tools/check_link.py --board
python3 tools/check_toolchain.py --board      # builds and runs the cases with cc on the board
python3 tools/check_toolchain.py --selfhost   # the board rebuilds the toolchain identically
python3 tools/check_jobs.py --board           # background jobs and named pipes on the board
python3 tools/check_basic.py --board          # the BASIC cases on the board
```

`check_jobs.py` uploads the programs of `tests/jobs/`, runs every case
(spawn, kill, redirection, `exec` inside a job, pipes) and checks that the
free PSRAM is the same afterwards. `check_ref.py` lists `userland/sh.c` and
`userland/demosrv.c` as excluded ("newer builtins"): they use calls that the
reference compiler does not know, so they cannot be compared with it.

### Self-hosting

`cc.c`, `as.c`, `ld.c` and the binutils are written in the C subset that
`cc` compiles, so they build on the board:

```
/src> cc -I /lib -o cc2.bin cc.c
```

`cc2.bin` is byte-identical to the PC build; compiling `cc.c` takes about
22 s on the board. Because of this, the toolchain's sources may only use
features that the previous `cc` already supports.

---

## 11. Status

### Verified on the board (CPico 2350, flash filesystem)

- Boot into the shell from flash; files survive reset and reflashing.
- The shell, redirection, scripts, line editing and history; the file and
  system commands; PSRAM is fully returned after every program.
- Background jobs: `&`, `jobs`, `kill`, the exit reports, the limit of 4,
  output to a file, and cooperative switching between the shell and jobs.
  Named pipes between jobs and the shell, `demosrv` in pipe mode, and
  Ctrl-C ending a wait on a pipe. `tools/check_jobs.py --board` runs these.
- `edit` through `screen`, including cut, copy, paste and the mark.
- The toolchain: every case in `tests/toolchain/` (C calling assembly,
  assembly calling C, globals shared with `extern`, symbols named like
  registers, a library of which only the needed member is linked, big data
  before and after other files) builds with `cc` on the board, prints what
  it should, and equals the PC build. Errors stop `cc` with the tool's
  message and leave no files in `/tmp`.
- Self-hosting: `cc` on the board rebuilds `cc`, `as`, `ld`, `ar`, `nm` and
  `objdump` byte-identical to the PC builds, and that `cc` compiles test4.c
  to test12.c with the same output as gcc.
- The assembler matches GNU `as` byte for byte on about 120,000 instructions
  from the real programs, and `objdump` matches GNU objdump's text.
- BASIC: every case of `tests/basic/` runs on the board with the expected
  output (`tools/check_basic.py --board`), and PSRAM is fully returned after
  each one. The five examples of `/src/basic/` run on the board, and the
  outputs in [Programming in BASIC](#6-programming-in-basic) are copied from
  those runs. This includes Ctrl-C in a running program and in `INPUT`, keys
  typed while a program prints, deep recursion (`Out of stack space`), files
  and pipes, `basic` as a background job, immediate mode and `^R` in `edit`.
  The PC build of the interpreter also passes the same cases under
  AddressSanitizer without a leak.

### BASIC: speed and size

- Speed: `examples/basic/bench.bas` runs `FOR i = 1 TO 100000: s = s + i * 2:
  NEXT`, which is 200,000 statements (the assignment and `NEXT` on every
  pass), and takes 1.06 s on the board: about 188,000 statements per second,
  the same in every run. That is above the 100,000 per second the design
  asked for. It is the figure for a loop of simple arithmetic; calls, string
  work and output are slower.
- Size: `basic.bin` is 179,848 bytes (it runs from SRAM, of which about 370 KB
  are free). The design hoped for well below 100 KB; the language turned out
  larger, and the size has not been tuned. Every run loads it into SRAM, so
  two BASIC background jobs fit and a third does not, and `edit`, `^R` and a
  program that runs `SHELL "cc ..."` can run out of program memory.

### Not yet verified, known gaps

- SD card: not tested yet (the kernel expects it on SPI0 with CS on GP17).
- Network tools: tested only under qemu, not on real hardware. This includes
  `demosrv -p`, the TCP mode of the reference server.
- No preemption. A job that never calls a waiting function freezes the board
  and only a power cycle helps. The CPU is never idle while jobs exist: they
  and the shell poll in turn.
- At most 4 jobs and 8 named pipes.
- A job has an 8 KB kernel stack, so `exec` nested deeply inside a job can
  overflow it.
- The recovery prompt (after `exit` from the shell) pauses the jobs until the
  shell runs again.
- `cat` of a reply pipe waits until a reply arrives; press Ctrl-C to give up.
- The shell has no `|` pipes. `host_linux.c` does not run jobs (`spawn`
  returns `SPAWN_UNSUPPORTED`).
- Uploading a whole `root/` tree to the flash filesystem is manual (`put` per
  file).
- No `static`: all globals of a program share one namespace.
- `ld` links a library member whole, not single functions.
- BASIC: a `DOUBLE` is a 32-bit float, an `INTEGER` is 32 bits, and numbers
  have about seven digits. There is no graphics, sound, `PRINT USING`,
  `ON ERROR`, user types or random-access files (the program stops with
  `Advanced feature unavailable`). Without a real-time clock there is no
  `DATE$` or `TIME$`. The special-key codes of `INKEY$` depend on the
  terminal's escape sequences. Whether the E form of very small numbers
  such as 1/30 matches QBasic's in every case is not verified (no QBasic was
  available to compare with).
- `ld -o X X` (the output also named as an input) deletes `X` when the link
  fails.
- `host_linux.c` does not yet free or zero a program's `malloc` memory, and
  its float formatting is untested.
