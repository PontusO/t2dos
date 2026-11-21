#!/usr/bin/env python3
"""Check telnetd: a telnet session that shares the console with USB.

  check_telnetd.py --pc       (needs `make` and `make host`)
  check_telnetd.py --board    (the board on tools/board.py's port, on WiFi, with the new kernel
                               and telnetd.bin; this PC must reach it on port 2323)

Types into the console (sh.bin under qemu-arm with host_linux, or the board's USB serial) and
logs in over TCP the way a telnet app does, then checks what both sides see.  The board run
also reboots the board to check that the setting is kept.  Exit status 0 when every check passes.
"""
import os, re, socket, subprocess, sys, threading, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
IAC, DONT, DO, WONT, WILL, SB, SE, IP = 255, 254, 253, 252, 251, 250, 240, 244
HELLO = bytes([IAC, WILL, 1, IAC, WILL, 3, IAC, DO, 31])
PW = 'sesame'
MTAB = open(os.path.join(ROOT, 'src', 'lib', 'mtab.c'), 'rb').read()
bad = 0


def fail(msg):
    global bad
    bad += 1
    print('FAIL', msg)


class Reader:
    """Collects what a stream sends, so a check can wait for a pattern."""
    def __init__(self, read):
        self.buf = b''; self.seen = b''; self.eof = False; self.lock = threading.Lock()
        threading.Thread(target=self._run, args=(read,), daemon=True).start()

    def _run(self, read):
        while True:
            try: d = read()
            except (OSError, ValueError): d = b''
            if not d: self.eof = True; return
            with self.lock: self.buf += d

    def wait(self, what, timeout=10.0, name=''):
        """Waits until the bytes or the regex turn up after the mark; True and moves the mark past them."""
        t0 = time.time()
        while time.time() - t0 < timeout:
            with self.lock:
                m = what.search(self.buf) if isinstance(what, re.Pattern) else None
                i = -1 if isinstance(what, re.Pattern) else self.buf.find(what)
                if m or i >= 0:
                    end = m.end() if m else i + len(what)
                    self.seen = self.buf[:end]; self.buf = self.buf[end:]
                    return True
            if self.eof: break
            time.sleep(0.02)
        fail('%s: no %r in %r' % (name, what, self.buf[-300:]))
        return False

    def closed(self, timeout=5.0, name=''):
        t0 = time.time()
        while time.time() - t0 < timeout:
            if self.eof: return True
            time.sleep(0.02)
        fail('%s: still open' % name)
        return False


class Client:
    def __init__(self, host, port):
        self.s = socket.create_connection((host, port), 10)
        self.s.settimeout(None)
        self.r = Reader(lambda: self.s.recv(4096))

    def send(self, b):
        self.s.sendall(b.encode() if isinstance(b, str) else b)

    def login(self, name):
        self.r.wait(b'password: ', name=name + ' prompt')
        self.send(PW + '\r\n')
        return self.r.wait(b'Press Enter for a prompt.\r\n', name=name + ' login')

    def close(self):                    # shutdown first: the reader thread's recv keeps a bare close from sending FIN
        try: self.s.shutdown(socket.SHUT_RDWR)
        except OSError: pass
        try: self.s.close()
        except OSError: pass


