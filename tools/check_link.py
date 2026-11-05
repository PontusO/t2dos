#!/usr/bin/env python3
"""Check the linker ld with the programs in tests/link/.

A test NAME has NAME.expected and its inputs NAME.s or NAME_*.s (assembled with as) and
NAME.o or NAME_*.o (used as they are), linked in file name order.
  tests/link/NAME.expected          a run test: the exact output lines, then "exit N"
  tests/link/errors/NAME.expected   an error test: the exact output of ld, which must exit 1
                                    and leave no output file (an older one is removed)

  check_link.py --pc      assemble and link every test with build/host/as and build/host/ld in
                          build/link/NAME/: error tests give their message, run tests link
                          silently into a .bin whose header and layout match the objects
  check_link.py --board   upload root/bin/as.bin, root/bin/ld.bin and the run tests' sources,
                          assemble, link and run them on the board, compare output and exit
                          code with NAME.expected and the board's .bin with the PC's
"""
import glob
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
TESTS = os.path.join(ROOT, 'tests', 'link')
WORK = os.path.join(ROOT, 'build', 'link')
AS = os.path.join(ROOT, 'build', 'host', 'as')
LD = os.path.join(ROOT, 'build', 'host', 'ld')
AR = os.path.join(ROOT, 'build', 'host', 'ar')
NM = os.path.join(ROOT, 'build', 'host', 'nm')
ARCHIVE_TESTS = ('ar_chain',)       # NAME.expected files that check_ar and the board's archive run own
REMOTE = '/src/lk'
SYM_GLOBAL = 1
SYM_INIT = 2


def inputs(d, name):
    """the input files of test name in directory d, in link order"""
    fs = []
    for ext in ('.s', '.o'):
        fs += glob.glob(os.path.join(d, name + ext)) + glob.glob(os.path.join(d, name + '_*' + ext))
    return sorted(fs, key=os.path.basename)


def objname(f):
    return os.path.splitext(os.path.basename(f))[0] + '.o'


def prepare(d, name):
    """assemble or copy the inputs into build/link/name/; returns (dir, object names) or None"""
    wd = os.path.join(WORK, name)
    os.makedirs(wd, exist_ok=True)
    objs = []
    for f in inputs(d, name):
        o = os.path.join(wd, objname(f))
        if f.endswith('.o'):
            shutil.copyfile(f, o)
        else:
            r = subprocess.run([AS, '-o', o, f], capture_output=True, text=True)
            if r.returncode:
                print('FAIL %s: as %s: %s' % (name, f, (r.stdout + r.stderr).strip()))
                return None
        objs.append(objname(f))
    if not objs:
        print('FAIL %s: no inputs' % name)
        return None
    return wd, objs


