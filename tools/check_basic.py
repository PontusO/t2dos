#!/usr/bin/env python3
"""Run the BASIC interpreter's test cases (tests/basic/) on the PC or on the board.

A case is tests/basic/NAME.bas with NAME.out, the expected stdout followed by a last line
`exit N` (the interpreter's exit code). NAME.in, if present, is the program's stdin. NAME.args, if present, holds the arguments after
`basic NAME.bas` (an empty NAME.args means `basic` alone, with no file). A NAME.bas without a
NAME.out is a board-only case run by a function below (pipesrv).

  check_basic.py --pc      build build/basic-pc (gcc -m32, basic/pcsys.h maps the T2DOS
                           library onto libc) and run every case with `basic-pc NAME.bas`
                           in tests/basic/; stdout must match NAME.out byte for byte.
                           Then build build/basic-asan (the same with -fsanitize=address)
                           and run every case again: any AddressSanitizer or
                           LeakSanitizer report (a leak, a double free, a bad access)
                           fails the case; this is the interpreter's leak check
  check_basic.py --board   upload root/bin/basic.bin to /bin/basic.bin (and edit.bin, for editrun) and the cases to
                           /src/basic/t/, run `basic NAME.bas > NAME.txt` there through the
                           shell and fetch NAME.txt: the output must match NAME.out byte for
                           byte and the exit code too. A case with a NAME.in is run without
                           the redirection (INPUT's echo is part of its output on the
                           console) and compared line by line, trailing spaces not compared
                           (the shell's serial output loses them). Around every case the
                           `free` command's PSRAM free value is read: it must be the same
                           before and after the run. This checks the kernel's cleanup when
                           a program ends (it frees every block the program allocated), so
                           an interpreter leak does not show here: --pc's AddressSanitizer
                           run is the leak check.
                           Cases that need the PC (screen1: the board's conctl output does
                           not go through the `>` redirection; inkey1, ctrlc1, esc1, sleep1, sleep2: they feed
                           stdin at once) are not run on the board. The board key cases
                           (ctrlc, keyqueue, bgyield, pins, pipesrv, fileerr20, pipeeof, pipedup, editrun) drive the console directly and
                           run only on the board, after the ordinary cases.
  --only NAME              run only that case (may be given more than once); the board key
                           cases are named like that too
Both may be given; the PC run comes first. Exit status 0 when every case passes.
"""
import glob
import os
import re
import shutil
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, 'tests', 'basic')
PC_BIN = os.path.join(ROOT, 'build', 'basic-pc')
ASAN_BIN = os.path.join(ROOT, 'build', 'basic-asan')
BOARD_DIR = '/src/basic/t'
PC_ONLY = ('screen1', 'inkey1', 'ctrlc1', 'esc1', 'sleep1', 'sleep2')
KEY_CASES = ('ctrlc', 'keyqueue', 'bgyield', 'pins', 'pipesrv', 'fileerr20', 'pipeeof', 'pipedup', 'editrun')
BOARD_ONLY = ('pipesrv',)        # tests/basic/NAME.bas without a NAME.out: only the board runs it
# -msse2 -mfpmath=sse and -fsingle-precision-constant: float arithmetic rounds to single
# precision at every step, as on the board's FPU (the x87 default keeps extra precision)
PC_CFLAGS = ['-m32', '-w', '-msse2', '-mfpmath=sse', '-fsingle-precision-constant',
             '-I', ROOT, '-I', os.path.join(ROOT, 'basic'), '-include', os.path.join(ROOT, 'basic', 'pcsys.h')]


class Case:
    def __init__(self, name):
        self.name = name
        self.bas = os.path.join(CASES, name + '.bas')
        lines = open(os.path.join(CASES, name + '.out'), 'rb').read().decode('latin-1').split('\n')
        if lines and lines[-1] == '':
            lines.pop()
        if not lines or not lines[-1].startswith('exit '):
            raise SystemExit('%s.out: the last line must be "exit N"' % name)
        self.code = int(lines[-1][5:])
        self.out = ''.join(l + '\n' for l in lines[:-1])     # expected stdout
        p = os.path.join(CASES, name + '.in')
        self.stdin = open(p, 'rb').read() if os.path.exists(p) else None
        p = os.path.join(CASES, name + '.args')
        # the arguments after `basic NAME.bas`; an empty file means `basic` alone, no file
        self.args = None
        if os.path.exists(p):
            self.args = open(p).read().split()
        self.cmd_args = [] if self.args is None else self.args

    def argv(self):
        """the words after the interpreter: the file name, then NAME.args if there is one"""
        if self.args is not None and not self.args:
            return []
        return [self.name + '.bas'] + self.cmd_args


