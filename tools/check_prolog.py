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
    cc = os.path.join(ROOT, 'compiler', 'cc')
    subprocess.run([cc, '-O', '-S', '-I', os.path.join(ROOT, 'lib'), '-o', os.path.join(WORK, 'prolog.s'), SRC],
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
    # (frameless() leaves csc's frame alone today - its parameter load reads our r7 - so
    # the prologue still loads the slot into r4, which the push saves across ext().)
    if not any(re.match(r'ldr\tr4, \[r7, #\d+\]$', l) for l in head):
        fail('csc reads its parameter from no stack slot: %r' % head)
    # cmixed: n (the bound) is loaded once before the loop and never read again - dead at
    # every call - while s (accumulator) and i (counter, re-read after ext returns) stay.
    if f.get('cmixed', [])[:1] != ['push\t{r4, r5, r7, lr}']:
        fail('cmixed push list changed: %r' % f.get('cmixed', [])[:1])
    # rec: self-recursion. Only one slot is promotable (the argument n), and it is read
    # after the recursive call - so it keeps its register. The accumulator lives in a
    # peep-created stack slot, which promote() never touches. One callee-saved reg today.
    if f.get('rec', [])[:1] != ['push\t{r4, r7, lr}']:
        fail('rec push list changed: %r' % f.get('rec', [])[:1])
    # cret: trimframe copies the epilogue pop into the branch that targets the exit label.
    if sum(1 for l in f.get('cret', []) if l == 'pop\t{r7, pc}') < 2:
        fail('cret lost its copied return: %r' % f.get('cret', []))
    # cfloat: promote parked y in s16; the vpush/vpop pair brackets the body.
    if not any(l.startswith('vpush\t{') for l in f.get('cfloat', [])):
        fail('cfloat lost its vpush: %r' % f.get('cfloat', [])[:4])
    # frameless(): cret keeps its frame pointer. Ruling: reading an incoming slot with
    # [r7, #off] forces the frame to stay. A frameless function saves one word less at
    # entry (push {lr} vs push {r7, lr}), so once any function in the call chain drops r7
    # the caller's fp and ours no longer see the same words - an incoming read then names
    # the wrong word (the leaf+early crash witness in tests/opt/frame.c). cret reads its
    # parameter that way, so entry push {r7, lr}, both pops keep r7, bookkeeping stays.
    body = f.get('cret', [])
    if body[:1] != ['push\t{r7, lr}']: fail('cret push: %r' % body[:1])
    if sorted(l for l in body if l.startswith('pop\t{') and l.endswith('pc}')) != ['pop\t{r7, pc}', 'pop\t{r7, pc}']:
        fail('cret pops: %r' % [l for l in body if 'pc}' in l])
    if 'mov\tr7, sp' not in body:
        fail('cret lost frame-pointer bookkeeping')
    # cfloat parks a float in s16: the vpush/vpop move sp around our locals, so r7 stays.
    cb = f.get('cfloat', [])
    if 'mov\tr7, sp' not in cb or 'mov\tsp, r7' not in cb or cb[:1] != ['push\t{r7, lr}']:
        fail('cfloat lost its frame pointer: %r' % cb[:2])
    # csys: the syscall builtin's shape. The kernel switches tasks inside sys_read/sys_write,
    # so r7 must keep pointing at this task's frame across the call: a converted fp-free body
    # would leave task_switch() restoring a stale pointer. sprel bails on any incoming-slot
    # read (A1), and the syscall sequence itself moves sp (push {r0} / add sp, #4), which the
    # screen bails on too. Pin: entry push keeps r7, bookkeeping stays, pop keeps r7.
    cs = f.get('csys', [])
    if cs[:1] != ['push\t{r7, lr}']: fail('csys lost its frame pointer: %r' % cs[:1])
    if 'mov\tr7, sp' not in cs or not any(l == 'pop\t{r7, pc}' for l in cs):
        fail('csys entry/exit bookkeeping: %r' % cs)

    # behavior: cc build (with the stub object) must print what native gcc prints
    subprocess.run([cc, '-c', '-I', os.path.join(ROOT, 'lib'), '-o', os.path.join(WORK, 'prolog_stub.o'), STUB],
                   check=True, cwd=ROOT)
    subprocess.run([cc, '-O', '-I', os.path.join(ROOT, 'lib'), '-o', os.path.join(WORK, 'prolog.bin'), SRC,
                    os.path.join(WORK, 'prolog_stub.o')], check=True, cwd=ROOT)
    out = subprocess.run(['qemu-arm', './host_linux', '-r', 'root', '/tmp/prolog.bin'],
                         capture_output=True, cwd=ROOT, timeout=30).stdout
    if out != b'check 9 16 3 8 6 16\n': fail('prolog.bin printed %r' % out)

    if bad: print('%d failed' % bad); return 1
    print('ok: prolog')
    return 0


if __name__ == '__main__':
    sys.exit(main())
