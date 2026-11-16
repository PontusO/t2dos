# Network filesystem (/mnt/fs)

## Why

Projects live on the PC, but working on them from the board means `get`,
`send` or `rcp` after every change. With a network filesystem a directory on
the PC shows up on the board as `/mnt/fs`, and every T2DOS program (`ls`,
`edit`, `cc`, `cp`, running a `.bin`) works on it like any other directory.
The first use: edit and compile a project on the board while its files stay
on the PC. Slow is accepted ("slow yes but cool"). Caching will be tuned
later for different uses, maybe adjusting itself, so the first version must
not lock in one caching method.

## Goals

- `mount host[:port] [remote dir]` makes the PC directory appear at `/mnt/fs`;
  `mount` alone shows it, `umount` removes it. The mount survives a reboot.
- All file syscalls work under `/mnt/fs`: open/read/write/seek/close (read,
  write and append modes), stat, unlink, rename, mkdir, rmdir, opendir/readdir,
  chdir, and loading a program from there.
- `cd /mnt/fs/myproj; edit main.c; cc -o main.bin main.c; main.bin` works.
- Works on the board and under qemu with `host_linux`, so it can be tested on
  the PC.

## Non-goals (first version)

- Caching beyond the blocks of open files, read-ahead, or caching rules per
  use; the design leaves room for them (see Cache).
- More than one mount, or mount points other than `/mnt/fs`.
- Locking or merging when the PC and the board change the same file: the last
  write wins.
- Users, permissions, times, links.
- Keeping other tasks running during a network call (the kernel is not
  preempted, see Constraints).

## Anti-goals

- A broken connection must never hang the board or lose data silently: file
  calls fail with -1 and the program reports it.
- `/mnt/fs` must not slow down or change anything outside `/mnt/fs`.
- Nothing may leave the served directory on the PC (same `safe()` rule as
  today).

## Constraints

- Kernel code is never preempted: every request blocks all tasks for one
  round trip (about 15-30 ms on the board). Do not add a switch point inside a
  WiFi call.
- PSRAM shares the QSPI bus with flash: never touch PSRAM while flash is
  written (`k_rename` copies through an SRAM buffer; keep that rule).
- 16 file slots (`MAX_FD`), 4 open directories (`MAX_DIR`), paths up to 160
  bytes after `resolve()`.
- arduino-pico 6.3.2 with ESP-Hosted: a connect within a few ms of a close
  stalls for 5 s; `SYS_connect` already keeps 10 ms after a close, and the
  filesystem's own connection must keep the same gap.
- Syscall changes go in all four places (`sys.h`, `cc.c` builtins,
  `kernel.ino`, `host_linux.c`); numbers are frozen.

## Approaches

- **A. Whole files.** Opening fetches the file with `GET` into PSRAM, closing
  a changed file sends it back with `PUT`. Fewest round trips and almost no
  server change, but it builds one caching method into the design.
- **B. Byte ranges plus a block cache on the board (chosen).** The server
  answers requests for parts of files on one long-lived connection; on the
  board a cache of 4 KB blocks sits between the file calls and the network.
  Changing how it caches later touches only the cache layer.
- **C. 9P with an existing PC server (diod).** Standard, no server to write,
  but a larger, stricter protocol on the board, a separate install on the PC,
  and no reuse of `fileserver.py` or `lib/net.c`.

B was picked because it is the only one that keeps the caching open, which is
the stated next step.

## Protocol (tools/fileserver.py)

A connection whose first line is `SESSION` gets `OK` and then serves
requests until the client closes it; the existing one-request connections
(`GET`, `PUT`, `LS`, `MKDIR`) are unchanged, so `get`, `send`, `rls`, `rcp`
keep working against the same server. Numbers come first and the name last,
so names may hold spaces. Names are relative to the server root and pass
`safe()`.

```
STAT name                  -> OK f size | OK d | ERR text
READ offset length name    -> OK n\n + n bytes   (n < length only at end of file)
WRITE offset length name\n + length bytes -> OK  (creates the file; grows it with zeros)
TRUNC size name            -> OK               (creates the file if missing)
LS name                    -> OK size\n + listing (as today)
MKDIR name / RM name / RMDIR name -> OK | ERR text   (RMDIR only of an empty directory)
RENAME name\n + newname\n  -> OK | ERR text
```

`READ` and `WRITE` lengths are at most 4096 (one cache block). Every request
except `RENAME` is safe to repeat.

## Board side

**Shared core.** The protocol client and the cache are one plain C file
(`kernel/netfs.c`), used by the kernel through a thin `fs::FSImpl` wrapper
(`kernel/netfs.cpp`, modelled on `ramfs.cpp`) and included directly by
`host_linux.c`. The core reaches the network through a small transport
interface (connect, send, recv with timeout, close) that each side provides:
`WiFiClient` on the board, POSIX sockets in `host_linux`.

**Routing.** `fs_for()` sends `/mnt/fs` and `/mnt/fs/...` to the network
filesystem when one is mounted. `host_linux` routes its file syscalls the
same way for those paths. The remote name is the remote directory plus the
rest of the path.

