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
  check_asm.py --objdump-corpus    run build/host/objdump on the corpus programs and on the objects
                                   build/obj/sample.o (tests/obj/mkobj.py) and of tests/asm/{core,fpu,reloc}.s: every
                                   instruction line must equal GNU objdump's mnemonic and operands
                                   for the same bytes (comments dropped); .short only where GNU
                                   finds no instruction or as does not assemble GNU's text to
                                   those bytes; and the converter of --corpus applied to our output
                                   must reassemble with build/host/as to the original bytes
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
BRANCH_RE = re.compile(r'^bl?(eq|ne|cs|cc|hs|lo|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|al)?(\.[nw])?$')


def build_corpus():
    """root/bin/*.bin plus compiler/tests/*.c built with compiler/cc -S, build/host/as and
    build/host/ld into build/corpus/. A program that does not build fails the check."""
    out = os.path.join(ROOT, 'build', 'corpus')
    os.makedirs(out, exist_ok=True)
    bins = sorted(glob.glob(os.path.join(ROOT, 'root', 'bin', '*.bin')))
    for t in sorted(glob.glob(os.path.join(ROOT, 'compiler', 'tests', '*.c'))):
        base = os.path.join(out, os.path.basename(t)[:-2])
        for cmd in ([os.path.join(ROOT, 'compiler', 'cc'), '-S', '-I', os.path.dirname(t), '-o', base + '.s', t],
                    [os.path.join(ROOT, 'build', 'host', 'as'), '-o', base + '.o', base + '.s'],
                    [os.path.join(ROOT, 'build', 'host', 'ld'), '-o', base + '.bin', base + '.o']):
            r = subprocess.run(cmd, capture_output=True, text=True, cwd=ROOT)
            if r.returncode != 0:
                print('corpus: cannot build %s: %s' % (t, (r.stdout + r.stderr).strip()))
                sys.exit(1)
        bins.append(base + '.bin')
    return bins


def gnu_dis(code, work):
    """GNU objdump's disassembly text of code bytes (written to work.code)."""
    raw = work + '.code'
    open(raw, 'wb').write(code)
    return subprocess.run([gnu('objdump'), '-D', '-b', 'binary', '-m', 'arm', '-M', 'force-thumb', raw],
                          capture_output=True, text=True, check=True).stdout


def parse_dis(dis):
    """Instruction lines of objdump output (GNU's or ours): [(addr, bytes, mnemonic, operands)];
    comments after @ and our <SYMBOL+0xOFF> branch annotations are dropped."""
    insns = []
    for l in dis.splitlines():
        m = LINE_RE.match(l)
        if not m:
            continue
        addr = int(m.group(1), 16)
        hw = m.group(2).split()
        b = b''.join(bytes.fromhex(h)[::-1] for h in hw)    # objdump shows halfwords/words big end first
        ops = (m.group(4) or '').split('@')[0].strip()
        ops = re.sub(r'\s*<[^<>]*>$', '', ops)
        insns.append((addr, b, m.group(3), ops))
    return insns


COND = r'(eq|ne|cs|cc|mi|pl|vs|vc|hi|ls|ge|lt|gt|le|hs|lo)?'
REWRITES = [
    # GNU spelling -> the as spelling of the same encoding
    (re.compile(r'^ldmia' + COND + r'\.w$'), re.compile(r'^sp!, (\{.*\})$'), lambda m, o: ('pop%s.w' % (m.group(1) or ''), o.group(1))),
    (re.compile(r'^stmdb' + COND + r'$'), re.compile(r'^sp!, (\{.*\})$'), lambda m, o: ('push%s.w' % (m.group(1) or ''), o.group(1))),
    (re.compile(r'^str' + COND + r'\.w$'), re.compile(r'^(\w+), \[sp, #-4\]!$'), lambda m, o: ('push%s.w' % (m.group(1) or ''), '{%s}' % o.group(1))),
    (re.compile(r'^ldr' + COND + r'\.w$'), re.compile(r'^(\w+), \[sp\], #4$'), lambda m, o: ('pop%s.w' % (m.group(1) or ''), '{%s}' % o.group(1))),
    (re.compile(r'^mov(s?)' + COND + r'\.w$'), re.compile(r'^(\w+), (\w+), (lsl|lsr|asr|ror) #(\d+)$'),
     lambda m, o: ('%s%s%s.w' % (o.group(3), m.group(1), m.group(2) or ''), '%s, %s, #%s' % (o.group(1), o.group(2), o.group(4)))),
    (re.compile(r'^rsb(s?)' + COND + r'$'), re.compile(r'^(\w+), (\w+), #0$'),
     lambda m, o: ('neg%s%s.w' % (m.group(1), m.group(2) or ''), '%s, %s' % (o.group(1), o.group(2)))),
    (re.compile(r'^orn(s?)' + COND + r'$'), re.compile(r'^(\w+), (\w+), #(\d+)$'),
     lambda m, o: ('orr%s%s' % (m.group(1), m.group(2) or ''), '%s, %s, #%d' % (o.group(1), o.group(2), ~int(o.group(3)) & 0xffffffff))),
]


