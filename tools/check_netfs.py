#!/usr/bin/env python3
"""Check the network filesystem (/mnt/fs) and fileserver.py's session mode.

  check_netfs.py --pc    (needs `make` and `make host`)

Starts tools/fileserver.py on a random local port with a fresh root under
build/s/netfs/srv. Sends raw session requests and checks the replies, then runs
the board tools under qemu-arm with host_linux against it.
Exit status 0 when every check passes.
"""
import os, random, re, shutil, socket, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
WORK = os.path.join(ROOT, 'build', 's', 'netfs')
SRV = os.path.join(WORK, 'srv')
BOARD = os.path.join(WORK, 'board')
BIG = bytes((i * 7 + i // 300) & 255 for i in range(10000))   # crosses blocks, not a block multiple

# Tool steps under qemu: (command, expected output, expected exit status[, stdin]).  A command
# is a string split at spaces or a list (names with spaces); "@" is 127.0.0.1:PORT.  The
# expected output is a string (all of it) or a regex that must match all of it.
STEPS = [
    ('mount', 'nothing mounted\n', 1),
    ('mount @', '@ /\n', 0),
    ('mount', '@ /\n', 0),
    ('mount 127.0.0.1:1', 'mount: cannot reach 127.0.0.1:1\n', 1),
    ('mount @ nothere', 'mount: nothere: not a directory\n', 1),
    ('ls /mnt/fs', '   <dir>  LONGD/\n   10000  big.bin\nECHO  echo.bin\n       5  m1.txt\n      21  o.txt\n   <dir>  projects/\n'
     '       1  s2.txt\n   <dir>  sp/\n       4  t.txt\n       4  top.txt\n7 files, ALL bytes\n', 0),
    ('ls /', re.compile(r'(?s).*   <dir>  mnt/\n.*'), 0),
    ('cat /mnt/fs/top.txt', 'top\n', 0),
    ('cp /mnt/fs/big.bin b.bin', '', 0),
    ('sh', re.compile(r'(?s).*> pwd\n/mnt/fs/projects\n.*> cat game/a.txt\nhello\n.*> pwd\n/mnt\n.*> cd /mnt/fs\n/mnt/fs> exit\n'), 0,
     'cd /mnt/fs/projects\npwd\ncat game/a.txt\ncd ..\ncd ..\npwd\ncd /mnt/fs\nexit\n'),
    ('/mnt/fs/echo.bin hi', 'hi\n', 0),
    (['cat', '/mnt/fs/sp/my file.txt'], 'S\n', 0),
    ('ls /mnt/fs/sp', '       2  my file.txt\n1 files, 2 bytes\n', 0),
    # writing (Task 3)
    ('cp s.txt /mnt/fs/s.txt', '', 0),
    ('sh', re.compile(r'(?s).*'), 0, 'echo more >> /mnt/fs/s.txt\nexit\n'),
    ('cp short.txt /mnt/fs/o.txt', '', 0),
    ('cp b.bin /mnt/fs/big2.bin', '', 0),
    ('cmp /mnt/fs/big2.bin b.bin', '', 0),
    ('mkdir /mnt/fs/nd', '', 0),
    ('ls /mnt/fs/nd', '0 files, 0 bytes\n', 0),
    ('rmdir /mnt/fs/nd', '', 0),
    ('rmdir /mnt/fs/projects', 'rmdir: /mnt/fs/projects failed (not empty?)\n', 1),
    ('rm /mnt/fs/s2.txt', '', 0),
    ('mv /mnt/fs/m1.txt /mnt/fs/m2.txt', '', 0),
    ('mv /mnt/fs/m2.txt local.txt', '', 0),
    ('mv local.txt /mnt/fs/m3.txt', '', 0),
    ('twice', 'reader: old\nclose 0\nreader again: old\nnew open: new\n', 0),
    ('noclose', '', 0),
    ('cp s.txt /mnt/fs/LONGL', '', 0),           # /mnt/fs/ + 151: a 159-byte path
    ('cat /mnt/fs/LONGL', 'sent\n', 0),
    ('mount @ LONGD', '@ /LONGD\n', 0),
    ('wr /mnt/fs/LONGN hi', 'close -1\n', 0),   # fits, but its temp name does not
    ('mount @', '@ /\n', 0),
    ('full', 'wrote 8192\nclose 0\n', 0, '', {'T2_NETFS_CACHE': '8300'}),
    ('umount', '', 0),
    ('mount', 'nothing mounted\n', 1),
]
LONG = {'LONGL': 'L' * 151, 'LONGD': 'D' * 60, 'LONGN': 'N' * 135}
FILES = {'b.bin': BIG, 'local.txt': None}
SERVER_FILES = {'s.txt': b'sent\nmore\n', 'o.txt': b'x\n', 'big2.bin': BIG, 's2.txt': None, 'm1.txt': None,
                'm2.txt': None, 'm3.txt': b'move\n', 't.txt': b'new\n', 'nc.txt': b'data\n', 'L' * 151: b'sent\n',
                'D' * 60 + '/' + 'N' * 135: None, 'full.bin': b'z' * 8192}
TEST_PROGS = ['twice', 'noclose', 'wr', 'full']

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


def run_steps(port):
    hp = '127.0.0.1:%d' % port
    echo = os.path.getsize(os.path.join(SRV, 'echo.bin'))
    for step in STEPS:
        cmd, want, status = step[:3]
        stdin = step[3].encode() if len(step) > 3 else b''
        env = dict(os.environ, **step[4]) if len(step) > 4 else None
        for k, v in LONG.items(): cmd = cmd.replace(k, v) if isinstance(cmd, str) else cmd
        args = cmd.replace('@', hp).split() if isinstance(cmd, str) else cmd
        prog = args[0] if args[0].startswith('/') else '/bin/%s.bin' % args[0]
        try:
            r = subprocess.run(['qemu-arm', os.path.join(ROOT, 'host_linux'), '-r', BOARD, prog] + args[1:],
                               input=stdin, capture_output=True, timeout=60, env=env)
        except subprocess.TimeoutExpired:
            fail('%s: timed out' % cmd); continue
        out = r.stdout.decode(errors='replace').replace(hp, '@')
        for k, v in LONG.items(): out = out.replace(v, k)
        if isinstance(want, str):
            want = want.replace('ECHO', '%8d' % echo).replace('ALL', str(10000 + echo + 5 + 21 + 1 + 4 + 4))
            ok = out == want
        else:
            ok = want.fullmatch(out)
        if r.returncode != status or not ok:
            fail('%s: exit %d (want %d)\n%s' % (cmd, r.returncode, status, out))
    for name, data in FILES.items():
        try: got = open(os.path.join(BOARD, name), 'rb').read()
        except OSError: got = None
        if got != data: fail('board file %s' % name)
    for name, data in SERVER_FILES.items():
        if srv_file(name) != data: fail('server file %s: %r' % (name[:40], srv_file(name)))
    for d, _, names in os.walk(SRV):
        for n in names:
            if n.endswith('.t2tmp'): fail('temp file left: %s' % n)
    log = open(os.path.join(WORK, 'srv.log')).read()
    if not re.search(r'TRUNC /\.s\.txt\.t2tmp 0\n.*WRITE /\.s\.txt\.t2tmp 0 5\n.*RENAME /\.s\.txt\.t2tmp /s\.txt\n', log, re.S):
        fail('cp to /mnt/fs did not save through a temp file')
    if not re.search(r'WRITE /s\.txt 0 10\n', log) or 'TRUNC /s.txt' in log:
        fail('the append did not write in place')


def main(argv):
    if '--pc' not in argv:
        print(__doc__); return 2
    shutil.rmtree(WORK, ignore_errors=True)
    os.makedirs(os.path.join(SRV, 'projects', 'game'))
    open(os.path.join(SRV, 'top.txt'), 'w').write('top\n')
    open(os.path.join(SRV, 'projects', 'game', 'a.txt'), 'w').write('hello\n')
    os.makedirs(os.path.join(BOARD, 'tmp'))
    shutil.copytree(os.path.join(ROOT, 'root', 'bin'), os.path.join(BOARD, 'bin'))
    port = random.randint(20000, 29999)
    os.makedirs(WORK, exist_ok=True)
    server = subprocess.Popen([sys.executable, '-u', os.path.join(ROOT, 'tools', 'fileserver.py'), SRV, str(port)],
                              stdout=open(os.path.join(WORK, 'srv.log'), 'w'), stderr=subprocess.STDOUT)
    try:
        for _ in range(50):
            try: socket.create_connection(('127.0.0.1', port), 0.2).close(); break
            except OSError: time.sleep(0.1)
        check_server(port)
        for n in os.listdir(SRV):                 # the tool steps start from a known server root
            p = os.path.join(SRV, n)
            if n not in ('top.txt', 'projects'): shutil.rmtree(p) if os.path.isdir(p) else os.remove(p)
        open(os.path.join(SRV, 'big.bin'), 'wb').write(BIG)
        shutil.copy(os.path.join(ROOT, 'root', 'bin', 'echo.bin'), os.path.join(SRV, 'echo.bin'))
        os.makedirs(os.path.join(SRV, 'sp')); open(os.path.join(SRV, 'sp', 'my file.txt'), 'w').write('S\n')
        for n, t in [('o.txt', 'a longer old content\n'), ('s2.txt', 'x'), ('m1.txt', 'move\n'), ('t.txt', 'old\n')]:
            open(os.path.join(SRV, n), 'w').write(t)
        os.makedirs(os.path.join(SRV, 'D' * 60))
        open(os.path.join(BOARD, 's.txt'), 'w').write('sent\n')
        open(os.path.join(BOARD, 'short.txt'), 'w').write('x\n')
        for t in TEST_PROGS:
            subprocess.run([os.path.join(ROOT, 'compiler', 'cc'), '-I', os.path.join(ROOT, 'lib'), '-o',
                            os.path.join(BOARD, 'bin', t + '.bin'), os.path.join(ROOT, 'tests', 'netfs', t + '.c')], check=True)
        run_steps(port)
    finally:
        server.kill()
    if bad: print('%d failed' % bad); return 1
    print('ok: server session requests, %d tool steps' % len(STEPS))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