def scenario(con, host, port, local_ip):
    """con: .type(text) types on the local console; con.r is its Reader."""
    def local(cmd, want, name, timeout=10.0):
        con.type(cmd + '\r')
        con.r.wait(want, timeout, name)

    local('telnetd off', b'telnetd is off', 'off at the start')
    local('telnetd', b'telnetd is off', 'status off')
    con.type('telnetd on %d\r' % port); time.sleep(0.3); con.type('a\r'); time.sleep(0.3); con.type('b\r')
    con.r.wait(b'telnetd: the passwords differ', name='passwords differ')
    con.type('telnetd on %d\r' % port); time.sleep(0.3); con.type('ses\tame\r'); time.sleep(0.3); con.type(PW + '\r')
    con.r.wait(b'telnetd on port %d' % port, name='on')
    time.sleep(0.5)
    local('telnetd', re.compile(rb'telnetd on port %d\r?\n' % port), 'status on')

    # the greeting, refusing an option, a wrong password, a second login meanwhile
    c1 = Client(host, port)
    c1.r.wait(HELLO + b'T2DOS password: ', name='hello')
    if not c1.r.seen.startswith(HELLO): fail('hello: %r' % c1.r.seen)
    c1.send(bytes([IAC, WILL, 31, IAC, SB, 31, 0, 100, 0, 30, IAC, SE, IAC, DO, 24]))
    c1.r.wait(bytes([IAC, WONT, 24]), name='refuse DO 24')
    t0 = time.time()
    c1.send('nope\r\0')
    c1.r.wait(b'\r\nwrong password\r\n', name='wrong password')
    c2 = Client(host, port)
    c2.r.wait(b'another login is in progress\r\n', name='busy')
    c2.r.closed(name='busy close'); c2.close()
    c1.r.wait(b'password: ', name='second prompt')
    if time.time() - t0 < 1.5: fail('the second try came after %.1f s' % (time.time() - t0))
    c1.send(PW + '\r\necho fast\r\n')
    c1.r.wait(b'Press Enter for a prompt.\r\n', name='login')
    c1.r.wait(b'fast\r\n', name='keys sent with the password')
    con.r.wait(b'telnetd: %s logged in' % local_ip.encode(), name='local note')

    # both sides type, both see the output
    c1.send('\r\n'); c1.r.wait(b'/> ', name='prompt')
    c1.send('echo hi\r\n')
    c1.r.wait(b'echo hi\r\nhi\r\n/> ', name='remote echo')
    con.r.wait(re.compile(rb'hi\r?\n'), name='remote command seen locally')
    con.type('echo lo\r')
    c1.r.wait(b'echo lo\r\nlo\r\n', name='local command seen remotely')
    c1.send('telnetd\r\n')
    c1.r.wait(b'telnetd on port %d, %s logged in\r\n' % (port, local_ip.encode()), name='status live')
    c1.send('/tmp/tdsize.bin\r\n')
    c1.r.wait(b'size 100 30\r\n', name='window size')
    c1.send(bytes([IAC, SB, 31, 0, 50, 0x90, 0, IAC, SE]) + b'/tmp/tdsize.bin\r\n')
    c1.r.wait(b'size 100 30\r\n', name='huge window ignored')
    c1.send('telnetd on %d\r\n' % port); time.sleep(0.3); c1.send(PW + '\r\n'); time.sleep(0.3); c1.send(PW + '\r\n')
    c1.r.wait(b'telnetd on port %d\r\n/> ' % port, name='on again from the session')
    c1.send('echo still\r\n'); c1.r.wait(b'still\r\n', name='session kept')
    c1.send(b'abc' + bytes([IAC, IP]))
    c1.r.wait(b'abc\r\n/> ', name='IAC IP is Ctrl-C')
    c1.send('cat /lib/mtab.c\r\n')                # more than one output buffer
    c1.r.wait(MTAB.replace(b'\n', b'\r\n') + b'/> ', name='long output')

    # a new login takes over
    c3 = Client(host, port)
    c3.login('takeover')
    c1.r.wait(b'[another login took over this console]\r\n', name='taken over')
    c1.r.closed(name='taken over close'); c1.close()
    c3.send('echo three\r\n'); c3.r.wait(b'three\r\n', name='after takeover')
    c3.close()
    con.r.wait(b'telnetd: %s disconnected' % local_ip.encode(), name='disconnect note')

    # exit in the session logs it out and starts sh again, so USB keeps a shell
    c10 = Client(host, port)
    c10.login('before exit')
    c10.send('exit\r\n')
    c10.r.wait(b'[logged out]\r\n', name='exit logs out'); c10.r.closed(name='exit close'); c10.close()
    con.r.wait(b'telnetd: %s logged out' % local_ip.encode(), name='logged out note')
    local('echo again', re.compile(rb'again\r?\n'), 'sh after exit')
    time.sleep(0.3)

    # a wrong try then a new connection: the next prompt still waits 2 s
    c8 = Client(host, port); c8.r.wait(b'password: ', name='retry prompt'); c8.send('bad\r\n')
    c8.r.wait(b'wrong password\r\n', name='retry wrong'); c8.close()
    t0 = time.time(); time.sleep(0.2)
    c9 = Client(host, port); c9.r.wait(b'password: ', name='retry after reconnect')
    if time.time() - t0 < 1.5: fail('a new connection got a prompt %.1f s after a wrong password' % (time.time() - t0))
    c9.send('bad\r\n'); c9.r.wait(b'wrong password\r\n', name='second wrong'); c9.close(); time.sleep(0.3)
    # the third wrong one in a row (whatever the connection) locks logins for a while
    c4 = Client(host, port)
    c4.r.wait(b'password: ', 10, 'third prompt'); c4.send('bad\r\n')
    c4.r.wait(b'wrong password\r\n', name='third wrong'); c4.r.closed(name='third wrong close'); c4.close()
    con.r.wait(b'failed to log in', name='failed note')
    c5 = Client(host, port)
    c5.r.wait(b'try again later\r\n', name='locked'); c5.r.closed(name='locked close'); c5.close()
    time.sleep(5.5)
    # three wrong passwords on one connection
    c4 = Client(host, port)
    for i in range(3):
        c4.r.wait(b'password: ', 10, 'lock prompt %d' % i); c4.send('bad\r\n')
    c4.r.wait(b'wrong password\r\n', name='third wrong'); c4.r.closed(name='third wrong close'); c4.close()
    con.r.wait(b'failed to log in', name='failed note')
    c5 = Client(host, port)
    c5.r.wait(b'try again later\r\n', name='locked'); c5.r.closed(name='locked close'); c5.close()
    time.sleep(5.5)


