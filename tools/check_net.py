#!/usr/bin/env python3
"""Check the network tools (wifi -s, rcd, rls, get, send, rcp, ping) on the PC.

Runs the root/bin/*.bin tools under qemu-arm with host_linux (whose sockets are real)
against tools/fileserver.py on a random local port. The board side's root and the server's
directory are fresh folders under build/s/net/. Each step's output (the port shown as PORT)
and exit status must match. Also sends raw requests that try to leave the server's root.

  check_net.py --pc    (needs `make` and `make host`)
Exit status 0 when every step passes.
"""
import os, random, re, shutil, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, 'build', 's', 'net')
BOARD = os.path.join(WORK, 'board')
SRV = os.path.join(WORK, 'srv')

# (command, expected output or a regex for all of it, expected exit status); "@" is 127.0.0.1:PORT
STEPS = [
    ('rls', 'no server: give host[:port] or set one with wifi -s host[:port]\n', 1),
    ('wifi -s @', 'server @\nconnected, ip 127.0.0.1\n', 0),
    ('wifi', 'connected, ip 127.0.0.1\nserver @ /\n', 0),
    ('rls', '   <dir>  projects/\n   <dir>  sp/\n       4  top.txt\n   <dir>  tree/\n', 0),
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
    ('rls ../..', '   <dir>  projects/\n   <dir>  sp/\n       4  top.txt\n   <dir>  tree/\n       5  up.txt\n', 0),
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
    ('rcp', 'usage: rcp [-r] src dst\n', 1),
    ('rcp a.txt b.txt', 'rcp: one side must be remote (host:path or :path)\n', 1),
    ('rcp :top.txt r1.txt', 'r1.txt: 4 bytes\n', 0),
    ('rcp @:top.txt r2.txt', None, 0),       # host:port:path
    ('rcp s.txt :projects/game', '/projects/game/s.txt: 5 bytes\n', 0),   # into an existing directory
    ('rcp s.txt :projects/game/s2.txt', None, 0),
    ('rcp s.txt :nodir/x.txt', 'server: ERR No such file or directory\n', 1),
    ('rcp :projects r3', 'rcp: projects: is a directory, use -r\n', 1),
    ('mkdir ld', None, 0),
    ('rcp ld :x', 'rcp: ld: is a directory, use -r\n', 1),
    ('rcp -r :tree t1', 't1/a.txt: 2 bytes\nt1/sub/b.txt: 2 bytes\n2 files, 4 bytes\n', 0),
    ('rcp -r :tree t1', None, 0),             # t1 exists now: copied into it
    ('rcp -r t1 :up1', None, 0),
    ('rcp -r t1/sub :projects', None, 0),
    ('rcp -r :nothere t2', 'server: ERR No such file or directory\n', 1),
    ('rcp -r :sp sp1', None, 0),               # a name with a space, both ways
    ('rcp -r sp1 :sp2', None, 0),
    ('rcp :top.txt full', 'cannot write full\n', 1),   # full -> /dev/full: the write fails
    ('ping -c 2 127.0.0.1', re.compile(r'127\.0\.0\.1: \d+ ms\n127\.0\.0\.1: \d+ ms\n2 sent, 2 received\n'), 0),
    ('ping nosuchhost.invalid', 'ping: cannot resolve nosuchhost.invalid\n', 1),
    ('ping', 'usage: ping host [-c count]\n', 1),
]
FILES = {'a.txt': b'hello\n', 'b.txt': b'hello\n', 't2.txt': b'top\n', 'r1.txt': b'top\n', 'r2.txt': b'top\n',
         't1/a.txt': b'A\n', 't1/sub/b.txt': b'B\n', 't1/tree/a.txt': b'A\n', 't1/tree/sub/b.txt': b'B\n',
         'sp1/my file.txt': b'S\n'}
SERVER_FILES = {'projects/s.txt': b'sent\n', 'up.txt': b'sent\n', 'projects/game/s.txt': b'sent\n',
                'projects/game/s2.txt': b'sent\n', 'up1/a.txt': b'A\n', 'up1/sub/b.txt': b'B\n',
                'up1/tree/a.txt': b'A\n', 'projects/sub/b.txt': b'B\n',
                'sp2/my file.txt': b'S\n'}
BOARD_DIRS = ['t1/empty', 't1/tree/empty']
SERVER_DIRS = ['up1/empty', 'up1/tree/empty']
ESCAPES = ['GET ../srv2/secret', 'GET /../srv2/secret', 'LS ..', 'PUT ../srv2/x 1', 'MKDIR ../srv2/d']


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
    os.makedirs(os.path.join(SRV, 'tree', 'sub')); os.makedirs(os.path.join(SRV, 'tree', 'empty'))
    open(os.path.join(SRV, 'tree', 'a.txt'), 'w').write('A\n')
    open(os.path.join(SRV, 'tree', 'sub', 'b.txt'), 'w').write('B\n')
    os.makedirs(os.path.join(SRV, 'sp')); open(os.path.join(SRV, 'sp', 'my file.txt'), 'w').write('S\n')
    os.symlink('/dev/full', os.path.join(BOARD, 'full'))
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
            ok = want is None or (want.fullmatch(out) if isinstance(want, re.Pattern) else out == want)
            if r.returncode != status or not ok:
                bad += 1
                print('FAIL %s: exit %d (want %d)\n%s' % (cmd, r.returncode, status, out))
        for name, data in FILES.items():
            if content(os.path.join(BOARD, name)) != data: bad += 1; print('FAIL board file', name)
        for name, data in SERVER_FILES.items():
            if content(os.path.join(SRV, name)) != data: bad += 1; print('FAIL server file', name)
        for d in BOARD_DIRS:
            if not os.path.isdir(os.path.join(BOARD, d)): bad += 1; print('FAIL board dir', d)
        for d in SERVER_DIRS:
            if not os.path.isdir(os.path.join(SRV, d)): bad += 1; print('FAIL server dir', d)
        if content(os.path.join(BOARD, 'etc', 'remote')) != (hp + '\n/\n').encode(): bad += 1; print('FAIL /etc/remote')
        for req in ESCAPES:
            s = socket.create_connection(('127.0.0.1', port), 5)
            s.sendall(req.encode() + b'\n' + (b'x' if req.startswith('PUT') else b''))
            reply = s.recv(100); s.close()
            if reply != b'ERR outside root\n': bad += 1; print('FAIL %s: %r' % (req, reply))
        if os.path.exists(os.path.join(WORK, 'srv2', 'x')): bad += 1; print('FAIL PUT wrote outside the root')
        for _ in range(2):                     # MKDIR, also of a directory that exists
            s = socket.create_connection(('127.0.0.1', port), 5)
            s.sendall(b'MKDIR projects/new\n'); reply = s.recv(100); s.close()
            if reply != b'OK\n': bad += 1; print('FAIL MKDIR projects/new: %r' % reply)
        if not os.path.isdir(os.path.join(SRV, 'projects', 'new')): bad += 1; print('FAIL MKDIR made no directory')
    finally:
        server.kill()
    if bad: print('%d failed' % bad); return 1
    print('ok: %d tool steps, %d escape requests' % (len(STEPS), len(ESCAPES)))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
