#!/usr/bin/env python3
"""Check build/host/as against GNU as.

  check_asm.py file.s...           assemble each with build/host/as and arm-none-eabi-as,
                                   compare the text and data bytes
  check_asm.py --errors file.s...  each file must fail: exit status 1, no output file, and the
                                   first line of output equal to file.expected (next to file.s)
  check_asm.py --corpus            disassemble every root/bin/*.bin and compiler/tests/*.c (built
                                   with compiler/cc into build/corpus/), turn it back into
                                   assembly, and require build/host/as and GNU as to reproduce
                                   the original code bytes
"""
import glob
import os
import re
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
GNU = os.path.expanduser('~/.arduino15/packages/rp2040/tools/pqt-gcc/5.0.0-9576866/bin')
AS = os.path.join(ROOT, 'build', 'host', 'as')
WORK = os.path.join(ROOT, 'build', 'asm')
GNU_FLAGS = ['-mcpu=cortex-m33', '-mthumb', '-mfpu=fpv5-sp-d16']
T2OBJ_MAGIC = 0x424F3254


def gnu(tool):
    return os.path.join(GNU, 'arm-none-eabi-' + tool)


def read_t2obj(path):
    """(text, data) of a T2DOS object: seven LE ints, then text, then data."""
    b = open(path, 'rb').read()
    magic, version, tsize, dsize = struct.unpack('<4i', b[:16])
    if magic != T2OBJ_MAGIC:
        raise ValueError('%s: not a T2DOS object' % path)
    return b[28:28 + tsize], b[28 + tsize:28 + tsize + dsize]


def ours(src, obj):
    if os.path.exists(obj):
        os.unlink(obj)
    r = subprocess.run([AS, '-o', obj, src], capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stdout + r.stderr).strip()
    return read_t2obj(obj), None


def theirs(src, base):
    """GNU text and data bytes; prepends .syntax unified / .thumb when the file lacks them."""
    text = open(src).read()
    head = ''
    if not re.search(r'^\s*\.syntax\s+unified', text, re.M):
        head += '\t.syntax unified\n'
    if not re.search(r'^\s*\.thumb\b', text, re.M):
        head += '\t.thumb\n'
    gsrc = base + '.gnu.s'
    open(gsrc, 'w').write(head + text)
    gobj = base + '.gnu.o'
    r = subprocess.run([gnu('as')] + GNU_FLAGS + ['-o', gobj, gsrc], capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stdout + r.stderr).strip()
    out = []
    for sec in ('.text', '.data'):
        f = base + sec
        subprocess.run([gnu('objcopy'), '-O', 'binary', '-j', sec, gobj, f], check=True)
        out.append(open(f, 'rb').read() if os.path.exists(f) else b'')
    return tuple(out), None


def first_diff(a, b):
    for i in range(min(len(a), len(b))):
        if a[i] != b[i]:
            return i
    return min(len(a), len(b))


def compare(name, sec, a, b):
    if a == b:
        return True
    i = first_diff(a, b)
    print('FAIL %s %s: sizes ours %d gnu %d, first difference at 0x%x' % (name, sec, len(a), len(b), i))
    lo = max(0, i - 8) & ~1
    print('  ours %s' % a[lo:i + 8].hex(' '))
    print('  gnu  %s' % b[lo:i + 8].hex(' '))
    return False


def check_file(src):
    os.makedirs(WORK, exist_ok=True)
    base = os.path.join(WORK, os.path.basename(src)[:-2])
    o, err = ours(src, base + '.o')
    if o is None:
        print('FAIL %s: build/host/as: %s' % (src, err))
        return False
    g, err = theirs(src, base)
    if g is None:
        print('FAIL %s: GNU as: %s' % (src, err))
        return False
    ok = compare(src, '.text', o[0], g[0]) & compare(src, '.data', o[1], g[1])
    if ok:
        print('ok %s: %d bytes text, %d bytes data equal to GNU as' % (src, len(o[0]), len(o[1])))
    return ok


def check_error(src):
    os.makedirs(WORK, exist_ok=True)
    exp = src[:-2] + '.expected'
    want = open(exp).read().splitlines()[0]
    obj = os.path.join(WORK, 'error.o')
    if os.path.exists(obj):
        os.unlink(obj)
    r = subprocess.run([AS, '-o', obj, src], capture_output=True, text=True)
    lines = (r.stdout + r.stderr).splitlines()
    got = lines[0] if lines else ''
    if r.returncode != 1 or got != want or os.path.exists(obj):
        print('FAIL %s: status %d, output file %s, first line %r, expected %r'
              % (src, r.returncode, 'written' if os.path.exists(obj) else 'absent', got, want))
        return False
    print('ok %s: %s' % (src, got))
    return True


# ------------------------------------------------------------------ corpus
LINE_RE = re.compile(r'^\s*([0-9a-f]+):\t([0-9a-f ]+?)\s*\t(\S+)(?:\t(.*))?$')
BRANCH_RE = re.compile(r'^(b|bl|b(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al))(\.[nw])?$')


