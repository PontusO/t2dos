#!/usr/bin/env python3
"""Check cc -O's call-aware prologue work (tests/opt/prolog.c): the program's output
under qemu, and later the shapes of entry/exit per function.  Needs `make`.

  check_prolog.py      prints `ok: prolog` and exits 0 when every check passes
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'tests', 'opt', 'prolog.c')
STUB = os.path.join(ROOT, 'tests', 'opt', 'prolog_stub.c')
WORK = os.path.join(ROOT, 'root', 'tmp')
bad = 0


def fail(msg):
    global bad
    bad = bad + 1
    print('FAIL %s' % msg)


def funcs(text):                       # label -> list of instruction lines, like check_frame.py
    out = {}
    cur = None
    for l in text.splitlines():
        m = re.match(r'([A-Za-z_]\w*):$', l)
        if m: cur = m.group(1); out[cur] = []; continue
        if cur and l and not l.startswith('\t.global'): out[cur].append(l.strip())
    return out


def main():
    os.makedirs(WORK, exist_ok=True)
    cc = os.path.join(ROOT, 'compiler', 'cc')
    subprocess.run([cc, '-O', '-S', '-I', os.path.join(ROOT, 'lib'), '-o', os.path.join(WORK, 'prolog.s'), SRC],
                   check=True, cwd=ROOT)
    f = funcs(open(os.path.join(WORK, 'prolog.s')).read())
    for name in ('csc', 'cleaf', 'cmixed', 'clocals', 'cfloat', 'cret', 'main'):
        if name not in f: fail('no function %s in prolog.s' % name)

    # behavior: cc build (with the stub object) must print what native gcc prints
    subprocess.run([cc, '-c', '-I', os.path.join(ROOT, 'lib'), '-o', os.path.join(WORK, 'prolog_stub.o'), STUB],
                   check=True, cwd=ROOT)
    subprocess.run([cc, '-O', '-I', os.path.join(ROOT, 'lib'), '-o', os.path.join(WORK, 'prolog.bin'), SRC,
                    os.path.join(WORK, 'prolog_stub.o')], check=True, cwd=ROOT)
    out = subprocess.run(['qemu-arm', './host_linux', '-r', 'root', '/tmp/prolog.bin'],
                         capture_output=True, cwd=ROOT, timeout=30).stdout
    if out != b'check 9 16 3 8 6\n': fail('prolog.bin printed %r' % out)

    if bad: print('%d failed' % bad); return 1
    print('ok: prolog')
    return 0


if __name__ == '__main__':
    sys.exit(main())
