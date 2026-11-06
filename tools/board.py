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
            self.s = serial.Serial(self.port, 115200, timeout=0.05, write_timeout=5, exclusive=True)
        except serial.SerialException as e:
            msg = str(e)
            if 'lock' in msg or 'busy' in msg.lower() or 'Resource temporarily' in msg:
                raise BoardError('port busy: %s is in use by another process (%s)' % (self.port, msg))
            raise BoardError('cannot open %s: %s' % (self.port, msg))

    def _write(self, data):
        try:
            self.s.write(data)  # bounded by write_timeout; no flush (tcdrain has no timeout)
        except serial.SerialTimeoutException:
            raise BoardError('write timeout: the board has hung, reset it')

    def _read(self, wait=3.0, quiet=0.3, until_prompt=False):
        """Read until quiet seconds pass with no new bytes, or wait seconds total.
        With until_prompt, quiet only counts once the output ends in a shell prompt."""
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
                if not until_prompt or PROMPT_RE.match(buf.replace(b'\r', b'\n').split(b'\n')[-1].decode('latin-1')):
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
        """Run a shell command; return output lines without echo, prompt and exit line.
        Raises BoardError if the echoed command and a final prompt do not come back."""
        self.s.reset_input_buffer()
        self._write(cmd.encode() + b'\r')
        raw = self._read(wait, 0.3, True).decode('latin-1')
        lines = [l.rstrip() for l in raw.replace('\r\n', '\n').replace('\r', '\n').split('\n')]
        if not lines or not lines[0].strip().endswith(cmd.strip()) or not PROMPT_RE.match(lines[-1]):
            raise BoardError('no answer from the board: it may have hung, reset it (got %r)' % raw[-120:])
        out = []
        for l in lines[1:]:
            if PROMPT_RE.match(l) or EXIT_RE.match(l):
                continue
            out.append(l)
        while out and not out[-1]:
            out.pop()
        return out

    def fetch(self, remote, local=None, size=None):
        """Read a remote file via hexdump; check contiguous offsets and the optional size."""
        data = b''
        for l in self.run('hexdump ' + remote, 300.0):
            m = HEX_RE.match(l)
            if m:
                if int(m.group(1), 16) != len(data):
                    raise BoardError('fetch %s: gap at offset %d (next line says %s)' % (remote, len(data), m.group(1)))
                data += bytes.fromhex(m.group(2).replace(' ', ''))
        if not data:
            raise BoardError('fetch %s: no data' % remote)
        if size is not None and len(data) != size:
            raise BoardError('fetch %s: got %d bytes, expected %d' % (remote, len(data), size))
        if local:
            with open(local, 'wb') as f:
                f.write(data)
        return data

    def expect_gcc(self, test_c, flags=''):
        """Compare the board's run of test_c (built by compiler/cc -S, as and ld) with a native
        gcc run; flags (e.g. -D NAME=1) go to both compilers. Trailing white space is ignored."""
        name = os.path.splitext(os.path.basename(test_c))[0]
        bdir = os.path.join(ROOT, 'build', 'expect')
        os.makedirs(bdir, exist_ok=True)
        native = os.path.join(bdir, name + '.native')
        subprocess.run(['gcc', '-w'] + flags.split() + ['-o', native, test_c], check=True)
        want = [l.rstrip() for l in subprocess.run([native], capture_output=True, text=True).stdout.split('\n')]
        while want and not want[-1]:
            want.pop()
        binp = os.path.join(bdir, name + '.bin')
        asm = os.path.join(bdir, name + '.s')
        obj = os.path.join(bdir, name + '.o')
        subprocess.run([os.path.join(ROOT, 'compiler', 'cc'), '-S', '-mshort-data'] + flags.split() + ['-o', asm, test_c], check=True)
        subprocess.run([os.path.join(ROOT, 'build', 'host', 'as'), '-o', obj, asm], check=True)
        subprocess.run([os.path.join(ROOT, 'build', 'host', 'ld'), '-o', binp, obj], check=True)
        self.put(binp, '/src/%s.bin' % name)
        got = [l.rstrip() for l in self.run('/src/%s.bin' % name, 10.0)]
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
            Board().fetch(argv[2], argv[3])
        else:
            print(__doc__)
            return 1
    except (BoardError, OSError, subprocess.CalledProcessError) as e:
        print('board.py: %s' % e, file=sys.stderr)
        return 1
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
