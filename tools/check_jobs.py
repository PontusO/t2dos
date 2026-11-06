#!/usr/bin/env python3
"""Check background jobs on the board: spawn, yield, jobs, kill and cooperative switching.

The test programs are tests/jobs/*.c: tick (prints "tick K" with sleep() between, returns 7),
exectick (`exectick DIR`: chdir DIR, print "exectick cwd DIR", then exec `tick 0 1000`),
hog (getchar() forever), listener (listen(port), then sleeps forever) and spawner (the foreground driver: `spawner CASE` prints one
"ok ..."/"FAIL ..." line per check, then PASS or FAIL, and leaves the job table empty).
The pipe cases use pecho (`pecho NAME` makes /pipe/NAME and /pipe/NAME.out and answers each
message uppercased; "quit" answers "BYE" and exits 3) and pclient (`pclient CASE [NAME [TEXT]]`,
the pipe driver, same output style; `start` spawns pecho, `finish` makes it quit).

  check_jobs.py --build   build tests/jobs/*.c into build/jobs/ and root/bin/sh.bin with
                          compiler/cc (PC only, no board)
  check_jobs.py --board   --build, upload the programs to /src/jobs/ and sh.bin to /bin/,
                          record `free`, run every case, then `free` must show the same
                          free PSRAM as before the first case

Cases (spawner CASE on the board):
  spawn_basic         spawn `tick 3 100`, sleep 1000: jobs() has one entry, done, exit 7,
                      named tick; a second jobs() returns 0; tick 1..3 appear on the console
  spawn_errors        /nosuch.bin gives SPAWN_NOTFOUND, a non-T2CC file SPAWN_BADPROG; four
                      `tick 0 1000` run, the fifth gives SPAWN_FULL; kill of each returns 0,
                      a second kill -1; jobs() reports four done with JOB_KILLED
  kill_sleeping       spawn `tick 0 5000`, kill it inside its sleep: 0, done with JOB_KILLED
  kill_getchar        spawn hog (blocked in getchar in the background); the foreground still
                      reads the line this script types; kill of the hog returns 0
  redirect_isolation  spawn `tick 3 100`, the foreground redirects to /tmp/r.txt and writes
                      "fg" while sleeping 500: the file holds exactly "fg", the ticks are on
                      the console
  fg_exec_with_job    with `tick 0 300` running, the board's cc builds the tests/toolchain
                      case `globals` (cc execs as and ld), its output equals globals.expected;
                      a nested foreground `tick 2 400` returns 7; then kill the tick
  listen_owner        job A listens (prints "listen 0"); the foreground's listen and job B's
                      (prints "listen -1") fail while A runs; kill both (WiFi need not be up)
  pipe_roundtrip      pclient start p1; `pclient send p1 hello` prints HELLO; pclient finish p1
  reply_before_request  with pecho p1 running, `pclient early p1` spawns `pclient writer p1 abc`
                      (sleeps 300 ms, then sends abc) and reads /pipe/p1.out at once: it blocks
                      until pecho replies, prints ABC
  shell_redirect      with pecho p1 running, the shell runs `echo hi > /pipe/p1`, then
                      `cat /pipe/p1.out` prints HI
  large_message       with pecho p1 running, a 2000-byte message (more than PIPE_SIZE) round
                      trips intact: the writer blocks on the full pipe while pecho drains it
  owner_exit_eof      `pclient orphan p1` reads /pipe/p1.out while pecho quits: BYE, end of
                      message, then a blocked read returns 0 once pecho is done (exit 3); a
                      writer on the removed pipe gets -1, open("/pipe/p1") fails
  kill_pipe_reader    `pclient killer p2` spawns pecho p2, kills it while it waits on its empty
                      pipe: kill returns 0, stat("/pipe/p2") fails
  mkpipe_errors       `pclient mkpipe`: duplicate name, ninth pipe, names with '/', empty or too
                      long give -1; open of a missing pipe -1, unlink -1, stat size, a message
                      read back; then `pclient nopipe m0`: the exited program's pipes are gone
  sh_bg_done          (the shell cases run in a nested /bin/sh.bin, this branch's shell, in
                      /src/jobs) `tick 2 100 &` prints [1] tick, the ticks follow; after a
                      foreground `tick 1 400` the next prompt reports [1] done tick (exit 7)
  sh_jobs_kill        `tick 0 1000 &`, `jobs` shows [1] running tick, `kill 1` reports
                      [1] done tick (exit -9); `kill 9` says no job, `kill` prints the usage
  sh_bg_redirect      `tick 3 50 > /tmp/t.txt &` prints only [1] tick; the file holds tick 1..3
  sh_bg_errors        `nosuch &`, `notprog.txt &`, a fifth `tick 0 1000 &` (too many jobs), the
                      help text
  demosrv_pipe        (nested shell) `demosrv &`; `echo status > /pipe/demosrv`, `cat /pipe/demosrv.out`
                      prints "demosrv: up N s, 1 requests"; echo hello gives hello; frob gives
                      error: unknown command 'frob'; with the server running edit starts and
                      quits (^Q) and the board's cc builds and runs a program; quit gives bye
                      and the shell reports [1] done demosrv (exit 0)
  demosrv_kill        `demosrv x &`, `kill 1`: its pipes are gone (cat /pipe/x: not found)
  demosrv_tcp         only when the network is up (else SKIP): `demosrv -p 2323 &`, a PC socket to
                      the board's IP reads the status line; then kill
  sh_script_spawn     (nested shell) `sh srv.sh`, a script of `demosrv &`, `echo status >
                      /pipe/demosrv`, `cat /pipe/demosrv.out`: the shell runs a new job to its
                      first wait before the next line, so the status line comes back; then quit
  job_exec_kill       (nested shell) `exectick /tmp &`: the job's cd does not move the
                      foreground (pwd is still /src/jobs); kill 1 while the job is one exec level
                      down (in tick's sleep) reports [1] done exectick (exit -9), and free PSRAM
                      equals the value before the spawn
  ctrl_c_pipe         with pecho p1 running, `cat /pipe/p1.out` blocks (no request); a Ctrl-C
                      byte ends the read, the prompt comes back and the board still answers;
                      then the same with a CR typed before the Ctrl-C (the CR is kept and
                      read by the shell as an empty line)
"""
import os
import re
import socket
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
JOBS = os.path.join(ROOT, 'tests', 'jobs')
TOOLCHAIN = os.path.join(ROOT, 'tests', 'toolchain')
WORK = os.path.join(ROOT, 'build', 'jobs')
CC = os.path.join(ROOT, 'compiler', 'cc')
PROGS = ['tick', 'hog', 'listener', 'spawner', 'pecho', 'pclient', 'demosrv', 'exectick']
SRCS = {'demosrv': os.path.join(ROOT, 'userland', 'demosrv.c')}   # the others are tests/jobs/NAME.c
REMOTE = '/src/jobs'
SPAWNER = REMOTE + '/spawner.bin'
PCLIENT = REMOTE + '/pclient.bin'
EXEC_CASE = 'globals'                      # the tests/toolchain case fg_exec_with_job builds
EXEC_SRCS = ['globals_a.c', 'globals_b.c']
TICK_RE = re.compile(r'^tick \d+$')
FREE_RE = re.compile(r'^psram\s+\d+ bytes, (\d+) free$')


