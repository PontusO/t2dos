#!/usr/bin/env python3
"""Byte identity of the assembly pipeline against the reference compiler.

  check_ref.py            for every src/userland/*.c, src/assembler/as.c, src/linker/ld.c,
                          src/binutils/*.c and src/compiler/tests/test*.c: build/ref/cc -o
                          ref.bin X versus src/compiler/cc -o new.bin X (cc runs build/host/as
                          and build/host/ld); the .bin files must be byte-identical.
                          src/compiler/cc.c itself left the corpus with the split into
                          src/compiler/*.c (see sources()).

A file whose reference .bin starts at an init stub (its entry is not main: the reference
cc's stub begins with push {r4, lr} = 0xB510) has run-before-main initializer code, which
now lives in a .t2init function, so its layout differs: it is excluded here and checked by
behaviour on the board instead.

A file the reference cc rejects with "undefined function: NAME(" for a builtin newer than
the reference cc (NEWER_BUILTINS: spawn, yield, jobs, kill, mkpipe, reboot, wifi_save, wifi_saved, ping and the maths calls sin
to fmod) cannot be compared either: it is excluded too and checked by behaviour on the board
(check_jobs.py; test13.c with `board.py expect`). Any other reference failure counts as
different.

A source that uses the `static` keyword (STATIC_TESTS) is unknown to the reference cc,
which dies with a generic parse error there; it is excluded by file name and checked by
behaviour instead (qemu vs gcc, and on the board by check_toolchain --selfhost).

Prints "identical: N, excluded (init code): M, excluded (newer builtins): B, excluded (static syntax): S, different: K",
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
CC = os.path.join(ROOT, 'src', 'compiler', 'cc')
NEWER_BUILTINS = ('spawn', 'yield', 'jobs', 'kill', 'mkpipe', 'reboot', 'wifi_save', 'wifi_saved', 'ping', 'mount', 'mountat', 'telnetd', 'time', 'localtime', 'tzset', 'tzlocal',   # unknown to the reference cc
                  'sin', 'cos', 'tan', 'asin', 'acos', 'atan', 'atan2', 'sinh', 'cosh', 'tanh',
                  'exp', 'log', 'log10', 'pow', 'sqrt', 'fabs', 'floor', 'ceil', 'fmod')
UNDEF_RE = re.compile(r'undefined function: (\w+)\(')
STATIC_TESTS = ('src/compiler/tests/test15.c',)    # uses `static`: unparseable by the ref cc


def newer_builtin(msg):
    """the reference cc failed only because the source calls a builtin it predates"""
    m = UNDEF_RE.search(msg)
    return bool(m) and m.group(1) in NEWER_BUILTINS


def sources():
    """(source, flags) for the identity set; -I lib where the Makefile uses it."""
    lib = ['-I', 'src/lib']
    out = [(f, lib) for f in sorted(glob.glob('src/userland/*.c'))]
    # src/compiler/cc.c is not compared: since the split into src/compiler/*.c the sources use
    # extern (the ref cc at e4fb427 predates it); the split cc is gated by behaviour
    # (bench, tests) and on-board byte identity (check_make --board, --selfhost).
    out.append(('src/assembler/as.c', lib))
    out.append(('src/linker/ld.c', lib))
    out += [(f, lib) for f in sorted(glob.glob('src/binutils/*.c'))]
    out += [(f, []) for f in sorted(glob.glob('src/compiler/tests/test*.c'))]
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
    for d in ('src/userland', 'src/compiler/tests', 'src/binutils'):
        if not any(s.startswith(d + '/') for s, _ in srcs):
            print('check_ref: no sources found in %s/' % d)
            return 1
    for f in ('src/compiler/cc.c', 'src/assembler/as.c', 'src/linker/ld.c'):
        if not os.path.exists(f):
            print('check_ref: %s is missing' % f)
            return 1
    os.makedirs(WORK, exist_ok=True)
    same = []
    excluded = []
    newer = []
    staticx = []
    diff = []
    for src, flags in srcs:
        if src in STATIC_TESTS:
            staticx.append(src)
            continue
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
    print('identical: %d, excluded (init code): %d, excluded (newer builtins): %d, excluded (static syntax): %d, different: %d' %
          (len(same), len(excluded), len(newer), len(staticx), len(diff)))
    if excluded:
        print('excluded (init code): ' + ' '.join(excluded))
    if newer:
        print('excluded (newer builtins): ' + ' '.join(newer))
    if staticx:
        print('excluded (static syntax): ' + ' '.join(staticx))
    for src, why in diff:
        print('different: %s: %s' % (src, why))
    return 1 if diff else 0


if __name__ == '__main__':
    sys.exit(main())
