// ramfs.h - /tmp in PSRAM.  The compiler driver writes its .s and .o files to /tmp and reads
// them straight back; on the flash filesystem (about 56 KB/s) that writing was most of a board
// build.  The kernel sends every path that is /tmp or under /tmp/ here when ramfs_begin()
// succeeded.  Files are lost at reset, like any /tmp.
#pragma once
#include <FS.h>

// The RAM filesystem, or nullptr when the board has no PSRAM.  A write that would leave less
// than margin bytes of free PSRAM (for programs' data, stacks and malloc) fails short.
fs::FS *ramfs_begin(size_t margin);