def fail(msg):
    print('FAIL ' + msg)
    return False


def build():
    os.makedirs(WORK, exist_ok=True)
    jobs = [(os.path.join(WORK, p + '.bin'), SRCS.get(p, os.path.join(JOBS, p + '.c'))) for p in PROGS]
    for out, src in jobs + [(os.path.join(ROOT, 'root', 'bin', 'sh.bin'), os.path.join(ROOT, 'userland', 'sh.c'))]:
        r = subprocess.run([CC, '-I', 'lib', '-o', out, src], cwd=ROOT, capture_output=True, text=True)
        if r.returncode != 0 or r.stdout or r.stderr:
            return fail('cc %s: exit %d, %r' % (os.path.relpath(src, ROOT), r.returncode, r.stdout + r.stderr))
    print('ok: built %s and root/bin/sh.bin' % ', '.join(p + '.bin' for p in PROGS))
    return True


# ------------------------------------------------------------------ board
def split(lines):
    """the job's tick lines and the rest"""
    ticks = [l for l in lines if TICK_RE.match(l)]
    rest = [l for l in lines if not TICK_RE.match(l)]
    return ticks, rest


def verdict(name, lines, code):
    """spawner's own checks: no FAIL line, PASS last, exit 0"""
    ticks, rest = split(lines)
    good = code == 0 and rest and rest[-1] == 'PASS' and not any(l.startswith('FAIL') for l in rest)
    if not good:
        fail('%s: spawner exit %d, output:' % (name, code))
        for l in lines:
            print('    ' + l)
    return bool(good), ticks, rest


