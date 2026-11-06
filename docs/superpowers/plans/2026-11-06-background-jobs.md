# Background Jobs and Named Pipes Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Cooperative multitasking stage 1 for T2DOS: `cmd &` background jobs, `jobs`/`kill`, named pipes under `/pipe/`, and the reference server `demosrv`.

**Architecture:** The kernel gains a task table. Task 0 is the foreground (started by `loop()`, runs on the Arduino main stack and the existing `kstack`); tasks 1 to 4 are background jobs, each with its own SRAM kernel stack. A task switches only inside waiting system calls, through one `k_yield()` that saves callee-saved registers (core and FPU) and swaps stacks plus the per-task globals (`t2_sys_sp`, `cwd`, `stdout_fd`, `nest`/`procs`). Pipes are kernel buffers addressed by virtual fds 200+, with message boundaries recorded at writer close.

**Tech Stack:** C++ Arduino sketch (arduino-pico core, `rp2040:rp2040`), Thumb-2 inline asm, T2DOS C (userland, compiled by `compiler/cc`), Python 3 board test (`tools/board.py`).

**Spec:** `docs/superpowers/specs/2026-11-06-background-jobs-design.md`

## Global Constraints

- Nothing changes without `&`: foreground behaviour identical; every existing userland binary keeps working unrebuilt (ABI only grows); every userland `.bin` except `sh.bin` (and the new `demosrv.bin`) compiles byte-identical to before.
- New syscall numbers (frozen once committed): `spawn = 5`, `yield = 6`, `jobs = 7`, `kill = 8` (process group); `mkpipe = 47` (files group).
- Up to 4 background jobs; `spawn` fails cleanly beyond that or when memory runs out.
- Switch points only: `getchar`, `kbhit`, `sleep`, `recv`, `accept`, read of an empty pipe, write to a full pipe, pipe writer close with a full boundary queue, `kill` waiting for its target, `yield()`. Never inside a file, flash, or WiFi library operation.
- Pipe buffer 512 bytes. Messages end at writer close.
- Each task's kernel stack is in SRAM (flash writes make PSRAM unreachable).
- Bootstrap rule: `cc.c` only gains builtin names, never uses them; userland may use what `cc` compiles (no `static`, `const`, do-while, struct assignment, user variadics).
- ABI rule: each new call in `sys.h`, the `cc.c` builtin string, `kernel/kernel.ino`, `host_linux.c`.
- No em dashes anywhere (files, comments, commit messages).
- The board port is often held by the owner's `screen` session: never kill it; if `fuser` shows it busy, stop and ask the owner to close it. Power-cycle only with exactly `uhubctl -l 3-1.4 -p 4 -a cycle -d 3`.
- Kernel build: `FQBN=rp2040:rp2040:ilabs_cpico_2350:flash=8388608_7340032; arduino-cli compile --fqbn $FQBN --build-property "compiler.cpp.extra_flags=-I$PWD" --output-dir build/kernel kernel`; upload with `arduino-cli upload --fqbn $FQBN -p <port> --input-dir build/kernel`. qemu is not installed, so on-device tests go through `tools/board.py`.

## Review Focus

1. A pipe client run before the server replies (`cat /pipe/demosrv.out` typed first): it must wait, then print the reply and stop, not return empty. Pinned in Task 3 (`reply_before_request`).
2. Killing a job while it is blocked inside a system call (pipe read, `sleep`, `getchar`): resources freed, PSRAM back to the pre-spawn value, foreground unaffected. Pinned in Task 2 (`kill_sleeping`, `kill_getchar`) and Task 3 (`kill_pipe_reader`).
3. A nested exec in the foreground while a job runs (`cc` runs `as` and `ld`): must still work, since `t2_sys_sp` nesting is now per task. Pinned in Task 2 (`fg_exec_with_job`: build a toolchain case with `cc` on the board while `tick` runs).
4. Background job output while the foreground redirects to a file (`ls > x` with a job printing): the job's text goes to the console, not into `x`. Pinned in Task 2 (`redirect_isolation`).
5. Job exits while the foreground is inside a blocking call and the job owned the pipe the foreground reads: foreground read returns 0, not a hang. Pinned in Task 3 (`owner_exit_eof`).