def as_form(mn, ops):
    """An instruction as objdump prints it, in the spelling build/host/as takes."""
    for mre, ore, f in REWRITES:
        m = mre.match(mn)
        o = ore.match(ops) if m else None
        if o:
            return f(m, o)
    return mn, ops


def insns_to_asm(insns):
    """Assembly for disassembled instructions; returns (source, instructions, deferred, raw)."""
    starts = set(a for a, _, _, _ in insns)
    targets = set()
    for a, b, mn, ops in insns:
        if BRANCH_RE.match(mn) and re.match(r'^0x[0-9a-f]+$', ops):
            t = int(ops, 16)
            if t in starts:
                targets.add(t)
    out = []                         # (raw bytes?, mnemonic, operands) per line
    for a, b, mn, ops in insns:
        raw_bytes = False
        if (mn.startswith('v') and not FPU_RE.match(mn)) or mn.startswith('.') or 'UNDEFINED' in mn or mn.startswith('<'):
            raw_bytes = True
        elif BRANCH_RE.match(mn):
            t = int(ops, 16) if re.match(r'^0x[0-9a-f]+$', ops) else None
            if t is None or t not in targets:
                raw_bytes = True
            else:
                ops = 'L_%x' % t
        mn, ops = as_form(mn, ops)
        out.append([raw_bytes, mn, ops])
    # an IT block with a line as bytes cannot be assembled: then the whole block is bytes (an
    # instruction is one line, or two .short lines for a 32-bit encoding)
    def is_it(b):
        return len(b) == 2 and b[1] == 0xBF and b[0] & 15
    for i, (a, b, mn, ops) in enumerate(insns):
        if not is_it(b):
            continue
        j = i + 1
        k = 4
        while not (b[0] & (1 << (4 - k))):
            k -= 1
        for _ in range(k):
            if j < len(insns) and len(insns[j][1]) == 2 and (insns[j][1][1] >> 3) >= 0x1D:
                j += 1
            j += 1
        if out[i][0] or j > len(insns) or any(out[x][0] or is_it(insns[x][1]) for x in range(i + 1, j)):
            for x in range(i, min(j, len(insns))):
                out[x][0] = True
    lines = []
    n = 0
    deferred = 0
    raw = 0
    for (a, b, mn0, ops0), (raw_bytes, mn, ops) in zip(insns, out):
        if a in targets:
            lines.append('L_%x:' % a)
        if raw_bytes:
            if mn0.startswith('v') and not FPU_RE.match(mn0):
                deferred += 1
            else:
                raw += 1
            for i in range(0, len(b), 2):
                lines.append('\t.short\t0x%02x%02x' % (b[i + 1], b[i]) if i + 1 < len(b) else '\t.byte\t0x%02x' % b[i])
        else:
            lines.append('\t%s\t%s' % (mn, ops))
            n += 1
    return '\t.syntax unified\n\t.thumb\n\t.text\n' + '\n'.join(lines) + '\n', n, deferred, raw


def to_asm(code, work):
    """Disassemble code bytes with GNU objdump into assembly; returns (source, instructions, deferred, raw)."""
    return insns_to_asm(parse_dis(gnu_dis(code, work)))


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


# ------------------------------------------------------------------ objdump
OBJDUMP = os.path.join(ROOT, 'build', 'host', 'objdump')
GNU_ANY_RE = re.compile(r'^\s*([0-9a-f]+):\t([0-9a-f]{4}(?: [0-9a-f]{4})?) *\t(.*)$')
OUR_RE = re.compile(r'^ *([0-9a-f]+):\t([0-9a-f]{4}(?: [0-9a-f]{4})?) *\t(\S+)(?:\t(.*))?$')


def norm(ops):
    """Operands without @ / ; comments and with single spaces."""
    return ' '.join(re.split('[@;]', ops)[0].split())


def gnu_all(code, work):
    """Every line of GNU's disassembly: {addr: (bytes, mnemonic, operands, comment)}, mnemonic ''
    for undefined encodings."""
    out = {}
    for l in gnu_dis(code, work).splitlines():
        m = GNU_ANY_RE.match(l)
        if not m:
            continue
        b = b''.join(bytes.fromhex(h)[::-1] for h in m.group(2).split())
        parts = re.split('[@;]', m.group(3), 1)
        mn, _, ops = parts[0].strip().partition('\t')
        comment = parts[1].strip() if len(parts) > 1 else ''
        out[int(m.group(1), 16)] = (b, mn.strip(), norm(ops), comment)
    return out