def ticks_in_order(name, ticks, n):
    want = ['tick %d' % k for k in range(1, n + 1)]
    if ticks != want:
        return fail('%s: console ticks %r, want %r' % (name, ticks, want))
    return True


def psram_free(r):
    out, code = r.run('free', 10.0)
    for l in out:
        m = FREE_RE.match(l.strip())
        if m:
            return int(m.group(1))
    raise r.board.BoardError('free: no psram line in %r' % out)


def run_typing(r, cmd, wait_for, text, wait=30.0):
    """run cmd, type text once wait_for appears on a line; return (lines, exit code) like Remote.run"""
    b = r.b
    b.s.reset_input_buffer()
    b._write(cmd.encode() + b'\r')
    raw = b''
    start = time.time()
    typed = False
    while time.time() - start < wait:
        d = b.s.read(65536)
        raw += d
        txt = raw.decode('latin-1').replace('\r', '\n')
        if not typed and ('\n' + wait_for + '\n') in txt:
            b._write(text.encode() + b'\r')
            typed = True
        last = txt.split('\n')[-1]
        if not d and r.board.PROMPT_RE.match(last) and len(txt.split('\n')) > 1:
            break
    raw = raw.decode('latin-1')
    lines = [l.rstrip() for l in raw.replace('\r\n', '\n').replace('\r', '\n').split('\n')]
    if not lines or not lines[0].strip().endswith(cmd.strip()) or not r.board.PROMPT_RE.match(lines[-1]):
        raise r.board.BoardError('no answer from the board: it may have hung, reset it (got %r)' % raw[-120:])
    out = []
    code = 0
    for l in lines[1:]:
        if r.board.PROMPT_RE.match(l):
            continue
        if r.board.EXIT_RE.match(l):
            code = int(l[6:].split(',')[0])
            continue
        out.append(l)
    while out and not out[-1]:
        out.pop()
    if not typed:
        out.append('(the script never saw %r, so it typed nothing)' % wait_for)
    return out, code


def case_spawn_basic(r):
    out, code = r.run(SPAWNER + ' basic', 30.0)
    good, ticks, rest = verdict('spawn_basic', out, code)
    return ticks_in_order('spawn_basic', ticks, 3) and good


def case_spawn_errors(r):
    out, code = r.run(SPAWNER + ' errors', 60.0)
    good, ticks, rest = verdict('spawn_errors', out, code)
    return good


def case_kill_sleeping(r):
    out, code = r.run(SPAWNER + ' sleeping', 30.0)
    good, ticks, rest = verdict('kill_sleeping', out, code)
    if ticks:
        good = fail('kill_sleeping: the killed job printed %r' % ticks)
    return good


def case_kill_getchar(r):
    out, code = run_typing(r, SPAWNER + ' getchar', 'ready', 'hello jobs')
    good, ticks, rest = verdict('kill_getchar', out, code)
    return good


def case_redirect_isolation(r):
    out, code = r.run(SPAWNER + ' redirect', 30.0)
    good, ticks, rest = verdict('redirect_isolation', out, code)
    return ticks_in_order('redirect_isolation', ticks, 3) and good


