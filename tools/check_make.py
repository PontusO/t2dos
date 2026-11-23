#!/usr/bin/env python3
"""Check the make tool (src/userland/make.c) on the PC and on the board.

make reads a target-line build file ("name: [-flags] prereqs": leading -tokens
are that target's build flags, -I takes the next token, # starts a comment, a
prereq naming another target makes the line a collection) and runs one cc call
per .bin target (as -o first for .s prereqs). host_linux has no spawn, so the
PC mode verifies only the -n dry-run plan over a scratch tree staged under
root/ (which host_linux maps as /); the board mode runs the real build and
requires byte equality with root/bin/*.bin. -v (echo each command before
running it) lives on that same real-run path, so it is board-observable only.

  check_make.py --pc                     dry-run plan cases + rm/mkdir tool cases (needs `make` and `make host`)
  check_make.py --board [--tools NAMES]  build on the board, byte-compare each
Exit status 0 when every case passes.
"""
import os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MKTMP = os.path.join(ROOT, 'root', 'tmp', 'make')     # staged: the Makefile(s)
MKSRC = os.path.join(ROOT, 'root', 'src', 'mk')       # staged: scratch sources
RMT = os.path.join(ROOT, 'root', 'tmp', 'rmt')        # staged: the rm tool's test tree
MKT = os.path.join(ROOT, 'root', 'tmp', 'mkt')        # host-side tree the mkdir tool builds
CREATED = []                                          # files this checker wrote (cleanup)


def stage(path, text=''):
    full = os.path.join(ROOT, 'root', path.lstrip('/'))
    if os.path.exists(full):
        return
    os.makedirs(os.path.dirname(full), exist_ok=True)
    with open(full, 'w') as f:
        f.write(text)
    CREATED.append(full)


def run_make(args):
    r = subprocess.run(['qemu-arm', './host_linux', '-r', 'root', '/bin/make.bin'] + args,
                       capture_output=True, text=True, cwd=ROOT, timeout=30)
    return r.returncode, r.stdout


def run_tool(name, args):
    r = subprocess.run(['qemu-arm', './host_linux', '-r', 'root', '/bin/%s.bin' % name] + args,
                       capture_output=True, text=True, cwd=ROOT, timeout=30)
    return r.returncode, r.stdout


def write_makefile(text, name='Makefile'):
    os.makedirs(MKTMP, exist_ok=True)
    with open(os.path.join(MKTMP, name), 'w') as f:
        f.write(text)


def expect_ok(name, mk, args, lines):
    write_makefile(mk)
    rc, out = run_make(['-n', '-f', '/tmp/make/Makefile'] + args)
    got = [l for l in out.splitlines() if l.strip()]
    if rc != 0 or got != lines:
        fail(name, 'want %r, got exit %d out %r' % (lines, rc, out))
    else:
        ok(name)


def expect_err(name, mk, args, *needles):
    write_makefile(mk)
    rc, out = run_make(['-n', '-f', '/tmp/make/Makefile'] + args)
    if rc == 0 or any(n not in out for n in needles):
        fail(name, 'want exit != 0 naming %r, got exit %d out %r' % (needles, rc, out))
    else:
        ok(name)


PASSED = []


def ok(name):
    PASSED.append(name)


def fail(name, msg):
    print('FAIL %s: %s' % (name, msg))
    sys.exit(1)


