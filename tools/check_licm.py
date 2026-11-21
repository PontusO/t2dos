#!/usr/bin/env python3
"""check_licm.py: cc -O's loop-invariant code motion (tests/opt/licm.c): the shapes of the
assembly and the program's output under qemu.  Needs `make` and `make host`.

  check_licm.py      prints `ok: licm` and exits 0 when every check passes

licm_torture.c is the fail-closed half: loops whose guards must reject a hoist (a body
branch that re-enters the test, a call in the loop) sit beside loops that must hoist, and
its output must equal gcc's.  A miscompile shows up there as a wrong number.
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'tests', 'opt', 'licm.c')
TORTURE = os.path.join(ROOT, 'tests', 'opt', 'licm_torture.c')
WORK = os.path.join(ROOT, 'root', 'tmp')
bad = 0


def fail(msg):
    global bad
    bad += 1
    print('FAIL', msg)


def funcs(asm):
    """name -> its lines (instructions and labels), from the assembly text."""
    out = {}; cur = None
    for l in asm.split('\n'):
        m = re.match(r'([A-Za-z_]\w*):$', l)
        if m: cur = m.group(1); out[cur] = []; continue
        if cur and l and not l.startswith('\t.global'): out[cur].append(l.strip())
    return out


def loop_body(body):
    """the lines between a loop's first label and the line that branches back to it."""
    start = None
    for n, l in enumerate(body):
        if re.match(r'\.L\d+:$', l): start = n; break
    if start is None: return body
    lab = body[start]
    for n in range(start + 1, len(body)):
        if re.match(r'b(?:\.\w+)?\t' + lab + r'$', body[n]):
            return body[start:n]
    return body[start:]


def run(name):
    """build root/tmp/name.bin with -O and run it under qemu; its stdout"""
    cc = os.path.join(ROOT, 'src', 'compiler', 'cc')
    src = SRC if name == 'licm' else TORTURE
    subprocess.run([cc, '-O', '-I', os.path.join(ROOT, 'src', 'lib'), '-o', os.path.join(WORK, name + '.bin'), src],
                   check=True, cwd=ROOT)
    return subprocess.run(['qemu-arm', './host_linux', '-r', 'root', '/tmp/%s.bin' % name],
                          capture_output=True, cwd=ROOT, timeout=60).stdout


def main():
    os.makedirs(WORK, exist_ok=True)
    cc = os.path.join(ROOT, 'src', 'compiler', 'cc')
    subprocess.run([cc, '-O', '-S', '-I', os.path.join(ROOT, 'src', 'lib'), '-o', os.path.join(WORK, 'licm.s'), SRC],
                   check=True, cwd=ROOT)
    f = funcs(open(os.path.join(WORK, 'licm.s')).read())
    sum_, two, nohoist = f['sum'], f['two'], f['nohoist']

    # sum: the base address is built once above the loop. promote() renames r9 to a
    # callee-saved register (r10, pushed at the top); peep_tohi folds the movw into the
    # add.w, and the movt for the upper half stays beside it.
    head = sum_.index('.L0:')
    pre = sum_[:head]
    if not any(l.startswith('add.w') and 'r9' in l.split('\t')[1] for l in pre):
        fail('sum hoists no base address above the loop: %r' % pre)
    body = sum_[head:]
    if any(':lower16:B' in l or (l.startswith('add.w') and 'B' in l) for l in body):
        fail('sum still rebuilds B\'s address inside the loop: %r' % body)

    # two: the inner loop hoists its base to its own head, not out of the outer loop.
    # (No fold here: peep() ran before licm_loops(), and a second peep round folds only
    # where nothing writes the flag-setting low form.)
    i4 = two.index('.L4:')
    inner = loop_body(two[i4:])
    if not any(l.startswith('add.w') for l in inner):
        fail('two hoists no base to the inner loop\'s head: %r' % inner[:8])
    i3 = two.index('.L3:')
    outer = loop_body(two[i3:i4])
    if any(l.startswith('add.w') for l in outer):
        fail('two hoisted the inner loop\'s base out of the outer loop: %r' % outer)

    # nohoist: p moves every round, so its address math stays in the loop. The movw/movt
    # pair for B is a fixed symbol and does hoist; what may not move is the add.w that
    # turns it into p's current address (the base register changes every round). Here the
    # fold did happen above the loop: the low form sets no flags, so peep_tohi folded it.
    nhead = nohoist.index('.L8:')
    nbody = nohoist[nhead:]
    if any(l.startswith('add.w') for l in nbody):
        fail('nohoist moved address math out of a loop whose base it writes: %r' % nbody)

    # nothing may name the placeholder register: only licm() writes rB, and every move
    # substitutes a real register at both ends.
    for name, body_ in f.items():
        for l in body_:
            if re.search(r'\brB\b', l):
                fail('%s emits the placeholder register: %r' % (name, l))

    out = run('licm')
    if out != b'44 297 33\n': fail('licm.bin printed %r' % out)

    # licm_torture.c: the guards' own cases. reentry's exit branches below the outer head,
    # so licm() must refuse its loops; callloop makes a call every round. Both sit beside
    # loops that hoist, and the printed sums must equal gcc's for -O and plain builds alike.
    subprocess.run([cc, '-O', '-S', '-I', os.path.join(ROOT, 'src', 'lib'), '-o', os.path.join(WORK, 'licm_torture.s'), TORTURE],
                   check=True, cwd=ROOT)
    t = funcs(open(os.path.join(WORK, 'licm_torture.s')).read())
    for name, body_ in t.items():
        for l in body_:
            if re.search(r'\brB\b', l):
                fail('torture %s emits the placeholder register: %r' % (name, l))
    want = b'108 105 84 189 140 112\n'
    out = run('licm_torture')
    if out != want: fail('licm_torture.bin printed %r' % out)
    subprocess.run([cc, '-I', os.path.join(ROOT, 'src', 'lib'), '-o', os.path.join(WORK, 'licm_torture_p.bin'), TORTURE],
                   check=True, cwd=ROOT)
    out = subprocess.run(['qemu-arm', './host_linux', '-r', 'root', '/tmp/licm_torture_p.bin'],
                         capture_output=True, cwd=ROOT, timeout=60).stdout
    if out != want: fail('plain licm_torture.bin printed %r' % out)

    if bad: print('%d failed' % bad); return 1
    print('ok: licm')
    return 0


if __name__ == '__main__':
    sys.exit(main())