def case_fg_exec_with_job(r):
    d = '%s/%s' % (REMOTE, EXEC_CASE)
    r.run('mkdir ' + d, 10.0)
    for s in EXEC_SRCS:
        r.put(os.path.join(TOOLCHAIN, s), '%s/%s' % (d, s))
    r.run('cd ' + d, 10.0)
    try:
        out, code = r.run(SPAWNER + ' exec', 300.0)
    finally:
        r.run('cd /', 10.0)
    good, ticks, rest = verdict('fg_exec_with_job', out, code)
    want = open(os.path.join(TOOLCHAIN, EXEC_CASE + '.expected')).read().rstrip('\n').split('\n')
    try:
        i = next(k for k, l in enumerate(rest) if l.startswith('ok cc ='))
        got = rest[i + 1:i + 1 + len(want)]
    except StopIteration:
        got = []
    if got != want:
        good = fail('fg_exec_with_job: %s.bin printed %r, want %r' % (EXEC_CASE, got, want))
    if len(ticks) < 2:
        good = fail('fg_exec_with_job: only %d tick lines (the job and the nested tick both print)' % len(ticks))
    return good


def case_listen_owner(r):
    out, code = r.run(SPAWNER + ' listen', 30.0)
    good, ticks, rest = verdict('listen_owner', out, code)
    got = [l for l in rest if l.startswith('listen ')]
    if got != ['listen 0', 'listen -1']:
        good = fail('listen_owner: the jobs printed %r, want job A "listen 0", then job B "listen -1"' % got)
    return good


def pclient(r, name, args, wait=30.0):
    out, code = r.run(PCLIENT + ' ' + args, wait)
    good, ticks, rest = verdict(name + ' (pclient ' + args + ')', out, code)
    return good, rest


def with_pecho(r, name, body):
    """start pecho p1, run body(), then make pecho quit (also when body failed)"""
    good, rest = pclient(r, name, 'start p1')
    try:
        good = body() and good
    finally:
        g2, rest = pclient(r, name, 'finish p1')
    return good and g2


def case_pipe_roundtrip(r):
    def body():
        good, rest = pclient(r, 'pipe_roundtrip', 'send p1 hello')
        if 'HELLO' not in rest:
            good = fail('pipe_roundtrip: no HELLO line in %r' % rest)
        return good
    return with_pecho(r, 'pipe_roundtrip', body)


def case_reply_before_request(r):
    def body():
        good, rest = pclient(r, 'reply_before_request', 'early p1')
        if 'ABC' not in rest:
            good = fail('reply_before_request: no ABC line in %r' % rest)
        return good
    return with_pecho(r, 'reply_before_request', body)


def case_shell_redirect(r):
    def body():
        out, code = r.run('echo hi > /pipe/p1', 10.0)
        good = True
        if out or code:
            good = fail('shell_redirect: echo hi > /pipe/p1 printed %r, exit %d' % (out, code))
        out, code = r.run('cat /pipe/p1.out', 10.0)
        if out != ['HI'] or code:
            good = fail('shell_redirect: cat /pipe/p1.out printed %r, exit %d (want [\'HI\'], 0)' % (out, code))
        return good
    return with_pecho(r, 'shell_redirect', body)


def case_large_message(r):
    def body():
        good, rest = pclient(r, 'large_message', 'large p1')
        return good
    return with_pecho(r, 'large_message', body)


def case_owner_exit_eof(r):
    good, rest = pclient(r, 'owner_exit_eof', 'start p1')
    g2, rest = pclient(r, 'owner_exit_eof', 'orphan p1')
    return good and g2


def case_kill_pipe_reader(r):
    good, rest = pclient(r, 'kill_pipe_reader', 'killer p2')
    return good


def case_mkpipe_errors(r):
    good, rest = pclient(r, 'mkpipe_errors', 'mkpipe')
    g2, rest = pclient(r, 'mkpipe_errors', 'nopipe m0')
    return good and g2


# ------------------------------------------------------------------ shell job control
PROMPT_PREFIX_RE = re.compile(r'^\S*> ')


