# rcp and ping

## Why

`get` and `send` copy one file at a time. Moving a project directory between
the board and the PC means one command per file. `rcp -r` copies whole
directories, like Unix `rcp`. `ping` checks from the board that a host can be
reached and what its name resolves to (the mDNS trouble on 2026-11-16 showed
the need).

## Goals

- `rcp [-r] src dst`, one side local and one side remote, either direction.
- `-r` copies directory trees; without `-r` a directory source is refused.
- `ping host [-c count]` with round-trip times, working with `.local` names.

## Non-goals

- Copying between two remote servers, or local to local (`cp` does that).
- Preserving times or attributes, deleting extra files (no sync/mirror).
- Speed tuning for many small files (one connection per file is accepted).

## Approaches

- **A. Recursion on the board, one request per file (chosen).** `rcp` walks
  the tree itself: `LS` and `GET` to download, `opendir`/`readdir` and `PUT`
  to upload, plus a new `MKDIR` request in `fileserver.py`. Reuses `net.c`
  and the protocol almost unchanged.
- **B. One stream for the tree.** Board and server pack/unpack a tar-like
  stream. Faster with many small files, but a new format on both sides, more
  server code and harder error reporting. Not chosen.

## rcp

Syntax (agreed):

```
rcp [-r] src dst
rcp -r :projects/game /src/game      saved server, relative to the rcd directory
rcp -r /src/game pc.local:backup     another server, default port 7777
rcp game.c pc.local:7778:src/        host:port:path
```

- A remote side contains a `:`. `:path` uses the saved server and its remote
  directory (`/etc/remote`); `host:path` and `host:port:path` use that host,
  starting at `/`, the same rule as `net_pick`. Remote paths go through
  `net_resolve`, so `..` and leading `/` work as in `get`/`send`.
- Exactly one side must be remote, otherwise a usage error.
- Destination rules follow `cp`: if dst is an existing directory, the source
  is copied into it under its own name; otherwise dst is the new name. For a
  remote dst, "existing directory" is checked with `LS` (OK means directory).
- Directories are created as needed (`mkdir` locally, `MKDIR` remotely); an
  existing directory is not an error. Existing files are overwritten.
- Each file printed as it is copied (`name: N bytes`), then a total line.
  The first error stops the copy with a message and exit code 1.

Protocol change in `tools/fileserver.py`: `MKDIR name` answers `OK` (also
when it exists) or `ERR text`, with the same `safe()` check. README's network
protocol section and the `net.c` header comment list it.

## ping

- New syscall `SYS_ping = 91` (free in the network group):
  `int ping(char *host, char *ip)`. Resolves the name (mDNS included), writes
  the address as text into `ip` (16 bytes), sends one echo request and
  returns the round-trip time in ms, -1 on timeout, -2 if the name does not
  resolve, -3 if the network is down. Added to all four places: `sys.h`,
  `cc.c` builtins, `kernel.ino` (`WiFi.hostByName` + `WiFi.ping` with a
  1 s timeout; the library default is 15 s), `host_linux.c` (ICMP datagram
  socket, or -1 where the PC does not allow it).
- `userland/ping.c`: `ping host [-c count]`, default 4, one per second,
  prints `192.168.1.189: 12 ms` or `192.168.1.189: timeout`, then a summary
  (`4 sent, 4 received`). Ctrl-C stops it (the normal break, since it
  sleeps between pings). Exit 0 if any reply came, else 1.

## Tests and docs

- `check_net.py --pc`: rcp both directions with and without `-r`, nested
  directories, copy-into-existing-directory, the refusal cases; ping against
  127.0.0.1 where the PC allows it.
- Board: rcp -r of a small tree both ways, ping of the PC by `.local` name.
- README: `rcp` and `ping` sections under Network, repository layout list.

## Left to the builder

Buffer sizes, exact message wording, how the remote walk parses `LS` lines
(`   <dir>  name/` vs `size  name`), path length limits (stay within
`net_path`'s 200 bytes and report overflow).