def stop_check(con, host, port):
    c6 = Client(host, port)
    c6.login('before off')
    con.type('telnetd off\r')
    c6.r.wait(b'[telnetd stopped]\r\n', name='stopped'); c6.r.closed(name='stopped close'); c6.close()
    con.r.wait(b'telnetd is off', name='off')
    time.sleep(0.5)
    try:
        c7 = Client(host, port); c7.r.closed(3, 'after off'); c7.close()
    except OSError:
        pass


def edit_check(con, host, port):
    """conctl codes reach the session; edit redraws at a new size without a key, and still saves."""
    c = Client(host, port); c.login('edit')
    c.send(bytes([IAC, SB, 31, 0, 100, 0, 30, IAC, SE]))
    c.send('/tmp/tdctl.bin\r\n')
    c.r.wait(b'\x1b[4;3H\x1b[31;44mctl\r\n', name='conctl reaches the session')
    c.send('edit /tmp/rs.txt\r\n')
    c.r.wait(b'\x1b[30;1H\x1b[7m', 5, 'edit status line at row 30')          # 100x30
    c.send(bytes([IAC, SB, 31, 0, 60, 0, 20, IAC, SE]))                       # no key pressed
    c.r.wait(b'\x1b[20;1H\x1b[7m', 2, 'edit redraws at row 20')
    c.send(bytes([6])); c.r.wait(b'find: ', 2, 'find prompt')                  # ^F, then a resize at its prompt
    time.sleep(0.3); c.r.wait(b'', name='mark')
    c.send(bytes([IAC, SB, 31, 0, 70, 0, 22, IAC, SE])); time.sleep(1.0)
    with c.r.lock: got = len(c.r.buf)
    if got > 3000: fail('the find prompt sent %d bytes in 1 s after a resize' % got)
    c.r.wait(b'\x1b[22;1H', 2, 'find prompt moved to row 22')
    c.send(bytes([27])); c.r.wait(b'\x1b[22;1H\x1b[7m', 2, 'status line at row 22 after find')
    c.send('abc'); time.sleep(0.3); c.send(bytes([19])); time.sleep(0.3); c.send(bytes([24]))   # type, ^S, ^X
    c.r.wait(b'/> ', 5, 'edit quit'); c.close()
    time.sleep(0.3)
    con.type('cat /tmp/rs.txt\r'); con.r.wait(b'abc', name='edit saved after the resize')


class PcConsole:
    def __init__(self):
        self.p = subprocess.Popen(['qemu-arm', os.path.join(ROOT, 'host_linux'), '-r', os.path.join(ROOT, 'root'), '/bin/sh.bin'],
                                  stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, bufsize=0)
        self.r = Reader(lambda: self.p.stdout.read1(4096) if hasattr(self.p.stdout, 'read1') else os.read(self.p.stdout.fileno(), 4096))

    def type(self, text):
        self.p.stdin.write(text.encode()); self.p.stdin.flush()


