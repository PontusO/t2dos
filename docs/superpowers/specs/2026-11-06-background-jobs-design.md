# T2DOS stage 1 multitasking: background jobs and named pipes

Date: 2026-11-06. Status: approved by the owner.

## Intent

T2DOS runs one program at a time. The owner wants a server to keep running
in the background while they work in the foreground (shell, `edit`, `cc`),
and wants programs on the board to be able to talk to that server.

This iteration builds the mechanism, not a particular server:

- cooperative switching between tasks,
- background jobs started from the shell,
- named pipes, so a foreground program (or the shell, through redirection)
  can send requests to a background server and read its replies.

A small dummy server, `demosrv`, is the reference implementation that future
background programs are copied from. (Owner's choice: "lets just make it a
dummy server that can be used as a reference implementation".)

This is stage 1 of a staged plan; stage 2 is shell pipes (`a | b`), stage 3
is preemption.

## Goals

- **`cmd args &`** in the shell starts `cmd` as a background job, prints its
  job number, and returns to the prompt at once.
- **Cooperative switching.** A task gives up the CPU only inside a system
  call that waits. Background tasks run while the foreground task waits; the
  foreground task runs again as soon as what it waits for is ready.
- **Named pipes** in a `/pipe/` namespace, used with the ordinary `open`,
  `read`, `write`, `close`, so existing programs and shell redirection work
  with them unchanged (`echo status > /pipe/demosrv`, `cat /pipe/demosrv.out`).
- **Job control in the shell:** `jobs` lists background jobs, `kill N` stops
  one, and the shell reports a finished job at the next prompt.
- **`demosrv`**, the reference server: controlled through named pipes, with
  an optional TCP mode, written and commented as a template.
- **Nothing changes without `&`.** A program run in the foreground behaves
  exactly as today, and every existing userland binary keeps working
  unrebuilt (the ABI only grows).

## Non-goals (this iteration)

- Preemption or time slicing (stage 3). A foreground program that computes
  without making a waiting system call (a long `cc` run) holds the CPU, and
  background jobs wait until it calls one or exits. This is accepted.
- A safeguard against a runaway background job. A job that computes forever
  without reaching a switch point freezes the board (the foreground never
  runs again, so `kill` cannot be typed); the fix is a power-cycle. This is
  accepted for stage 1 (owner's choice) and documented as a rule for
  background programs: wait or call `yield()` regularly. `demosrv` models it.
  Stage 3 removes the limit.
- Shell pipes `a | b` (stage 2: they need per-task standard input from a
  pipe; `getchar` reads only the keyboard today).
- `fg`/`bg`, moving a job between foreground and background, Ctrl-Z.
- Memory protection between tasks (MPU), use of the second core.
- Real servers (file server, web server). `demosrv` is a dummy.

## Anti-goals

- Anything that makes today's single-tasking use less reliable: a change that
  can hang the board when no background job runs is a failure even if jobs
  work.
- Switching in the middle of a flash write (see Constraints).

## How it behaves

### Tasks and switching

- There is one foreground task (the shell, or the program it runs) and up
  to 4 background jobs. (My call: 4, mainly because program code is loaded
  into SRAM; `spawn` fails cleanly when the limit or memory is reached.)
- **Switch points**: the waiting system calls `getchar`, `kbhit`, `sleep`,
  `recv`, `accept`, reading an empty pipe, writing a full pipe, and a new
  explicit `yield()`. `kbhit` counts because the shell and `edit` poll it in
  a loop while waiting for a key. No other system call switches.
- **Scheduling**: round robin. When a task reaches a switch point, the other
  runnable tasks each run until their next switch point, then the waiting
  task checks its condition again. A task blocked on something (empty pipe,
  sleep not over) is skipped until the condition can be true. The foreground
  should not be noticeably slowed when background jobs are idle (blocked on a
  pipe or in `sleep`/`accept`).
- **Keyboard**: only the foreground task reads keys. In a background task,
  `kbhit` returns 0 (and yields) and `getchar` waits forever (yielding).
- **Console output**: a background task's output goes to the console and may
  appear in the middle of the foreground's output or the line being typed.
  A job started with `cmd > file &` writes to the file instead, so stdout
  redirection becomes per task.
- **Per-task state**: each task has its own current directory (a job starts
  in the shell's directory), stdout redirection, `malloc` list, open files,
  sockets and listening server. Resource cleanup on exit must use ownership
  per task, not today's "what the parent had open" masks, because tasks no
  longer nest.
- **exec inside a job**: a background job may `exec` a program; it runs
  within that job, nested as today.
- **Exit and kill**: when a job exits or is killed, its memory, files,
  sockets, listener and pipes are freed, as for a foreground program today.
  `kill` of a job blocked in a system call must unwind it cleanly.
- **The foreground shell exiting** does not kill background jobs. The job
  table belongs to the kernel, not to a shell: a new `sh` (or a nested one)
  sees the same jobs in `jobs` and reports their endings. The kernel's
  recovery prompt is left untouched and is not a switch point, so jobs are
  frozen while it runs and continue once a shell runs again. (My call: keeps
  the recovery path, the one used when the system is broken, as simple as
  today.)

### Named pipes

- `mkpipe(name)` creates `/pipe/name`; the creating task owns it. Any task
  can `open("/pipe/name", ...)` it; the result is an ordinary fd.
- Each pipe has a fixed buffer. (My call: 512 bytes.)
- **Messages end at close.** A writer opens the pipe, writes, and closes; the
  close marks the end of one message. A reader's `read` returns data while
  there is some; when the pipe is empty it waits (yielding) until more data
  arrives or the current message has ended, in which case it returns 0 (end
  of file) once. This lets `echo status > /pipe/demosrv` deliver one complete
  request, and `cat /pipe/demosrv.out` print one reply and stop, whichever of
  the client and server runs first.
- Writing to a full pipe waits (yielding) until a reader makes room.
- When the owner exits, its pipes are removed: blocked or later readers get
  end of file, writers get -1, and `open` fails.
- `ls /pipe` is not required; `stat` on a pipe name succeeds while it exists.

### Shell

- `cmd args &` (the `&` as the last word) starts a background job and prints
  `[N] cmd`. Combined with `> file` it redirects that job's output.
- `jobs` prints each job's number, state and command name.
- `kill N` stops job N.
- At each prompt the shell prints `[N] done cmd (exit E)` for jobs that ended
  since the last prompt.
- `help` mentions `&`, `jobs` and `kill`.

### `demosrv`, the reference server

- `demosrv [name] &` creates `/pipe/name` (requests) and `/pipe/name.out`
  (replies); the default name is `demosrv`.
- Requests, one per message: `status` replies with uptime in seconds and the
  number of requests served; `echo text` replies with the text; `quit` replies
  `bye` and exits; anything else replies with an error line.
- Optional TCP mode, `demosrv -p port &`: also listens on the port and answers
  each connection with the same status line, interleaved with the pipe
  handling. Its loop must not block on one source while the other waits:
  it uses short timeouts or `yield()`.
- It prints a one-line log to the console for each request handled.
- The source is commented as a template: what each part does and which
  pattern (wait, handle, reply, yield) a new background program should copy.

## The system-call interface

New numbers go in the gaps of their groups in `sys.h` (my call on the exact
numbers):

- process group: `spawn(path, argc, argv)` starts a background job and
  returns its job number or a negative error (the kernel copies the
  arguments). The job's stdout is the caller's current `redirect()` target
  at the moment of the call; if that is a file, ownership of that fd moves
  to the job, which closes it when it ends. So the shell does
  `open`, `redirect(fd)`, `spawn`, `redirect(old)` and does *not* close the
  fd itself after a successful `spawn`. (My call: keeps `spawn` at three
  arguments and needs no `dup`.) `yield()`; `jobs(buf, max)` fills an array of a new
  `struct job_t` (job number, running or done, exit code, command name) and
  returns the count; `kill(job)`.
- files group: `mkpipe(name)`.

Per the project's ABI rule, each new call is added in `sys.h`, the builtin
table in `cc.c` `main()`, `kernel/kernel.ino` and `host_linux.c`.

`host_linux` (my call): pipes, `mkpipe` and `yield` work; `spawn` returns an
error ("not supported on the host"), and `jobs` returns 0. Background jobs
are tested on the board. qemu is not installed on the development PC at the
moment, so the host harness cannot be run anyway.

## Constraints

- **Flash writes and PSRAM.** While flash is written nothing may touch PSRAM,
  where program data and stacks live. Switching happens only at the switch
  points above, never inside a file operation, so a flash write is never
  interrupted by another task. Each task's kernel-side stack must be in SRAM
  (as `kstack` is today), because a task can be switched out in the middle of
  a system call.
- **SRAM budget**: program code goes in SRAM (PSRAM fallback exists);
  per-task kernel stacks also cost SRAM. Choose sizes so that the shell plus
  a 4-job load fits, and so that `cc` (which nests `as` and `ld` through
  `exec`) still runs in the foreground as today.
- **The kernel is not reentrant** (LittleFS, SdFat, WiFi, the shared `printf`
  buffer). Cooperative switching at the listed points keeps only one task
  inside such code at a time; do not add switch points inside them.
- **Bootstrap rule**: `sh.c` and `demosrv.c` are userland and may use what
  `cc` compiles; `cc.c` only gains builtin names.
- **No em dashes** in any file or comment (owner's global rule).

## Verification

- `make check` stays green; every userland binary other than `sh` (and the
  new `demosrv`) compiles byte-identically to before; `check_toolchain.py
  --selfhost` still passes on the board (the foreground path is unchanged).
- On the board, through `tools/board.py`, a scripted scenario:
  start `demosrv &`, `jobs` lists it, `echo status > /pipe/demosrv` then
  `cat /pipe/demosrv.out` prints the status line, an `echo` request round
  trips, `edit` and `cc` still work with the job running, a request sent and
  answered while the shell waits at the prompt, `kill` and `quit` both end
  the job with its memory returned (`free` shows the same PSRAM as before),
  and 4 jobs at once plus a fifth that fails cleanly.
- A job that blocks forever (`getchar` in the background) does not hang the
  foreground and can be killed.
- TCP mode is tested on the board if WiFi is available; it is the first test
  of the network code on hardware, so failures there are reported, not
  blocking.

## Documentation

- README (the user's manual): background jobs, `jobs`, `kill`, named pipes,
  `demosrv`, the new system calls, and the limits (no preemption).
- CLAUDE.md: the task model, switch points, and the rule that no switch
  point may sit inside a file or network operation.

## Left to the builder

- The context-switch mechanism (saving registers and stacks), the task table
  layout, and how the existing `Proc` nesting stack fits into it.
- Kernel stack sizes, and the exact error codes.
- How `kill` unwinds a blocked task.
- `demosrv`'s exact reply wording and log format.
