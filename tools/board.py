#!/usr/bin/env python3
"""Drive the T2DOS shell on the board over its serial console.

CLI:
  board.py put LOCAL REMOTE
  board.py run 'COMMAND' [WAIT_SECONDS]
  board.py fetch REMOTE LOCAL
Port: $T2DOS_PORT, else the first /dev/serial/by-id/*CPico*.
"""
import difflib
import glob
import os
import re
import subprocess
import sys
import time

import serial

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
PROMPT_RE = re.compile(r'^\S*> ?$')
EXIT_RE = re.compile(r'^\[exit -?\d+, \d+ ms\]$')
HEX_RE = re.compile(r'^([0-9a-f]{6})  ((?:[0-9a-f]{2} ?)+)')


class BoardError(Exception):
    pass


def find_port():
    p = os.environ.get('T2DOS_PORT')
    if p:
        return p
    m = sorted(glob.glob('/dev/serial/by-id/*CPico*'))
    if not m:
        raise BoardError('no board found: set T2DOS_PORT or plug in the CPico')
    return m[0]


class Board:
    def __init__(self, port=None):
        self.port = port or find_port()
        try:
            self.s = serial.Serial(self.port, 115200, timeout=0.05, write_timeout=5)
        except serial.SerialException as e:
            raise BoardError('cannot open %s: %s' % (self.port, e))

    def _write(self, data):
        try:
            self.s.write(data)
            self.s.flush()
        except serial.SerialTimeoutException:
            raise BoardError('write timeout: the board has hung, reset it')

    def _read(self, wait=3.0, quiet=0.3):
        """Read until quiet seconds pass with no new bytes, or wait seconds total."""
        buf = b''
        start = time.time()
        last = start
        while True:
            d = self.s.read(65536)
            now = time.time()
            if d:
                buf += d
                last = now
            if now - last >= quiet and now - start >= 0.1:
                break
            if now - start >= wait:
                break
        return buf

    def put(self, local, remote):
        data = local if isinstance(local, bytes) else open(local, 'rb').read()
        self.s.reset_input_buffer()
        self._write(b'put %s %d\r' % (remote.encode(), len(data)))
        time.sleep(0.5)
        for i in range(0, len(data), 512):
            self._write(data[i:i + 512])
            time.sleep(0.01)
        out = self._read(5.0, 0.5).decode('latin-1')
        if 'ok, %d bytes' % len(data) not in out:
            raise BoardError('put %s failed: %r' % (remote, out[-200:]))

    def run(self, cmd, wait=3.0):
        """Run a shell command; return output lines without echo, prompt and exit line."""
        self.s.reset_input_buffer()
        self._write(cmd.encode() + b'\r')
        raw = self._read(wait, 0.3).decode('latin-1')
        lines = [l.rstrip() for l in raw.replace('\r\n', '\n').replace('\r', '\n').split('\n')]
        out = []
        for i, l in enumerate(lines):
            if i == 0 and l.strip().endswith(cmd.strip()):
                continue  # echoed command line
            if PROMPT_RE.match(l) or EXIT_RE.match(l):
                continue
            out.append(l)
        while out and not out[-1]:
            out.pop()
        return out

    def fetch(self, remote):
        data = b''
        for l in self.run('hexdump ' + remote, 10.0):
            m = HEX_RE.match(l)
            if m:
                data += bytes.fromhex(m.group(2).replace(' ', ''))
        return data

    def expect_gcc(self, test_c, flags=''):
        """Compare the board's run of test_c (built by compiler/cc) with a native gcc run."""
        name = os.path.splitext(os.path.basename(test_c))[0]
        bdir = os.path.join(ROOT, 'build', 'expect')
        os.makedirs(bdir, exist_ok=True)
        native = os.path.join(bdir, name + '.native')
        subprocess.run(['gcc', '-w', '-o', native, test_c], check=True)
        want = subprocess.run([native], capture_output=True, text=True).stdout.split('\n')
        while want and not want[-1]:
            want.pop()
        binp = os.path.join(bdir, name + '.bin')
        subprocess.run([os.path.join(ROOT, 'compiler', 'cc')] + flags.split() + ['-o', binp, test_c], check=True)
        self.put(binp, '/src/%s.bin' % name)
        got = self.run('/src/%s.bin' % name, 10.0)
        if got == want:
            return True
        print('MISMATCH %s' % name)
        for l in difflib.unified_diff(want, got, 'gcc', 'board', lineterm=''):
            print(l)
        return False


def main(argv):
    try:
        if len(argv) >= 4 and argv[1] == 'put':
            Board().put(argv[2], argv[3])
        elif len(argv) >= 3 and argv[1] == 'run':
            for l in Board().run(argv[2], float(argv[3]) if len(argv) > 3 else 3.0):
                print(l)
        elif len(argv) >= 4 and argv[1] == 'fetch':
            d = Board().fetch(argv[2])
            if not d:
                raise BoardError('fetch %s: no data' % argv[2])
            with open(argv[3], 'wb') as f:
                f.write(d)
        else:
            print(__doc__)
            return 1
    except (BoardError, OSError, subprocess.CalledProcessError) as e:
        print('board.py: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