def pc_cases():
    for f in ('/src/mk/a.c', '/src/mk/b.c', '/src/mk/util.s', '/src/mk/hdr.h'):
        stage(f, 'int main(void) { return 0; }\n')

    # 1: one cc line; flags right after the program, -o target, then sources.
    # headers are prereqs (checked, future timestamp input) but never sources.
    expect_ok('plan-simple',
              '/src/mk/bin.bin: -I /src/mk /src/mk/a.c /src/mk/hdr.h\n', ['/src/mk/bin.bin'],
              ['/bin/cc.bin -I /src/mk -o /src/mk/bin.bin /src/mk/a.c'])

    # 2: a .s prereq assembles first; the .o it names becomes the cc input.
    expect_ok('plan-asm',
              '/src/mk/asm.bin: /src/mk/util.s\n', ['/src/mk/asm.bin'],
              ['/bin/as.bin -o /src/mk/util.o /src/mk/util.s',
               '/bin/cc.bin -o /src/mk/asm.bin /src/mk/util.o'])

    # 3: collections recurse in order and a target already built plans once.
    coll = '/src/mk/bin.bin: -I /src/mk /src/mk/a.c\n/src/mk/asm.bin: /src/mk/util.s\ntools: /src/mk/bin.bin /src/mk/asm.bin\n'
    expect_ok('plan-collection', coll, ['tools'],
              ['/bin/cc.bin -I /src/mk -o /src/mk/bin.bin /src/mk/a.c',
               '/bin/as.bin -o /src/mk/util.o /src/mk/util.s',
               '/bin/cc.bin -o /src/mk/asm.bin /src/mk/util.o'])
    expect_ok('plan-plan-once', coll, ['tools', '/src/mk/bin.bin'],
              ['/bin/cc.bin -I /src/mk -o /src/mk/bin.bin /src/mk/a.c',
               '/bin/as.bin -o /src/mk/util.o /src/mk/util.s',
               '/bin/cc.bin -o /src/mk/asm.bin /src/mk/util.o'])

    # 4-5: grammar errors name the line; make never runs a partial build.
    expect_err('err-no-colon', 'bad /src/mk/a.c\n', [], 'bad')
    expect_err('err-flags-eol', 'x: -I\n', [], 'x')

    # 6: a cycle says cycle (and returns), it never hangs.
    expect_err('err-cycle', 'c1: c2\nc2: c1\n', ['c1'], 'cycle')

    # 7-8: a missing file names file and target; an unknown target says so.
    expect_err('err-missing-file', '/src/mk/m.bin: /src/mk/nope.c\n', ['/src/mk/m.bin'], 'nope.c', 'm.bin')
    expect_err('err-unknown-target', '/src/mk/bin.bin: /src/mk/a.c\n', ['nosuch'], 'nosuch')

    # 9: flags keep their order from the file.
    expect_ok('plan-flag-order',
              '/src/mk/ip.bin: -I /src/mk -O /src/mk/a.c\n', ['/src/mk/ip.bin'],
              ['/bin/cc.bin -I /src/mk -O -o /src/mk/ip.bin /src/mk/a.c'])

    # 10: argv cap (MAXARGS 32): program + flags + -o + target + sources counted
    # exactly. 7 -I pairs (14 words) + 15 sources + 3 fixed = 32 fits, 16 over.
    fit = '/src/mk/cap.bin: ' + ' -I /src/mk' * 7 + (' /src/mk/b.c' * 15) + '\n'
    srcs = ' '.join(['/src/mk/b.c'] * 15)
    expect_ok('cap-exact', fit, ['/src/mk/cap.bin'], ['/bin/cc.bin ' + ' '.join(['-I', '/src/mk'] * 7) +
                                          ' -o /src/mk/cap.bin ' + srcs])
    over = '/src/mk/capo.bin: ' + ' -I /src/mk' * 8 + (' /src/mk/b.c' * 15) + '\n'
    expect_err('cap-over', over, ['/src/mk/capo.bin'], 'capo.bin')

    # 11: the real Makefile.board parses and plans the whole toolchain in order.
    with open(os.path.join(ROOT, 'Makefile.board')) as f:
        board = f.read()
    os.makedirs(MKTMP, exist_ok=True)
    with open(os.path.join(MKTMP, 'Makefile.board'), 'w') as f:
        f.write(board)
    body = '\n'.join(l.split('#')[0] for l in board.splitlines())
    targets = {l.split(':')[0].strip() for l in board.splitlines() if ':' in l}
    for tok in body.replace(':', ' ').replace('\n', ' ').split():
        if tok.startswith('/') and tok not in targets:
            stage(tok, 'int main(void) { return 0; }\n')
    rc, out = run_make(['-n', '-f', '/tmp/make/Makefile.board', 'all'])
    # the cc sources below mirror Makefile.board's /src/cc.bin line (which mirrors the
    # PC Makefile's CC_SRC) - the three lists are one fact, update them together
    want = ['/bin/cc.bin -I /lib -o /src/cc.bin /src/compiler/cc.c /src/compiler/emit.c '
            '/src/compiler/expr.c /src/compiler/fp.c /src/compiler/lex.c /src/compiler/licm.c '
            '/src/compiler/opt.c /src/compiler/regs.c /src/compiler/state.c '
            '/src/compiler/stmt.c /src/compiler/types.c',
            '/bin/cc.bin -I /lib -o /src/as.bin /src/assembler/as.c',
            '/bin/cc.bin -I /lib -o /src/ld.bin /src/linker/ld.c'] + \
           ['/bin/cc.bin -I /lib -o /src/%s.bin /src/binutils/%s.c' % (n, n)
            for n in ('ar', 'nm', 'objdump')] + \
           ['/bin/cc.bin -I /lib -o /src/basic.bin /src/basic/comp.c /src/basic/fmt.c '
            '/src/basic/funcs.c /src/basic/io.c /src/basic/lex.c /src/basic/main.c '
            '/src/basic/vm.c',
            '/bin/cc.bin -I /lib -o /src/sh.bin /src/userland/sh.c']
    got = [l for l in out.splitlines() if l.strip()]
    if rc != 0 or got != want:
        fail('plan-board', 'exit %d, want %r, got %r' % (rc, want, got))
    ok('plan-board')

    tool_cases()