def load_cases(only):
    names = sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(CASES, '*.bas')))
    names = [n for n in names if n not in BOARD_ONLY]
    if only:
        missing = [n for n in only if n not in names and n not in KEY_CASES]
        if missing:
            raise SystemExit('no such case: %s' % ', '.join(missing))
        names = [n for n in names if n in only]
    return [Case(n) for n in names]


def clean_scratch():
    """the files and directories the cases leave in tests/basic (*.tmp; the board's file
    system drops a directory with its last entry, the PC's does not)"""
    for p in glob.glob(os.path.join(CASES, '*.tmp')):
        if os.path.isdir(p):
            shutil.rmtree(p, ignore_errors=True)
        else:
            os.remove(p)


def report(where, case, ok, want, got):
    if ok:
        print('PASS %s' % case.name)
        return
    print('FAIL %s (%s)' % (case.name, where))
    print('  expected: %r' % want)
    print('  got:      %r' % got)


# ------------------------------------------------------------------ PC
def build_pc(out=PC_BIN, extra=()):
    os.makedirs(os.path.dirname(out), exist_ok=True)
    srcs = sorted(glob.glob(os.path.join(ROOT, 'basic', '*.c')))
    r = subprocess.run(['gcc'] + PC_CFLAGS + list(extra) + ['-o', out] + srcs + ['-lm'])
    return r.returncode == 0


def run_pc(cases):
    if not build_pc():
        for c in cases:
            print('FAIL %s (pc: no interpreter, the build of %s failed)' % (c.name, PC_BIN))
        return False
    ok = True
    for c in cases:
        try:
            r = subprocess.run([PC_BIN] + c.argv(), cwd=CASES, input=c.stdin or b'',
                               capture_output=True, timeout=20)
            got = r.stdout.decode('latin-1') + 'exit %d\n' % r.returncode
        except subprocess.TimeoutExpired:
            got = 'TIMEOUT\n'
        want = c.out + 'exit %d\n' % c.code
        clean_scratch()
        report('pc', c, got == want, want, got)
        ok = ok and got == want
    return run_asan(cases) and ok


def run_asan(cases):
    """every case again under AddressSanitizer (leaks included): its report is the failure"""
    if not build_pc(ASAN_BIN, ['-g', '-fsanitize=address', '-fno-omit-frame-pointer']):
        print('FAIL: the AddressSanitizer build failed')
        return False
    env = dict(os.environ, ASAN_OPTIONS='detect_leaks=1')
    ok = True
    for c in cases:
        try:
            r = subprocess.run([ASAN_BIN] + c.argv(), cwd=CASES, input=c.stdin or b'',
                               capture_output=True, timeout=120, env=env)
            err = r.stderr.decode('latin-1')
            good = 'Sanitizer' not in err
            got = '' if good else err[:1500]
        except subprocess.TimeoutExpired:
            good, got = False, 'TIMEOUT'
        clean_scratch()
        if not good:
            print('FAIL %s (asan)' % c.name)
            print('  ' + got.replace('\n', '\n  '))
        ok = ok and good
    if ok:
        print('PASS asan: %d cases, no leaks or memory errors' % len(cases))
    return ok


# ------------------------------------------------------------------ board
class Remote:
    """The shell on the board: run a command, get its output lines and exit code"""
    def __init__(self):
        sys.path.insert(0, os.path.join(ROOT, 'tools'))
        import board
        self.board = board
        self.b = board.Board()

    def run(self, cmd, wait=30.0, stdin=None):
        """output lines and exit code of a shell command (sh prints "[exit N, T ms]" when N != 0);
        stdin, if given, is typed after the command starts (newlines sent as CR, like a terminal)"""
        b = self.b
        b.s.reset_input_buffer()
        b._write(cmd.encode() + b'\r')
        if stdin:
            time.sleep(0.5)
            b._write(stdin.replace(b'\r\n', b'\n').replace(b'\n', b'\r'))
        raw = b._read(wait, 0.5, True).decode('latin-1')
        lines = [l.rstrip() for l in raw.replace('\r\n', '\n').replace('\r', '\n').split('\n')]
        if not lines or not lines[0].strip().endswith(cmd.strip()) or not self.board.PROMPT_RE.match(lines[-1]):
            raise self.board.BoardError('no answer from the board: it may have hung, reset it (got %r)' % raw[-160:])
        out = []
        code = 0
        for l in lines[1:-1]:
            if self.board.EXIT_RE.match(l):
                code = int(l[6:].split(',')[0])
                continue
            out.append(l)
        return out, code


