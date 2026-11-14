#!/usr/bin/env python3
"""Check the cc driver (cc runs as and ld) with the multi-file programs in tests/toolchain/.

A case is a list of build steps (cc, ar, mkdir) run in its own directory; it ends with
NAME.bin, whose run must print NAME.expected (the output lines, then "exit N").
tests/toolchain/pipeline_errors/NAME.c or NAME.s with NAME.expected: `cc -o NAME.bin SRC` must
print the tool's message (a /tmp/ccXXXX_N_NAME name stands for any temporary file) and exit with
the code on the last line, write no NAME.bin and leave no /tmp/cc* files.

  check_toolchain.py --pc        build every case with compiler/cc (build/host/as and ld) in
                                 build/toolchain/: silent steps, .bin headers, symbol tables of
                                 the objects, only the needed archive member linked, the error
                                 cases, the driver's own errors, temporary files cleaned
  check_toolchain.py --board     --pc, then upload cc.bin, as.bin, ld.bin, ar.bin and the
                                 sources, build every case with cc ON THE BOARD, run it, compare
                                 with NAME.expected and the board .bin with the PC one; the error
                                 cases on the board
  check_toolchain.py --selfhost  on the board, cc.bin rebuilds cc, as, ld, ar, nm and objdump
                                 from source with plain cc; each must equal root/bin/X.bin. Then
                                 the board-built cc compiles compiler/tests/test4..12 on the
                                 board and their output is compared with gcc -m32 (SSE floats)
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
TESTS = os.path.join(ROOT, 'tests', 'toolchain')
ERRORS = os.path.join(TESTS, 'pipeline_errors')
WORK = os.path.join(ROOT, 'build', 'toolchain')
TOOL = {'cc': os.path.join(ROOT, 'compiler', 'cc'), 'ar': os.path.join(ROOT, 'build', 'host', 'ar'), 'mkdir': 'mkdir'}
NM = os.path.join(ROOT, 'build', 'host', 'nm')
OBJDUMP = os.path.join(ROOT, 'build', 'host', 'objdump')
REMOTE = '/src/tc'
TMP_RE = re.compile(r'^cc[0-9a-f]{1,4}_\d+_.+\.[so]$')  # the driver's temporary files: cc<id>_<n>_<source base>

# name: (sources, steps); every case ends with NAME.bin
CASES = [
    ('c_calls_asm', ['c_calls_asm.c', 'asm_fn.s'],
     [['cc', '-o', 'c_calls_asm.bin', 'c_calls_asm.c', 'asm_fn.s']]),
    ('asm_calls_c', ['asm_calls_c.s', 'c_fn.c'],
     [['cc', '-c', 'c_fn.c'],
      ['cc', '-o', 'asm_calls_c.bin', 'asm_calls_c.s', 'c_fn.o']]),
    ('globals', ['globals_a.c', 'globals_b.c'],
     [['cc', '-o', 'globals.bin', 'globals_a.c', 'globals_b.c']]),
    ('names', ['names.c', 'names_lib.c'],
     [['cc', '-S', 'names_lib.c'],
      ['cc', '-c', '-o', 'nl.o', 'names_lib.s'],
      ['cc', '-o', 'names.bin', 'names.c', 'nl.o']]),
    ('datalimit', ['datalimit_main.c', 'datalimit_big.c'],
     [['cc', '-o', 'datalimit.bin', 'datalimit_main.c', 'datalimit_big.c']]),
    ('datalimit_rev', ['datalimit_big.c', 'datalimit_main.c'],
     [['cc', '-o', 'datalimit_rev.bin', 'datalimit_big.c', 'datalimit_main.c']]),
    ('lib_use', ['lib_use.c', 'lib_square.c', 'lib_cube.c'],
     [['cc', '-c', 'lib_square.c', 'lib_cube.c'],
      ['mkdir', 'lib'],
      ['ar', 'rc', 'lib/libtc.a', 'lib_square.o', 'lib_cube.o'],
      ['cc', '-o', 'lib_use.bin', 'lib_use.c', '-L', 'lib', '-l', 'tc']]),
]

# symbols (nm type letter, name) the objects of a case must have
SYMBOLS = {
    'names_lib.c': {('T', 'sp'), ('T', 'add'), ('T', 'b'), ('T', 'push'), ('D', 'pc'), ('D', 'r1')},
    'names.c': {('T', 'main'), ('D', 'gadd'), ('D', 'gpc'), ('U', 'sp'), ('U', 'add'), ('U', 'b'), ('U', 'push'),
                ('U', 'pc'), ('U', 'r1')},
    'globals_a.c': {('T', 'main'), ('T', 'read_late'), ('T', 'read_late2'), ('D', 'shared'), ('D', 'table'),
                    ('D', 'late'), ('U', 'counter'), ('U', 'name'), ('U', 'greeting'), ('U', 'bump')},
    'globals_b.c': {('T', 'bump'), ('D', 'big'), ('D', 'counter'), ('D', 'name'), ('D', 'greeting'),
                    ('U', 'shared'), ('U', 'table')},
    'c_calls_asm.c': {('T', 'main'), ('T', 'twice'), ('U', 'asm_val'), ('U', 'asm_sub3'), ('U', 'asm_get'),
                      ('U', 'asm_apply')},
}


def run(cmd, cwd, env=None):
    exe = TOOL.get(cmd[0], cmd[0])
    r = subprocess.run([exe] + cmd[1:], cwd=cwd, capture_output=True, text=True, env=env)
    return r.returncode, r.stdout + r.stderr


def tmp_files():
    try:
        return set(f for f in os.listdir('/tmp') if TMP_RE.match(f))
    except OSError:
        return set()


def norm(text):
    return re.sub(r'/tmp/cc[0-9a-f]{1,4}_', '/tmp/ccXXXX_', text)


def bin_header(path):
    b = open(path, 'rb').read()
    code, data, zero, entry = struct.unpack('<4i', b[4:20])
    return (code, data, entry), len(b) == 20 + code + data and b[:4] == b'T2CC' and zero == 0


def nm_syms(path):
    r = subprocess.run([NM, path], capture_output=True, text=True)
    out = set()
    for l in r.stdout.splitlines():
        parts = l.split()
        if len(parts) >= 2 and len(parts[-2]) == 1:
            out.add((parts[-2], parts[-1]))
    return out


def fail(msg):
    print('FAIL ' + msg)
    return False


def build_case(name, srcs, steps):
    wd = os.path.join(WORK, name)
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    for s in srcs:
        shutil.copyfile(os.path.join(TESTS, s), os.path.join(wd, s))
    for st in steps:
        code, out = run(st, wd)
        if code != 0 or out:
            return fail('%s: %s: exit %d, output %r' % (name, ' '.join(st), code, out))
    hdr, good = bin_header(os.path.join(wd, name + '.bin'))
    if not good:
        return fail('%s: bad .bin header %r' % (name, hdr))
    return True


def check_symbols():
    """cc -c each source alone and check its symbol table and relocations"""
    ok = True
    wd = os.path.join(WORK, 'syms')
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    for src, want in sorted(SYMBOLS.items()):
        shutil.copyfile(os.path.join(TESTS, src), os.path.join(wd, src))
        code, out = run(['cc', '-c', '-mshort-data', src], wd)
        if code != 0 or out:
            ok = fail('symbols %s: cc -c exit %d, %r' % (src, code, out))
            continue
        got = nm_syms(os.path.join(wd, src[:-2] + '.o'))
        if not want <= got:
            ok = fail('symbols %s: missing %r (nm has %r)' % (src, sorted(want - got), sorted(got)))
    # extern references are movw/movt pairs to the symbol; with -mshort-data the reference to
    # late after its definition is the short addw form
    r = subprocess.run([OBJDUMP, os.path.join(wd, 'globals_a.o')], capture_output=True, text=True).stdout
    for sym, n in (('counter', None), ('late', 1)):      # None: at least one
        for kind in ('R_DATA_MOVW', 'R_DATA_MOVT'):
            k = len(re.findall(r'\b%s %s\+0\b' % (kind, sym), r))
            if (n is None and k < 1) or (n is not None and k != n):
                ok = fail('globals_a.o: %d %s relocations to %s, want %s' % (k, kind, sym, n or 'some'))
    # without -mshort-data, -c cannot know where the data lands: every data reference is movw/movt
    code, out = run(['cc', '-c', '-o', 'ga_long.o', 'globals_a.c'], wd)
    r = subprocess.run([OBJDUMP, os.path.join(wd, 'ga_long.o')], capture_output=True, text=True).stdout
    if code != 0 or re.search(r'\bR_DATA_(ADDW|MOVW16)\b', r) or not re.search(r'\bR_DATA_MOVT late\+0\b', r):
        ok = fail('globals_a.o without -mshort-data: data references are not all movw/movt')
    code, out = run(['cc', '-S', '-o', 'ga.s', 'globals_a.c'], wd)
    asm = open(os.path.join(wd, 'ga.s')).read() if code == 0 else ''
    if '#:lower16:counter' not in asm or '#:upper16:counter' not in asm or 'add.w\tr0, r9, r1' not in asm:
        ok = fail('globals_a.s: no movw/movt/add.w for the extern counter')
    return ok


def check_lib():
    """only lib_square.o of libtc.a is linked: the code size is lib_use.o + lib_square.o"""
    from check_link import layout
    wd = os.path.join(WORK, 'lib_use')
    code, out = run(['cc', '-c', '-mshort-data', 'lib_use.c'], wd)   # first object: as cc lib_use.c
    if code != 0 or out:
        return fail('lib_use: cc -c lib_use.c: %r' % out)
    hdr, good = bin_header(os.path.join(wd, 'lib_use.bin'))
    want = layout(wd, ['lib_use.o', 'lib_square.o'])
    if layout(wd, ['lib_use.o', 'lib_square.o', 'lib_cube.o']) == want:
        return fail('lib_use: the test needs lib_cube to change the size')
    if hdr != want:
        return fail('lib_use: header %r, want %r (lib_cube.o must not be linked)' % (hdr, want))
    code, out = run(['ar', 't', 'lib/libtc.a'], wd)
    if out != 'lib_square.o\nlib_cube.o\n':
        return fail('lib_use: ar t: %r' % out)
    return True


def check_errors_pc():
    ok = True
    wd = os.path.join(WORK, 'errors')
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(wd)
    for e in sorted(glob.glob(os.path.join(ERRORS, '*.expected'))):
        name = os.path.basename(e)[:-9]
        src = [f for f in os.listdir(ERRORS) if os.path.splitext(f)[0] == name and f[-2:] in ('.c', '.s')][0]
        shutil.copyfile(os.path.join(ERRORS, src), os.path.join(wd, src))
        lines = open(e).read().rstrip('\n').split('\n')
        want, status = '\n'.join(lines[:-1]) + '\n', int(lines[-1].split()[1])
        before = tmp_files()
        code, out = run(['cc', '-o', name + '.bin', src], wd)
        left = tmp_files() - before
        if code != status or norm(out) != want:
            ok = fail('error %s: exit %d, output %r, want exit %d, %r' % (name, code, out, status, want))
        if os.path.exists(os.path.join(wd, name + '.bin')):
            ok = fail('error %s: %s.bin was written' % (name, name))
        if left:
            ok = fail('error %s: temporary files left: %r' % (name, sorted(left)))
    return ok


def check_driver_pc():
    """the driver's own errors, -l search, default output, -c naming, a tool's exit code"""
    ok = True
    wd = os.path.join(WORK, 'driver')
    shutil.rmtree(wd, ignore_errors=True)
    os.makedirs(os.path.join(wd, 'sub'))
    for s in ('globals_a.c', 'globals_b.c', 'asm_fn.s', 'lib_use.c', 'lib_square.c'):
        shutil.copyfile(os.path.join(TESTS, s), os.path.join(wd, s))
    lib = os.path.join(WORK, 'lib_use', 'lib', 'libtc.a')
    if not os.path.exists(lib):
        return fail('driver: needs the lib_use case built first')
    shutil.copyfile(lib, os.path.join(wd, 'sub', 'libtc.a'))
    open(os.path.join(wd, 'notes.txt'), 'w').write('x\n')

    def expect(cmd, want, status, cwd=wd, env=None):
        code, out = run(cmd, cwd, env)
        if code != status or norm(out) != want:
            return fail('driver %s: exit %d, output %r, want exit %d, %r' % (' '.join(cmd), code, out, status, want))
        return True

    usage = ('usage: cc [-o out] [-S|-c] [-O] [-mshort-data] [-I dir] [-D name[=v]] [-L dir] [-l name] '
             'file.c|file.s|file.o|file.a...\n')
    ok = expect(['cc'], usage, 1) and ok
    ok = expect(['cc', 'notes.txt'], 'cc: notes.txt: unknown file type\n', 1) and ok
    ok = expect(['cc', '-c', '-o', 'x.o', 'globals_a.c', 'globals_b.c'], 'cc: -o with -c needs exactly one input\n', 1) and ok
    ok = expect(['cc', '-S', 'asm_fn.s'], 'cc: -S needs .c inputs\n', 1) and ok
    ok = expect(['cc', '-o', 'x.bin', 'lib_use.c', '-l', 'nosuch'], 'cc: library nosuch not found\n', 1) and ok
    for f in ('x.o', 'x.bin', 'asm_fn.o'):
        if os.path.exists(os.path.join(wd, f)):
            ok = fail('driver: %s was written after an error' % f)
    # default output a.bin, equal to the globals case's .bin
    ok = expect(['cc', 'globals_a.c', 'globals_b.c'], '', 0) and ok
    if open(os.path.join(wd, 'a.bin'), 'rb').read() != open(os.path.join(WORK, 'globals', 'globals.bin'), 'rb').read():
        ok = fail('driver: a.bin differs from globals.bin')
    # -l NAME found in the current directory (no -L), and -lNAME
    for cmd in (['cc', '-o', 'y.bin', '../lib_use.c', '-l', 'tc'], ['cc', '-o', 'y.bin', '../lib_use.c', '-ltc']):
        ok = expect(cmd, '', 0, os.path.join(wd, 'sub')) and ok
        if open(os.path.join(wd, 'sub', 'y.bin'), 'rb').read() != open(os.path.join(WORK, 'lib_use', 'lib_use.bin'), 'rb').read():
            ok = fail('driver: %s differs from lib_use.bin' % ' '.join(cmd))
    # -c and -S write NAME.o / NAME.s in the current directory
    ok = expect(['cc', '-c', '../lib_square.c', '../asm_fn.s'], '', 0, os.path.join(wd, 'sub')) and ok
    ok = expect(['cc', '-S', '../lib_square.c'], '', 0, os.path.join(wd, 'sub')) and ok
    for f in ('lib_square.o', 'asm_fn.o', 'lib_square.s'):
        if not os.path.exists(os.path.join(wd, 'sub', f)):
            ok = fail('driver: -c/-S did not write sub/%s' % f)
    # a tool's exit code is cc's, and the temporary files go ($T2_TOOLS names the tools)
    fake = os.path.join(wd, 'fake')
    os.makedirs(fake)
    open(os.path.join(fake, 'as'), 'w').write('#!/bin/sh\necho "fake as: $*" | sed "s|/tmp/cc[0-9a-f]*_|/tmp/ccXXXX_|g"\nexit 3\n')
    os.chmod(os.path.join(fake, 'as'), 0o755)
    env = dict(os.environ, T2_TOOLS=fake)
    before = tmp_files()
    ok = expect(['cc', '-o', 'z.bin', 'lib_square.c'],
                'fake as: -o /tmp/ccXXXX_1_lib_square.o /tmp/ccXXXX_1_lib_square.s\n', 3, wd, env) and ok
    if tmp_files() - before:
        ok = fail('driver: temporary files left after a failing as: %r' % sorted(tmp_files() - before))
    # the data limit (README): cc picks each reference's form from where ld puts the data, so the
    # big data first still links (the datalimit_rev case runs it). An object compiled with
    # -mshort-data and linked after it has an addw past 4095: ld names the global and the
    # object, and no .bin is left
    for s in ('datalimit_main.c', 'datalimit_big.c'):
        shutil.copyfile(os.path.join(TESTS, s), os.path.join(wd, s))
    ok = expect(['cc', '-c', '-mshort-data', 'datalimit_main.c'], '', 0) and ok
    open(os.path.join(wd, 'dl.bin'), 'w').write('an older output\n')
    ok = expect(['cc', '-o', 'dl.bin', 'datalimit_big.c', 'datalimit_main.o'],
                'ld: data offset of counter (datalimit_main.o) is 5000, too far for addw\n', 1) and ok
    if os.path.exists(os.path.join(wd, 'dl.bin')):
        ok = fail('driver: dl.bin is still there after the ld error')
    return ok


def check_pc():
    os.makedirs(WORK, exist_ok=True)
    before = tmp_files()
    ok = True
    n = 0
    for name, srcs, steps in CASES:
        ok = build_case(name, srcs, steps) and ok
        n += 1
    ok = check_symbols() and ok
    ok = check_lib() and ok
    ok = check_errors_pc() and ok
    ok = check_driver_pc() and ok
    left = tmp_files() - before
    if left:
        ok = fail('temporary files left in /tmp: %r' % sorted(left))
    print('%s: %d toolchain cases, %d error cases, symbols, archive, driver' %
          ('ok' if ok else 'FAILED', n, len(glob.glob(os.path.join(ERRORS, '*.expected')))))
    return ok


# ------------------------------------------------------------------ board
class Remote:
    def __init__(self):
        import board
        self.board = board
        self.b = board.Board()

    def put(self, local, remote):
        self.b.put(local, remote)

    def fetch(self, remote):
        return self.b.fetch(remote)

    def run(self, cmd, wait=60.0):
        """output lines and exit code of a shell command (sh prints "[exit N, T ms]" when N != 0)"""
        b = self.b
        b.s.reset_input_buffer()
        b._write(cmd.encode() + b'\r')
        raw = b._read(wait, 0.3, True).decode('latin-1')
        lines = [l.rstrip() for l in raw.replace('\r\n', '\n').replace('\r', '\n').split('\n')]
        if not lines or not lines[0].strip().endswith(cmd.strip()) or not self.board.PROMPT_RE.match(lines[-1]):
            raise self.board.BoardError('no answer from the board: it may have hung, reset it (got %r)' % raw[-120:])
        out = []
        code = 0
        for l in lines[1:]:
            if self.board.PROMPT_RE.match(l):
                continue
            if self.board.EXIT_RE.match(l):
                code = int(l[6:].split(',')[0])
                continue
            out.append(l)
        while out and not out[-1]:
            out.pop()
        return out, code

    def wipe(self, d):
        """remove directory d and everything in it (left from an earlier run)"""
        out, code = self.run('ls ' + d, 10.0)
        if code != 0:
            return
        for l in out:
            parts = l.split()
            if len(parts) == 2 and parts[0] == '<dir>':
                self.wipe('%s/%s' % (d, parts[1].rstrip('/')))
            elif len(parts) == 2 and parts[0].isdigit():
                self.run('rm %s/%s' % (d, parts[1]), 10.0)
        self.run('rmdir ' + d, 10.0)

    def tmp_files(self):
        out, code = self.run('ls /tmp', 10.0)
        names = set()
        for l in out:
            parts = l.split()
            if len(parts) == 2 and parts[0].isdigit() and TMP_RE.match(parts[1]):
                names.add(parts[1])
        return names


def put_tools(r):
    for t in ('cc', 'as', 'ld', 'ar'):
        r.put(os.path.join(ROOT, 'root', 'bin', t + '.bin'), '/bin/%s.bin' % t)


def check_board():
    r = Remote()
    put_tools(r)
    r.run('mkdir /src')
    r.wipe(REMOTE)
    r.run('mkdir ' + REMOTE)
    ok = True
    before = r.tmp_files()
    for name, srcs, steps in CASES:
        good = True
        d = '%s/%s' % (REMOTE, name)
        r.run('mkdir ' + d)
        for s in srcs:
            r.put(os.path.join(TESTS, s), '%s/%s' % (d, s))
        r.run('cd ' + d)
        for st in steps:
            out, code = r.run(' '.join(st), 120.0)
            if out or code != 0:
                good = fail('%s on the board: %s: exit %d, %r' % (name, ' '.join(st), code, out))
                break
        if good:
            got = r.fetch('%s/%s.bin' % (d, name))
            pc = open(os.path.join(WORK, name, name + '.bin'), 'rb').read()
            if got != pc:
                good = fail('%s: the board .bin (%d bytes) differs from the PC .bin (%d bytes)' % (name, len(got), len(pc)))
        if good:
            want = open(os.path.join(TESTS, name + '.expected')).read().rstrip('\n').split('\n')
            out, code = r.run('%s/%s.bin' % (d, name), 20.0)
            out.append('exit %d' % code)
            if out != want:
                good = fail('%s: board output %r, want %r' % (name, out, want))
        r.run('cd /')
        print('%s %s' % ('ok' if good else 'FAIL', name))
        ok = ok and good
    # the error cases on the board
    d = REMOTE + '/errors'
    r.run('mkdir ' + d)
    for e in sorted(glob.glob(os.path.join(ERRORS, '*.expected'))):
        name = os.path.basename(e)[:-9]
        src = [f for f in os.listdir(ERRORS) if os.path.splitext(f)[0] == name and f[-2:] in ('.c', '.s')][0]
        r.put(os.path.join(ERRORS, src), '%s/%s' % (d, src))
        lines = open(e).read().rstrip('\n').split('\n')
        r.run('cd ' + d)
        out, code = r.run('cc -o %s.bin %s' % (name, src), 60.0)
        listing, _ = r.run('ls ' + d, 10.0)
        r.run('cd /')
        got = [norm(l) for l in out] + ['exit %d' % code]
        good = True
        if got != lines:
            good = fail('error %s on the board: %r, want %r' % (name, got, lines))
        if any(l.split()[-1] == name + '.bin' for l in listing if l.split()):
            good = fail('error %s on the board: %s.bin was written' % (name, name))
        print('%s error %s' % ('ok' if good else 'FAIL', name))
        ok = ok and good
    left = r.tmp_files() - before
    if left:
        ok = fail('temporary files left in /tmp on the board: %r' % sorted(left))
    return ok


SELFHOST = [('cc', 'compiler/cc.c'), ('as', 'assembler/as.c'), ('ld', 'linker/ld.c'),
            ('ar', 'binutils/ar.c'), ('nm', 'binutils/nm.c'), ('objdump', 'binutils/objdump.c')]


def check_selfhost():
    """the toolchain on the board rebuilds itself byte-identically, then compiles the tests"""
    import time
    r = Remote()
    put_tools(r)
    r.run('mkdir /src')
    r.run('mkdir /lib')
    for f in ('sys.h', 'lib/t2obj.h', 'lib/t2obj.c', 'lib/net.c'):
        r.put(os.path.join(ROOT, f), '/lib/' + os.path.basename(f))
    ok = True
    for name, src in SELFHOST:
        r.put(os.path.join(ROOT, src), '/src/%s.c' % name)
        cmd = 'cc -I /lib -o /src/%s2.bin /src/%s.c' % (name, name)
        t0 = time.time()
        out, code = r.run(cmd, 300.0)
        dt = time.time() - t0
        if out or code != 0:
            ok = fail('selfhost %s: %s: exit %d, %r' % (name, cmd, code, out))
            continue
        pc = open(os.path.join(ROOT, 'root', 'bin', name + '.bin'), 'rb').read()
        got = r.fetch('/src/%s2.bin' % name)
        same = got == pc
        print('%s $ %s   (%.1f s)  %s2.bin %d bytes, root/bin/%s.bin %d bytes: %s'
              % ('ok' if same else 'FAIL', cmd, dt, name, len(got), name, len(pc), 'IDENTICAL' if same else 'DIFFERENT'))
        ok = ok and same
    # the board-built cc compiles and runs the compiler tests; gcc gives the reference output
    tests = [('test%d' % i, '-D FROM_CMDLINE=7' if i == 10 else '') for i in range(4, 13)]
    bdir = os.path.join(WORK, 'selfhost')
    os.makedirs(bdir, exist_ok=True)
    for name, flags in tests:
        src = os.path.join(ROOT, 'compiler', 'tests', name + '.c')
        native = os.path.join(bdir, name + '.native')
        subprocess.run(['gcc', '-m32', '-msse2', '-mfpmath=sse', '-w'] + flags.split() + ['-o', native, src], check=True)   # 32-bit pointers, IEEE single floats as on the M33
        want = [l.rstrip() for l in subprocess.run([native], capture_output=True, text=True).stdout.split('\n')]
        while want and not want[-1]:
            want.pop()
        r.put(src, '/src/%s.c' % name)
        out, code = r.run('/src/cc2.bin %s -o /src/%s.bin /src/%s.c' % (flags, name, name), 120.0)
        if out or code != 0:
            ok = fail('selfhost %s: cc2 exit %d, %r' % (name, code, out))
            continue
        got, code = r.run('/src/%s.bin' % name, 20.0)
        got = [l.rstrip() for l in got]
        good = got == want
        print('%s %s (built on the board by cc2.bin)' % ('PASS' if good else 'FAIL', name))
        if not good:
            import difflib
            for l in difflib.unified_diff(want, got, 'gcc', 'board', lineterm=''):
                print(l)
        ok = ok and good
    return ok


def main(argv):
    if len(argv) == 2 and argv[1] == '--pc':
        return 0 if check_pc() else 1
    if len(argv) == 2 and argv[1] in ('--board', '--selfhost'):
        if argv[1] == '--board' and not check_pc():
            return 1
        try:
            ok = check_board() if argv[1] == '--board' else check_selfhost()
        except Exception as e:
            print('check_toolchain.py: %s' % e)
            return 1
        print('%s: %s' % ('ok' if ok else 'FAILED', argv[1][2:]))
        return 0 if ok else 1
    print(__doc__.strip())
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
