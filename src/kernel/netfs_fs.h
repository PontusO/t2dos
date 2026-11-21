// netfs_fs.h - the arduino-pico side of the network filesystem (netfs.cpp)
#pragma once
#include <FS.h>
fs::FS *netfs_begin(void);
extern int netfs_close_result;   // nfs_close()'s result for the last file closed through the FS
