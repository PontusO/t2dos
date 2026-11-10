#!/usr/bin/env python3
"""Run the shell's test cases (tests/sh/) on the PC.

A case is tests/sh/NAME.in, fed to /bin/sh.bin as its keyboard input under qemu-arm with
host_linux, and NAME.out, the expected console output. The .sh files in tests/sh/ are copied
to root/tmp/sh/ first, so cases run them as /tmp/sh/NAME.sh; files the cases write go there
too, and the folder is removed afterwards.

  check_sh.py --pc [--only NAME]   run the cases (needs `make` and `make host`)
  check_sh.py --update NAME        write NAME.out from the current output (check it by eye)
Exit status 0 when every case passes.
"""
import glob, os, shutil, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
CASES = os.path.join(ROOT, 'tests', 'sh')
TMP = os.path.join(ROOT, 'root', 'tmp', 'sh')

def run(name):
    with open(os.path.join(CASES, name + '.in'), 'rb') as f:
        r = subprocess.run(['qemu-arm', './host_linux', '-r', 'root', '/bin/sh.bin'], stdin=f,
                           capture_output=True, cwd=ROOT, timeout=30)
    return r.stdout

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
            if out == want: print('PASS', n)
            else:
                bad += 1; print('FAIL', n)
                sys.stdout.write(out.decode('latin-1'))
    finally:
        shutil.rmtree(TMP, ignore_errors=True)
    if '--update' not in argv: print('ok: %d cases' % len(names) if not bad else '%d failed' % bad)
    return 1 if bad else 0

if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
