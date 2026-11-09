#!/usr/bin/env python3
"""Run the BASIC interpreter's test cases (tests/basic/) on the PC or on the board.

A case is tests/basic/NAME.bas with NAME.out, the expected stdout followed by a last line
`exit N` (the interpreter's exit code). NAME.in, if present, is the program's stdin.

  check_basic.py --pc      build build/basic-pc (gcc -m32, basic/pcsys.h maps the T2DOS
                           library onto libc) and run every case with `basic-pc NAME.bas`
                           in tests/basic/; stdout must match NAME.out byte for byte
  check_basic.py --board   upload root/bin/basic.bin to /bin/basic.bin and the cases to
                           /src/basic/t/, run `basic NAME.bas` there through the shell;
                           the lines must match NAME.out (trailing spaces are not compared,
                           the shell's serial output loses them) and the exit code must too
  --only NAME              run only that case (may be given more than once)
Both may be given; the PC run comes first. Exit status 0 when every case passes.
"""
import glob
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, 'tests', 'basic')
PC_BIN = os.path.join(ROOT, 'build', 'basic-pc')
BOARD_DIR = '/src/basic/t'
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


def load_cases(only):
    names = sorted(os.path.splitext(os.path.basename(p))[0] for p in glob.glob(os.path.join(CASES, '*.bas')))
    if only:
        missing = [n for n in only if n not in names]
        if missing:
            raise SystemExit('no such case: %s' % ', '.join(missing))
        names = [n for n in names if n in only]
    return [Case(n) for n in names]


def report(where, case, ok, want, got):
    if ok:
        print('PASS %s' % case.name)
        return
    print('FAIL %s (%s)' % (case.name, where))
    print('  expected: %r' % want)
    print('  got:      %r' % got)


# ------------------------------------------------------------------ PC
def build_pc():
    os.makedirs(os.path.dirname(PC_BIN), exist_ok=True)
    srcs = sorted(glob.glob(os.path.join(ROOT, 'basic', '*.c')))
    r = subprocess.run(['gcc'] + PC_CFLAGS + ['-o', PC_BIN] + srcs + ['-lm'])
    return r.returncode == 0


def run_pc(cases):
    if not build_pc():
        for c in cases:
            print('FAIL %s (pc: no interpreter, the build of %s failed)' % (c.name, PC_BIN))
        return False
    ok = True
    for c in cases:
        try:
            r = subprocess.run([PC_BIN, c.name + '.bas'], cwd=CASES, input=c.stdin or b'',
                               capture_output=True, timeout=20)
            got = r.stdout.decode('latin-1') + 'exit %d\n' % r.returncode
        except subprocess.TimeoutExpired:
            got = 'TIMEOUT\n'
        want = c.out + 'exit %d\n' % c.code
        report('pc', c, got == want, want, got)
        ok = ok and got == want
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


def run_board(cases):
    try:
        r = Remote()
    except Exception as e:
        print('FAIL: board: %s' % e)
        return False
    binp = os.path.join(ROOT, 'root', 'bin', 'basic.bin')
    r.b.put(binp, '/bin/basic.bin')
    for d in ('/src', '/src/basic', BOARD_DIR):
        r.run('mkdir ' + d, 10.0)                    # fails harmlessly when it exists
    for c in cases:
        r.b.put(c.bas, '%s/%s.bas' % (BOARD_DIR, c.name))
    r.run('cd ' + BOARD_DIR, 10.0)
    ok = True
    try:
        for c in cases:
            out, code = r.run('basic %s.bas' % c.name, 60.0, c.stdin)
            got = [l.rstrip() for l in out] + ['exit %d' % code]
            want = [l.rstrip() for l in c.out.split('\n')[:-1]] + ['exit %d' % c.code]
            report('board', c, got == want, want, got)
            ok = ok and got == want
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
        ok = run_board(cases) and ok
    print('%s: %d cases' % ('ok' if ok else 'FAILED', len(cases)))
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