def build_corpus():
    """root/bin/*.bin plus compiler/tests/*.c compiled with compiler/cc into build/corpus/."""
    out = os.path.join(ROOT, 'build', 'corpus')
    os.makedirs(out, exist_ok=True)
    bins = sorted(glob.glob(os.path.join(ROOT, 'root', 'bin', '*.bin')))
    for t in sorted(glob.glob(os.path.join(ROOT, 'compiler', 'tests', '*.c'))):
        b = os.path.join(out, os.path.basename(t)[:-2] + '.bin')
        r = subprocess.run([os.path.join(ROOT, 'compiler', 'cc'), '-I', os.path.dirname(t), '-o', b, t],
                           capture_output=True, text=True, cwd=ROOT)
        if r.returncode != 0:
            print('corpus: cannot compile %s: %s' % (t, r.stdout.strip()))
            continue
        bins.append(b)
    return bins


def to_asm(code, work):
    """Disassemble code bytes into assembly; returns (source, instructions, deferred)."""
    raw = work + '.code'
    open(raw, 'wb').write(code)
    dis = subprocess.run([gnu('objdump'), '-D', '-b', 'binary', '-m', 'arm', '-M', 'force-thumb', raw],
                         capture_output=True, text=True, check=True).stdout
    insns = []                  # (addr, bytes, mnemonic, operands)
    for l in dis.splitlines():
        m = LINE_RE.match(l)
        if not m:
            continue
        addr = int(m.group(1), 16)
        hw = m.group(2).split()
        b = b''.join(bytes.fromhex(h)[::-1] for h in hw)    # objdump shows halfwords/words big end first
        ops = (m.group(4) or '').split('@')[0].strip()
        insns.append((addr, b, m.group(3), ops))
    starts = set(a for a, _, _, _ in insns)
    targets = set()
    for a, b, mn, ops in insns:
        if BRANCH_RE.match(mn) and re.match(r'^0x[0-9a-f]+$', ops):
            t = int(ops, 16)
            if t in starts:
                targets.add(t)
    lines = []
    n = 0
    deferred = 0
    raw = 0
    for a, b, mn, ops in insns:
        if a in targets:
            lines.append('L_%x:' % a)
        raw_bytes = False
        if mn.startswith('v') or mn.startswith('.') or 'UNDEFINED' in mn or mn.startswith('<'):
            raw_bytes = True
            if mn.startswith('v'):
                deferred += 1
            else:
                raw += 1
        elif BRANCH_RE.match(mn):
            t = int(ops, 16) if re.match(r'^0x[0-9a-f]+$', ops) else None
            if t is None or t not in targets:
                raw_bytes = True
                raw += 1
            else:
                ops = 'L_%x' % t
        if mn == 'ldmia.w' and ops.startswith('sp!, '):
            mn, ops = 'pop.w', ops[5:]
        elif mn == 'stmdb' and ops.startswith('sp!, '):
            mn, ops = 'push.w', ops[5:]
        if raw_bytes:
            for i in range(0, len(b), 2):
                lines.append('\t.short\t0x%02x%02x' % (b[i + 1], b[i]) if i + 1 < len(b) else '\t.byte\t0x%02x' % b[i])
        else:
            lines.append('\t%s\t%s' % (mn, ops))
            n += 1
    return '\t.syntax unified\n\t.thumb\n\t.text\n' + '\n'.join(lines) + '\n', n, deferred, raw


def check_corpus():
    bins = build_corpus()
    os.makedirs(os.path.join(WORK, 'corpus'), exist_ok=True)
    total = 0
    deferred = 0
    rawtotal = 0
    bad = 0
    for f in bins:
        d = open(f, 'rb').read()
        csize = struct.unpack('<i', d[4:8])[0]
        code = d[20:20 + csize]
        work = os.path.join(WORK, 'corpus', os.path.basename(os.path.dirname(f)) + '-' + os.path.basename(f)[:-4])
        src, n, nd, nr = to_asm(code, work)
        open(work + '.s', 'w').write(src)
        o, err = ours(work + '.s', work + '.o')
        g, gerr = theirs(work + '.s', work)
        ok = True
        if o is None:
            print('FAIL %s: build/host/as: %s' % (f, err))
            ok = False
        elif not compare(f + ' (ours)', 'code', o[0], code):
            ok = False
        if g is None:
            print('FAIL %s: GNU as: %s' % (f, gerr))
            ok = False
        elif not compare(f + ' (gnu)', 'code', g[0], code):
            ok = False
        if not ok:
            bad += 1
        total += n
        deferred += nd
        rawtotal += nr
    print('corpus: %d files, %d instructions reproduced, %d deferred (FPU, Task 4), '
          '%d other lines as .short (undecodable or branch to a non-instruction), %d failed'
          % (len(bins), total, deferred, rawtotal, bad))
    return bad == 0


def main(argv):
    if len(argv) > 1 and argv[1] == '--corpus':
        return 0 if check_corpus() else 1
    if len(argv) > 1 and argv[1] == '--errors':
        ok = True
        for f in argv[2:]:
            ok = check_error(f) and ok
        return 0 if ok else 1
    if len(argv) < 2:
        print(__doc__.strip())
        return 2
    ok = True
    for f in argv[1:]:
        ok = check_file(f) and ok
    return 0 if ok else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv))
