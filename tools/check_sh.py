#!/usr/bin/env python3
"""Run the shell's test cases (tests/sh/) on the PC.

A case is tests/sh/NAME.in, fed to /bin/sh.bin as its keyboard input under qemu-arm with
host_linux, and NAME.out, the expected console output. The .sh files in tests/sh/ are copied
to root/tmp/sh/ first, so cases run them as /tmp/sh/NAME.sh; files the cases write go there
too, and the folder is removed afterwards. The case ls_times gets a staged root/tmp/lsdir/
of files with fixed mtimes (see stage_ls_times), also removed afterwards. A NAME.env file,
if present, holds VAR=VALUE lines applied to that case's run after the ambient TZ strip
(same convention as check_basic.py).

  check_sh.py --pc [--only NAME]   run the cases (needs `make` and `make host`)
  check_sh.py --update NAME        write NAME.out from the current output (check it by eye)
Exit status 0 when every case passes.
"""
import glob, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, 'tests', 'sh')
TMP = os.path.join(ROOT, 'root', 'tmp', 'sh')
LSDIR = os.path.join(ROOT, 'root', 'tmp', 'lsdir')

# ls_times needs files with fixed mtimes: a sibling of the wiped tmp/sh, staged right before
# that case and removed right after (also under --update, which runs the same cases).
# os.utime writes absolute epochs, so with TZ stripped from the qemu env the golden is fixed.
def stage_ls_times():
    shutil.rmtree(LSDIR, ignore_errors=True)
    os.makedirs(LSDIR)
    for name, size, epoch in (('summer.bin', 1, 1752573600),     # 2025-07-15 10:00 UTC, DST (+02:00) side
                              ('winter.bin', 2, 1763812800),     # 2025-11-22 12:00 UTC, std (+01:00) side
                              ('junk.bin', 3, 4102444800),       # 2100: past MTIME_MAX, junk stamp
                              ('zero.bin', 4, 0),                # epoch 0: pre-DOS-floor
                              ('sentinel.bin', 5, 315532800),    # exactly MTIME_UNKNOWN: lower bound, exclusive
                              ('ceiling.bin', 6, 2147483647)):   # exactly MTIME_MAX: upper bound, exclusive
        p = os.path.join(LSDIR, name)
        with open(p, 'wb') as f:
            f.write(b'x' * size)
        os.utime(p, (epoch, epoch))

def run(name):
    # strip TZ: host_linux honors it at startup, so a dev or CI shell exporting TZ
    # would rewrite the zone legs (mirror of check_toolchain.py). A case NAME.env
    # (VAR=VALUE lines, the check_basic.py convention) is applied after the strip,
    # so an explicit case zone always wins over the ambient one.
    env = {k: v for k, v in os.environ.items() if k != 'TZ'}
    p = os.path.join(CASES, name + '.env')
    if os.path.exists(p):
        for l in open(p).read().split('\n'):
            if '=' in l:
                k, v = l.split('=', 1)
                env[k.strip()] = v.strip()
    if name == 'ls_times':
        stage_ls_times()
    try:
        with open(os.path.join(CASES, name + '.in'), 'rb') as f:
            r = subprocess.run(['qemu-arm', './host_linux', '-r', 'root', '/bin/sh.bin'], stdin=f,
                               capture_output=True, cwd=ROOT, timeout=30, env=env)
    finally:
        if name == 'ls_times':
            shutil.rmtree(LSDIR, ignore_errors=True)
    return r.stdout

# readdir order is filesystem-dependent (ext4 hashes names, FAT returns dir order), so
# for ls_times the listing block between the `/> ls` echo and the `N files, M bytes`
# trailer is compared sorted; the surrounding transcript and every other case stay
# byte-exact. The golden keeps the same sorted shape, so the file still reads naturally.
def sorted_listing(out, name):
    if name != 'ls_times':
        return out
    lines = out.split(b'\n')
    beg = end = None
    for i, l in enumerate(lines):
        if beg is None and l.startswith(b'/> ls '):
            beg = i + 1
        elif beg is not None and l[:1].isdigit() and l.endswith(b' bytes'):
            end = i
            break
    if beg is None or end is None:
        return out
    lines[beg:end] = sorted(lines[beg:end])
    return b'\n'.join(lines)

def main(argv):
    only = [argv[i + 1] for i in range(len(argv) - 1) if argv[i] in ('--only', '--update')]
    if not ({'--pc', '--update'} & set(argv)):
        print(__doc__); return 2
    shutil.rmtree(TMP, ignore_errors=True)
    os.makedirs(TMP)
    for s in glob.glob(os.path.join(CASES, '*.sh')):
        shutil.copy(s, TMP)
    names = sorted(os.path.basename(p)[:-3] for p in glob.glob(os.path.join(CASES, '*.in')))
    bad = 0
    try:
        for n in names:
            if only and n not in only: continue
            out = run(n)
            if '--update' in argv:
                open(os.path.join(CASES, n + '.out'), 'wb').write(out); print('wrote', n + '.out'); continue
            want = open(os.path.join(CASES, n + '.out'), 'rb').read()
            if sorted_listing(out, n) == sorted_listing(want, n): print('PASS', n)
            else:
                bad += 1; print('FAIL', n)
                sys.stdout.write(out.decode('latin-1'))
    finally:
        shutil.rmtree(TMP, ignore_errors=True)
    if '--update' not in argv: print('ok: %d cases' % len(names) if not bad else '%d failed' % bad)
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
