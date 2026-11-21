#!/usr/bin/env python3
"""Check cc -O's call-aware prologue work (tests/opt/prolog.c): the program's output
under qemu, and later the shapes of entry/exit per function.  Needs `make`.

  check_prolog.py      prints `ok: prolog` and exits 0 when every check passes
"""
import os, re, subprocess, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'tests', 'opt', 'prolog.c')
STUB = os.path.join(ROOT, 'tests', 'opt', 'prolog_stub.c')
WORK = os.path.join(ROOT, 'root', 'tmp')
bad = 0

# cleaf as today's -O emits it: call-free, so calllive() must change nothing here.
CLEAF = ['push\t{r4, r7, lr}', 'mov\tr7, sp', 'sub\tsp, #8', 'ldr\tr1, [r7, #12]',
         'movw\tr0, #3', 'muls\tr0, r1', 'mov\tr4, r0', 'adds\tr0, #1',
         'mov\tsp, r7', 'pop\t{r4, r7, pc}']


def cleaf_shape(body):
    return [l for l in body if not l.startswith('.L')]


def fail(msg):
    global bad
    bad = bad + 1
    print('FAIL %s' % msg)


def funcs(text):                       # label -> list of instruction lines, like check_frame.py
    out = {}
    cur = None
    for l in text.splitlines():
        m = re.match(r'([A-Za-z_]\w*):$', l)
        if m: cur = m.group(1); out[cur] = []; continue
        if cur and l and not l.startswith('\t.global'): out[cur].append(l.strip())
    return out


