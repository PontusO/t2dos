#!/usr/bin/env python3
"""Benchmarks for the code cc writes: tests/bench/NAME.c, built plain and with -O.

Each benchmark is standard C that stresses one kind of code (loops on locals, calls, byte
arrays, struct pointers, globals, function pointer dispatch, floats, ...). It prints
"check ..." lines, which must equal the output of the same program built natively by gcc
(so a miscompile shows up as a failure, not as a fast time), and "time MS", the time the
work took by millis().

  bench.py --pc      build with compiler/cc (plain and -O) and gcc, run under qemu and compare
                     the check lines; prints code sizes (qemu times are only a rough guide)
  bench.py --board   the same, then upload both builds to /tmp/bench on the board and run
                     them there: a table of times, speedup and code sizes
  --only NAME        just that benchmark (may be repeated)
  --opt "FLAGS"      the optimized build's flags instead of -O
"""
import glob
import os
import re
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
BENCH = os.path.join(ROOT, 'tests', 'bench')
WORK = os.path.join(ROOT, 'build', 'bench')
CC = os.path.join(ROOT, 'compiler', 'cc')
QROOT = os.path.join(ROOT, 'root')


def fail(msg):
    print('FAIL: ' + msg)
    sys.exit(1)


def code_size(path):
    with open(path, 'rb') as f:
        hdr = struct.unpack('<5I', f.read(20))
    return hdr[1]


def checks(lines):
    return [l.strip() for l in lines if l.startswith('check')]


def ms(lines):
    for l in lines:
        m = re.match(r'time (-?\d+)', l)
        if m:
            return int(m.group(1))
    return None


def build(names, opt):
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(WORK)
    ref = {}
    for n in names:
        src = os.path.join(BENCH, n + '.c')
        exe = os.path.join(WORK, n + '.native')
        subprocess.run(['gcc', '-w', '-O0', '-fsingle-precision-constant', '-include',
                        os.path.join(BENCH, 'native.h'), '-o', exe, src], check=True)
        out = subprocess.run([exe], capture_output=True, text=True, timeout=120).stdout.splitlines()
        ref[n] = checks(out)
        if not ref[n]:
            fail('%s: gcc build prints no check line' % n)
        for tag, flags in (('p', []), ('o', opt)):
            r = subprocess.run([CC] + flags + ['-I', os.path.join(ROOT, 'lib'), '-o',
                               os.path.join(WORK, '%s_%s.bin' % (n, tag)), src],
                               capture_output=True, text=True, cwd=ROOT)
            if r.returncode:
                fail('%s: cc %s: %s' % (n, ' '.join(flags), r.stdout + r.stderr))
    return ref


def run_pc(names, ref):
    # host_linux sees only root/: run the builds from a temporary directory in it
    tmp = os.path.join(QROOT, 'tmp', 'bench')
    shutil.rmtree(tmp, ignore_errors=True)
    os.makedirs(tmp)
    res = {}
    try:
        for n in names:
            for tag in ('p', 'o'):
                shutil.copy(os.path.join(WORK, '%s_%s.bin' % (n, tag)), tmp)
                r = subprocess.run(['qemu-arm', os.path.join(ROOT, 'host_linux'), '-r', QROOT,
                                    '/tmp/bench/%s_%s.bin' % (n, tag)],
                                   capture_output=True, text=True, timeout=300)
                out = r.stdout.splitlines()
                if r.returncode or checks(out) != ref[n]:
                    fail('%s (%s): got %s (exit %d), gcc %s' % (n, 'plain' if tag == 'p' else 'opt',
                         checks(out), r.returncode, ref[n]))
                res[n, tag] = ms(out)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    return res


def run_board(names, ref):
    import board
    b = board.Board()
    try:
        b.run('echo hi', 5)                 # the first command after a reset may be misread
    except board.BoardError:
        pass
    b.run('mkdir /tmp', 5)
    b.run('mkdir /tmp/bench', 5)
    res = {}
    try:
        for n in names:
            for tag in ('p', 'o'):
                f = '%s_%s.bin' % (n, tag)
                b.put(os.path.join(WORK, f), '/tmp/bench/' + f)
                out = b.run('/tmp/bench/' + f, 300)
                if checks(out) != ref[n]:
                    fail('%s (%s) on the board: got %s, gcc %s' % (n, 'plain' if tag == 'p' else 'opt',
                         out, ref[n]))
                res[n, tag] = ms(out)
                b.run('rm /tmp/bench/' + f, 10)
    finally:
        b.run('rmdir /tmp/bench', 10)
    return res


def table(names, res, where, opt):
    o = ' '.join(opt)
    print('%-9s %9s %9s %8s %9s %9s' % ('', 'plain ms', o + ' ms', 'speedup', 'plain B', o + ' B'))
    tp = to = sp = so = 0
    for n in names:
        p, q = res[n, 'p'], res[n, 'o']
        a = code_size(os.path.join(WORK, n + '_p.bin'))
        c = code_size(os.path.join(WORK, n + '_o.bin'))
        tp += p; to += q; sp += a; so += c
        print('%-9s %9d %9d %7.2fx %9d %9d' % (n, p, q, p / q if q else 0, a, c))
    print('%-9s %9d %9d %7.2fx %9d %9d' % ('total', tp, to, tp / to if to else 0, sp, so))
    print('ok: %d benchmarks, output equal to gcc (%s)' % (len(names), where))


def main(argv):
    mode = None; only = []; opt = ['-O']
    i = 0
    while i < len(argv):
        a = argv[i]
        if a in ('--pc', '--board'):
            mode = a
        elif a == '--only' and i + 1 < len(argv):
            i += 1; only.append(argv[i])
        elif a == '--opt' and i + 1 < len(argv):
            i += 1; opt = argv[i].split()
        else:
            print(__doc__); return 2
        i += 1
    if not mode:
        print(__doc__); return 2
    names = sorted(os.path.basename(p)[:-2] for p in glob.glob(os.path.join(BENCH, '*.c')))
    if only:
        names = [n for n in names if n in only]
        if not names:
            fail('no benchmark named ' + ', '.join(only))
    ref = build(names, opt)
    if mode == '--pc':
        table(names, run_pc(names, ref), 'qemu', opt)
    else:
        table(names, run_board(names, ref), 'board', opt)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
