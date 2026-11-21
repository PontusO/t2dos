# NOTES: board cc miscompiles and the make --board flakiness (2026-11-21)

Two separate things were found while landing `make --board`. Do not
re-investigate from scratch; read this first.

## 1. cc VARREF double-load (settled earlier, still true)

The committed cc (and build/ref/cc at e4fb427, so check_ref cannot see it)
miscompiles rvalue reads of `char *` VARIABLES (VARREF emits a second load);
lvalue stores, array-slot addresses, and builtin calls that receive literal /
argv-slot / address-expression pointers work. make.c is written to that safe
dialect (strings as int offsets into buf; helpers xlen/xcmp/xlit over globals;
cmd[] slots only passed straight to builtins). See commits beff5ca.

## 2. make --board flakiness (found this round, NOT make's fault)

Symptom: `/bin/cc.bin -I /lib -o X /src/assembler/as.c` fails on the board,
error message varies run to run ("cannot write /tmp/ccNNNN_1_as.s", then
"as.c:18: error: no such member near token 130"), while the same binary
compiles nm.c/sh.c/cc.c fine, and as.c works sometimes. Parent (sh vs make vs
a tiny exec-wrapper) changes the outcome.

Measured facts (board Challenger 2350, ttyACM2):
- It is NOT flash full (df-style: 3.6 MB used / 7 MB) and NOT fd exhaustion
  (k_open slots reclaim per exec frame, kernel.ino ~943/964).
- /tmp is ramfs in PSRAM (kernel/ramfs.cpp), capacity down to TMP_MARGIN
  (1 MB) free PSRAM heap; leftover probe files shrink it. `free` shows the
  global psram number; ramfs room() checks getFreePSRAMHeap.
- A plain program could write 2 MB files to /tmp while cc failed to write its
  460 KB .s -> not raw capacity.
- On a freshly booted board every source builds byte-identically through
  make EXCEPT `assembler/as.c`, deterministically and without make involved:
  `/bin/cc.bin -S -I /lib -o /tmp/x.s /src/assembler/as.c` from the shell
  fails the same way, while ld/nm/ar/objdump/sh/basic all pass at the same
  moment and `free` never moves. as.c stays broken after reboot; other
  tools still build after it fails once.

Diagnosis status (not finished - next round picks it up here):
- the same cc.bin (byte-identical to the PC build) compiles the same as.c
  bytes fine under host_linux/qemu and as a gcc build on Linux; on the
  board the .s to FLASH (/src) succeeds while the .s to /tmp (ramfs) fails;
  a small program writing 512 KB files to /tmp succeeds in the same moment;
  cc's own 1.24 MB .s for cc.c to /tmp succeeds; ramfs.cpp audited, remove/
  grow free correctly; not fd exhaustion (per-exec-frame reclaim in
  kernel.ino ~943/964); not flash full; error text varies with session state
  (once even a bogus "no such member" parse error, at the #include line!).
- best remaining hypothesis: address-dependent PSRAM corruption tied to
  where cc's 64 KB obuf/name strings land in the PSRAM heap (layout shifts
  with parent program and /tmp usage), i.e. a kernel/PSRAM/cache bug that
  selfhost (single source, small /tmp) never triggers.
- reproduce recipe (board, fresh boot): `/bin/cc.bin -S -I /lib -o
  /tmp/zz.s /src/t2/zz.c` with zz.c = assembler/as.c copied to /src/t2.
  Compare with nm.c under the identical command (passes).

Open follow-up candidates (next rounds):
- unlink tmp .s in cc.c right after assemble (lower /tmp peak for
  multi-source targets like basic.bin).
- bisect as.c content at brace-balanced cuts for the board failure (host cc
  compiles every cut fine; find the cut where the BOARD starts failing and
  shrink from there); and/or instrument k_open/k_write error paths in a
  debug kernel build to see which step returns -1.

