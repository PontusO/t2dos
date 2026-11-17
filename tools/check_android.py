#!/usr/bin/env python3
"""Check the Android terminal (android/): its JVM unit tests, and BoardE2ETest against the real
shell and edit under qemu (sh.bin in host_linux with telnetd on a free port).

  check_android.py      (needs make, make host's qemu-arm, and a full JDK 17+: JAVA_HOME, else
                         /usr/lib/jvm/java-17-openjdk-amd64)

Prints `ok: android` and exits 0 when everything passes.
"""
import os, socket, subprocess, sys, time

TOOLS = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(TOOLS)
sys.path.insert(0, TOOLS)
import check_telnetd as ct                     # PcConsole, Reader, PW


def main():
    subprocess.run(['make', '-s'], check=True, cwd=ROOT, stdout=subprocess.DEVNULL)
    subprocess.run(['make', '-s', 'host'], check=True, cwd=ROOT, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    mtab = open(os.path.join(ROOT, 'lib', 'mtab.c'), 'rb').read().decode('latin-1')
    last = [l for l in mtab.split('\n') if l.strip()][-1].expandtabs(8).rstrip()
    if len(last) > 79: print('check_android: the last line of lib/mtab.c is too long for the test'); return 1
    s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
    con = ct.PcConsole()
    try:
        con.r.wait(b'/> ', name='shell')
        con.type('telnetd on %d\r' % port); time.sleep(0.3); con.type(ct.PW + '\r'); time.sleep(0.3); con.type(ct.PW + '\r')
        con.r.wait(b'telnetd on port %d' % port, name='telnetd on')
        con.type('cp /lib/mtab.c /tmp/e2e.txt\r'); con.r.wait(b'/> ', name='cp')
        con.type('rm /tmp/e2e3.txt\r'); con.r.wait(b'/> ', name='rm')
        if ct.bad: return 1
        env = dict(os.environ, T2DOS_E2E_PORT=str(port), T2DOS_E2E_PASS=ct.PW, T2DOS_E2E_LAST=last)
        env.setdefault('JAVA_HOME', '/usr/lib/jvm/java-17-openjdk-amd64')
        r = subprocess.run(['./gradlew', '--offline', '-q', ':app:testDebugUnitTest', '--rerun'], cwd=os.path.join(ROOT, 'android'), env=env)
        con.type('telnetd off\r'); time.sleep(0.5); con.type('exit\r')
    finally:
        con.p.kill()
    if r.returncode != 0: print('check_android: unit or end-to-end tests failed'); return 1
    print('ok: android')
    return 0


if __name__ == '__main__':
    sys.exit(main())
