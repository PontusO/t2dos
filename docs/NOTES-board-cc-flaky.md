# NOTES: board cc "cannot write /tmp/..." - RESOLVED 2026-11-21

Symptom (was): on-board `/bin/cc.bin -I /lib -o X /src/assembler/as.c`
and the multi-source `basic.bin` failed with `cannot write /tmp/ccNNNN_1_x.s`
(error text sometimes varied), while single-source tools built fine. Made
visible by `make --board`; NOT make's fault.

## Root cause

Memory budget, not corruption. `readfile()` malloced **1 MB per file**
(source + every `#include`) and cc also mallocs `data` = 1 MB. On the
board the arena lives in the ~7.8 MB PSRAM heap; since commit `de0a5bc`
(2026-11-16, "Keep /tmp in PSRAM") `/tmp` is ramfs and its block
allocator refuses to descend below `TMP_MARGIN` (1 MB) free - and cc's
temporary `.s` files live in `/tmp`. as.c holds 3 MB of srcbufs for 72 KB
of content (as.c + t2obj.c + t2obj.h); basic peaks at 3 MB at a time
(source + basic.h + sys.h). Peak cc ≈ 6.8 MB -> free hits ~1,045,000 <
TMP_MARGIN+4096, ramfs refuses a block for cc's own `.s` write
(`grow()` room() false), `oflush` sees a short write and prints
"cannot write". Intermittency was luck at that ~50 KB cliff (contents of
/tmp, allocation order); it "worked flawlessly before" because before
`de0a5bc` `/tmp` was on flash: no margin floor, no PSRAM pressure.
Under host_linux/qemu the same binary passes (host heap is unbounded) -
which is why only board runs caught it.

## Fix (this round)

`readfile()` now `stat`s the resolved path and mallocs `size+1`, growing
(doubling, updating `srcbuf`) only if the file changed under it; the PC
gcc build keeps the old 1 MB fixed buffer (its `stat` binds to glibc -
using it smashed the stack until the probe got an `#ifdef __T2CC__`).
Codegen unchanged: `check_ref` still `different: 0`.

## Method notes (what finally pinned it)

- Instrumented cc (`ccdbg`): psramfree + obuf at the `.s` open; kernel
  `k_open`/`k_write` and ramfs `grow`/`make` prints via a debug kernel
  flash. The decisive line: `k_write want 60001 got 0 psram 1045416` -
  under the 1 MB margin, at a WRITE (open had succeeded: "cannot write"
  is printed by oflush too, not only by the open failure!).
- The `k_open fs-null /basic.h` lines are a red herring: readfile's
  first raw-name attempt against cwd, then `-I` succeeds. Normal.
- Debug kernel: copy kernel.ino/ramfs.cpp, add Serial.printf in failure
  branches only, compile+upload (Challenger FQBN per AGENTS.md), restore
  from backup afterwards. `arduino-cli upload` keeps the flash FS.
