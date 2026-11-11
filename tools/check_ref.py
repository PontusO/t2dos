#!/usr/bin/env python3
"""Byte identity of the assembly pipeline against the reference compiler.

  check_ref.py            for every userland/*.c, compiler/cc.c, assembler/as.c, linker/ld.c,
                          binutils/*.c and compiler/tests/test*.c: build/ref/cc -o ref.bin X
                          versus compiler/cc -o new.bin X (cc runs build/host/as and
                          build/host/ld); the .bin files must be byte-identical.

A file whose reference .bin starts at an init stub (its entry is not main: the reference
cc's stub begins with push {r4, lr} = 0xB510) has run-before-main initializer code, which
now lives in a .t2init function, so its layout differs: it is excluded here and checked by
behaviour on the board instead.

A file the reference cc rejects with "undefined function: NAME(" for a builtin newer than
the reference cc (NEWER_BUILTINS: spawn, yield, jobs, kill, mkpipe, reboot and the maths calls sin
to fmod) cannot be compared either: it is excluded too and checked by behaviour on the board
(check_jobs.py; test13.c with Board().expect_gcc). Any other reference failure counts as
different.

Prints "identical: N, excluded (init code): M, excluded (newer builtins): B, different: K",
the excluded files on one line per class and the names of the different files; exit
status 1 if K > 0. Work files go to build/s/ref/.
"""
import glob
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, 'build', 's', 'ref')
REF_CC = os.path.join(ROOT, 'build', 'ref', 'cc')
CC = os.path.join(ROOT, 'compiler', 'cc')
NEWER_BUILTINS = ('spawn', 'yield', 'jobs', 'kill', 'mkpipe', 'reboot',   # unknown to the reference cc
                  'sin', 'cos', 'tan', 'asin', 'acos', 'atan', 'atan2', 'sinh', 'cosh', 'tanh',
                  'exp', 'log', 'log10', 'pow', 'sqrt', 'fabs', 'floor', 'ceil', 'fmod')
UNDEF_RE = re.compile(r'undefined function: (\w+)\(')


def newer_builtin(msg):
    """the reference cc failed only because the source calls a builtin it predates"""
    m = UNDEF_RE.search(msg)
    return bool(m) and m.group(1) in NEWER_BUILTINS


def sources():
    """(source, flags) for the identity set; -I lib where the Makefile uses it."""
    lib = ['-I', 'lib']
    out = [(f, lib) for f in sorted(glob.glob('userland/*.c'))]
    out.append(('compiler/cc.c', []))
    out.append(('assembler/as.c', lib))
    out.append(('linker/ld.c', lib))
    out += [(f, lib) for f in sorted(glob.glob('binutils/*.c'))]
    out += [(f, []) for f in sorted(glob.glob('compiler/tests/test*.c'))]
    return out


def run(cmd):
    r = subprocess.run(cmd, cwd=ROOT, capture_output=True, text=True)
    return r.returncode, (r.stdout + r.stderr).strip()


def has_init_stub(path):
    b = open(path, 'rb').read()
    entry = struct.unpack('<I', b[16:20])[0]
    return b[20 + entry:22 + entry] == b'\x10\xb5'


def main():
    os.chdir(ROOT)                   # the globs in sources() are relative to the repo root
    srcs = sources()
    for d in ('userland', 'compiler/tests', 'binutils'):
        if not any(s.startswith(d + '/') for s, _ in srcs):
            print('check_ref: no sources found in %s/' % d)
            return 1
    for f in ('compiler/cc.c', 'assembler/as.c', 'linker/ld.c'):
        if not os.path.exists(f):
            print('check_ref: %s is missing' % f)
            return 1
    os.makedirs(WORK, exist_ok=True)
    same = []
    excluded = []
    newer = []
    diff = []
    for src, flags in srcs:
        name = src.replace('/', '-')[:-2]
        ref = os.path.join(WORK, name + '.ref.bin')
        new = os.path.join(WORK, name + '.bin')
        for f in (ref, new):
            if os.path.exists(f):
                os.remove(f)
        st, msg = run([REF_CC] + flags + ['-o', ref, src])
        if st != 0:
            if newer_builtin(msg):
                newer.append(src)
            else:
                diff.append((src, 'reference cc failed: ' + msg))
            continue
        if has_init_stub(ref):
            excluded.append(src)
            continue
        st, msg = run([CC] + flags + ['-o', new, src])
        if st != 0:
            diff.append((src, msg.splitlines()[0] if msg else 'exit %d' % st))
            continue
        a = open(ref, 'rb').read()
        b = open(new, 'rb').read()
        if a == b:
            same.append(src)
        else:
            k = next((i for i in range(min(len(a), len(b))) if a[i] != b[i]), min(len(a), len(b)))
            diff.append((src, 'first difference at byte %d (sizes %d and %d)' % (k, len(a), len(b))))
    print('identical: %d, excluded (init code): %d, excluded (newer builtins): %d, different: %d' %
          (len(same), len(excluded), len(newer), len(diff)))
    if excluded:
        print('excluded (init code): ' + ' '.join(excluded))
    if newer:
        print('excluded (newer builtins): ' + ' '.join(newer))
    for src, why in diff:
        print('different: %s: %s' % (src, why))
    return 1 if diff else 0


if __name__ == '__main__':
    sys.exit(main())