---

### Task 1: ABI additions

**Files:**
- Modify: `sys.h` (enum, new constants and `struct job_t`)
- Modify: `compiler/cc.c:1954` (builtin name/number string only)
- Modify: `host_linux.c` (dispatch for the five calls; pipes)

**Interfaces:**
- Produces (in `sys.h`):
  - `SYS_spawn = 5, SYS_yield = 6, SYS_jobs = 7, SYS_kill = 8` in the process group, `SYS_mkpipe = 47` in the files group.
  - `struct job_t { int id; int state; int exit_code; char name[32]; };`
  - `enum { JOB_RUNNING = 1, JOB_DONE = 2 };`
  - `enum { SPAWN_NOTFOUND = -1, SPAWN_BADPROG = -2, SPAWN_NOMEM = -3, SPAWN_FULL = -4, SPAWN_UNSUPPORTED = -5 };`
  - `#define MAX_JOBS 4`, `#define PIPE_SIZE 512`, `#define JOB_KILLED -9` (exit code of a killed job).
  - C usage: `int spawn(char *path, int argc, char **argv)` returns job id 1..4 or a SPAWN_ error; `int yield()` returns 1 if another task ran, else 0; `int jobs(struct job_t *buf, int max)` returns count; `int kill(int job)` returns 0 after the job has ended, -1 if no such running job; `int mkpipe(char *name)` returns 0, -1 if it exists, the table is full, or the name is invalid (`name` is the part after `/pipe/`, at most 31 chars, no `/`).
- `host_linux.c`: `spawn` returns `SPAWN_UNSUPPORTED`; `yield` returns 0; `jobs` returns 0; `kill` returns -1; `mkpipe`, and `open`/`read`/`write`/`close`/`stat` on `/pipe/NAME`, work on virtual fds 200+ with the kernel's semantics, except a read of an empty pipe with no pending boundary returns 0 (no other task could fill it).

- [ ] **Step 1:** Add the enum values, constants and struct to `sys.h`, with a comment line per call in the style of the existing `recv` comment.
- [ ] **Step 2:** Append `spawn 5 yield 6 jobs 7 kill 8` after `sysinfo 4` and `mkpipe 47` after `getcwd 46` in the builtin string at `compiler/cc.c:1954`. No other `cc.c` change.
- [ ] **Step 3:** Add the five cases and the `/pipe/` handling to `host_linux.c` (pipe semantics as in Task 3's Interfaces; one owner, so owner-exit removal is done in `run_bin`'s cleanup for the exiting nest level).
- [ ] **Step 4: Verify.** Record before the change (from the parent commit): `make && md5sum root/bin/*.bin > /tmp/claude-1000/bin-before.md5`. After: `make && md5sum -c /tmp/claude-1000/bin-before.md5` → every line `OK`; `make check` → no failures. `gcc -fsyntax-only` of `host_linux.c` passes (or `make host` if the cross compiler exists).
- [ ] **Step 5: Commit** `sys.h compiler/cc.c host_linux.c` with message `ABI: spawn, yield, jobs, kill and mkpipe`.

### Task 2: Kernel task table, switching, spawn/jobs/kill/yield

**Files:**
- Modify: `kernel/kernel.ino`
- Create: `tests/jobs/tick.c`, `tests/jobs/spawner.c`, `tests/jobs/hog.c`
- Create: `tools/check_jobs.py`

