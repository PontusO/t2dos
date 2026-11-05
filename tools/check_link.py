#!/usr/bin/env python3
"""Check the linker ld with the programs in tests/link/.

A test NAME has NAME.expected and its inputs NAME.s or NAME_*.s (assembled with as) and
NAME.o or NAME_*.o (used as they are), linked in file name order.
  tests/link/NAME.expected          a run test: the exact output lines, then "exit N"
  tests/link/errors/NAME.expected   an error test: the exact output of ld, which must exit 1
                                    and leave no output file

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
    """ld args must exit 1 with output want and leave no file out"""
    p = os.path.join(wd, out)
    if os.path.exists(p):
        os.remove(p)
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
        data = (data + 3) & ~3
        for (n, sect, value, flags) in syms:
            if n == 'main' and sect == 1 and flags & SYM_GLOBAL:
                entry = text + value
            if flags & SYM_INIT:
                inits += 1
        text += len(t)
        data += len(dd)
    if inits:
        text = (text + 1) & ~1
        entry = text
        text += 2 + 4 * inits + 4 + 4
    return text, data, entry


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
    ok = expect_error('archive', wd, ['lib.a'], 'ld: lib.a: not a T2DOS object\n', 'a.bin') and ok
    # -e: noentry.o has no main but links with its start as the entry (offset 0)
    shutil.copyfile(os.path.join(WORK, 'noentry', 'noentry.o'), os.path.join(wd, 'noentry.o'))
    st, got = ld(wd, ['-o', 'e.bin', '-e', 'start', 'noentry.o'])
    if st != 0 or got or open(os.path.join(wd, 'e.bin'), 'rb').read()[4:20] != struct.pack('<4i', 2, 0, 0, 0):
        print('FAIL -e start: status %d, output %r' % (st, got))
        ok = False
    n += 3
    for e in sorted(glob.glob(os.path.join(TESTS, '*.expected'))):
        name = os.path.basename(e)[:-9]
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
    return ok


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
