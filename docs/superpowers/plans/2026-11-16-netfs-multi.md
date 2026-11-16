# Several network mounts Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Up to 4 network mounts at `/mnt/NAME`, each with its own server, connection and down state.

**Architecture:** `kernel/netfs.c` replaces its single-mount globals with a table of 4 mounts; files and listings remember their mount. A new syscall `mountat` (49) picks the name; `mount` (48) stays as the `fs` shortcut. `/etc/mount` gets one line per mount, read at start by both kernels.

**Tech Stack:** C (netfs core, host_linux, kernel.ino on arduino-pico 6.3.2), T2DOS C subset (userland), Python 3 (check_netfs.py).

**Spec:** `docs/superpowers/specs/2026-11-16-netfs-multi-design.md` (builds on `docs/superpowers/specs/2026-11-16-netfs-design.md`)

## Global Constraints

- Up to 4 mounts (`NFS_MOUNTS 4`), each at `/mnt/NAME`; NAME 1 to 15 characters from letters, digits, `_`, `-`, `.`, not `.` or `..`. `/mnt/NAME` is accepted by the tools and stripped.
- `SYS_mount = 48` keeps its meaning (`fs`); new `SYS_mountat = 49`: `int mountat(char *name, char *host, int port, char *dir)`, host `""` unmounts.
- Codes: `MOUNT_UNREACHABLE -1`, `MOUNT_NOTDIR -2`, `MOUNT_BUSY -3`, new `MOUNT_FULL -4`, `MOUNT_BADNAME -5` (also returned when unmounting a name that is not mounted, except through `SYS_mount` 48, which returns 0 as today).
- `fileserver.py` and the session protocol do not change.
- `/etc/mount`: one line per mount, `name host[:port] dir` (dir is the rest of the line). The old two-line format (`host[:port]`, then `dir`; first line has no space) is read as `fs`.
- `mount` alone prints each mount as `host[:port] dir /mnt/name`, in `/etc/mount` order; a successful `mount` prints the same line for the new mount.
- Messages: `mount: too many mounts (4)`, `mount: bad name NAME`, `mount: files under /mnt/NAME are open`, `umount: NAME is not mounted`; the rest as today with `/mnt/NAME` for `/mnt/fs`.
- Each mount has its own connection, retries (3, 10 ms apart) and down state (10 s); the 10 ms gap after a close is shared.
- A rename between two mounts copies then removes (a directory is refused), like SD to `/mnt/fs` today.
- Four places for a syscall: `sys.h`, `cc.c` builtin string, `kernel.ino`, `host_linux.c`; `mountat` joins `NEWER_BUILTINS` in `tools/check_ref.py`.
- No em dashes; commit messages end with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>` only.

## Review Focus

- Remounting a name that is in use by an open file on a *different* mount must succeed (busy is per mount).
- A path like `/mnt/fsx` or `/mnt/f` must not be taken for mount `fs` (exact component match).
- One mount down must not mark another down, and a file opened on mount A must keep using A's connection after mount B reconnects.
- `/etc/mount` with a line for a bad or overlong name, or 5 lines, must be skipped at start, not crash or overflow.
- Unmounting one name must not drop another mount's connection or cached blocks.

---

### Task 1: The mount table in the core, the syscall, host_linux

**Files:**
- Modify: `kernel/netfs.h`, `kernel/netfs.c`, `sys.h`, `compiler/cc.c` (builtin string), `host_linux.c` (SYS_mount, new SYS_mountat, nfs_cross_rename call site, mkdir of `/mnt/NAME`), `tools/check_ref.py` (NEWER_BUILTINS)
- Create: `tests/netfs/two.c`
- Test: `tools/check_netfs.py` (new step `TWO`)

**Interfaces:**
- Produces:
  - `int nfs_mountat(const char *name, const char *host, int port, const char *dir, int check);` 0 or -1..-5 as in Global Constraints. Replacing an existing name is refused with -3 only if that mount has open files or listings.
  - `int nfs_mount(host, port, dir, check)` = `nfs_mountat("fs", ...)`, except unmounting `fs` when it is not mounted returns 0.
  - `int nfs_same(const char *a, const char *b);` 1 if both resolved paths are under the same mount.
  - `nfs_path(abs)` unchanged in meaning: 1 for `/mnt/NAME` or below for any mounted NAME.
  - Transport unchanged in signature; `nt_connect` may now be called while other connections are open and must return a distinct handle.
  - `sys.h`: `SYS_mountat = 49`, `MOUNT_FULL = -4`, `MOUNT_BADNAME = -5`.
- Consumes: nothing new.

- [ ] **Step 1: Write the failing test.** `tests/netfs/two.c`, run as `two PORT1 PORT2` (both servers on 127.0.0.1). It prints one line per check:
  - `a 0` / `b 0`: `mountat("a", "127.0.0.1", PORT1, "/")`, `mountat("b", "127.0.0.1", PORT2, "/")`
  - writes `/mnt/a/x.txt` = `"one\n"` and `/mnt/b/y.txt` = `"two\n"`, prints `w 0 0` (the two close() results)
  - `rename("/mnt/a/x.txt", "/mnt/b/x.txt")` prints `mv 0`; `rename` of directory `/mnt/a/d1` (mkdir'd first) to `/mnt/b/d1` prints `mvdir -1`
  - with `/mnt/b/y.txt` open for reading, `mountat("a", "127.0.0.1", PORT1, "/")` prints `reb 0` (busy is per mount), then reading the open file prints `r two` (it still uses b's connection), then `mountat("b", "", 0, "")` prints `busy -3`; close it
  - `mountat("c", ...)`, `mountat("d", ...)` print `c 0`, `d 0`; `mountat("e", ...)` prints `full -4`
  - `mountat("bad/n", ...)`, `mountat("..", ...)`, `mountat("sixteen_chars_xx", ...)` print `bad -5 -5 -5`
  - `stat("/mnt/ax", &st)` prints `ax -1` (no prefix match)
  - `mountat("c", "", 0, "")` prints `um 0`; again prints `um2 -5`; reading `/mnt/b/y.txt` afterwards prints `r2 two` (b untouched); `mount("", 0, "")` twice prints `fs 0 0`
  - exits 0.
  In `check_netfs.py`: start a second `fileserver.py` on `port + 2` with root `build/s/netfs/srv2`; add `'two'` to `TEST_PROGS`; add a `TWO` step that runs `/bin/two.bin PORT PORT2` under qemu and compares the full expected output above; afterwards `srv2/x.txt == b"one\n"`, `srv2/y.txt == b"two\n"`, `srv/x.txt` gone.
- [ ] **Step 2: Run it to make sure it fails.** Run: `make host && python3 tools/check_netfs.py --pc`. Expected: FAIL (cc rejects `mountat`, unknown builtin).
- [ ] **Step 3: Add the syscall in `sys.h`, `cc.c` (`mountat 49` after `mount 48`), `check_ref.py` NEWER_BUILTINS.**
- [ ] **Step 4: Implement the mount table in `kernel/netfs.c`.** `struct mount { char name[16], host[64], dir[NAME]; int port, on, conn, down; unsigned down_at; } mounts[NFS_MOUNTS]`; `nfile` and `ndir` get `int m`. One helper `static struct mount *mnt_of(const char *abs, const char **rest)` (exact component match after `/mnt/`) replaces every `abs + 7` and `NFS_ROOT` use (a path that is the mount root itself has `*rest == 0`). `open_conn`, `attempt`, `request`, `simple`, `drop_conn`, `send_all`, `recv_line`, `recv_all` take the mount. `busy(m)` checks one mount. `nfs_rename` returns -1 when `!nfs_same(a, b)` (callers copy before reaching it). Unmount closes only that mount's connection.
- [ ] **Step 5: host_linux.** `SYS_mountat` calls `nfs_mountat(..., 1)` and on success creates `ROOT/mnt` and `ROOT/mnt/NAME`; `SYS_mount` keeps calling `nfs_mount`. `SYS_rename`: both under netfs and `nfs_same` → `nfs_rename`; otherwise either under netfs → `nfs_cross_rename` (which already handles netfs on both sides through `nfs_open`).
- [ ] **Step 6: Run the tests.** Run: `make && make host && python3 tools/check_netfs.py --pc && make check && python3 tools/check_ref.py`. Expected: all pass; every earlier step unchanged.
- [ ] **Step 7: Commit** `netfs: several mounts in the core, mountat syscall`.

### Task 2: mount and umount tools, /etc/mount, PC checks, README

**Files:**
- Modify: `userland/mount.c`, `userland/umount.c`, `host_linux.c` (`nfs_boot`), `tools/check_netfs.py` (STEPS), `README.md`

**Interfaces:**
- Consumes: `mountat()` and the codes from Task 1.
- Produces: `/etc/mount` format (Global Constraints) that Task 3's `netfs_boot` reads with the same rules as host_linux's `nfs_boot`: skip a line whose name is bad or whose mount fails; old format read as `fs`.

- [ ] **Step 1: Write the failing steps** in `STEPS` (`SRV2` in commands and outputs is `127.0.0.1:PORT2`; `OLDFMT` writes `BOARD/etc/mount` = `@\n/projects\n`; `STOP2` kills the second server). Update every existing `mount` expectation from `'@ /\n'` to `'@ / /mnt/fs\n'` (and `PROXY /` the same way). New steps, in order, after the existing `mount @` at the start:
  - `('mount SRV2 / two', 'SRV2 / /mnt/two\n', 0)`, `('mount', '@ / /mnt/fs\nSRV2 / /mnt/two\n', 0)`
  - `('cp /mnt/fs/top.txt /mnt/two/top.txt', '', 0)`, `('cat /mnt/two/top.txt', 'top\n', 0)`, `('mv /mnt/two/top.txt /mnt/fs/top2.txt', '', 0)`, `('mv /mnt/fs/projects /mnt/two/projects', 'mv failed\n', 1)`
  - `('cc -o /mnt/two/h2.bin /mnt/fs/hs.c', '', 0)`, `('/mnt/two/h2.bin', '', 0)` (hs.c = `int main() { return 0; }` on srv)
  - `('mount @ / /mnt/x', '@ / /mnt/x\n', 0)`, `('mount @ / y', '@ / /mnt/y\n', 0)`, `('mount @ / z', 'mount: too many mounts (4)\n', 1)`
  - `('mount @ / bad/name', 'mount: bad name bad/name\n', 1)`, `('mount @ / ..', 'mount: bad name ..\n', 1)`
  - `('mount @ projects x', '@ /projects /mnt/x\n', 0)`, `('mount', '@ / /mnt/fs\nSRV2 / /mnt/two\n@ /projects /mnt/x\n@ / /mnt/y\n', 0)`, `('cat /mnt/x/game/a.txt', 'hello\n', 0)`
  - `('umount x', '', 0)`, `('umount /mnt/y', '', 0)`, `('umount nope', 'umount: nope is not mounted\n', 1)`
  - `('STOP2', ...)`, `('ls /mnt/two', 'ls: /mnt/two: not found\n', 1)`, `('cat /mnt/fs/top2.txt', 'top\n', 0)`, `('umount two', '', 0)`
  - `('OLDFMT', ...)`, `('cat /mnt/fs/game/a.txt', 'hello\n', 0)`, `('mount', '@ /projects /mnt/fs\n', 0)`, `('mount @', '@ / /mnt/fs\n', 0)`, then the file `BOARD/etc/mount` must equal `'fs 127.0.0.1:PORT /\n'`
  - `('ETCBAD', ...)` writes `BOARD/etc/mount` = `bad/n @ /\n` + `fs @ /\n` + `a @ /\n` + `b @ /\n` + `c @ /\n` + `d @ /\n` (a bad name, then 5 valid lines), then `('cat /mnt/fs/top2.txt', 'top\n', 0)` and `('cat /mnt/c/top2.txt', 'top\n', 0)` and `('ls /mnt/d', 'ls: /mnt/d: not found\n', 1)` (bad line skipped, 5th mount refused, no crash; `@` in the file is the server address). This group runs before the `OLDFMT` group, which overwrites the file.
  - server files afterwards: `srv/top2.txt == b'top\n'`, `srv2/top.txt` gone, `srv2/h2.bin` exists.
- [ ] **Step 2: Run them to make sure they fail.** Run: `python3 tools/check_netfs.py --pc`. Expected: FAIL on the `mount` list and the named mounts.
- [ ] **Step 3: `userland/mount.c`.** `mount host[:port] [dir] [name]`; name defaults to `fs`, a leading `/mnt/` is stripped; calls `mountat`; maps codes to the messages; rewrites `/etc/mount` replacing that name's line in place or appending; `mount` alone prints each line as `host dir /mnt/name` (old format shown as `fs`); exit 1 and `nothing mounted` when the file is missing or empty. Usage: `usage: mount [host[:port] [dir] [name]]`.
- [ ] **Step 4: `userland/umount.c`.** `umount [name]` (default `fs`, `/mnt/` stripped): `mountat(name, "", 0, "")`; -3 → `umount: files under /mnt/NAME are open`, -5 → `umount: NAME is not mounted` (exit 1); removes the name's line from `/etc/mount` in both the 0 and -5 cases, and deletes the file when no line is left.
- [ ] **Step 5: host_linux `nfs_boot`.** Reads every line; old format as `fs`; `nfs_mountat(name, host, port, dir, 0)` per line, ignoring failures.
- [ ] **Step 6: README.** `#### mount`: names, the list, `umount NAME`, up to 4 mounts, and the 5 TCP connections shared by programs' sockets and mounts in use (a connect that fails for lack of one behaves like an unreachable server); `/etc/mount` format.
- [ ] **Step 7: Run the tests.** Run: `make && make host && python3 tools/check_netfs.py --pc && python3 tools/check_net.py --pc && python3 tools/check_sh.py --pc`. Expected: all pass.
- [ ] **Step 8: Commit** `netfs: mount and umount take a name; /etc/mount holds several mounts`.