SIZE_C = os.path.join(ROOT, 'tests', 'telnetd', 'size.c')
CTL_C = os.path.join(ROOT, 'tests', 'telnetd', 'ctl.c')


def pc_main():
    os.makedirs(os.path.join(ROOT, 'root', 'tmp'), exist_ok=True)
    subprocess.run([os.path.join(ROOT, 'src', 'compiler', 'cc'), '-I', os.path.join(ROOT, 'src', 'lib'), '-o',
                    os.path.join(ROOT, 'root', 'tmp', 'tdsize.bin'), SIZE_C], check=True, cwd=ROOT)
    subprocess.run([os.path.join(ROOT, 'src', 'compiler', 'cc'), '-I', os.path.join(ROOT, 'src', 'lib'), '-o',
                    os.path.join(ROOT, 'root', 'tmp', 'tdctl.bin'), CTL_C], check=True, cwd=ROOT)
    s = socket.socket(); s.bind(('127.0.0.1', 0)); port = s.getsockname()[1]; s.close()
    con = PcConsole()
    try:
        scenario(con, '127.0.0.1', port, '127.0.0.1')
        edit_check(con, '127.0.0.1', port)
        stop_check(con, '127.0.0.1', port)
        con.type('exit\r')
    finally:
        con.p.kill()
    if bad: print('%d failed' % bad); return 1
    print('ok: telnetd under qemu')
    return 0


def serial_reader(ser):
    class R(Reader):
        def __init__(self):
            self.buf = b''; self.seen = b''; self.eof = False; self.lock = threading.Lock(); self.stop = False
            threading.Thread(target=self.loop, daemon=True).start()
        def loop(self):
            while not self.stop:
                try: d = ser.read(4096)
                except Exception: self.eof = True; return
                if d:
                    with self.lock: self.buf += d
    return R()


def board_main():
    sys.path.insert(0, os.path.join(ROOT, 'tools'))
    import board
    port = 2323
    b = board.Board()
    subprocess.run([os.path.join(ROOT, 'src', 'compiler', 'cc'), '-I', os.path.join(ROOT, 'src', 'lib'), '-o',
                    os.path.join(ROOT, 'build', 'tdsize.bin'), SIZE_C], check=True, cwd=ROOT)
    b.put(os.path.join(ROOT, 'build', 'tdsize.bin'), '/tmp/tdsize.bin')
    ip = None
    for line in b.run('wifi', 5):
        m = re.match(r'connected, ip (\S+)', line)
        if m: ip = m.group(1)
    if not ip: print('the board is not on WiFi'); return 1
    me = socket.socket(socket.AF_INET, socket.SOCK_DGRAM); me.connect((ip, 9)); local_ip = me.getsockname()[0]; me.close()

    class Con:
        pass
    con = Con(); con.r = serial_reader(b.s)
    con.type = lambda text: [b._write(ch.encode()) for ch in text]
    scenario(con, ip, port, local_ip)

    # kept across a reboot
    con.r.stop = True; time.sleep(0.2)
    b._write(b'reboot\r'); time.sleep(0.5); b.s.close()
    time.sleep(3)
    for _ in range(60):
        try: b = board.Board(); break
        except board.BoardError: time.sleep(0.5)
    c = None
    for _ in range(40):
        try: c = Client(ip, port); break
        except OSError: time.sleep(1)
    if not c: fail('no telnetd after the reboot')
    else:
        c.login('after reboot'); c.send('echo back\r\n'); c.r.wait(b'back\r\n', name='after reboot'); c.close()
    con.r = serial_reader(b.s)
    stop_check(con, ip, port)
    con.r.stop = True
    if bad: print('%d failed' % bad); return 1
    print('ok: telnetd on the board')
    return 0


def main(argv):
    if argv[1:] == ['--pc']: return pc_main()
    if argv[1:] == ['--board']: return board_main()
    print(__doc__); return 2


if __name__ == '__main__':
    sys.exit(main(sys.argv))
