#!/usr/bin/env python3
"""Check cc -O's function entry and exit (tests/opt/frame.c): the shapes of the assembly and
the program's output under qemu.  Needs `make` and `make host`.

  check_frame.py      prints `ok: frame` and exits 0 when every check passes
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'tests', 'opt', 'frame.c')
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


def main():
    os.makedirs(WORK, exist_ok=True)
    cc = os.path.join(ROOT, 'compiler', 'cc')
    subprocess.run([cc, '-O', '-S', '-I', os.path.join(ROOT, 'lib'), '-o', os.path.join(WORK, 'frame.s'), SRC],
                   check=True, cwd=ROOT)
    f = funcs(open(os.path.join(WORK, 'frame.s')).read())
    leaf, small, big, early, noret = f['leaf'], f['small'], f['big'], f['early'], f['noret']
    for name, body in (('leaf', leaf), ('noret', noret)):
        if 'mov\tr7, sp' in body or 'mov\tsp, r7' in body: fail('%s keeps the frame pointer: %r' % (name, body))
        if any(l.startswith('movt\tr1, #0') for l in body[:4]) or 'sub.w\tsp, sp, r1' in body:
            fail('%s still reserves an empty frame: %r' % (name, body))
    if 'sub.w\tsp, sp, r1' in small or not any(re.match(r'sub\tsp, #\d+$', l) for l in small):
        fail('small does not reserve its frame with sub sp, #N: %r' % small[:8])
    if 'sub.w\tsp, sp, r1' not in big: fail('big lost its long frame reservation: %r' % big[:8])
    if 'mov\tr7, sp' not in small or 'mov\tr7, sp' not in early: fail('a function with a frame lost r7')
    pops = [l for l in early if l.startswith('pop\t{') and l.endswith('pc}')]
    if len(pops) < 2: fail('early does not return on the spot (%d pops): %r' % (len(pops), early))
    subprocess.run([cc, '-O', '-I', os.path.join(ROOT, 'lib'), '-o', os.path.join(WORK, 'frame.bin'), SRC], check=True, cwd=ROOT)
    out = subprocess.run(['qemu-arm', './host_linux', '-r', 'root', '/tmp/frame.bin'], capture_output=True, cwd=ROOT, timeout=30).stdout
    if out != b'1 18 7 5 3\n': fail('frame.bin printed %r' % out)
    if bad: print('%d failed' % bad); return 1
    print('ok: frame')
    return 0


if __name__ == '__main__':
    sys.exit(main())