FREE_RE = re.compile(r'^psram\s+\d+\s+bytes,\s+(\d+)\s+free')


def psram_free(r):
    """free PSRAM in bytes, from the `free` command"""
    out, _ = r.run('free', 10.0)
    for l in out:
        m = FREE_RE.match(l.strip())
        if m:
            return int(m.group(1))
    raise r.board.BoardError('free: no psram line in %r' % out)


def fetch_text(r, path):
    """a file from the board as text ('' when it is empty: hexdump has nothing to show)"""
    try:
        return r.b.fetch(path).decode('latin-1')
    except r.board.BoardError as e:
        if 'no data' in str(e):
            return ''
        raise


def run_board_case(r, c):
    """(ok, wanted, got) for one case, comparing as the docstring says"""
    if c.stdin is not None:
        out, code = r.run(' '.join(['basic'] + c.argv()), 60.0, c.stdin)
        got = [l.rstrip() for l in out] + ['exit %d' % code]
        want = [l.rstrip() for l in c.out.split('\n')[:-1]] + ['exit %d' % c.code]
        return got == want, want, got
    out_file = '%s.txt' % c.name
    out, code = r.run(' '.join(['basic'] + c.argv()) + ' > ' + out_file, 60.0)
    text = fetch_text(r, out_file)
    r.run('rm ' + out_file, 10.0)
    got = text + 'exit %d\n' % code
    want = c.out + 'exit %d\n' % c.code
    return got == want, want, got


# ------------------------------------------------------------------ board key cases
def lines_of(raw):
    return [l.rstrip() for l in raw.decode('latin-1').replace('\r\n', '\n').replace('\r', '\n').split('\n')]


def at_prompt(raw):
    ls = lines_of(raw)
    return bool(ls) and bool(PROMPT_MATCH(ls[-1]))


def PROMPT_MATCH(line):
    import board
    return board.PROMPT_RE.match(line)


def key_result(name, good, detail=''):
    if good:
        print('PASS %s' % name)
    else:
        print('FAIL %s (board)' % name)
        print('  ' + detail)
    return good


def put_text(r, text, name):
    r.b.put(text.encode(), '%s/%s' % (BOARD_DIR, name))


def case_ctrlc(r):
    """a DO: LOOP program, byte 3 typed: `Break at` and the prompt come back"""
    put_text(r, 'DO: LOOP\n', 'spin.bas')
    b = r.b
    b.s.reset_input_buffer()
    b._write(b'basic spin.bas\r')
    time.sleep(1.0)
    b._write(b'\x03')
    raw = b._read(10.0, 0.5, True)
    good = b'Break at spin.bas:1' in raw and at_prompt(raw)
    out, _ = r.run('echo hi', 10.0)
    return key_result('ctrlc', good and out == ['hi'], repr(raw[-200:]))


def case_keyqueue(r):
    """keys typed while a program prints reach a later LINE INPUT in order"""
    put_text(r, 't = TIMER\nDO WHILE TIMER - t < 1.5\n  PRINT "x"\nLOOP\nPRINT "ready"\n'
             'LINE INPUT s$\nPRINT "got:["; s$; "]"\n', 'kq.bas')
    b = r.b
    b.s.reset_input_buffer()
    b._write(b'basic kq.bas\r')
    time.sleep(0.5)
    b._write(b'a')
    time.sleep(0.2)
    b._write(b'b')
    raw = b''
    end = time.time() + 15
    while b'ready' not in raw and time.time() < end:
        raw += b._read(1.0, 0.3)
    b._write(b'\r')
    raw += b._read(10.0, 0.5, True)
    good = b'got:[ab]' in raw and at_prompt(raw)
    return key_result('keyqueue', good, repr(raw[-200:]))


