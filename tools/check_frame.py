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


def l_end_pc(l):
    return l.startswith('pop\t{') and l.endswith('pc}')


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
    # float locals and parameters live in s16-s31, saved by a vpush and restored by a vpop on
    # every exit; the shared exit ends mov sp, r7 / vpop / pop pc, and an inline pop at an
    # early return must only replace that last pop (trimframe copies nothing before the vpop)
    for name in ('fpm', 'fmany', 'fearly'):
        body = f[name]
        vp = [i for i, l in enumerate(body) if l.startswith('vpush\t{')]
        if len(vp) != 1: fail('%s has no single vpush: %r' % (name, body[:4]))
        exit = [i for i, l in enumerate(body) if l == 'pop\t{r7, pc}' or l.startswith('pop\t{r')]
        exit = [i for i in exit if l_end_pc(body[i])]
        if not exit: fail('%s has no return: %r' % (name, body[-3:]))
        e = exit[0]
        if not (body[e - 1].startswith('vpop\t{') and body[e - 2] == 'mov\tsp, r7'):
            fail('%s does not restore its s registers at the exit: %r' % (name, body[max(0, e - 3):e + 1]))
        for i in exit[1:]:
            if not l_end_pc(body[i - 1]): fail('%s returns on the spot past its vpop: %r' % (name, body[i - 1:i + 1]))
    if not any(l.startswith('vldr\ts') for l in f['fmany']): fail('fmany loads no float parameter with vldr')
    # promoted parameters are loaded once into a callee-saved register after the prologue
    if not any(re.match(r'ldr\tr\d+, \[r7, #\d+\]$', l) for l in f['plarge'][:6]):
        fail('plarge loads no parameter into a register: %r' % f['plarge'][:6])
    # a long frame keeps the movw/movt reservation even with a promoted parameter (r4 holds n);
    # its early return is an inline pop only when trimframe's exit-label window holds it
    bp = f['bigprom']
    if 'sub.w\tsp, sp, r1' not in bp or not any(l.startswith('ldr\tr4, [r7, #') for l in bp):
        fail('bigprom lost its long frame or its promoted parameter: %r' % bp[:7])
    bps = [l for l in bp if l_end_pc(l)]
    if len(bps) not in (1, 2): fail('bigprom has %d returns: %r' % (len(bps), bp))
    # a local array's pointer word is never promoted: the frame slot keeps its lea/str init,
    # and every read of the array goes through r7, never through a register copy of the pointer
    am = f['amem']
    if not any(re.match(r'str\tr\d+, \[r\d+\]$', l) for l in am[:8]): fail('amem lost its array init: %r' % am[:6])
    for l in am[3:]:
        if re.search(r'\[r(4|5|6|10|11|12)[,\]]', l):
            fail('amem reads the frame through a promoted register: %r' % l)
    subprocess.run([cc, '-O', '-I', os.path.join(ROOT, 'lib'), '-o', os.path.join(WORK, 'frame.bin'), SRC], check=True, cwd=ROOT)
    out = subprocess.run(['qemu-arm', './host_linux', '-r', 'root', '/tmp/frame.bin'], capture_output=True, cwd=ROOT, timeout=30).stdout
    if out != b'1 18 7 5 3 11 14 0 94 29\n': fail('frame.bin printed %r' % out)
    if bad: print('%d failed' % bad); return 1
    print('ok: frame')
    return 0


if __name__ == '__main__':
    sys.exit(main())