def our_dis(path):
    """Our objdump's DISASSEMBLY lines of a file: (source text, [(addr, bytes, mn, ops)], errors)."""
    r = subprocess.run([OBJDUMP, path], capture_output=True, text=True)
    errs = []
    if r.returncode != 0:
        errs.append('objdump exit status %d: %s' % (r.returncode, r.stdout.strip()[:200]))
    lines = r.stdout.splitlines()
    if 'DISASSEMBLY:' not in lines or 'DATA:' not in lines:
        return '', [], errs + ['no DISASSEMBLY: / DATA: section']
    part = lines[lines.index('DISASSEMBLY:') + 1:lines.index('DATA:')]
    insns = []
    for l in part:
        m = OUR_RE.match(l)
        if not m:
            errs.append('bad line %r' % l)
            continue
        addr, hexcol, mn, ops = int(m.group(1), 16), m.group(2), m.group(3), m.group(4) or ''
        want = '%8x:\t%-10s\t%s' % (addr, hexcol + ' ' if ' ' in hexcol else hexcol, mn) + ('\t' + ops if ops else '')
        if l != want:
            errs.append('line format %r, expected %r' % (l, want))
        b = b''.join(bytes.fromhex(h)[::-1] for h in hexcol.split())
        insns.append((addr, b, mn, re.sub(r'\s*<[^<>]*>$', '', ops)))
    return '\n'.join(part) + '\n', insns, errs


CONDS = ['eq', 'ne', 'cs', 'cc', 'mi', 'pl', 'vs', 'vc', 'hi', 'ls', 'ge', 'lt', 'gt', 'le', 'al']
IT_RE = re.compile(r'^it([te]{0,3})$')


def it_conds(mn, ops):
    """The conditions of the instructions of an IT block, from GNU's "itte\tCOND"."""
    m = IT_RE.match(mn)
    if not m or ops not in CONDS:
        return None
    c = CONDS.index(ops)
    inv = CONDS[c ^ 1] if c < 14 else ops
    return [ops] + [ops if x == 't' else inv for x in m.group(1)]


def assembles_to(mn, ops, b, work, block=None, at=0):
    """1 if build/host/as assembles the instruction (as GNU prints it, at offset at) to the bytes b;
    inside an IT block (block: the conditions of this and the remaining instructions of the block)
    at the same place in a block of the same conditions, the rest filled with nop. A branch to
    0xT goes to a label at offset T (.space fills the gaps)."""
    mn, ops = as_form(mn, ops)
    items = []                       # (offset, size, text)
    if block:
        items.append((at - 2, 2, '\tit%s\t%s' % (''.join('t' if c == block[0] else 'e' for c in block[1:]), block[0])))
    label = None
    if BRANCH_RE.match(mn) and re.match(r'^0x[0-9a-f]+$', ops):
        label = int(ops, 16)
        ops = 'L_target'
    items.append((at, len(b), '\t%s\t%s' % (mn, ops)))
    for i, c in enumerate(block[1:] if block else []):
        items.append((at + len(b) + 2 * i, 2, '\tnop%s' % c))
    if label is not None:
        items.append((label, 0, 'L_target:'))
    items.sort(key=lambda x: (x[0], x[1]))
    if items[0][0] < 0:
        return False
    t = ''
    pos = 0
    for off, size, text in items:
        if off < pos:
            return False             # a label inside an instruction
        if off > pos:
            t += '\t.space\t%d\n' % (off - pos)
        t += text + '\n'
        pos = off + size
    open(work + '.s', 'w').write('\t.syntax unified\n\t.thumb\n\t.text\n' + t)
    o, err = ours(work + '.s', work + '.o')
    return o is not None and o[0][at:at + len(b)] == b