### Task 3: The board

**Files:**
- Modify: `kernel/kernel.ino` (transport with one `WiFiClient` per handle, `SYS_mountat`, `netfs_boot`, `k_rename`), `tools/check_netfs.py` (`BOARD_STEPS`, second server)

**Interfaces:**
- Consumes: `nfs_mountat`, `nfs_same` (Task 1), `/etc/mount` rules (Task 2).
- Produces: nothing further.

- [ ] **Step 1: Write the failing board steps.** `board_main` starts a second `fileserver.py` on `PORT + 2` with root `build/s/netfs/bsrv2` holding `two.txt` = `two\n`; `SERVER2` is `PCADDR:PORT+2`. Update `mount` expectations to the new format (`SERVER / /mnt/fs`). New steps after `mount SERVER`:
  - `('mount SERVER2 / two', ['SERVER2 / /mnt/two'])`, `('mount', ['SERVER / /mnt/fs', 'SERVER2 / /mnt/two'])`
  - `('cat /mnt/two/two.txt', ['two'])`, `('cp /mnt/fs/big.bin /mnt/two/big.bin', [])`, `('cmp /mnt/fs/big.bin /mnt/two/big.bin', [])`
  - `('REBOOT', None)` (`reboot`, wait for the prompt as other board checks do), then `('mount', ['SERVER / /mnt/fs', 'SERVER2 / /mnt/two'])`, `('cat /mnt/two/two.txt', ['two'])`
  - `('umount two', [])` before the existing final `umount`.
  - afterwards `bsrv2/big.bin == BIG`.