def stage_rmt():
    os.makedirs(os.path.join(RMT, 'sub', 'deep'), exist_ok=True)
    for f in ('/tmp/rmt/a.txt', '/tmp/rmt/sub/b.txt', '/tmp/rmt/sub/deep/c.txt'):
        stage(f, 'x\n')          # stage() skips existing files, recreates deleted ones


def tool_cases():
    # rm -rf removes the whole tree (host view: root/tmp/rmt gone).
    stage_rmt()
    rc, out = run_tool('rm', ['-rf', '/tmp/rmt'])
    if rc != 0 or os.path.isdir(RMT):
        fail('rm-tree', 'want exit 0 and tree gone, got exit %d out %r gone=%s'
             % (rc, out, not os.path.isdir(RMT)))
    ok('rm-tree')

    # a directory without -r: fails, message names it.
    stage_rmt()
    rc, out = run_tool('rm', ['/tmp/rmt/sub'])
    if rc == 0 or 'sub' not in out:
        fail('rm-dir-norec', 'want exit != 0 naming sub, got exit %d out %r' % (rc, out))
    ok('rm-dir-norec')

    # missing path: fails without -f; with -f silent success.
    rc, out = run_tool('rm', ['/tmp/rmt/nope'])
    if rc == 0:
        fail('rm-missing-fail', 'want exit != 0, got exit %d out %r' % (rc, out))
    ok('rm-missing-fail')

    rc, out = run_tool('rm', ['-f', '/tmp/rmt/nope'])
    if rc != 0 or out != '':
        fail('rm-missing-force', 'want exit 0 no output, got exit %d out %r' % (rc, out))
    ok('rm-missing-force')

    # a plain file goes, the rest of the tree stays.
    rc, out = run_tool('rm', ['/tmp/rmt/a.txt'])
    gone = not os.path.exists(os.path.join(RMT, 'a.txt'))
    intact = (os.path.isfile(os.path.join(RMT, 'sub', 'b.txt')) and
              os.path.isdir(os.path.join(RMT, 'sub', 'deep')))
    if rc != 0 or not gone or not intact:
        fail('rm-file', 'want exit 0 gone intact, got exit %d out %r gone=%s intact=%s'
             % (rc, out, gone, intact))
    ok('rm-file')

    # unknown flag: usage line, exit != 0.
    rc, out = run_tool('rm', ['-q', '/tmp'])
    if rc == 0 or 'usage' not in out:
        fail('rm-badflag', 'want exit != 0 with usage, got exit %d out %r' % (rc, out))
    ok('rm-badflag')

    # letters reversed: -fr behaves exactly like -rf (combined-flag parse both ways).
    stage_rmt()
    rc, out = run_tool('rm', ['-fr', '/tmp/rmt'])
    if rc != 0 or os.path.isdir(RMT):
        fail('rm-fr', 'want exit 0 and tree gone, got exit %d out %r gone=%s'
             % (rc, out, not os.path.isdir(RMT)))
    ok('rm-fr')

    # mkdir -p creates every parent (host view: root/tmp/mkt/sub/deep exists).
    shutil.rmtree(MKT, ignore_errors=True)
    rc, out = run_tool('mkdir', ['-p', '/tmp/mkt/sub/deep'])
    if rc != 0 or not os.path.isdir(os.path.join(MKT, 'sub', 'deep')):
        fail('mk-p', 'want exit 0 and mkt/sub/deep, got exit %d out %r exists=%s'
             % (rc, out, os.path.isdir(os.path.join(MKT, 'sub', 'deep'))))
    ok('mk-p')

    # -p again over the same tree: existing directories are not an error.
    rc, out = run_tool('mkdir', ['-p', '/tmp/mkt/sub/deep'])
    if rc != 0 or not os.path.isdir(os.path.join(MKT, 'sub', 'deep')):
        fail('mk-p-exists', 'want exit 0 (idempotent), got exit %d out %r' % (rc, out))
    ok('mk-p-exists')

    # without -p an existing directory fails.
    rc, out = run_tool('mkdir', ['/tmp/mkt'])
    if rc == 0:
        fail('mk-exists-fail', 'want exit != 0, got exit %d out %r' % (rc, out))
    ok('mk-exists-fail')

    # plain mkdir under an existing parent.
    rc, out = run_tool('mkdir', ['/tmp/mkt/x'])
    if rc != 0 or not os.path.isdir(os.path.join(MKT, 'x')):
        fail('mk-plain', 'want exit 0 and mkt/x, got exit %d out %r' % (rc, out))
    ok('mk-plain')

    # without -p a missing parent fails.
    rc, out = run_tool('mkdir', ['/tmp/mkt/ghost/x'])
    if rc == 0:
        fail('mk-nop-parent-fail', 'want exit != 0, got exit %d out %r' % (rc, out))
    ok('mk-nop-parent-fail')

    # a path that does not fit the 200-byte buffer is rejected, not overflowed
    # (sh hands out ~255-char words).
    rc, out = run_tool('mkdir', ['-p', '/tmp/mkt/' + 'a' * 200])
    if rc == 0 or 'too long' not in out:
        fail('mk-toobig', 'want exit != 0 naming too long, got exit %d out %r' % (rc, out))
    ok('mk-toobig')