def case_bgyield(r):
    """an endless BASIC loop as a background job: the shell still answers"""
    put_text(r, 'x = 0\nDO: x = x + 1: LOOP\n', 'spin2.bas')
    out, _ = r.run('basic spin2.bas &', 10.0)
    m = re.search(r'\[(\d+)\] basic', ' '.join(out))
    if not m:
        return key_result('bgyield', False, 'no job line in %r' % out)
    job = m.group(1)
    try:
        time.sleep(0.5)
        echo, _ = r.run('echo hi', 10.0)
        jobs, _ = r.run('jobs', 10.0)
    finally:
        r.run('kill ' + job, 10.0)
    jobs2, _ = r.run('jobs', 10.0)
    good = echo == ['hi'] and any(l.startswith('[%s] running' % job) for l in jobs) and not any(l.startswith('[%s] running' % job) for l in jobs2)
    return key_result('bgyield', good, 'echo %r jobs %r after kill %r' % (echo, jobs, jobs2))


def case_pins(r):
    """PINMODE/PINWRITE/PAUSE/PWM on the board LED pin, no error"""
    put_text(r, 'PINMODE 25, 1\nPINWRITE 25, 1\nPAUSE 20\nPINWRITE 25, 0\nPWM 25, 0\n'
             'v = ANALOGREAD(26) + PINREAD(25)\nPRINT "pins ok"\n', 'pins.bas')
    out, code = r.run('basic pins.bas', 20.0)
    return key_result('pins', out == ['pins ok'] and code == 0, '%r exit %d' % (out, code))


def case_pipesrv(r):
    """a BASIC server in the background: it answers a request pipe on a reply pipe, `quit`
    ends it, and PSRAM is back to where it was"""
    before = psram_free(r)
    out, _ = r.run('basic pipesrv.bas &', 10.0)
    m = re.search(r'\[(\d+)\] basic', ' '.join(out))
    if not m:
        return key_result('pipesrv', False, 'no job line in %r' % out)
    job = m.group(1)
    try:
        r.run('echo hello > /pipe/bsrv', 10.0)
        rep, _ = r.run('cat /pipe/bsrv.out', 10.0)
        r.run('echo again > /pipe/bsrv', 10.0)
        rep2, _ = r.run('cat /pipe/bsrv.out', 10.0)
        r.run('echo quit > /pipe/bsrv', 10.0)
        time.sleep(0.5)
        jobs, _ = r.run('jobs', 10.0)
    finally:
        r.run('kill ' + job, 10.0)             # harmless once it has ended
    after = psram_free(r)
    running = any(l.startswith('[%s] running' % job) for l in jobs)
    good = rep == ['got: hello'] and rep2 == ['got: again'] and not running and before == after
    return key_result('pipesrv', good, 'replies %r %r jobs %r free %d -> %d' % (rep, rep2, jobs, before, after))


def case_fileerr20(r):
    """a program that errors with a file open, 20 times: the kernel's open-file table is not exhausted"""
    put_text(r, 'OPEN "fe20.tmp" FOR OUTPUT AS #1\nPRINT #1, "x"\nOPEN "fe20.tmp" FOR INPUT AS #2\n'
             'OPEN "fe20.tmp" FOR OUTPUT AS #1\n', 'fe20.bas')
    ok = True
    last = ''
    for i in range(20):
        out, code = r.run('basic fe20.bas', 20.0)
        last = repr((out, code))
        if code != 1 or not out or 'File already open' not in out[-1]:
            ok = False
            break
    r.run('rm fe20.tmp', 10.0)
    out, code = r.run('basic file1.bas', 20.0)
    ok = ok and code == 0
    return key_result('fileerr20', ok, 'run %d: %s; then %r' % (i, last, out[:2]))


def case_pipeeof(r):
    """EOF(n) on a pipe at the end of a message: DO UNTIL EOF reads both lines, then ends"""
    put_text(r, 'PIPE "be"\nOPEN "/pipe/be" FOR INPUT AS #1\nDO UNTIL EOF(1)\n  LINE INPUT #1, a$\n'
             '  PRINT a$\nLOOP\nPRINT "end"\n', 'pe_r.bas')
    put_text(r, 'OPEN "/pipe/be" FOR OUTPUT AS #1\nPRINT #1, "l1"\nPRINT #1, "l2"\nCLOSE #1\n', 'pe_w.bas')
    before = psram_free(r)
    r.run('basic pe_r.bas > pe.txt &', 10.0)
    r.run('basic pe_w.bas', 10.0)
    time.sleep(1.0)
    text = fetch_text(r, 'pe.txt')
    r.run('rm pe.txt', 10.0)
    after = psram_free(r)
    return key_result('pipeeof', text == 'l1\nl2\nend\n' and before == after, '%r free %d -> %d' % (text, before, after))


