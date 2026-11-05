#!/usr/bin/env python3
"""Tiny file server for T2DOS get/send/rls.  Usage: fileserver.py [dir] [port]
One request per connection:  GET name | PUT name size (+bytes) | LS [dir]"""
import os, socket, sys, threading

ROOT = os.path.abspath(sys.argv[1] if len(sys.argv) > 1 else ".")
PORT = int(sys.argv[2]) if len(sys.argv) > 2 else 7777

def safe(name):
    p = os.path.abspath(os.path.join(ROOT, name.lstrip("/")))
    if not p.startswith(ROOT): raise ValueError("outside root")
    return p

def readline(c):
    b = b""
    while not b.endswith(b"\n") and len(b) < 256:
        ch = c.recv(1)
        if not ch: break
        b += ch
    return b.decode(errors="replace").strip()

def handle(c, addr):
    try:
        req = readline(c).split(" ", 2)
        cmd = req[0].upper() if req else ""
        if cmd == "GET" and len(req) >= 2:
            data = open(safe(req[1]), "rb").read()
            c.sendall(f"OK {len(data)}\n".encode() + data)
            print(f"{addr[0]} GET {req[1]} {len(data)}")
        elif cmd == "PUT" and len(req) >= 3:
            size, got = int(req[2]), b""
            while len(got) < size:
                chunk = c.recv(min(4096, size - len(got)))
                if not chunk: raise IOError("short upload")
                got += chunk
            open(safe(req[1]), "wb").write(got)
            c.sendall(b"OK\n")
            print(f"{addr[0]} PUT {req[1]} {size}")
        elif cmd == "LS":
            d = safe(req[1]) if len(req) >= 2 and req[1] else ROOT
            lines = []
            for n in sorted(os.listdir(d)):
                p = os.path.join(d, n)
                lines.append(f"   <dir>  {n}/" if os.path.isdir(p) else f"{os.path.getsize(p):8d}  {n}")
            text = ("\n".join(lines) + "\n").encode()
            c.sendall(f"OK {len(text)}\n".encode() + text)
        else:
            c.sendall(b"ERR bad request\n")
    except Exception as e:
        try: c.sendall(f"ERR {e}\n".encode())
        except Exception: pass
    finally:
        c.close()

s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
s.bind(("", PORT)); s.listen(4)
print(f"serving {ROOT} on port {PORT}")
while True:
    c, addr = s.accept()
    threading.Thread(target=handle, args=(c, addr), daemon=True).start()