def ld(wd, args):
    r = subprocess.run([LD] + args, cwd=wd, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def expect_error(name, wd, args, want, out):
    """ld args must exit 1 with output want and leave no file out (an older out is removed)"""
    p = os.path.join(wd, out)
    open(p, 'w').write('an older output\n')
    st, got = ld(wd, args)
    if st == 1 and got == want and not os.path.exists(p):
        return True
    print('FAIL %s: status %d, %s, output %r, want %r' % (name, st, 'output file left' if os.path.exists(p) else 'no output file', got, want))
    return False


def layout(wd, objs):
    """the .bin header (code size, data size, entry) the layout rules give for these objects"""
    from check_asm import parse_t2obj
    text = 0
    data = 0
    inits = 0
    entry = None
    for o in objs:
        t, dd, syms, rels = parse_t2obj(os.path.join(wd, o))
        text = (text + 3) & ~3
        if len(dd):
            data = (data + 3) & ~3      # an object without data takes no alignment
        for (n, sect, value, flags) in syms:
            if n == 'main' and sect == 1 and flags & SYM_GLOBAL:
                entry = text + value
            if flags & SYM_INIT:
                inits += 1
        text += len(t)
        data += len(dd)         # the data size ends with the last object that has data
    if inits:
        text = (text + 1) & ~1
        entry = text
        text += 2 + 4 * inits + 4 + 4
    return text, data, entry


def tool(exe, args, cwd):
    r = subprocess.run([exe] + args, cwd=cwd, capture_output=True, text=True)
    return r.returncode, r.stdout + r.stderr


def bin_header(path):
    b = open(path, 'rb').read()
    code, data, zero, entry = struct.unpack('<4i', b[4:20])
    return b, (code, data, entry), len(b) == 20 + code + data and b[:4] == b'T2CC' and zero == 0


def fail(msg):
    print('FAIL ar: ' + msg)
    return False


def run_in(wd, exe, args, want, status=0):
    st, got = tool(exe, args, wd)
    if st != status or got != want:
        return fail('%s %s: status %d, output %r, want status %d, %r'
                    % (os.path.basename(exe), ' '.join(args), st, got, status, want))
    return True


USAGE = 'usage: ar rc|t|x ARCHIVE [FILE...]\n'


def check_ar():
    """ar, nm on archives and the archive search of ld, on the PC"""
    ok = True
    wd = os.path.join(WORK, 'ar')
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    for n in ('lib_a', 'lib_b', 'lib_c', 'lib_bad', 'main_ar', 'main_bad'):
        st, out = tool(AS, ['-o', n + '.o', os.path.join(TESTS, n + '.s')], wd)
        if st or out:
            return fail('as %s: %s' % (n, out))

    def run(exe, args, want, status=0):
        return run_in(wd, exe, args, want, status)

    # one archive with a, b, c; fa and fb in two archives in either order
    ok = run(AR, ['rc', 'lib.a', 'lib_a.o', 'lib_b.o', 'lib_c.o'], '') and ok
    ok = run(AR, ['t', 'lib.a'], 'lib_a.o\nlib_b.o\nlib_c.o\n') and ok
    ok = run(AR, ['rc', 'liba.a', 'lib_a.o'], '') and ok
    ok = run(AR, ['rc', 'libb.a', 'lib_b.o'], '') and ok
    ok = run(AR, ['rc', 'libc.a', 'lib_c.o'], '') and ok
    want = layout(wd, ['main_ar.o', 'lib_a.o', 'lib_b.o'])      # lib_c.o is absent
    if layout(wd, ['main_ar.o', 'lib_a.o', 'lib_b.o', 'lib_c.o']) == want:
        ok = fail('the test needs lib_c to change the size')
    first = None
    for name, args in (('one', ['main_ar.o', 'lib.a']), ('ab', ['main_ar.o', 'liba.a', 'libb.a']),
                       ('ba', ['main_ar.o', 'libb.a', 'liba.a']), ('cba', ['main_ar.o', 'libc.a', 'libb.a', 'liba.a']),
                       ('first', ['lib.a', 'main_ar.o'])):
        if not run(LD, ['-o', name + '.bin'] + args, ''):
            ok = False
            continue
        b, hdr, good = bin_header(os.path.join(wd, name + '.bin'))
        if not good or hdr != want:
            ok = fail('%s: header %r, want %r (lib_c must not be linked)' % (name, hdr, want))
        if first is None:
            first = b
        elif b != first:
            ok = fail('%s: differs from the first .bin' % name)
    # a pulled-in member with an undefined symbol is named ARCHIVE(MEMBER)
    ok = run(AR, ['rc', 'libbad.a', 'lib_bad.o'], '') and ok
    ok = run(LD, ['-o', 'bad.bin', 'main_bad.o', 'libbad.a'],
             'ld: undefined symbol: nowhere (in libbad.a(lib_bad.o))\n', 1) and ok
    if os.path.exists(os.path.join(wd, 'bad.bin')):
        ok = fail('bad.bin was written')
    # a member nobody needs is not linked, so its undefined symbol does not matter
    ok = run(LD, ['-o', 'u.bin', 'main_ar.o', 'liba.a', 'libb.a', 'libbad.a'], '') and ok
    # ar errors
    open(os.path.join(wd, 'text.o'), 'w').write('not an object\n')
    open(os.path.join(wd, 'text.a'), 'w').write('not an archive\n')
    ok = run(AR, ['rc', 'x.a', 'lib_a.o', 'text.o'], 'ar: text.o: not a T2DOS object\n', 1) and ok
    if os.path.exists(os.path.join(wd, 'x.a')):
        ok = fail('x.a was written after an error')
    ok = run(AR, ['rc', 'x.a', 'nofile.o'], 'ar: cannot read nofile.o\n', 1) and ok
    ok = run(AR, ['t', 'text.a'], 'ar: text.a: not a T2DOS archive\n', 1) and ok
    ok = run(AR, ['x', 'text.a'], 'ar: text.a: not a T2DOS archive\n', 1) and ok
    ok = run(AR, ['rc', 'text.a', 'lib_a.o'], 'ar: text.a: not a T2DOS archive\n', 1) and ok
    ok = run(AR, ['t', 'nofile.a'], 'ar: cannot read nofile.a\n', 1) and ok
    ok = run(AR, ['x', 'lib.a', 'nosuch.o'], 'ar: nosuch.o: no such member\n', 1) and ok
    ok = run(AR, ['q', 'lib.a'], USAGE, 1) and ok
    ok = run(AR, [], USAGE, 1) and ok
    # ar x: all members, or the named ones, byte for byte
    for d in ('xa', 'xb', 'xc', 'sub'):
        os.makedirs(os.path.join(wd, d))
    ok = run_in(os.path.join(wd, 'xa'), AR, ['x', '../lib.a'], '') and ok
    ok = run_in(os.path.join(wd, 'xb'), AR, ['x', '../lib.a', 'lib_b.o'], '') and ok
    for n in ('lib_a.o', 'lib_b.o', 'lib_c.o'):
        if open(os.path.join(wd, 'xa', n), 'rb').read() != open(os.path.join(wd, n), 'rb').read():
            ok = fail('ar x: %s differs' % n)
    if os.listdir(os.path.join(wd, 'xb')) != ['lib_b.o']:
        ok = fail('ar x lib.a lib_b.o wrote %r' % os.listdir(os.path.join(wd, 'xb')))
    # ar rc replaces a member in place (matching the base name) and appends new ones
    shutil.copyfile(os.path.join(wd, 'lib_c.o'), os.path.join(wd, 'sub', 'lib_b.o'))
    ok = run(AR, ['rc', 'lib.a', 'sub/lib_b.o', 'lib_bad.o'], '') and ok
    ok = run(AR, ['t', 'lib.a'], 'lib_a.o\nlib_b.o\nlib_c.o\nlib_bad.o\n') and ok
    ok = run_in(os.path.join(wd, 'xc'), AR, ['x', '../lib.a', 'lib_b.o'], '') and ok
    if open(os.path.join(wd, 'xc', 'lib_b.o'), 'rb').read() != open(os.path.join(wd, 'lib_c.o'), 'rb').read():
        ok = fail('the replaced member lib_b.o has the old contents')
    # nm lists the members as ARCHIVE(MEMBER)
    ok = run(NM, ['liba.a'], '00000000 T fa\n         U fb\n') and ok
    ok = run(NM, ['liba.a', 'libb.a'], 'liba.a(lib_a.o):\n00000000 T fa\n         U fb\n'
             'libb.a(lib_b.o):\n00000000 T fb\n00000006 T fz\n') and ok
    ok = run(NM, ['libc.a', 'lib_b.o'], 'libc.a(lib_c.o):\n00000000 T fc\nlib_b.o:\n00000000 T fb\n00000006 T fz\n') and ok
    ok = run(NM, ['text.a'], 'nm: text.a: not a T2DOS object\n', 1) and ok
    ok = check_damaged(wd) and ok
    return ok


def check_damaged(wd):
    """damaged archives are rejected by ar, nm and ld"""
    tool(AR, ['rc', 'dsrc.a', 'lib_a.o', 'lib_b.o', 'lib_c.o'], wd)
    good = open(os.path.join(wd, 'dsrc.a'), 'rb').read()
    ver, nmem, ni, ss = struct.unpack('<4i', good[4:20])
    idx = 20
    mem = idx + 8 * ni
    strt = mem + 12 * nmem
    off0 = struct.unpack('<i', good[mem + 4:mem + 8])[0]

    def put(b, at, v):
        return b[:at] + struct.pack('<i', v) + b[at + 4:]

    cases = [
        ('truncated', good[:len(good) // 2]),
        ('short header', good[:12]),
        ('bad version', put(good, 4, 2)),
        ('huge nindex', put(good, 12, 0x3FFFFFFF)),
        ('negative nmembers', put(good, 8, -1)),
        ('huge str_size', put(good, 16, 0x7FFFFFF0)),
        ('index name out of range', put(good, idx, ss)),
        ('index name negative', put(good, idx, -4)),
        ('index member out of range', put(good, idx + 4, nmem)),
        ('member name out of range', put(good, mem, ss + 8)),
        ('member offset beyond the file', put(good, mem + 4, len(good))),
        ('member offset negative', put(good, mem + 4, -4)),
        ('member size beyond the file', put(good, mem + 8, len(good))),
        ('member offset unaligned', put(good, mem + 4, off0 + 1)),
        ('string table not terminated', good[:strt + ss - 1] + b'x' + good[strt + ss:]),
        ('member is not an object', good[:off0] + b'XXXX' + good[off0 + 4:]),
    ]
    ok = True
    for label, data in cases:
        open(os.path.join(wd, 'dmg.a'), 'wb').write(data)
        want = 'ar: dmg.a: not a T2DOS archive\n'
        for exe, args, want, status in (
                (AR, ['t', 'dmg.a'], 'ar: dmg.a: not a T2DOS archive\n', 1),
                (LD, ['-o', 'dmg.bin', 'main_ar.o', 'dmg.a'], 'ld: dmg.a: not a T2DOS archive\n', 1),
                (NM, ['dmg.a'], 'nm: dmg.a: not a T2DOS archive\n', 1)):
            if label == 'member is not an object':
                if exe == AR:
                    continue        # ar t reads only the names
                if exe == NM:
                    want = ('nm: dmg.a(lib_a.o): not a T2DOS object\ndmg.a(lib_b.o):\n00000000 T fb\n00000006 T fz\n'
                            'dmg.a(lib_c.o):\n00000000 T fc\n')
            st, got = tool(exe, args, wd)
            if st != status or got != want:
                ok = fail('damaged (%s): %s status %d, output %r, want %r' % (label, os.path.basename(exe), st, got, want))
    return ok


def check_pc():
    ok = True
    n = 0
    for e in sorted(glob.glob(os.path.join(TESTS, 'errors', '*.expected'))):
        name = os.path.basename(e)[:-9]
        p = prepare(os.path.join(TESTS, 'errors'), name)
        if not p:
            ok = False
            continue
        wd, objs = p
        ok = expect_error(name, wd, ['-o', name + '.bin'] + objs, open(e).read(), name + '.bin') and ok
        n += 1
    # missing input, and the default output name a.bin with an archive argument
    wd = os.path.join(WORK, 'misc')
    os.makedirs(wd, exist_ok=True)
    ok = expect_error('missing', wd, ['-o', 'x.bin', 'nofile.o'], 'ld: cannot read nofile.o\n', 'x.bin') and ok
    open(os.path.join(wd, 'lib.a'), 'wb').write(b'T2AR' + b'\0' * 16)
    ok = expect_error('archive', wd, ['lib.a'], 'ld: lib.a: not a T2DOS archive\n', 'a.bin') and ok
    # a symbol value past the end of its section: not a T2DOS object
    src = os.path.join(WORK, 'noentry', 'noentry.o')
    b = bytearray(open(src, 'rb').read())
    tsize, dsize, nsyms = struct.unpack('<3i', b[8:20])
    at = 28 + tsize + dsize                    # the first symbol (start, a text label)
    b[at + 8:at + 12] = struct.pack('<i', tsize + 2)
    open(os.path.join(wd, 'past.o'), 'wb').write(bytes(b))
    ok = expect_error('symbol past its section', wd, ['-o', 'p.bin', 'past.o'], 'ld: past.o: not a T2DOS object\n', 'p.bin') and ok
    # -e: noentry.o has no main but links with its start as the entry (offset 0)
    shutil.copyfile(os.path.join(WORK, 'noentry', 'noentry.o'), os.path.join(wd, 'noentry.o'))
    st, got = ld(wd, ['-o', 'e.bin', '-e', 'start', 'noentry.o'])
    if st != 0 or got or open(os.path.join(wd, 'e.bin'), 'rb').read()[4:20] != struct.pack('<4i', 2, 0, 0, 0):
        print('FAIL -e start: status %d, output %r' % (st, got))
        ok = False
    n += 4
    for e in sorted(glob.glob(os.path.join(TESTS, '*.expected'))):
        name = os.path.basename(e)[:-9]
        if name in ARCHIVE_TESTS:
            continue
        p = prepare(TESTS, name)
        if not p:
            ok = False
            continue
        wd, objs = p
        st, got = ld(wd, ['-o', name + '.bin'] + objs)
        if st != 0 or got:
            print('FAIL %s: ld status %d, output %r' % (name, st, got))
            ok = False
            continue
        b = open(os.path.join(wd, name + '.bin'), 'rb').read()
        magic = b[:4]
        code, data, zero, entry = struct.unpack('<4i', b[4:20])
        want = layout(wd, objs)
        if magic != b'T2CC' or zero != 0 or len(b) != 20 + code + data or (code, data, entry) != want:
            print('FAIL %s: header %r %d %d %d %d (file %d bytes), want code, data, entry %r'
                  % (name, magic, code, data, zero, entry, len(b), want))
            ok = False
            continue
        n += 1
        if name == 'dsz' and data != 7:     # R18: 1 byte, 3 bytes (at 4), no data: 7, not 8
            print('FAIL dsz: data size %d, want 7' % data)
            ok = False
    ok = check_ar() and ok
    n += 1
    print('%s: %d link tests' % ('ok' if ok else 'FAILED', n))
    return ok


def check_board():
    import board
    b = board.Board()
    ok = True
    b.put(os.path.join(ROOT, 'root', 'bin', 'as.bin'), '/bin/as.bin')
    b.put(os.path.join(ROOT, 'root', 'bin', 'ld.bin'), '/bin/ld.bin')
    b.run('mkdir ' + REMOTE)

    def run(cmd, wait):
        """output lines and exit code of a shell command (sh prints "[exit N, T ms]" when N != 0)"""
        b.s.reset_input_buffer()
        b._write(cmd.encode() + b'\r')
        raw = b._read(wait, 0.3, True).decode('latin-1')
        lines = [l.rstrip() for l in raw.replace('\r\n', '\n').replace('\r', '\n').split('\n')]
        if not lines or not lines[0].strip().endswith(cmd.strip()) or not board.PROMPT_RE.match(lines[-1]):
            raise board.BoardError('no answer from the board: it may have hung, reset it (got %r)' % raw[-120:])
        out = []
        code = 0
        for l in lines[1:]:
            if board.PROMPT_RE.match(l):
                continue
            if board.EXIT_RE.match(l):
                code = int(l[6:].split(',')[0])
                continue
            out.append(l)
        while out and not out[-1]:
            out.pop()
        return out, code

    for e in sorted(glob.glob(os.path.join(TESTS, '*.expected'))):
        name = os.path.basename(e)[:-9]
        if name in ARCHIVE_TESTS:
            continue
        want = open(e).read().rstrip('\n').split('\n')
        objs = []
        good = True
        for f in inputs(TESTS, name):
            base = os.path.splitext(os.path.basename(f))[0]
            b.put(f, '%s/%s' % (REMOTE, os.path.basename(f)))
            if f.endswith('.s'):
                out, code = run('as -o %s/%s.o %s/%s.s' % (REMOTE, base, REMOTE, base), 30.0)
                if out or code != 0:
                    print('FAIL %s: as %s on the board: exit %r, %r' % (name, base, code, out))
                    good = False
            objs.append('%s/%s.o' % (REMOTE, base))
        if good:
            out, code = run('ld -o %s/%s.bin %s' % (REMOTE, name, ' '.join(objs)), 30.0)
            if out or code != 0:
                print('FAIL %s: ld on the board: exit %r, %r' % (name, code, out))
                good = False
        if good:
            pc = open(os.path.join(WORK, name, name + '.bin'), 'rb').read()
            got = b.fetch('%s/%s.bin' % (REMOTE, name))
            if got != pc:
                print('FAIL %s: the board .bin (%d bytes) differs from the PC .bin (%d bytes)' % (name, len(got), len(pc)))
                good = False
        if good:
            out, code = run('%s/%s.bin' % (REMOTE, name), 10.0)
            out.append('exit %r' % code)
            if out != want:
                print('FAIL %s: board output %r, want %r' % (name, out, want))
                good = False
        print('%s %s' % ('ok' if good else 'FAIL', name))
        ok = ok and good
    good = check_board_ar(b, run)
    print('%s ar_chain' % ('ok' if good else 'FAIL'))
    return ok and good


def check_board_ar(b, run):
    """ar on the board builds the archives; ld on the board links and runs the ar_chain cases"""
    b.put(os.path.join(ROOT, 'root', 'bin', 'ar.bin'), '/bin/ar.bin')
    wd = os.path.join(WORK, 'ar')
    want = open(os.path.join(TESTS, 'ar_chain.expected')).read().rstrip('\n').split('\n')
    good = True

    def step(cmd, wait=30.0):
        nonlocal good
        out, code = run(cmd, wait)
        if out or code != 0:
            print('FAIL ar_chain: %s: exit %r, %r' % (cmd, code, out))
            good = False
        return good

    for n in ('lib_a', 'lib_b', 'lib_c', 'main_ar'):
        b.put(os.path.join(TESTS, n + '.s'), '%s/%s.s' % (REMOTE, n))
        step('as -o %s/%s.o %s/%s.s' % (REMOTE, n, REMOTE, n))
    if not good:
        return False
    step('ar rc %s/lib.a %s/lib_a.o %s/lib_b.o %s/lib_c.o' % ((REMOTE,) * 4))
    step('ar rc %s/liba.a %s/lib_a.o' % ((REMOTE,) * 2))
    step('ar rc %s/libb.a %s/lib_b.o' % ((REMOTE,) * 2))
    if not good:
        return False
    # the board's archives equal the PC's
    for n in ('liba', 'libb'):
        if b.fetch('%s/%s.a' % (REMOTE, n)) != open(os.path.join(wd, n + '.a'), 'rb').read():
            print('FAIL ar_chain: the board %s.a differs from the PC one' % n)
            good = False
    out, code = run('ar t %s/lib.a' % REMOTE, 10.0)
    if out != ['lib_a.o', 'lib_b.o', 'lib_c.o'] or code != 0:
        print('FAIL ar_chain: ar t on the board: exit %r, %r' % (code, out))
        good = False
    for name, libs in (('one', ['lib.a']), ('ab', ['liba.a', 'libb.a']), ('ba', ['libb.a', 'liba.a'])):
        if not step('ld -o %s/%s.bin %s/main_ar.o %s' % (REMOTE, name, REMOTE, ' '.join('%s/%s' % (REMOTE, l) for l in libs))):
            continue
        if b.fetch('%s/%s.bin' % (REMOTE, name)) != open(os.path.join(wd, name + '.bin'), 'rb').read():
            print('FAIL ar_chain: the board %s.bin differs from the PC .bin' % name)
            good = False
        if name != 'ba':
            out, code = run('%s/%s.bin' % (REMOTE, name), 10.0)
            out.append('exit %r' % code)
            if out != want:
                print('FAIL ar_chain: %s.bin on the board: %r, want %r' % (name, out, want))
                good = False
    return good


def main(argv):
    if len(argv) == 2 and argv[1] == '--pc':
        return 0 if check_pc() else 1
    if len(argv) == 2 and argv[1] == '--board':
        if not check_pc():
            return 1
        try:
            return 0 if check_board() else 1
        except Exception as e:
            print('check_link.py: %s' % e)
            return 1
    print(__doc__.strip())
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