def case_pipedup(r):
    """creating a pipe twice is the run-time error Path/File access error at its line"""
    put_text(r, 'PIPE "pd"\nPIPE "pd"\n', 'pd.bas')
    out, code = r.run('basic pd.bas', 10.0)
    return key_result('pipedup', out == ['pd.bas:2: Path/File access error'] and code == 1, '%r exit %d' % (out, code))


def case_editrun(r):
    """edit hello.bas, ^R: the program's output and `Press any key`; a key brings the editor back,
    ^X leaves it, the prompt returns and PSRAM is as before"""
    before = psram_free(r)
    b = r.b
    b.s.reset_input_buffer()
    b._write(b'edit /src/basic/t/hello.bas\r')
    help_ok = b'^R run' in b._read(3.0, 1.0)        # the help line shows ^R
    b._write(b'\x12')
    raw = b''
    end = time.time() + 15
    while b'Press any key' not in raw and time.time() < end:
        raw += b._read(1.0, 0.3)
    shown = b'Hello from BASIC' in raw and b'Press any key' in raw
    b._write(b'x')
    time.sleep(1.0)
    b._write(b'\x18')
    tail = b._read(10.0, 0.5, True)
    good = shown and help_ok and tail.endswith(b'> ')      # the editor's redraw has no line breaks
    out, _ = r.run('echo hi', 10.0)
    after = psram_free(r)
    return key_result('editrun', good and out == ['hi'] and before == after,
                      '%r then %r out %r free %d -> %d' % (raw[-200:], tail[-200:], out, before, after))


def run_key_case(r, name):
    return {'ctrlc': case_ctrlc, 'keyqueue': case_keyqueue, 'bgyield': case_bgyield,
            'pins': case_pins, 'pipesrv': case_pipesrv, 'pipeeof': case_pipeeof, 'pipedup': case_pipedup, 'editrun': case_editrun, 'fileerr20': case_fileerr20}[name](r)


def run_board(cases, only=()):
    try:
        r = Remote()
    except Exception as e:
        print('FAIL: board: %s' % e)
        return False
    binp = os.path.join(ROOT, 'root', 'bin', 'basic.bin')
    r.b.put(binp, '/bin/basic.bin')
    r.b.put(os.path.join(ROOT, 'root', 'bin', 'edit.bin'), '/bin/edit.bin')    # editrun
    for d in ('/src', '/src/basic', BOARD_DIR):
        r.run('mkdir ' + d, 10.0)                    # fails harmlessly when it exists
    cases = [c for c in cases if c.name not in PC_ONLY]
    for c in cases:
        r.b.put(c.bas, '%s/%s.bas' % (BOARD_DIR, c.name))
    r.b.put(os.path.join(CASES, 'pipesrv.bas'), BOARD_DIR + '/pipesrv.bas')
    r.run('cd ' + BOARD_DIR, 10.0)
    ok = True
    try:
        for c in cases:
            before = psram_free(r)
            good, want, got = run_board_case(r, c)
            after = psram_free(r)
            if good and before != after:
                good = False
                want = 'free PSRAM %d after the run' % before
                got = 'free PSRAM %d after the run' % after
            report('board', c, good, want, got)
            ok = ok and good
        for name in KEY_CASES:
            if not only or name in only:
                ok = run_key_case(r, name) and ok
    finally:
        r.run('cd /', 10.0)
    return ok


def main(argv):
    only = []
    pc = board = False
    i = 0
    while i < len(argv):
        a = argv[i]
        if a == '--pc':
            pc = True
        elif a == '--board':
            board = True
        elif a == '--only' and i + 1 < len(argv):
            i += 1
            only.append(argv[i])
        else:
            print(__doc__)
            return 2
        i += 1
    if not pc and not board:
        print(__doc__)
        return 2
    cases = load_cases(only)
    ok = True
    if pc:
        ok = run_pc(cases) and ok
    if board:
        ok = run_board(cases, only) and ok
    print('%s: %d cases' % ('ok' if ok else 'FAILED', len(cases)))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