- [ ] **Step 2: Run them to make sure they fail.** Build and flash the current kernel is already on the board; upload the new `mount.bin`, `umount.bin`, `cc.bin`; run `python3 tools/check_netfs.py --board --server PCADDR:PORT`. Expected: FAIL at `mount SERVER2 / two` (old kernel has no syscall 49).
- [ ] **Step 3: kernel.ino.** Transport: `static WiFiClient nt_clients[NFS_MOUNTS]; static bool nt_used[NFS_MOUNTS];` `nt_connect` takes a free slot and returns its index (-1 when none); send/recv/close use the handle. `SYS_mountat` as in host_linux (creates `/mnt` and `/mnt/NAME` on `fsys`). `netfs_boot` reads every line like host_linux's `nfs_boot`. `k_rename`: same `FS*` but netfs and `!nfs_same(a, b)` takes the copy path.
- [ ] **Step 4: Build both boards, flash, run.** Run: `arduino-cli compile` for both FQBNs in CLAUDE.md, upload to the Challenger, then `python3 tools/check_netfs.py --board --server PCADDR:PORT`, `python3 tools/check_toolchain.py --selfhost`, `python3 tools/check_jobs.py --board`. Expected: all pass.
- [ ] **Step 5: Commit** `netfs: several mounts on the board`.
