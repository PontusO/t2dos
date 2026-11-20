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
        if body[:1] != ['push\t{lr}']: fail('%s is not the minimal frameless entry: %r' % (name, body[:1]))
    # framed: the frameless ruling's old bail witness, converted since the depth walk.
    # The uniform mapping [r7, #k] -> [sp, #k+sz+d-4] names the same word: the frameless
    # entry pushes exactly one word less (r7), so every literal drops by 4 - params from
    # the caller's band and locals from our own frame alike, at any live spill depth.
    fb = f['framed']
    if fb[:1] != ['push\t{lr}']: fail('framed lost its parameter read: %r' % fb[:1])
    if 'mov\tr7, sp' in fb or not any(l == 'pop\t{pc}' for l in fb) \
      or not any(re.match(r'ldr\tr\d+, \[sp, #4\]$', l) for l in fb):
        fail('framed entry/exit bookkeeping: %r' % fb)
    if 'sub.w\tsp, sp, r1' in small or not any(re.match(r'sub\tsp, #\d+$', l) for l in small):
        fail('small does not reserve its frame with sub sp, #N: %r' % small[:8])
    if 'sub.w\tsp, sp, r1' not in big: fail('big lost its long frame reservation: %r' % big[:8])
    # A reserved frame converts since 3b-iii: add sp, #sz takes the exit's place where
    # mov sp, r7 unwound the reservation (every early return branches to the drained
    # exit), and r7 leaves the save lists. small and big keep their frames: their &b
    # words are lea forms (subw rD, r7, #N) that sprel cannot map - split answers -1
    # and the screen bails on the line.
    for name in ('small', 'big'):
        body = f[name]
        if 'mov\tr7, sp' not in body or not any(l_end_pc(l) and 'r7' in l for l in body):
            fail('%s lost its frame pointer: %r' % (name, body[:2]))
    if 'mov\tr7, sp' in early or 'mov\tsp, r7' in early:
        fail('early keeps the frame: %r' % early[:4])   # converted since the depth walk:
                                                        # the second call re-reads the
                                                        # parameter below the first call's
                                                        # spill window at [sp, #N] (d > 0)
    if any(re.search(r'\[r7, #', l) for l in early):
        fail('early still reads a frame slot with r7: %r' % early)
    pops = [l for l in early if l.startswith('pop\t{') and l.endswith('pc}')]
    if len(pops) < 2: fail('early does not return on the spot (%d pops): %r' % (len(pops), early))
    if any('r7' in l for l in pops): fail('a pop of pc keeps r7 in converted early: %r' % pops)
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
    # (from the converted plarge: [sp, #N], its 8-byte reservation drained at the exit)
    if not any(re.match(r'ldr\tr\d+, \[sp, #\d+\]$', l) for l in f['plarge'][:6]):
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
    if out != b'1 18 7 5 3 11 14 0 94 29 18\n': fail('frame.bin printed %r' % out)
    if bad: print('%d failed' % bad); return 1
    print('ok: frame')
    return 0


if __name__ == '__main__':
    sys.exit(main())