def sh_lines(r, cmd, secs):
    """type cmd into the shell, read for secs seconds; the output lines without the echo,
    prompts, "[exit N, T ms]" lines and empty lines (job output may follow a prompt, so no final prompt is required)"""
    b = r.b
    b.s.reset_input_buffer()
    b._write(cmd.encode() + b'\r')
    raw = b._read(secs, secs + 1.0).decode('latin-1')
    lines = [l.rstrip() for l in raw.replace('\r\n', '\n').replace('\r', '\n').split('\n')]
    if not lines or not lines[0].strip().endswith(cmd.strip()):
        raise r.board.BoardError('no echo of %r from the board (got %r)' % (cmd, raw[-120:]))
    out = []
    for l in lines[1:]:
        while PROMPT_PREFIX_RE.match(l):
            l = PROMPT_PREFIX_RE.sub('', l, 1)
        if l.strip() and not r.board.PROMPT_RE.match(l) and not r.board.EXIT_RE.match(l):
            out.append(l)
    return out


def expect(name, cmd, got, want):
    """want: the exact lines (tick lines from jobs ignored)"""
    ticks, rest = split(got)
    if rest != want:
        return fail('%s: `%s` printed %r, want %r' % (name, cmd, got, want))
    return True


def in_shell(r, name, body):
    """run body() in a nested /bin/sh.bin started in REMOTE, then exit it"""
    r.run('cd ' + REMOTE, 10.0)
    out, code = r.run('/bin/sh.bin', 30.0)
    if 'T2DOS shell, type help' not in out:
        r.run('cd /', 10.0)
        return fail('%s: /bin/sh.bin printed %r' % (name, out))
    try:
        got = sh_lines(r, 'jobs', 0.5)  # an older sh without job control would run `tick 0 1000 &` forever
        if got:
            good = fail('%s: `jobs` printed %r, want nothing (no job control, or jobs left over)' % (name, got))
        else:
            good = body()
    finally:
        r.run('exit', 10.0)
        r.run('cd /', 10.0)
    return good


def case_sh_bg_done(r):
    def body():
        cmd = 'tick 2 100 &'
        got = sh_lines(r, cmd, 1.0)
        good = expect('sh_bg_done', cmd, got, ['[1] tick'])
        good = ticks_in_order('sh_bg_done', split(got)[0], 2) and good
        cmd = 'tick 1 400'
        got = sh_lines(r, cmd, 1.5)
        return expect('sh_bg_done', cmd, got, ['[1] done tick (exit 7)']) and good
    return in_shell(r, 'sh_bg_done', body)


def case_sh_jobs_kill(r):
    def body():
        good = True
        for cmd, secs, want in [('tick 0 1000 &', 0.5, ['[1] tick']), ('jobs', 0.5, ['[1] running tick']),
                                ('kill 1', 1.0, ['[1] done tick (exit -9)']), ('jobs', 0.5, []),
                                ('kill 9', 0.5, ['kill: no job 9']), ('kill', 0.5, ['usage: kill N'])]:
            good = expect('sh_jobs_kill', cmd, sh_lines(r, cmd, secs), want) and good
        return good
    return in_shell(r, 'sh_jobs_kill', body)


def case_sh_bg_redirect(r):
    def body():
        cmd = 'tick 3 50 > /tmp/t.txt &'
        got = sh_lines(r, cmd, 1.0)
        good = True
        if got != ['[1] tick']:
            good = fail('sh_bg_redirect: `%s` printed %r, want [\'[1] tick\'] (the ticks go to the file)' % (cmd, got))
        good = expect('sh_bg_redirect', 'pwd', sh_lines(r, 'pwd', 0.5), [REMOTE, '[1] done tick (exit 7)']) and good
        data = r.fetch('/tmp/t.txt')
        if data != b'tick 1\ntick 2\ntick 3\n':
            good = fail('sh_bg_redirect: /tmp/t.txt holds %r' % data)
        return good
    return in_shell(r, 'sh_bg_redirect', body)


def case_sh_bg_errors(r):
    def body():
        good = True
        steps = [('nosuch &', 0.5, ['nosuch: not found']),
                 ('notprog.txt &', 0.5, ['notprog.txt: not a valid program'])]
        for k in range(1, 5):
            steps.append(('tick 0 1000 &', 0.3, ['[%d] tick' % k]))
        steps.append(('tick 0 1000 &', 0.3, ['too many jobs']))
        for k in range(1, 5):
            steps.append(('kill %d' % k, 0.5, ['[%d] done tick (exit -9)' % k]))
        for cmd, secs, want in steps:
            good = expect('sh_bg_errors', cmd, sh_lines(r, cmd, secs), want) and good
        got = sh_lines(r, 'help', 0.5)
        if not any('cmd & runs in the background, jobs, kill N' in l for l in got):
            good = fail('sh_bg_errors: help printed %r' % got)
        return good
    return in_shell(r, 'sh_bg_errors', body)