**Connection.** Opened on first use, not at mount or boot, so booting
without WiFi or with the PC off is fine. This connection uses its own
timeouts: 2 s to connect (not the library's 15 s) and 5 s for a reply. When a
request fails (connect error, closed connection, no reply in time), the core
closes the connection, waits 10 ms, reconnects and repeats the request, up to
3 tries in all (the `net_call()` rule). After that the file call returns -1
and the mount is marked **down**: while down, calls under `/mnt/fs` fail at
once, and the core tries one new connection at most every 10 s, or at once
when `mount` is run again; a working connection clears the down state. So a
switched-off PC freezes the board once, for about 6 s, and then gives fast
errors. A rename that fails after the server may have done it is reported as
failed. (Agreed with the user.)

**Cache.** Per open file: its size (from `STAT` at open) and the 4 KB blocks
read or written so far, kept in PSRAM like `/tmp`'s blocks and limited by the
same free-PSRAM margin (`TMP_MARGIN`). Reads fill missing blocks with `READ`;
writes change blocks in the cache and mark them dirty; a clean block may be
dropped when room is needed, a dirty one is kept until it is saved (when
PSRAM runs short, `write()` returns less, like `/tmp`). Changes are saved on
close (and on the wrapper's internal `flush()`; there is no new flush
syscall). On close the file's blocks are dropped, so the next open sees
changes made on the PC.

**Saving safely (agreed with the user).** A save must never be lost silently
or leave the PC's file half-written:
- A file opened with `OPEN_WRITE` is not truncated on the server at open. At
  close its whole content (all blocks are in the cache, dirty) is written to
  a temporary name in the same remote directory (`.NAME.t2tmp`, `TRUNC` then
  `WRITE`s) and then renamed over the real name (`RENAME`, which replaces an
  existing file). If the connection drops part way, the old file stays as it
  was.
- A file opened for reading and writing in place, or for append, sends its
  dirty blocks with `WRITE` at their offsets (and `TRUNC` if its size
  shrank); the existing content is never cut short.
- A save is retried as a whole up to 3 times (a new connection each time,
  10 ms apart; a repeated temp-file save starts again from writing the temp
  file, so a lost `RENAME` reply is harmless). Requests inside one save try
  are not retried on their own, so the waiting stays bounded.
- If all 3 tries fail, `close()` returns -1. Today `SYS_close` always returns
  0: it changes in both kernels to return -1 for a failed save (files on SD,
  flash and `/tmp` keep returning 0). `edit`, `cp`, `mv`, `write`, shell
  output redirection and `cc`/`as`/`ld`/`ar` (their output files) check it
  and print an error; for the toolchain this is a source change within the
  subset, rebuilt and checked byte-identical by self-hosting as usual. The
  cached data is dropped after a failed close; the program reports it. The cache's decisions
(what to keep, when to write, read-ahead) are behind one small set of
functions, so later policies replace them without touching the protocol,
the server or the wrapper.

**Directories.** `opendir` fetches the `LS` listing once and serves `readdir`
from it. `stat` and the `is_dir` check use `STAT`. `/mnt` and `/mnt/fs` are
created as plain directories on the main filesystem when mounting, so
`ls /` shows `mnt`.

**Mount syscall and command.** New `SYS_mount = 48` (files group):
`int mount(char *host, int port, char *dir)`; host `""` unmounts. It checks
`STAT dir` on the server (answer `OK d`), returns 0, or -1 when the server
cannot be reached or dir is not a directory. Unmounting fails (-1) while a
file under `/mnt/fs` is open. `userland/mount.c` and `umount` (same program
or a second one, builder's choice) call it and keep the setting in
`/etc/mount` (`host[:port]` and `dir` lines, like `/etc/remote`); the kernel
reads `/etc/mount` in `setup()` and mounts without checking (the check
happens on first use). `mount` alone prints `host:port dir` or
`nothing mounted`.

## Tests and docs

- `tools/check_netfs.py --pc` (qemu + `host_linux` + `fileserver.py` on a
  random port): mount and the `mount` output; `ls`, `cat`, `cp` both ways;
  write, append, seek-and-overwrite, truncate on reopen; mkdir, rmdir, rm,
  rename (inside `/mnt/fs` and between `/mnt/fs` and the SD/`/tmp`); `cd`
  into it; `cc -o x.bin x.c` with an `#include` in `/mnt/fs`, then running
  `x.bin` from there; files larger than one block and not a block multiple;
  names with spaces; the server killed mid-run (calls fail with -1, no hang)
  and restarted (next call works); the `FLAKY` proxy from `check_net.py`
  (every request still works); escapes outside the root refused.
- `fileserver.py` session requests are covered by raw-socket checks in the
  same script, including the `safe()` refusals for each new request.
- Saving: a save whose connection breaks on the first and second try
  succeeds on the third; one that fails 3 times makes `close()` return -1,
  `edit`/`cp` print an error, and the PC's file is unchanged (temp-file save).
- Down state: with the server gone, the first call fails after its tries and
  the following ones fail at once; after the server is back and 10 s have
  passed (or `mount` is run again), calls work.
- Board: the same script with `--board` where it can (`board.py`), plus
  compiling a multi-file program from `/mnt/fs` and timing it against the SD
  card.
- `check_net.py`, `make check` and `check_ref` stay green; `mount` joins
  `NEWER_BUILTINS` in `check_ref.py`.
- README: a `#### mount` section under Network, the session requests in the
  protocol section, the repository layout.

## Left to the builder

(`flush` above means the `FSImpl` wrapper's internal flush; no new syscall.)


Exact message wording, the transport interface's names, buffer sizes, how
`host_linux` intercepts its file syscalls for `/mnt/fs`, the `LS` parsing
(the `rcp` parser is the model), and whether `umount` is its own program.
