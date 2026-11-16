# Several network mounts with free names

Builds on `2026-11-16-netfs-design.md` (the single `/mnt/fs` mount). Everything there
still holds unless this document changes it.

## Why

One mount at a fixed `/mnt/fs` means one PC directory at a time. The user wants to
connect to several file servers (or several directories of one server) at once, and
to choose the name each one appears under.

## Goals

- Up to 4 mounts at the same time, each at `/mnt/NAME`, each to its own
  `host:port` and remote directory.
- `mount host[:port] [dir] [name]`: `name` defaults to `fs`, so every existing use
  (`mount pc`, `mount pc proj`) works as before. `name` may also be written as
  `/mnt/name`.
- `mount` alone lists every mount, one per line: `host[:port] dir /mnt/name`.
- `umount [name]` removes one mount (`fs` when no name is given).
- All mounts survive a reboot through `/etc/mount`.
- Every file syscall works on every mount, including rename and copy between two
  mounts.
- Each mount has its own connection, retries and down state: a switched-off PC
  only makes its own mount fail; the others keep working.

## Non-goals

- Mount points outside `/mnt`, or nested (`/mnt/a/b`).
- More than 4 mounts.
- Renaming a directory from one mount to another (a file is copied, a directory is
  refused, the same rule as SD to `/mnt/fs` today).

## Anti-goals

- No change for anything outside `/mnt/NAME` paths, and no change on the PC side:
  `fileserver.py` and its protocol stay as they are.
- Old binaries keep working: `SYS_mount = 48` keeps its meaning.

## Constraints

- lwIP on the board has 5 TCP connections in all (`MEMP_NUM_TCP_PCB` 5 in
  arduino-pico 6.3.2, built into the core, so it cannot be raised from the sketch).
  Programs have 4 socket slots and today netfs takes the fifth. Each mount in use
  holds one connection. Connections are opened on first use (as today), so
  mounts that are not used cost nothing, but with programs holding several
  sockets and 2 or more mounts busy, a connect can fail. It then behaves like an
  unreachable server: the call fails after its tries and that mount is down for
  10 s. This is documented in the README rather than worked around.
- The 10 ms gap after any close before the next connect is shared by all
  connections (one `sock_closed_at`, as today).
- Paths stay at 160 bytes; remote names at 200.
- Syscall numbers are frozen; the file group has 49 free.

## Approaches

- **A. A table of mounts inside the one netfs core (chosen).** `netfs.c` keeps
  an array of 4 mounts (name, host, port, remote dir, connection, down state);
  files and listings remember their mount. The cache stays one shared pool, the
  wrapper stays one `FSImpl`, routing stays `nfs_path()`.
- **B. One core per mount.** The C core uses file-level globals, so this means
  either copying it or turning every function into one that takes a context
  pointer. More churn than A for the same result.
- **C. One shared connection, switched to whichever server is asked.** Saves
  lwIP connections, but `cc` reading from one mount and writing to another
  would reconnect on almost every request (10 ms gap plus connect each time).

## Design

**Core (`kernel/netfs.c`).** `struct mount { char name[16], host[64], dir[NAME];
int port, on, conn, down; unsigned down_at; }`, `NMOUNTS 4`. The connection
code (`open_conn`, `attempt`, `request`, `drop_conn`, the recv helpers) takes the
mount. `nfs_path(abs)` is true for `/mnt/NAME` or below for any mounted NAME;
`rname()` uses that mount's dir and the rest of the path after `/mnt/NAME`. Each
`nfile` and `ndir` keeps its mount index. `busy()` checks one mount.
The transport's `nt_connect` returns a handle per connection (the board
transport gets one `WiFiClient` per mount, `host_linux` already uses fds).

New core calls:
- `int nfs_mountat(const char *name, const char *host, int port, const char *dir, int check)`:
  host `""` unmounts NAME. Mounting a NAME that is already mounted replaces it
  (refused with -3 while its files are open, as today). Returns 0, -1
  unreachable, -2 not a directory, -3 busy, -4 no free mount (4 in use), -5 bad
  name.
- `nfs_mount(host, port, dir, check)` stays, as `nfs_mountat("fs", ...)`.
- `int nfs_same(const char *a, const char *b)`: 1 if both paths are on the same
  mount. Used by the rename paths.

**Names.** 1 to 15 characters from letters, digits, `_`, `-` and `.`, not `.` or
`..`. `/mnt/name` is accepted and stripped by `mount`/`umount`.

**Syscall.** New `SYS_mountat = 49`: `int mountat(char *name, char *host, int port, char *dir)`,
codes `MOUNT_UNREACHABLE` -1, `MOUNT_NOTDIR` -2, `MOUNT_BUSY` -3, new
`MOUNT_FULL` -4 and `MOUNT_BADNAME` -5 in `sys.h`. Four places as usual; joins
`NEWER_BUILTINS` in `check_ref.py`. `SYS_mount` 48 stays as mountat("fs", ...).
The kernel creates `/mnt` and `/mnt/NAME` on the main filesystem when mounting, so
`ls /mnt` shows the mounts (they stay after umount, as `/mnt/fs` does today).

**Renames.** `k_rename` in `kernel.ino` treats two paths on different mounts like
two different filesystems (copy then remove). `host_linux.c` does the same through
its cross-rename copy.

**`/etc/mount`.** One line per mount: `name host[:port] dir` (dir is the rest of
the line, so it may hold spaces). `mount` and `umount` rewrite the file, replacing
or removing that name's line; `mount` alone prints from it. The kernel reads it
at boot and mounts every line without a server check. The old two-line format
(`host[:port]` then `dir`, first line has no space) is still read, as name `fs`,
and is rewritten in the new format the next time `mount` or `umount` saves.

**Messages.** `mount: too many mounts (4)`, `mount: bad name NAME`,
`mount: files under /mnt/NAME are open`, `umount: NAME is not mounted`; the
others as today with `/mnt/NAME` in place of `/mnt/fs`.

## Tests and docs

- `check_netfs.py --pc` runs a second `fileserver.py` on another port with its own
  directory:
  - mount both with names, `mount` lists both, files are read and written on each,
    and land in the right PC directory;
  - `cp` and `mv` between two mounts, and `mv` of a directory between them refused;
  - `cc` with the source on one mount and `-o` on the other;
  - stop one server: its mount fails and goes down, the other keeps working;
  - a 5th mount refused, bad names refused, remount of a name replaces it,
    `umount name`, `umount` of a name that is not mounted;
  - several mounts in `/etc/mount` are all mounted at start, and the old
    two-line format is still read (`host_linux` reads it at start like the kernel);
  - every existing step stays (default name `fs`).
- `check_netfs.py --board`: two mounts to two `fileserver.py` ports on the PC, the
  list, a copy between them, reboot keeps both.
- `make check`, `check_ref`, `check_net`, `check_sh`, self-hosting stay green.
- README: the `#### mount` section (names, list, umount NAME, the 5-connection
  limit), `/etc/mount` format.

## Left to the builder

Exact list layout spacing, how the board transport stores its clients, test
directory names.