def case_ctrl_c_pipe(r):
    good, rest = pclient(r, 'ctrl_c_pipe', 'start p1')
    b = r.b
    for before in (b'', b'\r'):
        what = 'Ctrl-C' if not before else '%r then Ctrl-C' % before.decode()
        b.s.reset_input_buffer()
        b._write(b'cat /pipe/p1.out\r')
        raw = b._read(1.5, 1.5).decode('latin-1')
        last = raw.replace('\r', '\n').split('\n')[-1]
        if r.board.PROMPT_RE.match(last):
            good = fail('ctrl_c_pipe: cat /pipe/p1.out did not block (got %r)' % raw)
        if before:
            b._write(before)               # pending ahead of the Ctrl-C: fg_break must look past it
            time.sleep(0.5)
        b._write(b'\x03')
        raw = b._read(5.0, 0.5, True).decode('latin-1')
        last = raw.replace('\r', '\n').split('\n')[-1]
        if not r.board.PROMPT_RE.match(last):
            return fail('ctrl_c_pipe: no prompt after %s (got %r); the board needs a reset' % (what, raw))
        out, code = r.run('pwd', 10.0)
        if out != ['/'] or code:
            good = fail('ctrl_c_pipe: after %s, pwd printed %r, exit %d' % (what, out, code))
    g2, rest = pclient(r, 'ctrl_c_pipe', 'finish p1')
    return good and g2


# ------------------------------------------------------------------ demosrv
HELLO_C = b'int main() { printf("hi from cc\\n"); return 0; }\n'


def demosrv_ask(r, name, text, want, secs=1.5):
    """send text as one request to /pipe/demosrv and read the reply; want is a regex (full match)"""
    sh_lines(r, 'echo %s > /pipe/demosrv' % text, 0.7)
    got = sh_lines(r, 'cat /pipe/demosrv.out', secs)
    got = [l for l in got if not l.startswith('[1] done')]
    if len(got) != 1 or not re.match(want + '$', got[0]):
        return fail('%s: request %r got %r, want /%s/' % (name, text, got, want))
    return True


def case_demosrv_pipe(r):
    r.put(HELLO_C, REMOTE + '/hello.c')

    def body():
        n = 'demosrv_pipe'
        good = expect(n, 'demosrv &', sh_lines(r, 'demosrv &', 1.0), ['[1] demosrv'])
        good = demosrv_ask(r, n, 'status', r'demosrv: up \d+ s, 1 requests') and good
        good = demosrv_ask(r, n, 'echo hello', 'hello') and good
        good = demosrv_ask(r, n, 'frob', "error: unknown command 'frob'") and good
        good = demosrv_ask(r, n, 'status', r'demosrv: up \d+ s, 4 requests') and good
        # the server is running: a foreground editor and a toolchain run must behave
        b = r.b
        b.s.reset_input_buffer()
        b._write(b'edit /tmp/e.txt\r')
        time.sleep(1.0)
        b._write(b'\x11')
        raw = b._read(5.0, 0.5, True).decode('latin-1')
        # the editor leaves its screen codes before the prompt: look at the end only
        if not re.search(r'/\S*> $', raw):
            good = fail('%s: no prompt after edit and ^Q with demosrv running (got %r)' % (n, raw[-120:]))
        got = sh_lines(r, 'cc -o /tmp/h.bin hello.c', 20.0)
        got = [l for l in got if not l.startswith('demosrv:')]
        if got:
            good = fail('%s: cc printed %r' % (n, got))
        got = sh_lines(r, '/tmp/h.bin', 1.0)
        if got != ['hi from cc']:
            good = fail('%s: /tmp/h.bin printed %r' % (n, got))
        sh_lines(r, 'echo quit > /pipe/demosrv', 0.7)
        got = sh_lines(r, 'cat /pipe/demosrv.out', 1.5)
        got += sh_lines(r, 'pwd', 1.0)
        if [l for l in got if l == 'bye'] != ['bye']:
            good = fail('%s: quit did not give bye: %r' % (n, got))
        if got.count('[1] done demosrv (exit 0)') != 1:
            good = fail('%s: no [1] done demosrv (exit 0) in %r' % (n, got))
        return good
    return in_shell(r, 'demosrv_pipe', body)


