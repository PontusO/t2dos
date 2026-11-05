#!/usr/bin/env python3
# Writes build/obj/sample.o in the T2DOS object format (see lib/t2obj.h).
import os, struct, sys

here = os.path.dirname(os.path.abspath(__file__))
out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(here, "..", "..", "build", "obj", "sample.o")
SEC_UNDEF, SEC_TEXT, SEC_DATA, SEC_ABS = 0, 1, 2, 3
SYM_GLOBAL = 1
R_BL = 1

text = bytes(16)
data = bytes(8)
strtab = b""


def name(s):
    global strtab
    off = len(strtab)
    strtab += s.encode() + b"\0"
    return off


syms = [
    (name("main"), SEC_TEXT, 0, SYM_GLOBAL),
    (name("helper"), SEC_TEXT, 8, 0),
    (name("counter"), SEC_DATA, 4, SYM_GLOBAL),
    (name("puts"), SEC_UNDEF, 0, SYM_GLOBAL),
]
rels = [(SEC_TEXT, 4, 3, R_BL, 0)]   # bl puts at text+4

blob = struct.pack("<7i", 0x424F3254, 1, len(text), len(data), len(syms), len(rels), len(strtab))
blob += text + data
for s in syms:
    blob += struct.pack("<4i", *s)
for r in rels:
    blob += struct.pack("<5i", *r)
blob += strtab
os.makedirs(os.path.dirname(out), exist_ok=True)
open(out, "wb").write(blob)

# bad objects, next to the sample, for the nm error tests
outdir = os.path.dirname(os.path.abspath(out))
def bad(fname, data):
    open(os.path.join(outdir, fname), "wb").write(data)

bad("bad-magic.o", b"XXXX" + blob[4:])
bad("bad-trunc.o", blob[:-6])                       # header claims more than the file holds
badsyms = list(syms)
badsyms[0] = (len(strtab) + 100, SEC_TEXT, 0, SYM_GLOBAL)   # name offset past the string table
blob2 = struct.pack("<7i", 0x424F3254, 1, len(text), len(data), len(badsyms), len(rels), len(strtab)) + text + data
for s in badsyms:
    blob2 += struct.pack("<4i", *s)
for r in rels:
    blob2 += struct.pack("<5i", *r)
bad("bad-name.o", blob2 + strtab)
huge = struct.pack("<7i", 0x424F3254, 1, 0, 0, 0x10000000, 0, 0)   # nsyms * 16 wraps to 0
bad("bad-count.o", huge)
