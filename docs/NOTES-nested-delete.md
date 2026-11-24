# NOTES: board nested-delete corruption (found during Round E board session)

Goal: make check_make --board green. One FAIL: board-mkfeat-v1 (make V=1 clean).
Root cause is NOT make/rm userland. CORRECTED 2026-11-24 (the board has NO SD
card: boot banner says "fs flash" - the primary FS is LittleFS in QSPI flash,
not SDFS; the original SDFS attribution below was an unverified inference from
the kernel's first-choice mount branch). The culprit is the core's LittleFS
WRAPPER, libraries/LittleFS/src/LittleFS.h (core 6.3.2) LittleFSImpl::remove():
after lfs_remove(path) succeeds it walks UP the path and silently
lfs_remove()s every ancestor directory that is now empty ("remove any empty
subdirs this makes"), and rmdir() is literally `return remove(path)`. So
remove("/a/b/c.txt") deletes /a/b (the removal just emptied it), then /a if
that is empty too. Depth-2 looked correct only because /a still had siblings.
rename/open/stat/mkdir never touch this code, which is why they were fine.
littlefs itself is NOT at fault; the wrapper's cascade is. (Original findings,
re-read with the correct FS:)

  - rm /a/b/c.txt   -> deletes /a/b   (parent dir!), c.txt's entry untouched
  - rmdir /a/b/c    -> removes /a/b   (parent dir!)
  - open/stat/mkdir/ls/traverse at any depth: CORRECT
  - mv /a/b/f -> /a/b/g (files): CORRECT (rename traverses fine, tested ok)
  - depth <= 2 (/src/mkfeat2): correct
  - ramfs (/tmp): correct -> the wrapper cascade is the only broken path;
    kernel resolve() is fine

Caveat on the table (pre-correction reading): with the cascade known, the
"parent deleted, target untouched" phrasing is suspect - lfs_remove DOES remove
the target first, then eats the emptied ancestors; what survived was the
target's DATA under a deleted directory entry, not a skipped removal.

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

Root cause located in source (core 6.3.2, libraries/LittleFS/src/LittleFS.h,
LittleFSImpl::remove ~line 118): after lfs_remove(path) succeeds it strrchr's
back over the path calling lfs_remove on every ancestor ("Now try and remove
any empty subdirs this makes, silently"); rmdir() = remove(). lfs_remove on an
empty dir succeeds, so the just-emptied parent is deleted, then grandparent,
and so on. littlefs proper behaves correctly.

Status: FIXED in kernel (branch make-vars-recipes, merged): k_del_saf hoists
deep deletes to the FS root by rename (traversal correct at every depth) and
deletes the shallow copy. Gated to the primary FS (fs == fsys - LittleFS on
this board) and depth >= 3; ramfs hoists into /tmp (ramfs rejects paths outside
it - caught by on-board probe). The hoist is exactly the right shape for this
root cause: at depth 1 the ancestor walk reaches the leading '/', calls
lfs_remove("") and fails harmlessly, so nothing else is deleted.
Verified on board: rm -rf of a 3-level tree, nested file unlink, cross-FS mv
(ramfs->flash incl. its nested source unlink), rmdir chains; stat/open/mkdir/
rename untouched.

The kernel HARD-HANG from the earlier delete storm: consistent with the
cascade deleting directories out from under live handles; with k_del_saf the
cascading calls no longer reach the wrapper. Do not re-run the old repro storm.

Next steps:
1. Done: check_make --board full green, check_toolchain --selfhost six IDENTICAL.
2. Upstream: PARKED by owner decision 2026-11-24 - the cascade is deliberate
   SPIFFS-compat emulation (identical code in the esp8266 core; other littlefs
   wrappers like Adafruit's and Teensy's do a plain lfs_remove), so the ask
   would be "gate it behind a flag" and a fast answer is unlikely. The kernel
   workaround stands on its own; re-check the wrapper on a core bump and drop
   k_del_saf if the cascade ever gets gated.
3. host_linux needs no change (host FS is correct).

Board session state before the hang (all still valid):
  PASS x8 byte-identical through Makefile.board (cc 39 s, basic 28 s),
  PASS board-mkdir-p, PASS board-mkfeat-default (echo.bin byte-identical).