def case_demosrv_kill(r):
    def body():
        n = 'demosrv_kill'
        good = expect(n, 'demosrv x &', sh_lines(r, 'demosrv x &', 1.0), ['[1] demosrv'])
        good = expect(n, 'kill 1', sh_lines(r, 'kill 1', 1.0), ['[1] done demosrv (exit -9)']) and good
        good = expect(n, 'cat /pipe/x', sh_lines(r, 'cat /pipe/x', 0.7), ['cat: /pipe/x: not found']) and good
        return good
    return in_shell(r, 'demosrv_kill', body)


def case_demosrv_tcp(r):
    out, code = r.run('wifi', 10.0)
    ip = None
    for l in out:
        m = re.match(r'^connected, ip (\S+)$', l.strip())
        if m:
            ip = m.group(1)
    if ip is None:
        # network down: -p must say so and carry on pipe-only
        def down_body():
            n = 'demosrv_tcp (network down)'
            got = sh_lines(r, 'demosrv -p 2323 &', 1.0)
            good = expect(n, 'demosrv -p 2323 &', got, ['[1] demosrv', 'demosrv: network is down'])
            sh_lines(r, 'echo status > /pipe/demosrv', 0.7)
            got = sh_lines(r, 'cat /pipe/demosrv.out', 1.5)
            if len(got) != 1 or not re.match(r'^demosrv: up \d+ s, 1 requests$', got[0]):
                good = fail('%s: status over the pipe got %r' % (n, got))
            sh_lines(r, 'echo quit > /pipe/demosrv', 0.7)
            got = sh_lines(r, 'cat /pipe/demosrv.out', 1.5) + sh_lines(r, 'pwd', 1.0)
            if 'bye' not in got or got.count('[1] done demosrv (exit 0)') != 1:
                good = fail('%s: quit gave %r' % (n, got))
            return good
        good = in_shell(r, 'demosrv_tcp', down_body)
        print('SKIP demosrv_tcp: the network is not up (real TCP not tested)')
        return good

    def body():
        n = 'demosrv_tcp'
        good = expect(n, 'demosrv -p 2323 &', sh_lines(r, 'demosrv -p 2323 &', 1.0), ['[1] demosrv'])
        data = b''
        try:
            c = socket.create_connection((ip, 2323), timeout=10)
            try:
                c.settimeout(10)
                while True:
                    d = c.recv(256)
                    if not d:
                        break
                    data += d
            finally:
                c.close()
        except OSError as e:
            good = fail('%s: connect to %s:2323: %s' % (n, ip, e))
        if not re.match(r'^demosrv: up \d+ s, \d+ requests\n$', data.decode('latin-1')):
            good = fail('%s: the connection gave %r' % (n, data))
        sh_lines(r, 'kill 1', 1.0)
        return good
    return in_shell(r, 'demosrv_tcp', body)


SRV_SH = b'demosrv &\necho status > /pipe/demosrv\ncat /pipe/demosrv.out\n'


def case_sh_script_spawn(r):
    r.put(SRV_SH, REMOTE + '/srv.sh')

    def body():
        n = 'sh_script_spawn'
        got = sh_lines(r, 'sh srv.sh', 3.0)
        good = True
        if '[1] demosrv' not in got or any('cannot' in l or 'not found' in l for l in got) or \
                not any(re.match(r'^demosrv: up \d+ s, 1 requests$', l) for l in got):
            good = fail('%s: `sh srv.sh` printed %r, want [1] demosrv and the status line' % (n, got))
        sh_lines(r, 'echo quit > /pipe/demosrv', 0.7)
        got = sh_lines(r, 'cat /pipe/demosrv.out', 1.5) + sh_lines(r, 'pwd', 1.0)
        if 'bye' not in got or got.count('[1] done demosrv (exit 0)') != 1:
            good = fail('%s: quit gave %r' % (n, got))
        return good
    return in_shell(r, 'sh_script_spawn', body)


