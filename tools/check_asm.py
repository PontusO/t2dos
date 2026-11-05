#!/usr/bin/env python3
"""Check build/host/as against GNU as.

  check_asm.py file.s...           assemble each with build/host/as and arm-none-eabi-as,
                                   compare the text and data bytes. R_WORD relocations are
                                   applied as GNU leaves them in its object (S + A for a
                                   local symbol, A for a global or undefined one, sections at
                                   0); any other relocation fails. For GNU, the T2DOS forms
                                   `fnaddr rd, sym` and `movw.16 rd, #x` are rewritten into
                                   movw/movt/add rd, pc and movw rd, #:lower16:x
  check_asm.py --errors file.s...  each file must fail: exit status 1, no output file, and the
                                   first line of output equal to file.expected (next to file.s)
  check_asm.py --relocs file.s     assemble with build/host/as and print the relocations, one
                                   "SECT OFFSET TYPE SYMBOL+ADDEND" line each in object order,
                                   then a blank line and the nm listing of the object
  check_asm.py --inits file.s      print the names of the SYM_INIT (.t2init) symbols, sorted
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


SECTS = {1: 'text', 2: 'data'}
RELOCS = {1: 'R_BL', 2: 'R_BW', 3: 'R_BCOND', 4: 'R_DATA_ADDW', 5: 'R_DATA_MOVW', 6: 'R_DATA_MOVT',
          7: 'R_DATA_MOVW16', 8: 'R_FNADDR', 9: 'R_WORD'}
SYM_GLOBAL = 1
SYM_INIT = 2


def parse_t2obj(path):
    """text, data, symbols [(name, sect, value, flags)], relocations [(sect, off, sym, type, addend)]."""
    b = open(path, 'rb').read()
    magic, version, tsize, dsize, nsyms, nrels, strsize = struct.unpack('<7i', b[:28])
    if magic != T2OBJ_MAGIC:
        raise ValueError('%s: not a T2DOS object' % path)
    p = 28
    text = b[p:p + tsize]
    p += tsize
    data = b[p:p + dsize]
    p += dsize
    raw = [struct.unpack('<4i', b[p + 16 * i:p + 16 * i + 16]) for i in range(nsyms)]
    p += 16 * nsyms
    rels = [struct.unpack('<5i', b[p + 20 * i:p + 20 * i + 20]) for i in range(nrels)]
    p += 20 * nrels
    strs = b[p:p + strsize]
    syms = [(strs[n:strs.index(b'\0', n)].decode(), sect, value, flags) for n, sect, value, flags in raw]
    return text, data, syms, rels


def read_t2obj(path):
    """(text, data) of a T2DOS object, with R_WORD relocations applied as GNU leaves them."""
    text, data, syms, rels = parse_t2obj(path)
    secs = {1: bytearray(text), 2: bytearray(data)}
    for sect, off, sym, typ, addend in rels:
        name, ssect, value, flags = syms[sym]
        if typ != 9:
            raise ValueError('%s: %s at %s+0x%x cannot be compared with GNU'
                             % (path, RELOCS.get(typ, typ), SECTS[sect], off))
        v = addend + (value if ssect != 0 and not (flags & SYM_GLOBAL) else 0)
        secs[sect][off:off + 4] = struct.pack('<I', v & 0xffffffff)
    return bytes(secs[1]), bytes(secs[2])


def ours(src, obj):
    if os.path.exists(obj):
        os.unlink(obj)
    r = subprocess.run([AS, '-o', obj, src], capture_output=True, text=True)
    if r.returncode != 0:
        return None, (r.stdout + r.stderr).strip()
    try:
        return read_t2obj(obj), None
    except ValueError as e:
        return None, str(e)


LABEL = r'^(\s*(?:[\w.$]+:\s*)?)'
FNADDR_RE = re.compile(LABEL + r'fnaddr\s+(\w+)\s*,\s*([^@]*?)\s*(@.*)?$', re.M | re.I)
MOVW16_RE = re.compile(LABEL + r'movw\.16(\s+\w+\s*,\s*)#', re.M | re.I)


def gnu_forms(text):
    """Rewrite the T2DOS pseudo-instructions into GNU instructions with the same bytes."""
    n = [0]

    def fn(m):
        n[0] += 1
        lab = '.Lt2fn%d' % n[0]
        v = '(%s + 1 - (%s + 12))' % (m.group(3), lab)
        return '%s%s: movw %s, #:lower16:%s\n\tmovt %s, #:upper16:%s\n\tadd %s, pc' % (
            m.group(1), lab, m.group(2), v, m.group(2), v, m.group(2))
    return MOVW16_RE.sub(r'\1movw\2#:lower16:', FNADDR_RE.sub(fn, text))


def relocs(src):
    os.makedirs(WORK, exist_ok=True)
    obj = os.path.join(WORK, os.path.basename(src)[:-2] + '.o')
    if os.path.exists(obj):
        os.unlink(obj)
    r = subprocess.run([AS, '-o', obj, src], capture_output=True, text=True)
    if r.returncode != 0:
        print('FAIL %s: build/host/as: %s' % (src, (r.stdout + r.stderr).strip()))
        return 1
    text, data, syms, rels = parse_t2obj(obj)
    for sect, off, sym, typ, addend in rels:
        print('%s %08x %s %s%+d' % (SECTS.get(sect, '?'), off, RELOCS.get(typ, str(typ)), syms[sym][0], addend))
    print()
    sys.stdout.flush()
    return subprocess.run([os.path.join(ROOT, 'build', 'host', 'nm'), obj]).returncode


def inits(src):
    os.makedirs(WORK, exist_ok=True)
    obj = os.path.join(WORK, os.path.basename(src)[:-2] + '.o')
    r = subprocess.run([AS, '-o', obj, src], capture_output=True, text=True)
    if r.returncode != 0:
        print('FAIL %s: build/host/as: %s' % (src, (r.stdout + r.stderr).strip()))
        return 1
    for name in sorted(s[0] for s in parse_t2obj(obj)[2] if s[3] & SYM_INIT):
        print(name)
    return 0


def theirs(src, base):
    """GNU text and data bytes; prepends .syntax unified / .thumb when the file lacks them."""
    text = open(src).read()
    head = ''
    if not re.search(r'^\s*\.syntax\s+unified', text, re.M):
        head += '\t.syntax unified\n'
    if not re.search(r'^\s*\.thumb\b', text, re.M):
        head += '\t.thumb\n'
    gsrc = base + '.gnu.s'
    open(gsrc, 'w').write(head + gnu_forms(text))
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
FPU_RE = re.compile(r'^(v(add|sub|mul|div|neg|sqrt|cmp)\.f32|vcvt\.(f32\.[su]32|[su]32\.f32)|vmov|vmrs|vldr|vstr|vpush|vpop)$')
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
    """Disassemble code bytes into assembly; returns (source, instructions, deferred, raw)."""
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
        if (mn.startswith('v') and not FPU_RE.match(mn)) or mn.startswith('.') or 'UNDEFINED' in mn or mn.startswith('<'):
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
    print('corpus: %d files, %d instructions reproduced, %d deferred (FPU forms as does not support), '
          '%d other lines as .short (undecodable or branch to a non-instruction), %d failed'
          % (len(bins), total, deferred, rawtotal, bad))
    return bad == 0


def main(argv):
    if len(argv) > 1 and argv[1] == '--corpus':
        return 0 if check_corpus() else 1
    if len(argv) == 3 and argv[1] == '--relocs':
        return relocs(argv[2])
    if len(argv) == 3 and argv[1] == '--inits':
        return inits(argv[2])
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