def main():
    os.makedirs(WORK, exist_ok=True)
    cc = os.path.join(ROOT, 'src', 'compiler', 'cc')
    subprocess.run([cc, '-O', '-S', '-I', os.path.join(ROOT, 'src', 'lib'), '-o', os.path.join(WORK, 'prolog.s'), SRC],
                   check=True, cwd=ROOT)
    f = funcs(open(os.path.join(WORK, 'prolog.s')).read())
    for name in ('csc', 'cleaf', 'cmixed', 'rec', 'clocals', 'cfloat', 'cret', 'csys', 'main'):
        if name not in f: fail('no function %s in prolog.s' % name)

    # calllive(): a promoted register must stay saved when its value is live across a call.
    # csc reads s after ext() returns, so s keeps its register: the prologue loads it
    # (ldr r4, [r7, #off] in the first lines) and the push saves r4.
    head = f.get('csc', [])[:5]
    if not any(l.startswith('push\t{r4') for l in head[:1]):
        fail('csc lost its callee-saved register: %r' % f.get('csc', [])[:1])
    # csc's reserved frame converts since 3b-iii: the parameter read maps to [sp, #16]
    # (k=12, sz=8, one word less pushed for the dropped r7) and add sp, #8 drains at the exit.
    if not any(re.match(r'ldr\tr4, \[sp, #16\]$', l) for l in head):
        fail('csc reads its parameter from no converted stack slot: %r' % head)
    if any('r7' in l for l in f.get('csc', [])):
        fail('csc kept r7: %r' % f.get('csc', [])[:2])
    # cmixed: n (the bound) is loaded once before the loop and never read again - dead at
    # every call - while s (accumulator) and i (counter, re-read after ext returns) stay.
    # Its reserved frame converts too: the bound read inside the loop maps at its own
    # live depth, exactly as cret's does.
    if f.get('cmixed', [])[:1] != ['push\t{r4, r5, lr}']:
        fail('cmixed push list changed: %r' % f.get('cmixed', [])[:1])
    # rec: self-recursion. Only one slot is promotable (the argument n), and it is read
    # after the recursive call - so it keeps its register. The accumulator lives in a
    # peep-created stack slot. Since 3c rec converts: the result save after bl rec
    # (str r0, [sp, #4] once the arg push drained) is an own-frame slot, and those keep
    # the offset they had framed (spmap drops the -4 below r7: the reservation hangs
    # off sp, which moved with the push list). Drains add sp, #8 at the shared exit.
    rb = f.get('rec', [])
    if rb[:1] != ['push\t{r4, lr}'] or any('r7' in l for l in rb):
        fail('rec is not fp-free: %r' % rb[:2])
    if 'add\tsp, #8' not in rb or not any(l == 'pop\t{r4, pc}' for l in rb):
        fail('rec does not drain and return: %r' % rb[-3:])
    # cret: trimframe copies the epilogue pop into the branch targeting the exit label,
    # and sprel converts the function: the parameter read frameless' old bail witness is
    # exactly what the depth walk maps safely ([r7, #k] -> [sp, #k+sz+d-4], one word less
    # pushed for the dropped r7). No r7 anywhere; both pops plain; the reload after
    # bl ext sits at [sp, #4] once add sp, #4 has drained the arg window.
    body = f.get('cret', [])
    if body[:1] != ['push\t{lr}']: fail('cret push: %r' % body[:1])
    if sorted(l for l in body if l.startswith('pop\t{') and l.endswith('pc}')) != ['pop\t{pc}', 'pop\t{pc}']:
        fail('cret pops: %r' % [l for l in body if 'pc}' in l])
    if any('r7' in l for l in body):
        fail('cret kept r7 bookkeeping: %r' % body[:2])
    # cfloat: promote parked y in s16; the vpush/vpop pair brackets the body.
    if not any(l.startswith('vpush\t{') for l in f.get('cfloat', [])):
        fail('cfloat lost its vpush: %r' % f.get('cfloat', [])[:4])
    # cfloat parks a float in s16: the vpush/vpop move sp around our locals, so r7 stays.
    cb = f.get('cfloat', [])
    if 'mov\tr7, sp' not in cb or 'mov\tsp, r7' not in cb or cb[:1] != ['push\t{r7, lr}']:
        fail('cfloat lost its frame pointer: %r' % cb[:2])
    # csys: the syscall builtin's shape. The window converts since 3d: the prescan's
    # skipped spans are recorded (wstart/wend) and fl_frame excuses every line inside
    # one, so the mov r2/r4, sp and mov sp, r3/r4 lines that the depth walk proved to
    # cancel line for line no longer bail the acceptance scan. The kernel switches tasks
    # inside sys_read/sys_write saving only the user sp (sys_sp) and never touches r4.
    # Pin: fp-free, the argv words drained by their own cleanup add, plain exit pop.
    cs = f.get('csys', [])
    if cs[:1] != ['push\t{lr}'] or any('r7' in l for l in cs):
        fail('csys keeps a frame: %r' % cs[:2])
    if 'add\tsp, #12' not in cs or not any(l == 'pop\t{pc}' for l in cs):
        fail('csys does not drain the argv words or returns wrong: %r' % cs[-3:])

    # mixed chain: cmix is sprel-converted - the frameless caller's hold load reads
    # [sp, #16] (one word less pushed: r7 dropped). Its callees cover both shapes:
    # csys (converted with its argv window since 3d, pinned above), stublea (its
    # descriptor leas and float slot map through sp since 3c) - both converted,
    # across the object boundary - and sframe, from the hand-written
    # prolog_stubframed.s assembled below: never through cc, so always framed,
    # reading [r7, #12]. The linked run pins their agreement with the native gcc
    # reference (19 = stublea(5,1) + stublea(5,2) = 8 + 11; csys adds read(0, b, 0)
    # = 0, sframe(0) - 1 = 0).
    cm = f.get('cmix', [])
    if cm[:1] != ['push\t{r4, lr}'] or any('r7' in l for l in cm):
        fail('cmix is not fp-free (sprel-converted): %r' % cm[:3])
    if not any(re.match(r'ldr\tr4, \[sp, #16\]$', l) for l in cm):
        fail('cmix reads no converted band slot: %r' % cm[:3])
    subprocess.run([cc, '-O', '-S', '-I', os.path.join(ROOT, 'src', 'lib'),
                    '-o', os.path.join(WORK, 'prolog_stub.s'), STUB], check=True, cwd=ROOT)
    sb = funcs(open(os.path.join(WORK, 'prolog_stub.s')).read()).get('stublea', [])
    if any('r7' in l for l in sb) or 'add\tsp, #32' not in sb:
        fail('stublea is not fp-free since 3c: %r' % sb[:4])

    # behavior: cc build (with both stub objects) must print what native gcc prints
    subprocess.run([cc, '-c', '-I', os.path.join(ROOT, 'src', 'lib'), '-o', os.path.join(WORK, 'prolog_stub.o'), STUB],
                   check=True, cwd=ROOT)
    subprocess.run([os.path.join(ROOT, 'build', 'host', 'as'), '-o', os.path.join(WORK, 'prolog_stubframed.o'),
                    os.path.join(ROOT, 'tests', 'opt', 'prolog_stubframed.s')], check=True, cwd=ROOT)
    subprocess.run([cc, '-O', '-I', os.path.join(ROOT, 'src', 'lib'), '-o', os.path.join(WORK, 'prolog.bin'), SRC,
                    os.path.join(WORK, 'prolog_stub.o'),
                    os.path.join(WORK, 'prolog_stubframed.o')], check=True, cwd=ROOT)
    out = subprocess.run(['qemu-arm', './host_linux', '-r', 'root', '/tmp/prolog.bin'],
                         capture_output=True, cwd=ROOT, timeout=30).stdout
    if out != b'check 9 16 3 8 6 16 19\n': fail('prolog.bin printed %r' % out)

    if bad: print('%d failed' % bad); return 1
    print('ok: prolog')
    return 0


if __name__ == '__main__':
    sys.exit(main())
