# NOTES: board nested-delete corruption (found during Round E board session)

Goal: make check_make --board green. One FAIL: board-mkfeat-v1 (make V=1 clean).
Root cause is NOT make/rm userland. The SD/SDFS stack mis-resolves path-based
DELETES for paths with >= 3 components:

  - rm /a/b/c.txt   -> deletes /a/b   (parent dir!), c.txt's entry untouched
  - rmdir /a/b/c    -> removes /a/b   (parent dir!)
  - open/stat/mkdir/ls/traverse at any depth: CORRECT
  - mv /a/b/f -> /a/b/g (files): CORRECT (rename traverses fine, tested ok)
  - depth <= 2 (/src/mkfeat2): correct
  - ramfs (/tmp): correct -> SDFS/SdFat core 6.3.2 only, not kernel resolve()

rm1() then re-rescans the vanished path -> stat fails -> "rm: <path> failed"
(the board-mkfeat-v1 symptom). The spin guard does not fire (path is gone).

SECOND, WORSE FINDING: while exercising mkdir/mv/rmdir/remove on /src/rw, the
kernel HARD-HUNG (no serial bytes at all; needs reset/BOOTSEL; happened
immediately after mv /src/rw/a/b /src/rw/a/c + rmdir sequence; also ls /src
answered empty just before death). Suspect kernel fault on dangling dir handle
after a wrong-entry delete. MUST fix kernel robustness before any more
nested-delete testing on board.

Ruled out: kernel resolve() (canonical paths), fs_for (ramfs ok), time callback
(read-only), kernel build itself (reflash boots fine, 8/8 byte-compares green).

Not yet found in source: SdFat 6.3.2 remove(path) = tmp.open(path,O_WRONLY) &&
tmp.remove(); the off-by-one walk (lands on parent) is somewhere in
FatFile::open(O_WRONLY)/openByPath or the dir-entry bookkeeping. rename uses
the same walk and works, which is the puzzle.

Status: FIXED in kernel (branch make-vars-recipes): k_del_saf hoists deep
deletes to the FS root by rename (traversal proven correct at every depth) and
deletes the shallow copy. Gated to SDFS (fs == fsys) and depth >= 3; ramfs
hoists into /tmp (ramfs rejects paths outside it - caught by on-board probe).
Verified on board: rm -rf of a 3-level tree, nested file unlink, cross-FS mv
(ramfs->SD incl. its nested source unlink), rmdir chains; stat/open/mkdir/
rename untouched.

The kernel HARD-HANG from the earlier delete storm: root cause is the same
wrong-entry delete walking into freed cache state; with k_del_saf the corrupting
calls no longer reach SDFS. Do not re-run the old repro storm.

Next steps:
1. Rerun check_make --board to full green, then check_toolchain --selfhost.
2. Upstream: report the SDFS/SdFat nested-remove bug (core 6.3.2); revisit the
   workaround if a vendored SDFS fix lands.
3. host_linux needs no change (host FS is correct).

Board session state before the hang (all still valid):
  PASS x8 byte-identical through Makefile.board (cc 39 s, basic 28 s),
  PASS board-mkdir-p, PASS board-mkfeat-default (echo.bin byte-identical).