def check_objdump_file(name, path, code, work, stats):
    """Our objdump of path (whose text/code is code) against GNU's, then the round trip."""
    ok = True
    src, mine, errs = our_dis(path)
    for e in errs[:5]:
        print('FAIL %s: %s' % (name, e))
    if errs:
        return False
    gnu_lines = gnu_all(code, work)
    at = {}
    for a, b, mn, ops in mine:
        if code[a:a + len(b)] != b:
            print('FAIL %s: 0x%x: bytes %s, the file has %s' % (name, a, b.hex(), code[a:a + len(b)].hex()))
            return False
        at[a] = (b, mn, ops)
    if sum(len(b) for _, b, _, _ in mine) != len(code):
        print('FAIL %s: the disassembly covers %d of %d bytes' % (name, sum(len(b) for _, b, _, _ in mine), len(code)))
        return False
    bad = 0
    block = []                       # GNU's IT state: conditions of the rest of the block
    for a in sorted(gnu_lines):
        gb, gmn, gops, gcomment = gnu_lines[a]
        here = block
        block = block[1:]
        if it_conds(gmn, gops):
            block = it_conds(gmn, gops)
        if a not in at:
            print('FAIL %s: 0x%x: no line in our disassembly (GNU: %s %s)' % (name, a, gmn, gops))
            bad += 1
            continue
        b, mn, ops = at[a]
        if mn == '.short':
            covered = 0
            while covered < len(gb) and a + covered in at and at[a + covered][1] == '.short':
                covered += 2
            if covered != len(gb):
                print('FAIL %s: 0x%x: GNU decodes %d bytes (%s %s), ours not as .short' % (name, a, len(gb), gmn, gops))
                bad += 1
            elif gmn == '':
                stats['undefined'] += 1
            elif assembles_to(gmn, gops, gb, work + '-one', here, a):
                print('FAIL %s: 0x%x: .short for %s %s, which as assembles to these bytes' % (name, a, gmn, gops))
                bad += 1
            elif BRANCH_RE.match(gmn) and 'unpredictable' not in gcomment and not here:
                # a branch as bytes only where GNU calls it unpredictable or inside an IT block
                print('FAIL %s: 0x%x: .short for the branch %s %s' % (name, a, gmn, gops))
                bad += 1
            else:
                stats['notas'] += 1
                stats['notas_mn'][gmn] = stats['notas_mn'].get(gmn, 0) + 1
            continue
        if b != gb or mn != gmn or norm(ops) != gops:
            if bad < 10:
                print('FAIL %s: 0x%x %s: ours %s %s, GNU %s %s' % (name, a, b.hex(), mn, ops, gmn, gops))
            bad += 1
            continue
        stats['equal'] += 1
    stats['mismatch'] += bad
    if bad:
        ok = False
    # round trip: the corpus converter on our output, reassembled with build/host/as
    asm, n, nd, nr = insns_to_asm(mine)
    open(work + '.rt.s', 'w').write(asm)
    o, err = ours(work + '.rt.s', work + '.rt.o')
    if o is None:
        print('FAIL %s: round trip: build/host/as: %s' % (name, err))
        ok = False
    elif not compare(name + ' (round trip)', 'code', o[0], code):
        ok = False
    return ok


def check_objdump_corpus():
    bins = build_corpus()
    work = os.path.join(WORK, 'objdump')
    os.makedirs(work, exist_ok=True)
    files = []                       # (name, path, code)
    for f in bins:
        d = open(f, 'rb').read()
        csize = struct.unpack('<i', d[4:8])[0]
        files.append((f, f, d[20:20 + csize]))
    sample = os.path.join(ROOT, 'build', 'obj', 'sample.o')
    if not os.path.exists(sample):
        subprocess.run([sys.executable, os.path.join(ROOT, 'tests', 'obj', 'mkobj.py'), sample], check=True)
    objs = [sample]
    for src in ('core.s', 'fpu.s', 'reloc.s'):
        obj = os.path.join(work, src[:-2] + '.o')
        r = subprocess.run([AS, '-o', obj, os.path.join(ROOT, 'tests', 'asm', src)], capture_output=True, text=True)
        if r.returncode != 0:
            print('FAIL tests/asm/%s: build/host/as: %s' % (src, (r.stdout + r.stderr).strip()))
            return False
        objs.append(obj)
    for obj in objs:
        files.append((obj, obj, parse_t2obj(obj)[0]))
    stats = {'equal': 0, 'notas': 0, 'undefined': 0, 'mismatch': 0, 'notas_mn': {}}
    failed = 0
    for name, path, code in files:
        base = os.path.join(work, os.path.basename(os.path.dirname(path)) + '-' + os.path.basename(path).replace('.', '-'))
        if not check_objdump_file(os.path.relpath(name, ROOT), path, code, base, stats):
            failed += 1
    print('objdump corpus: %d files, %d instructions equal to GNU objdump, %d mismatches; .short: %d '
          'undefined for GNU, %d that as does not assemble (%s); %d files failed (round trip included)'
          % (len(files), stats['equal'], stats['mismatch'], stats['undefined'], stats['notas'],
             ', '.join('%s %d' % kv for kv in sorted(stats['notas_mn'].items())) or 'none', failed))
    return failed == 0


def main(argv):
    if len(argv) > 1 and argv[1] == '--objdump-corpus':
        return 0 if check_objdump_corpus() else 1
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
