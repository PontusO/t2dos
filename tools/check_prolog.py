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

# cleaf as today's -O emits it: call-free, so calllive() must change nothing here.
CLEAF = ['push\t{r4, r7, lr}', 'mov\tr7, sp', 'sub\tsp, #8', 'ldr\tr1, [r7, #12]',
         'movw\tr0, #3', 'muls\tr0, r1', 'mov\tr4, r0', 'adds\tr0, #1',
         'mov\tsp, r7', 'pop\t{r4, r7, pc}']


def cleaf_shape(body):
    return [l for l in body if not l.startswith('.L')]


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

    # calllive(): a promoted register must stay saved when its value is live across a call.
    # csc reads s after ext() returns, so s keeps its register: the prologue loads it
    # (ldr r4, [r7, #off] in the first lines) and the push saves r4.
    head = f.get('csc', [])[:5]
    if not any(l.startswith('push\t{r4') for l in head[:1]):
        fail('csc lost its callee-saved register: %r' % f.get('csc', [])[:1])
    if not any(re.match(r'ldr\tr4, \[r7, #\d+\]$', l) for l in head):
        fail('csc loads no promoted parameter into r4: %r' % head)
    # cmixed: n (the bound) is loaded once before the loop and never read again - dead at
    # every call - while s (accumulator) and i (counter, re-read after ext returns) stay.
    if f.get('cmixed', [])[:1] != ['push\t{r4, r5, r7, lr}']:
        fail('cmixed push list changed: %r' % f.get('cmixed', [])[:1])
    # Snapshot of cleaf: a call-free function must not change at all (has_calls gate).
    # Snapshot of cleaf: a call-free function must not change at all (has_calls gate).
    if 'cleaf' in f and cleaf_shape(f['cleaf']) != CLEAF: fail('cleaf changed: %r' % f['cleaf'])

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
