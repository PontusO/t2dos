#!/usr/bin/env python3
"""Check the network file tools (wifi -s, rcd, rls, get, send) on the PC.

Runs the root/bin/*.bin tools under qemu-arm with host_linux (whose sockets are real)
against tools/fileserver.py on a random local port. The board side's root and the server's
directory are fresh folders under build/s/net/. Each step's output (the port shown as PORT)
and exit status must match. Also sends raw requests that try to leave the server's root.

  check_net.py --pc    (needs `make` and `make host`)
Exit status 0 when every step passes.
"""
import os, random, shutil, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, 'build', 's', 'net')
BOARD = os.path.join(WORK, 'board')
SRV = os.path.join(WORK, 'srv')

# (command, expected output, expected exit status); "@" is 127.0.0.1:PORT
STEPS = [
    ('rls', 'no server: give host[:port] or set one with wifi -s host[:port]\n', 1),
    ('wifi -s @', 'server @\nconnected, ip 127.0.0.1\n', 0),
    ('wifi', 'connected, ip 127.0.0.1\nserver @ /\n', 0),
    ('rls', '   <dir>  projects/\n       4  top.txt\n', 0),
    ('rcd projects/game', '@ /projects/game\n', 0),
    ('rcd', '@ /projects/game\n', 0),
    ('rls', '       6  a.txt\n', 0),
    ('get a.txt', None, 0),
    ('rcd ..', '@ /projects\n', 0),
    ('rcd nothere', 'server: ERR No such file or directory\n', 1),
    ('rcd /projects/game/a.txt', 'server: ERR Not a directory\n', 1),
    ('rcd', '@ /projects\n', 0),
    ('send s.txt', None, 0),
    ('send s.txt ../up.txt', None, 0),
    ('rls .', '   <dir>  game/\n       5  s.txt\n', 0),
    ('rls ../..', '   <dir>  projects/\n       4  top.txt\n       5  up.txt\n', 0),
    ('rls ../../../srv2', 'server: ERR No such file or directory\n', 1),
    ('wifi -s 127.0.0.1:PORT', 'server @\nconnected, ip 127.0.0.1\n', 0),
    ('rcd', '@ /projects\n', 0),
    ('get @ /top.txt t2.txt', None, 0),     # the saved server: its saved dir applies
    ('get @ top.txt t3.txt', 'server: ERR No such file or directory\n', 1),
    ('rls 127.0.0.1:1 x', 'cannot connect to 127.0.0.1:1\n', 1),
    ('rls nosuchhost.local', None, 1),       # taken as a host, so it fails to resolve
    ('get', 'usage: get [host[:port]] remotefile [localfile]\n', 1),
    ('rcd @ /', '@ /\n', 0),
    ('get projects/game/a.txt b.txt', None, 0),
]
FILES = {'a.txt': b'hello\n', 'b.txt': b'hello\n', 't2.txt': b'top\n'}
SERVER_FILES = {'projects/s.txt': b'sent\n', 'up.txt': b'sent\n'}
ESCAPES = ['GET ../srv2/secret', 'GET /../srv2/secret', 'LS ..', 'PUT ../srv2/x 1']


def content(path):
    try: return open(path, 'rb').read()
    except OSError: return None


def main(argv):
    if '--pc' not in argv:
        print(__doc__); return 2
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(os.path.join(SRV, 'projects', 'game'))
    os.makedirs(os.path.join(WORK, 'srv2'))
    os.makedirs(os.path.join(BOARD, 'tmp'))
    shutil.copytree(os.path.join(ROOT, 'root', 'bin'), os.path.join(BOARD, 'bin'))
    open(os.path.join(SRV, 'projects', 'game', 'a.txt'), 'w').write('hello\n')
    open(os.path.join(SRV, 'top.txt'), 'w').write('top\n')
    open(os.path.join(WORK, 'srv2', 'secret'), 'w').write('secret\n')
    open(os.path.join(BOARD, 's.txt'), 'w').write('sent\n')
    port = random.randint(20000, 29999)
    server = subprocess.Popen([sys.executable, os.path.join(ROOT, 'tools', 'fileserver.py'), SRV, str(port)],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    hp = '127.0.0.1:%d' % port
    bad = 0
    try:
        for _ in range(50):                    # wait until the server listens
            try: socket.create_connection(('127.0.0.1', port), 0.2).close(); break
            except OSError: time.sleep(0.1)
        for cmd, want, status in STEPS:
            args = cmd.replace('@', hp).replace('PORT', str(port)).split()
            r = subprocess.run(['qemu-arm', os.path.join(ROOT, 'host_linux'), '-r', BOARD, '/bin/%s.bin' % args[0]] + args[1:],
                               capture_output=True, timeout=30)
            out = r.stdout.decode(errors='replace').replace(hp, '@')
            if r.returncode != status or (want is not None and out != want):
                bad += 1
                print('FAIL %s: exit %d (want %d)\n%s' % (cmd, r.returncode, status, out))
        for name, data in FILES.items():
            if content(os.path.join(BOARD, name)) != data: bad += 1; print('FAIL board file', name)
        for name, data in SERVER_FILES.items():
            if content(os.path.join(SRV, name)) != data: bad += 1; print('FAIL server file', name)
        if content(os.path.join(BOARD, 'etc', 'remote')) != (hp + '\n/\n').encode(): bad += 1; print('FAIL /etc/remote')
        for req in ESCAPES:
            s = socket.create_connection(('127.0.0.1', port), 5)
            s.sendall(req.encode() + b'\n' + (b'x' if req.startswith('PUT') else b''))
            reply = s.recv(100); s.close()
            if reply != b'ERR outside root\n': bad += 1; print('FAIL %s: %r' % (req, reply))
        if os.path.exists(os.path.join(WORK, 'srv2', 'x')): bad += 1; print('FAIL PUT wrote outside the root')
    finally:
        server.kill()
    if bad: print('%d failed' % bad); return 1
    print('ok: %d tool steps, %d escape requests' % (len(STEPS), len(ESCAPES)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
