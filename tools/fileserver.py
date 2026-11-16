#!/usr/bin/env python3
"""Tiny file server for T2DOS get/send/rls/rcp.  Usage: fileserver.py [dir] [port]
One request per connection:  GET name | PUT name size (+bytes) | LS [dir] | MKDIR name
or, after a SESSION line, many: STAT, READ, WRITE, TRUNC, LS, MKDIR, RM, RMDIR, RENAME
(see README, network protocol)"""
import os, socket, sys, threading

sys.stdout.reconfigure(errors="backslashreplace")   # the log may show names that are not UTF-8
ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 7777

def safe(name):
    p = os.path.abspath(os.path.join(ROOT, name.lstrip("/")))
    if os.path.commonpath([ROOT, p]) != ROOT: raise ValueError("outside root")
    return p

def readline(c):
    b = b""
    while not b.endswith(b"\n") and len(b) < 256:
        ch = c.recv(1)
        if not ch: break
        b += ch
    # only the line end goes: names keep their spaces, and bytes that are not UTF-8 survive
    return b.decode(errors="surrogateescape").rstrip("\r\n")

def recv_exact(c, size):
    got = b""
    while len(got) < size:
        chunk = c.recv(min(4096, size - len(got)))
        if not chunk: raise IOError("short upload")
        got += chunk
    return got

BLOCK = 4096    # the most a session READ or WRITE moves

def listing(name):
    d = safe(name) if name else ROOT
    lines = []
    for n in sorted(os.listdir(d)):
        p = os.path.join(d, n)
        lines.append(f"   <dir>  {n}/" if os.path.isdir(p) else f"{os.path.getsize(p):8d}  {n}")
    return ("\n".join(lines) + "\n").encode(errors="surrogateescape")

def one(c, addr, line):
    """Answers one request. Numbers come before the name, so names may hold spaces."""
    cmd, _, rest = line.partition(" ")
    cmd = cmd.upper()
    if cmd == "GET" and rest:
        data = open(safe(rest), "rb").read()
        c.sendall(f"OK {len(data)}\n".encode() + data)
        print(f"{addr[0]} GET {rest} {len(data)}")
    elif cmd == "PUT" and " " in rest:
        name, size = rest.rsplit(" ", 1)            # PUT has its size at the end
        got = recv_exact(c, int(size))
        open(safe(name), "wb").write(got)
        c.sendall(b"OK\n")
        print(f"{addr[0]} PUT {name} {size}")
    elif cmd == "MKDIR" and rest:
        os.makedirs(safe(rest), exist_ok=True)
        c.sendall(b"OK\n")
        print(f"{addr[0]} MKDIR {rest}")
    elif cmd == "LS":
        text = listing(rest)
        c.sendall(f"OK {len(text)}\n".encode() + text)
    elif cmd == "STAT" and rest:
        p = safe(rest)
        if os.path.isdir(p): c.sendall(b"OK d\n")
        else: c.sendall(f"OK f {os.stat(p).st_size}\n".encode())
    elif cmd == "READ" and rest.count(" ") >= 2:
        off, n, name = rest.split(" ", 2)
        off, n = int(off), int(n)
        if n > BLOCK or off < 0 or n < 0: raise ValueError("bad size")
        with open(safe(name), "rb") as f:
            f.seek(off); data = f.read(n)
        c.sendall(f"OK {len(data)}\n".encode() + data)
    elif cmd == "WRITE" and rest.count(" ") >= 2:
        off, n, name = rest.split(" ", 2)
        off, n = int(off), int(n)
        if n > BLOCK or off < 0 or n < 0: raise ValueError("bad size")
        data = recv_exact(c, n)                   # first: refused data must not be read as requests
        p = safe(name)
        with open(p, "r+b" if os.path.exists(p) else "wb") as f:
            f.seek(off); f.write(data)
        c.sendall(b"OK\n")
        print(f"{addr[0]} WRITE {name} {off} {n}")
    elif cmd == "TRUNC" and " " in rest:
        size, name = rest.split(" ", 1)
        p = safe(name)
        with open(p, "r+b" if os.path.exists(p) else "wb") as f: f.truncate(int(size))
        c.sendall(b"OK\n")
        print(f"{addr[0]} TRUNC {name} {size}")
    elif cmd == "RM" and rest:
        os.remove(safe(rest)); c.sendall(b"OK\n")
        print(f"{addr[0]} RM {rest}")
    elif cmd == "RMDIR" and rest:
        os.rmdir(safe(rest)); c.sendall(b"OK\n")
        print(f"{addr[0]} RMDIR {rest}")
    elif cmd == "RENAME" and rest:
        new = readline(c)
        os.replace(safe(rest), safe(new)); c.sendall(b"OK\n")
        print(f"{addr[0]} RENAME {rest} {new}")
    else:
        c.sendall(b"ERR bad request\n")

def answer(c, addr, line):
    """one() with errors sent as ERR; False if the connection broke."""
    try:
        one(c, addr, line)
    except (ConnectionError, BrokenPipeError):
        return False
    except Exception as e:
        # an OSError's text holds the full PC path: send only the reason
        msg = e.strerror if isinstance(e, OSError) and e.strerror else str(e)
        try: c.sendall(f"ERR {msg}\n".encode(errors="replace"))
        except Exception: return False
    return True

def handle(c, addr):
    try:
        line = readline(c)
        if line.upper() == "SESSION":               # many requests on one connection, until it closes
            c.sendall(b"OK\n")
            while True:
                line = readline(c)
                if not line or not answer(c, addr, line): break
        else:
            answer(c, addr, line)
    except Exception:
        pass
    finally:
        c.close()

s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", PORT)); s.listen(4)
print(f"serving {ROOT} on port {PORT}")
while True:
    c, addr = s.accept()
    threading.Thread(target=handle, args=(c, addr), daemon=True).start()