def case_job_exec_kill(r):
    def body():
        n = 'job_exec_kill'
        before = psram_free(r)
        cmd = 'exectick /tmp &'
        good = expect(n, cmd, sh_lines(r, cmd, 2.5), ['[1] exectick', 'exectick cwd /tmp'])
        good = expect(n, 'pwd', sh_lines(r, 'pwd', 0.5), [REMOTE]) and good
        good = expect(n, 'jobs', sh_lines(r, 'jobs', 0.5), ['[1] running exectick']) and good
        good = expect(n, 'kill 1', sh_lines(r, 'kill 1', 1.5), ['[1] done exectick (exit -9)']) and good
        got = sh_lines(r, 'jobs', 1.5)
        if got:
            good = fail('%s: `jobs` after the kill printed %r, want nothing (no job, no tick)' % (n, got))
        after = psram_free(r)
        if after != before:
            good = fail('%s: free PSRAM %d after the kill, %d before the spawn' % (n, after, before))
        return good
    return in_shell(r, 'job_exec_kill', body)


CASES = [('spawn_basic', case_spawn_basic), ('spawn_errors', case_spawn_errors),
         ('kill_sleeping', case_kill_sleeping), ('kill_getchar', case_kill_getchar),
         ('redirect_isolation', case_redirect_isolation), ('fg_exec_with_job', case_fg_exec_with_job),
         ('listen_owner', case_listen_owner),
         ('pipe_roundtrip', case_pipe_roundtrip), ('reply_before_request', case_reply_before_request),
         ('shell_redirect', case_shell_redirect), ('large_message', case_large_message),
         ('owner_exit_eof', case_owner_exit_eof), ('kill_pipe_reader', case_kill_pipe_reader),
         ('mkpipe_errors', case_mkpipe_errors),
         ('sh_bg_done', case_sh_bg_done), ('sh_jobs_kill', case_sh_jobs_kill),
         ('sh_bg_redirect', case_sh_bg_redirect), ('sh_bg_errors', case_sh_bg_errors),
         ('demosrv_pipe', case_demosrv_pipe), ('demosrv_kill', case_demosrv_kill),
         ('demosrv_tcp', case_demosrv_tcp),
         ('sh_script_spawn', case_sh_script_spawn), ('job_exec_kill', case_job_exec_kill),
         ('ctrl_c_pipe', case_ctrl_c_pipe)]   # last: on a kernel without it the board stays blocked


def check_board():
    from check_toolchain import Remote
    r = Remote()
    for d in ('/src', REMOTE, '/tmp'):
        r.run('mkdir ' + d, 10.0)
    for p in PROGS:
        r.put(os.path.join(WORK, p + '.bin'), '%s/%s.bin' % (REMOTE, p))
    r.put(b'not a T2CC program\n', REMOTE + '/notprog.txt')
    r.put(os.path.join(ROOT, 'root', 'bin', 'sh.bin'), '/bin/sh.bin')
    before = psram_free(r)
    print('free PSRAM before: %d' % before)
    ok = True
    for name, fn in CASES:
        try:
            good = fn(r)
        except r.board.BoardError as e:
            good = fail('%s: %s' % (name, e))
            print('%s %s' % ('FAIL', name))
            return False
        print('%s %s' % ('PASS' if good else 'FAIL', name))
        ok = ok and good
    after = psram_free(r)
    if after != before:
        ok = fail('free PSRAM %d after the cases, %d before' % (after, before))
    else:
        print('ok free PSRAM after: %d (equal)' % after)
    return ok


def main(argv):
    if len(argv) == 2 and argv[1] in ('--build', '--board'):
        if not build():
            return 1
        if argv[1] == '--build':
            return 0
        try:
            ok = check_board()
        except Exception as e:
            print('check_jobs.py: %s' % e)
            return 1
        print('%s: board' % ('ok' if ok else 'FAILED'))
        return 0 if ok else 1
    print(__doc__.strip())
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