**Interfaces:**
- Consumes: Task 1's `sys.h`.
- Produces (kernel-internal, used by Task 3):
  - `struct Task` with at least: `state` (FREE, RUN, DONE), `exit_code`, `name[32]`, `killed`, saved kernel `sp`, `sys_sp` (this task's `t2_sys_sp`), `cwd[128]`, `stdout_fd`, `nest`, `Proc procs[MAX_NEST]`, a `jmp_buf` for task-level unwind, and the argument copy. `static Task tasks[1 + MAX_JOBS]; static Task *cur;` with `tasks[0]` the foreground.
  - `Proc` gains `void *code, *data, *stack` so a task's nested levels can be freed when it is killed or unwound.
  - `static int k_yield();` switches to the next RUN task round robin, returns 1 if one ran, 0 if none; on return, if `cur->killed`, unwinds the task (longjmp to its task-level `jmp_buf`). Task 0 is never killed.
  - `static void k_wait();` = `if (!k_yield()) delay(1);` and replaces every `delay(1)` in a wait loop (`SerialConsole::getc`, `sock_recv`, `SYS_accept`), and `SYS_sleep` becomes a `k_wait()` loop until the time has passed.
  - `fd_owner[MAX_FD]` and `sock_owner[MAX_SOCK]` (task index); the listener also has an owner. Cleanup on task end closes everything the task owns; within a task, nested-exec cleanup keeps today's "opened by this level" behaviour.
- Context switch: one naked function `ctx_switch(uint32_t *save_sp, uint32_t new_sp)` that pushes `r4-r11, lr` and `vpush {d8-d15}`, stores `sp`, loads the new one, and pops the same. A new task's kernel stack is prepared so its first switch-in lands in a `task_start()` that calls `run()` with the already loaded program, then records the exit code, frees the task's resources, sets DONE, and switches away for good.
- `spawn` loads the program (header check, code in SRAM else PSRAM, data, 256 KB stack) in the caller's context so load errors return at once; copies path and arguments (at most 15 args, 256 bytes total) into the task; the job inherits the caller's `cwd`, and its stdout is the caller's current `stdout_fd`, whose fd ownership moves to the job.
- Background tasks: `kbhit` returns 0 after one `k_yield()`; `getchar` loops `k_wait()` forever.
- `jobs(buf, max)`: one entry per RUN or DONE task 1..4 (`id` = slot number); a DONE slot is returned once and then freed.
- `kill(job)`: sets `killed`, then `k_wait()`s until the job's state is not RUN; the job's `exit_code` is `JOB_KILLED`. A background task killing itself exits.
- Kernel stack per background task: 8 KB static SRAM array. (Ruling: nested exec inside a background job is supported but shallow; document it.)

Test programs (T2DOS C, built with `compiler/cc -I lib`):
- `tick.c`: `tick [n] [ms]` prints `tick K` n times (default 5) every ms (default 200) using `sleep`, then returns 7. `n = 0` means forever.
- `hog.c`: `hog` calls `getchar()` forever in the background.
- `spawner.c`: test driver run in the foreground, argument selects the case, prints one result line per check and `PASS`/`FAIL`.

`tools/check_jobs.py --board` (style of `check_toolchain.py`: docstring listing cases, exits non-zero on failure): builds `tests/jobs/*.c` and `root/bin/sh.bin` with `compiler/cc`, uploads to `/src/jobs/` and `/bin/`, records `free` output, runs each case, then checks `free` again equals the first value.

- [ ] **Step 1: Write the test programs and `check_jobs.py` cases:**
  - `spawn_basic`: `spawner basic` spawns `tick 3 100`, sleeps 1000, calls `jobs`: expects one entry `state == JOB_DONE`, `exit_code == 7`; a second `jobs` returns 0. Console shows `tick 1`..`tick 3` interleaved with spawner output.
  - `spawn_errors`: spawning `/nosuch.bin` returns `SPAWN_NOTFOUND`; spawning a non-T2CC file returns `SPAWN_BADPROG`; five `tick 0 1000` spawns: the fifth returns `SPAWN_FULL`; then `kill` all four, each returns 0.
  - `kill_sleeping`: spawn `tick 0 5000`, `kill` it while it sleeps: returns 0, `jobs` shows DONE with `JOB_KILLED`.
  - `kill_getchar`: spawn `hog`, the foreground still reads typed input (the driver reads a line the script sends) and `kill` of the hog returns 0.
  - `redirect_isolation`: spawn `tick 3 100`, then the foreground redirects to `/tmp/r.txt` and writes `fg` while sleeping 500; the file contains exactly `fg`, ticks appear on the console.
  - `fg_exec_with_job`: with `tick 0 300` running, the board's `cc` builds `tests/toolchain/` case `multi` (or the smallest existing multi-file case) and its output equals the expected file; then kill the tick.
  - After all cases: `free` PSRAM equals the value before the first case.
- [ ] **Step 2: Run** `python3 tools/check_jobs.py --board` against the unmodified kernel. Expected: FAIL (spawn returns "bad syscall").
- [ ] **Step 3: Implement** the task table, `ctx_switch`, `k_yield`/`k_wait`, per-task state swap, ownership-based cleanup, and the four syscalls in `kernel/kernel.ino` as specified in Interfaces. `run_bin` keeps its foreground behaviour, using `cur`'s `procs`/`nest` and storing `code/data/stack` in the `Proc`.
- [ ] **Step 4: Compile and flash** the kernel (Global Constraints commands), check the boot banner, then run `python3 tools/check_jobs.py --board`. Expected: every case PASS, PSRAM equal. Then `python3 tools/check_toolchain.py --selfhost` passes (foreground path unchanged).
- [ ] **Step 5: Commit** `kernel/kernel.ino tests/jobs/ tools/check_jobs.py` with message `kernel: background tasks with cooperative switching`.

### Task 3: Named pipes in the kernel

**Files:**
- Modify: `kernel/kernel.ino`
- Create: `tests/jobs/pecho.c`, `tests/jobs/pclient.c`
- Modify: `tools/check_jobs.py` (new cases)

**Interfaces:**
- Consumes: Task 2's `k_wait()`, `cur`, ownership cleanup hook.
- Pipe semantics (also what Task 1's host version follows):
  - `struct Pipe { used, name[32], owner, buf[PIPE_SIZE], head, count, bounds[8] (byte positions where a message ends), nbounds, total_read, total_written }`, at most 8 pipes. Open-pipe fds: `PIPE_FD = 200`, table of 16 entries `{ pipe index, mode (read or write), owner task, wrote }`.
  - `k_open` routes names that resolve under `/pipe/` to the pipe table: open fails (-1) if the pipe does not exist. `OPEN_READ` opens for reading; `OPEN_WRITE`/`OPEN_APPEND` for writing.
  - `read`: returns up to n bytes but never across the next boundary; if the next byte position is a boundary, return 0 once and drop that boundary; if empty with no boundary, `k_wait()` and retry; if the pipe was removed, return 0.
  - `write`: copies as much as fits, `k_wait()`s while full until all is written; -1 if the pipe was removed or has been removed while waiting. Console redirection: `k_write` of fd 1 to a pipe `stdout_fd` goes through this path.
  - writer `close`: appends a boundary at `total_written` (`k_wait()` while `nbounds == 8`). A writer that wrote nothing still ends a (empty) message.
  - `stat("/pipe/NAME")` succeeds with `size = count`, `isdir = 0` while the pipe exists. `unlink` on a pipe returns -1 (only the owner's exit removes it).
  - Owner task end (exit or kill) removes its pipes; open fds on them become dead (read 0, write -1, close frees the fd).
- Test programs:
  - `pecho.c`: `pecho NAME` creates `NAME` and `NAME.out`; loop: read one message from `/pipe/NAME`, open `/pipe/NAME.out`, write the message uppercased, close; message `quit` replies `BYE` and returns 3.
  - `pclient.c`: driver with cases selected by argument.

- [ ] **Step 1: Write the cases** in `check_jobs.py`:
  - `pipe_roundtrip`: spawn `pecho p1`; `pclient send p1 hello` writes `hello` and closes, then reads `/pipe/p1.out` to end of message: prints `HELLO`.
  - `reply_before_request`: `pclient early p1` spawns `pclient writer p1 abc` (a background job that sleeps 300 ms, then writes `abc` to `/pipe/p1` and closes), then at once opens `/pipe/p1.out` and reads to end of message, blocking until pecho replies: prints `ABC`. (This pins Review Focus 1.)
  - `shell_redirect`: through the shell, `echo hi > /pipe/p1` then `cat /pipe/p1.out` prints `HI`.
  - `large_message`: a 2000-byte message (bigger than `PIPE_SIZE`) round trips intact (writer blocks on full, reader drains).
  - `owner_exit_eof`: `pclient orphan p1` opens `/pipe/p1.out` for read, sends `quit`; after `BYE` and end of message, the next read returns 0 once pecho is DONE (exit 3), and `open("/pipe/p1")` then fails.
  - `kill_pipe_reader`: spawn `pecho p2`, kill it while it waits on its empty pipe: returns 0; `stat("/pipe/p2")` fails.
  - `mkpipe_errors`: duplicate name returns -1; ninth pipe returns -1; name with `/` returns -1.
  - PSRAM equal at the end (existing check).
- [ ] **Step 2: Run** `python3 tools/check_jobs.py --board`. Expected: new cases FAIL (mkpipe "bad syscall"), Task 2 cases still PASS.
- [ ] **Step 3: Implement** the pipe table and routing in `kernel/kernel.ino` per Interfaces.
- [ ] **Step 4: Compile, flash, run** `python3 tools/check_jobs.py --board`. Expected: all PASS.
- [ ] **Step 5: Commit** `kernel/kernel.ino tests/jobs/ tools/check_jobs.py` with message `kernel: named pipes under /pipe/`.

### Task 4: Shell job control

**Files:**
- Modify: `userland/sh.c`
- Modify: `tools/check_jobs.py` (shell cases)

**Interfaces:**
- Consumes: `spawn`, `jobs`, `kill`, `struct job_t`, `JOB_*`, `SPAWN_*` from `sys.h`.
- Behaviour (exact output):
  - `cmd args &` (`&` the last word): `[N] cmd`. With `> file` before `&`: open, `redirect(fd)`, `spawn`, `redirect(old)`, and the shell does not close `fd` after a successful spawn (it does close it when spawn fails). Errors: `cmd: not found`, `cmd: not a valid program`, `cmd: out of memory`, `too many jobs`.
  - `jobs`: one line per job, `[N] running cmd` or `[N] done cmd (exit E)`.
  - `kill N`: no output on success; `kill: no job N` otherwise; `kill` without an argument prints `usage: kill N`.
  - Before each prompt (interactive and not in scripts), the shell calls `jobs()` and prints `[N] done cmd (exit E)` for each DONE entry.
  - `help` text adds `cmd & runs in the background, jobs, kill N`.

- [ ] **Step 1: Write the cases** in `check_jobs.py` driving `sh` through `board.run`: `tick 2 100 &` prints `[1] tick`; after a following `sleep`-like wait (`tick 1 400` in the foreground), the next prompt shows `[1] done tick (exit 7)`; `tick 0 1000 &` then `jobs` shows `[1] running tick`; `kill 1` then the next prompt shows `[1] done tick (exit -9)`; `kill 9` prints `kill: no job 9`; `tick 3 50 > /tmp/t.txt &` leaves `/tmp/t.txt` with `tick 1`..`tick 3` (fetch, not `cat`, per `board.run`'s missing-final-line limit); `nosuch &` prints `nosuch: not found`; five `tick 0 1000 &` print `too many jobs` on the fifth.
- [ ] **Step 2: Run** them against the current `sh.bin`. Expected: FAIL.
- [ ] **Step 3: Implement** in `userland/sh.c`.
- [ ] **Step 4: Run** `make && python3 tools/check_jobs.py --board`. Expected: all PASS. `make check` still clean.
- [ ] **Step 5: Commit** `userland/sh.c tools/check_jobs.py` with message `sh: background jobs with &, jobs and kill`.

### Task 5: demosrv, the reference server

**Files:**
- Create: `userland/demosrv.c`
- Modify: `tools/check_jobs.py` (demosrv cases)

**Interfaces:**
- Consumes: pipes and `yield` (Tasks 1 to 3); `listen`, `accept`, `send`, `close`, `net_status` for TCP mode.
- Usage: `demosrv [name] [-p port]`; default name `demosrv`. Creates `/pipe/NAME` and `/pipe/NAME.out`.
- Replies (one per request message, written to `NAME.out` and closed):
  - `status` → `demosrv: up S s, R requests` (S = seconds since start, R = requests handled including this one)
  - `echo TEXT` → `TEXT`
  - `quit` → `bye`, then exit 0
  - anything else → `error: unknown command 'X'` where X is the first word
- Console log, one line per request: `demosrv: CMD` (pipe) or `demosrv: tcp status` (TCP).
- TCP mode: `listen(port)`; the main loop alternates `accept(0)` (no wait) with a non-blocking check of the request pipe (`stat("/pipe/NAME")` size or a read that would block is avoided by checking `size > 0` first), calling `yield()` once per idle loop; each accepted connection gets the status line plus `\n`, then `close`. Without the network up, `-p` prints `demosrv: network is down` and continues pipe-only.
- The source opens with a comment block explaining the template: create resources, loop (wait, handle, reply, yield), clean exit; each part is commented as the pattern to copy.

- [ ] **Step 1: Write the cases:** `demosrv &` then `echo status > /pipe/demosrv`, `cat /pipe/demosrv.out` prints a line matching `^demosrv: up \d+ s, 1 requests$`; `echo echo hello > /pipe/demosrv` then `cat` prints `hello`; `echo frob > /pipe/demosrv` gives `error: unknown command 'frob'`; with the server running, `edit` starts and quits (send `^Q`) normally and a toolchain case builds; `echo quit > /pipe/demosrv` gives `bye` and the next prompt shows `[1] done demosrv (exit 0)`; `demosrv x &` then `kill 1` leaves `stat /pipe/x` failing; PSRAM equal at the end. TCP (only if `net_status()` is up on the board, else print SKIP): `demosrv -p 2323 &`, a PC-side `socket.create_connection((board_ip, 2323))` reads the status line.
- [ ] **Step 2: Run.** Expected: FAIL (`demosrv: not found`).
- [ ] **Step 3: Implement** `userland/demosrv.c`.
- [ ] **Step 4: Run** `make && python3 tools/check_jobs.py --board`. Expected: all PASS (TCP PASS or SKIP). `make check` clean.
- [ ] **Step 5: Commit** `userland/demosrv.c tools/check_jobs.py` with message `demosrv: reference background server`.

### Task 6: Documentation and full regression

**Files:**
- Modify: `README.md` (user's manual): shell chapter (`&`, `jobs`, `kill`, done reports), command reference (`demosrv`), a section on named pipes and on writing a background program (the yield rule: a job that never waits freezes the board; power-cycle), system reference (the five calls, `struct job_t`, error codes), Status (verified items, limits: no preemption, 4 jobs, 8 pipes, shallow exec inside a job, recovery prompt freezes jobs, `cat` of a reply pipe waits until a reply arrives).
- Modify: `CLAUDE.md` (gitignored; main checkout): task model, switch points, the rule that no switch point may sit inside a file/flash/WiFi operation, per-task globals swapped in `k_yield`, `check_jobs.py`.

- [ ] **Step 1:** Write the README and CLAUDE.md changes. Every example output in the README is copied from a real board run.
- [ ] **Step 2: Full regression.** `make check` (0 failures), the byte-identity check from Task 1 Step 4 (all `OK` except `sh.bin`; `demosrv.bin` new), `python3 tools/check_toolchain.py --board`, `--selfhost`, `python3 tools/check_jobs.py --board`. All pass.
- [ ] **Step 3: Commit** `README.md` with message `README: background jobs, named pipes and demosrv`.
