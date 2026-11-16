#!/usr/bin/env python3
"""Check the network filesystem (/mnt/fs) and fileserver.py's session mode.

  check_netfs.py --pc    (needs `make` and `make host`)

Starts tools/fileserver.py on a random local port with a fresh root under
build/s/netfs/srv. Sends raw session requests and checks the replies, then runs
the board tools under qemu-arm with host_linux against it.
Exit status 0 when every check passes.
"""
import os, random, shutil, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, 'build', 's', 'netfs')
SRV = os.path.join(WORK, 'srv')

bad = 0


def fail(msg):
    global bad
    bad += 1
    print('FAIL', msg)


def recv_line(s):
    b = b''
    while not b.endswith(b'\n'):
        c = s.recv(1)
        if not c: break
        b += c
    return b.decode(errors='replace').rstrip('\n')


def recv_n(s, n):
    b = b''
    while len(b) < n:
        c = s.recv(n - len(b))
        if not c: break
        b += c
    return b


def session(port):
    s = socket.create_connection(('127.0.0.1', port), 5)
    s.settimeout(5)
    s.sendall(b'SESSION\n')
    r = recv_line(s)
    if r != 'OK': fail('SESSION answered %r' % r)
    return s


def ask(s, line, data=b''):
    """Sends one request; returns (reply line, payload). The payload is read for 'OK n' replies to READ and LS."""
    s.sendall(line.encode() + b'\n' + data)
    r = recv_line(s)
    payload = b''
    if (line.startswith('READ') or line.startswith('LS')) and r.startswith('OK '):
        payload = recv_n(s, int(r.split()[1]))
    return r, payload


def srv_file(name):
    try: return open(os.path.join(SRV, name), 'rb').read()
    except OSError: return None


def check_server(port):
    s = session(port)
    want = [
        ('STAT top.txt', 'OK f 4', b''),
        ('STAT projects', 'OK d', b''),
        ('READ 1 2 top.txt', 'OK 2', b'op'),
        ('READ 0 4096 top.txt', 'OK 4', b'top\n'),
        ('READ 10 5 top.txt', 'OK 0', b''),
    ]
    for line, reply, payload in want:
        r, p = ask(s, line)
        if r != reply or p != payload: fail('%s: %r %r' % (line, r, p))
    r, _ = ask(s, 'STAT nothere')
    if not r.startswith('ERR'): fail('STAT nothere: %r' % r)
    r, _ = ask(s, 'WRITE 2 3 new.txt', b'abc')
    if r != 'OK' or srv_file('new.txt') != b'\0\0abc': fail('WRITE 2 3 new.txt: %r %r' % (r, srv_file('new.txt')))
    r, _ = ask(s, 'WRITE 0 1 my file.txt', b'x')
    if r != 'OK' or srv_file('my file.txt') != b'x': fail('WRITE with a space: %r' % r)
    r, _ = ask(s, 'TRUNC 1 new.txt')
    if r != 'OK' or srv_file('new.txt') != b'\0': fail('TRUNC 1 new.txt: %r' % r)
    r, _ = ask(s, 'TRUNC 0 made.txt')
    if r != 'OK' or srv_file('made.txt') != b'': fail('TRUNC 0 made.txt: %r' % r)
    open(os.path.join(SRV, 'ren.txt'), 'w').write('old')
    r, _ = ask(s, 'RENAME new.txt', b'ren.txt\n')
    if r != 'OK' or srv_file('ren.txt') != b'\0' or srv_file('new.txt') is not None: fail('RENAME: %r' % r)
    for line in ('MKDIR d', 'RM ren.txt', 'RMDIR d'):
        r, _ = ask(s, line)
        if r != 'OK': fail('%s: %r' % (line, r))
    if os.path.exists(os.path.join(SRV, 'd')) or srv_file('ren.txt') is not None: fail('RM/RMDIR left files')
    r, _ = ask(s, 'RMDIR projects')
    if not r.startswith('ERR'): fail('RMDIR of a full directory: %r' % r)
    r, p = ask(s, 'LS projects')
    if r != 'OK %d' % len(p) or p != b'   <dir>  game/\n': fail('LS projects: %r %r' % (r, p))
    r, _ = ask(s, 'READ 0 5000 top.txt')
    if not r.startswith('ERR'): fail('READ over 4096: %r' % r)
    s.close()
    s = session(port)                            # a WRITE over 4096 is refused before its data
    s.sendall(b'WRITE 0 5000 x\n')
    r = recv_line(s)
    if not r.startswith('ERR'): fail('WRITE over 4096: %r' % r)
    s.close()
    for line, data in [('STAT ../x', b''), ('READ 0 1 ../x', b''), ('WRITE 0 1 ../x', b'y'), ('TRUNC 0 ../x', b''),
                       ('RM ../x', b''), ('RMDIR ../x', b''), ('RENAME top.txt', b'../x\n'), ('RENAME ../x', b'y\n')]:
        s = session(port)
        r, _ = ask(s, line, data)
        s.close()
        if r != 'ERR outside root': fail('%s: %r' % (line, r))
    if os.path.exists(os.path.join(WORK, 'x')): fail('a request wrote outside the root')
    s = socket.create_connection(('127.0.0.1', port), 5)   # the one-request mode is unchanged
    s.sendall(b'GET top.txt\n')
    r = recv_n(s, 10); s.close()
    if r != b'OK 4\ntop\n': fail('GET top.txt: %r' % r)


def main(argv):
    if '--pc' not in argv:
        print(__doc__); return 2
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(os.path.join(SRV, 'projects', 'game'))
    open(os.path.join(SRV, 'top.txt'), 'w').write('top\n')
    open(os.path.join(SRV, 'projects', 'game', 'a.txt'), 'w').write('hello\n')
    port = random.randint(20000, 29999)
    server = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'fileserver.py'), SRV, str(port)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    try:
        for _ in range(50):
            try: socket.create_connection(('127.0.0.1', port), 0.2).close(); break
            except OSError: time.sleep(0.1)
        check_server(port)
    finally:
        server.kill()
    if bad: print('%d failed' % bad); return 1
    print('ok: server session requests')
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
