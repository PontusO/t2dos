// netfs.h - the network filesystem at /mnt/fs (see netfs.c).  Plain C, used by the board
// kernel (netfs.cpp, kernel.ino) and by host_linux.c.
#ifndef NETFS_H
#define NETFS_H
#ifdef __cplusplus
extern "C" {
#endif

#define NFS_ROOT "/mnt/fs"
enum { NFS_READ = 0, NFS_WRITE = 1, NFS_APPEND = 2 };     // the same values as OPEN_*

// The transport, written by the user of the core.  nt_send sends all n bytes or returns -1;
// nt_recv returns the bytes read (> 0), or -1 on timeout or a closed connection; nt_alloc
// returns 0 when the memory set aside for the cache is used up.
int nt_connect(const char *host, int port, int timeout_ms);
int nt_send(int h, const void *b, int n);
int nt_recv(int h, void *b, int n, int timeout_ms);
void nt_close(int h);
unsigned nt_millis(void);
void nt_sleep(int ms);
void *nt_alloc(int n);
void nt_free(void *p);

// host "" unmounts.  0, -1 if the server cannot be reached, -2 if dir is not a directory
// there, -3 if files or directories under /mnt/fs are open.  check=0 skips the server check.
int nfs_mount(const char *host, int port, const char *dir, int check);
int nfs_mounted(void);
int nfs_path(const char *abs);          // 1 if a resolved path is /mnt/fs or below it, while mounted
int nfs_stat(const char *abs, int *size, int *isdir);
int nfs_open(const char *abs, int mode);   // a handle >= 0, or -1
int nfs_read(int h, void *b, int n);
int nfs_write(int h, const void *b, int n);
int nfs_seek(int h, int pos);              // the new position, or -1
int nfs_tell(int h);
int nfs_size(int h);
int nfs_isdir(int h);
const char *nfs_name(int h);               // the path below the mount, for FileImpl::name()
int nfs_close(int h);                      // 0, or -1 if the save failed
int nfs_remove(const char *abs);
int nfs_mkdir(const char *abs);
int nfs_rmdir(const char *abs);
int nfs_rename(const char *a, const char *b);
int nfs_opendir(const char *abs);          // a handle >= 0, or -1
int nfs_readdir(int d, char *name, int namesz, int *size, int *isdir);   // 1, or 0 at the end
void nfs_closedir(int d);

#ifdef __cplusplus
}
#endif
#endif