def cleanup():
    for f in CREATED:
        try:
            os.remove(f)
        except OSError:
            pass
    shutil.rmtree(MKSRC, ignore_errors=True)     # never root/src itself: it holds basic/
    shutil.rmtree(MKTMP, ignore_errors=True)
    shutil.rmtree(RMT, ignore_errors=True)
    shutil.rmtree(MKT, ignore_errors=True)


REPO_OF = {'/lib/sys.h': 'sys.h', '/lib/basic.h': 'src/basic/basic.h', '/lib/cc.h': 'src/compiler/cc.h',
           '/lib/t2obj.c': 'src/lib/t2obj.c', '/lib/t2obj.h': 'src/lib/t2obj.h',
           '/lib/net.c': 'src/lib/net.c', '/lib/mtab.c': 'src/lib/mtab.c',
           '/lib': 'src/lib'}   # -I /lib directory token: maps to the (existing) repo dir


def repo_path(tok):                       # board path -> repo file (check_toolchain layout)
    if tok in REPO_OF:
        return REPO_OF[tok]
    if tok.startswith('/src/'):
        return 'src/' + tok[len('/src/'):]   # board layout fixed: repo sources live under src/
    return tok.lstrip('/')


def board_main(tool_names=None):
    from check_toolchain import Remote
    import time
    r = Remote()
    with open(os.path.join(ROOT, 'Makefile.board')) as f:
        board = f.read()
    body = '\n'.join(l.split('#')[0] for l in board.splitlines())
    targets = {l.split(':')[0].strip() for l in board.splitlines() if ':' in l}
    toks = [t for t in body.replace(':', ' ').replace('\n', ' ').split()
            if t.startswith('/') and t not in targets]
    for d in sorted({os.path.dirname(t) for t in toks} - {'/'}):
        r.run('mkdir ' + d, 10.0)         # "exists" is a fine answer
    r.put(os.path.join(ROOT, 'root', 'bin', 'make.bin'), '/bin/make.bin')
    for t in ('cc', 'as'):                # make runs the toolchain already on the board
        r.put(os.path.join(ROOT, 'root', 'bin', t + '.bin'), '/bin/%s.bin' % t)
    for t in ['/lib/t2obj.c', '/lib/net.c', '/lib/mtab.c'] + toks:   # included/not-listed files too
        src = os.path.join(ROOT, repo_path(t))
        if os.path.isdir(src):                    # -I /lib: a directory token, push nothing
            r.run('mkdir ' + t, 10.0)
            continue
        if not os.path.exists(src):
            fail('board-stage', 'no repo file for board path %s' % t)
        r.put(src, t)
    r.put(os.path.join(ROOT, 'Makefile.board'), '/src/Makefile.board')
    names = tool_names or ['cc', 'as', 'ld', 'ar', 'nm', 'objdump', 'basic', 'sh']
    for n in names:
        t0 = time.time()
        out, code = r.run('/bin/make.bin -f /src/Makefile.board /src/%s.bin' % n, 1200.0)
        if code:
            fail('board-' + n, 'make exited %d: %r' % (code, out[-6:]))
        secs = time.time() - t0
        got = r.fetch('/src/%s.bin' % n)
        with open(os.path.join(ROOT, 'root', 'bin', '%s.bin' % n), 'rb') as f:
            want = f.read()
        if got != want:
            off = next(i for i in range(min(len(got), len(want))) if got[i] != want[i])
            fail('board-' + n, '%s.bin differs: %d vs %d bytes, first diff at %d'
                 % (n, len(got), len(want), off))
        print('PASS board-%s: byte-identical to root/bin, %d bytes, %.0f s' % (n, len(got), secs))
    # a child failure must stop make with a message and the child's code
    r.run('mkdir /src/mkfail', 10.0)
    r.put(b'int main(void) { bogus syntax here; }\n', '/src/mkfail/bad.c')
    r.put(b'/src/mkfail/bad.bin: /src/mkfail/bad.c\n', '/src/Makefile.fail')
    out, code = r.run('/bin/make.bin -f /src/Makefile.fail /src/mkfail/bad.bin', 120.0)
    if code == 0 or not any('failed' in l for l in out):
        fail('board-fail', 'expected a failed cc, exit %d out %r' % (code, out[-6:]))
    print('PASS board-fail: child failure stops make (%d lines, exit %d)' % (len(out), code))
    print('ok: make board')
    return 0


def main(argv):
    if '--board' in argv:
        names = None
        if '--tools' in argv:
            names = argv[argv.index('--tools') + 1].split(',')
        return board_main(names)
    if '--pc' in argv:
        try:
            pc_cases()
            print('ok: %d make plan and tool cases' % len(PASSED))
            return 0
        finally:
            cleanup()
    print(__doc__)
    return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
