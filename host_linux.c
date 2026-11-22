// host_linux.c - T2DOS kernel contract on Linux/ARM (qemu-arm), for testing userland on a PC.
//   qemu-arm ./host_linux /bin/sh.bin         (paths are relative to a root dir, default ./root)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>
#include <setjmp.h>
#include <dirent.h>
#include <poll.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "sys.h"
#include "src/kernel/tz.h"

#define MAX_NEST 4
static jmp_buf jbs[MAX_NEST]; static int exit_codes[MAX_NEST]; static int nest = -1;
static char root[256] = "./root"; static char cwd[128] = "/";
static DIR *dirs[4]; static char dirpaths[4][300];
static int stdout_fd = 1;
static char fmtbuf[1024];
#define A(i) (argv[n - 1 - (i)])

static const char *resolve(const char *in, char *out, int outsz) {
  char tmp[256];
  if (in[0] == '/') strncpy(tmp, in, sizeof tmp - 1), tmp[sizeof tmp - 1] = 0;
  else snprintf(tmp, sizeof tmp, "%s%s%s", cwd, strcmp(cwd, "/") ? "/" : "", in);
  int n = 0; out[n++] = '/'; out[n] = 0;
  for (char *t = strtok(tmp, "/"); t; t = strtok(NULL, "/")) {
    if (!strcmp(t, ".") || !*t) continue;
    if (!strcmp(t, "..")) { while (n > 1 && out[n - 1] != '/') n--; if (n > 1) n--; out[n] = 0; continue; }
    if (n > 1) out[n++] = '/';
    int l = strlen(t); if (n + l + 1 >= outsz) break;
    memcpy(out + n, t, l); n += l; out[n] = 0;
  }
  return out;
}
static const char *hostpath(const char *in, char *out) { char r[160]; resolve(in, r, sizeof r); sprintf(out, "%s%s", root, r); return out; }

static int fmt_num(char *out, unsigned v, int base, int neg) {
  char t[12]; int n = 0;
  do { int d = v % base; t[n++] = d < 10 ? '0' + d : 'a' + d - 10; v /= base; } while (v);
  int k = 0; if (neg) out[k++] = '-';
  while (n) out[k++] = t[--n];
  return k;
}
static int k_format(char *out, int max, const char *fmt, int *argv, int n, int first) {
  int o = 0, ai = first; char tmp[64];
  #define PUT(c) do { if (o < max - 1) out[o] = (c); o++; } while (0)
  for (const char *f = fmt; *f; f++) {
    if (*f != '%') { PUT(*f); continue; }
    f++;
    int left = 0, zero = 0, width = 0;
    if (*f == '-') { left = 1; f++; }
    if (*f == '0') { zero = 1; f++; }
    while (*f >= '0' && *f <= '9') width = width * 10 + *f++ - '0';
    int prec = -1;
    if (*f == '.') { f++; prec = 0; while (*f >= '0' && *f <= '9') prec = prec * 10 + *f++ - '0'; }
    const char *s = tmp; int len;
    if (*f == 'd') { int v = A(ai++); len = fmt_num(tmp, v < 0 ? -v : v, 10, v < 0); }
    else if (*f == 'u') { len = fmt_num(tmp, A(ai++), 10, 0); }
    else if (*f == 'x') { len = fmt_num(tmp, A(ai++), 16, 0); }
    else if (*f == 'c') { tmp[0] = A(ai++); len = 1; }
    else if (*f == 's') { s = (const char *)A(ai++); if (!s) s = "(null)"; len = strlen(s); }
    else if (*f == '%') { tmp[0] = '%'; len = 1; }
    else if (*f == 'f' || *f == 'e' || *f == 'g') {     // the argument is a float's bits (T2CC floats are 32-bit)
      union { int i; float v; } u; u.i = A(ai++);
      char spec[8] = { '%', '.', 0 }; int sp = 2, pr = prec < 0 ? 6 : (prec > 30 ? 30 : prec);
      if (pr >= 10) spec[sp++] = '0' + pr / 10;
      spec[sp++] = '0' + pr % 10; spec[sp++] = *f; spec[sp] = 0;
      len = snprintf(tmp, sizeof tmp, spec, (double)u.v);
      if (len < 0) len = 0; if (len >= (int)sizeof tmp) len = sizeof tmp - 1;
    }
    else { PUT('%'); PUT(*f); continue; }
    if (!left && zero && *f != 's' && s[0] == '-') { PUT('-'); s++; len--; width--; }   // -0005, not 00-5
    if (!left) for (int i = len; i < width; i++) PUT(zero && *f != 's' ? '0' : ' ');
    for (int i = 0; i < len; i++) PUT(s[i]);
    if (left) for (int i = len; i < width; i++) PUT(' ');
  }
  out[o < max ? o : max - 1] = 0;
  return o;
}

static int run_bin(const char *name, int argc, char **argv);
static char *netfs_loadbuf;          // a program read from /mnt/fs, while run_bin loads it
static int millis_() { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec * 1000 + tv.tv_usec / 1000; }
static int kbhit() { struct pollfd p = { 0, POLLIN, 0 }; return poll(&p, 1, 0) > 0 && (p.revents & POLLIN); }   // not at the end of piped input
static int listen_fd = -1; static char ipbuf[20] = "127.0.0.1";
static int sock_recv(int fd, char *buf, int n, int timeout) {
  struct pollfd p = { fd, POLLIN, 0 }; int r = poll(&p, 1, timeout);
  if (r == 0) return 0; if (r < 0) return -1;
  r = read(fd, buf, n); return r > 0 ? r : -1;
}
static int sock_connect(const char *host, int port) {
  struct addrinfo hints = {0}, *res; char ps[8]; hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; sprintf(ps, "%d", port);
  if (getaddrinfo(host, ps, &hints, &res)) return -1;
  int fd = socket(AF_INET, SOCK_STREAM, 0);
  if (connect(fd, res->ai_addr, res->ai_addrlen)) { close(fd); fd = -1; }
  freeaddrinfo(res); return fd;
}

// ping: the PC's own ping program, since unprivileged ICMP sockets are often switched off
static int host_ping(const char *host, char *ip) {
  struct addrinfo hints = {0}, *res; hints.ai_family = AF_INET;
  if (getaddrinfo(host, 0, &hints, &res)) return -2;
  inet_ntop(AF_INET, &((struct sockaddr_in *)res->ai_addr)->sin_addr, ip, 16); freeaddrinfo(res);
  char cmd[64], line[256]; snprintf(cmd, sizeof cmd, "ping -c 1 -W 1 -n %s 2>/dev/null", ip);
  FILE *f = popen(cmd, "r"); if (!f) return -1;
  int ms = -1; char *t;
  while (fgets(line, sizeof line, f)) if ((t = strstr(line, "time=")) && ms < 0) ms = (int)(atof(t + 5) + 0.5);
  pclose(f); return ms;
}

// named pipes under /pipe/ (one task on the host: nest == owner)
#define PIPE_FD 200
#define MAX_PIPES 8
#define MAX_PIPE_FDS 16
static struct { int used; char name[32]; int owner; char buf[PIPE_SIZE]; int head, count; int bounds[8], nbounds; long total_read, total_written; } pipes[MAX_PIPES];
static struct { int pipe, mode, owner, wrote, gen; } pfd[MAX_PIPE_FDS];
static int pipe_gen[MAX_PIPES];   // bumped on each mkpipe: an fd opened on an earlier pipe in the slot is dead
static int pfd_live(int e) { int i = pfd[e].pipe; return pipes[i].used && pipe_gen[i] == pfd[e].gen; }

static int is_pipename(const char *p) { return strncmp(p, "/pipe/", 6) == 0 && p[6] != 0; }
// resolved pipe name (the part after /pipe/) for a user path, or 0 if it is not under /pipe/
static const char *pipe_path(const char *in, char *buf, int sz) { resolve(in, buf, sz); return is_pipename(buf) ? buf + 6 : 0; }
static int find_pipe(const char *name) { for (int i = 0; i < MAX_PIPES; i++) if (pipes[i].used && !strcmp(pipes[i].name, name)) return i; return -1; }
static void pipe_remove(int i) { pipes[i].used = 0; }
static void owner_pipes_remove(int owner) { for (int i = 0; i < MAX_PIPES; i++) if (pipes[i].used && pipes[i].owner == owner) pipe_remove(i); }
// next message boundary at or after pos, or -1
static int next_bound(int i, long pos) { int b = -1; for (int k = 0; k < pipes[i].nbounds; k++) if (pipes[i].bounds[k] >= pos && (b < 0 || pipes[i].bounds[k] < b)) b = pipes[i].bounds[k]; return b; }
// drops one boundary at `at` (two writers closing at the same position are two messages)
static void bound_drop(int i, long at) { for (int k = 0; k < pipes[i].nbounds; k++) if (pipes[i].bounds[k] == at) { pipes[i].bounds[k] = pipes[i].bounds[--pipes[i].nbounds]; return; } }
static int pfd_find(int fd) { int i = fd - PIPE_FD; if (i >= 0 && i < MAX_PIPE_FDS && pfd[i].pipe >= 0) return i; return -1; }
static int pfd_alloc(int pipe, int mode) { for (int i = 0; i < MAX_PIPE_FDS; i++) if (pfd[i].pipe < 0) { pfd[i].pipe = pipe; pfd[i].mode = mode; pfd[i].owner = nest; pfd[i].wrote = 0; pfd[i].gen = pipe_gen[pipe]; return PIPE_FD + i; } return -1; }
static int pipe_write(int e, const char *buf, int n) {
  if (e < 0 || pfd[e].mode != 1) return -1;
  int i = pfd[e].pipe; if (!pfd_live(e)) return -1;
  if (n > PIPE_SIZE - pipes[i].count) n = PIPE_SIZE - pipes[i].count;   // one task: a full pipe cannot drain, short write
  if (n < 0) n = 0;
  for (int k = 0; k < n; k++) pipes[i].buf[(pipes[i].head + pipes[i].count + k) % PIPE_SIZE] = buf[k];
  pipes[i].count += n; pipes[i].total_written += n; pfd[e].wrote = 1;
  return n;
}
// ---- the network filesystem (/mnt/fs): kernel/netfs.c over POSIX sockets
#define NFS_FD 300                     // fds NFS_FD.. are netfs handles; directory handles 4..7
#define NFS_NFD 16
static int nfs_fd(int fd) { return fd >= NFS_FD && fd < NFS_FD + NFS_NFD ? fd - NFS_FD : -1; }
int nt_connect(const char *host, int port, int timeout_ms) {
  struct addrinfo hints = {0}, *ai; char ps[12]; int fd, e = 0; socklen_t el = sizeof e;
  hints.ai_family = AF_INET; hints.ai_socktype = SOCK_STREAM; sprintf(ps, "%d", port);
  if (getaddrinfo(host, ps, &hints, &ai)) return -1;
  fd = socket(AF_INET, SOCK_STREAM, 0);
  if (fd < 0) { freeaddrinfo(ai); return -1; }
  fcntl(fd, F_SETFL, O_NONBLOCK);
  int r = connect(fd, ai->ai_addr, ai->ai_addrlen); freeaddrinfo(ai);
  if (r < 0) {
    struct pollfd p = { fd, POLLOUT, 0 };
    if (poll(&p, 1, timeout_ms) != 1 || getsockopt(fd, SOL_SOCKET, SO_ERROR, &e, &el) || e) { close(fd); return -1; }
  }
  fcntl(fd, F_SETFL, 0);
  return fd;
}
int nt_send(int h, const void *b, int n) {
  int done = 0; while (done < n) { int r = send(h, (const char *)b + done, n - done, MSG_NOSIGNAL); if (r <= 0) return -1; done += r; }
  return n;
}
int nt_recv(int h, void *b, int n, int timeout_ms) {
  struct pollfd p = { h, POLLIN, 0 };
  if (poll(&p, 1, timeout_ms) != 1) return -1;
  int r = recv(h, b, n, 0); return r > 0 ? r : -1;
}
void nt_close(int h) { close(h); }
unsigned nt_millis(void) { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec * 1000 + tv.tv_usec / 1000; }
void nt_sleep(int ms) { usleep(ms * 1000); }
// T2_NETFS_CACHE (bytes) caps the cache, so tests can fill it
static long nt_cap = -1, nt_used;
void *nt_alloc(int n) {
  if (nt_cap < 0) { const char *e = getenv("T2_NETFS_CACHE"); nt_cap = e ? atol(e) : 0; }
  if (nt_cap && nt_used + n > nt_cap) return 0;
  char *p = malloc(n + 8); if (!p) return 0;
  *(int *)p = n; nt_used += n; return p + 8;
}
void nt_free(void *p) { if (p) { nt_used -= *(int *)((char *)p - 8); free((char *)p - 8); } }
#include "src/kernel/netfs.c"

// ---- telnetd: kernel/telnetd.c over POSIX sockets.  The setting lives until host_linux exits.
static int td_lfd = -1, td_port;
int td_accept(void) {
  struct pollfd p = { td_lfd, POLLIN, 0 };
  if (td_lfd < 0 || poll(&p, 1, 0) != 1) return -1;
  return accept(td_lfd, 0, 0);
}
int td_recv(int h, void *b, int n) {
  struct pollfd p = { h, POLLIN, 0 };
  if (poll(&p, 1, 0) != 1) return 0;
  int r = recv(h, b, n, 0); return r > 0 ? r : -1;
}
int td_send(int h, const void *b, int n) { return nt_send(h, b, n); }
int td_room(int h) { struct pollfd p = { h, POLLOUT, 0 }; return poll(&p, 1, 0) == 1 && (p.revents & POLLOUT) ? 4096 : 0; }
void td_close(int h) { close(h); }
void td_peer(int h, char *ip, int sz) {
  struct sockaddr_in a; socklen_t l = sizeof a;
  if (getpeername(h, (struct sockaddr *)&a, &l) || !inet_ntop(AF_INET, &a.sin_addr, ip, sz)) strcpy(ip, "?");
}
void td_note(const char *m) { char b[128]; int l = snprintf(b, sizeof b, "\n%s\n", m); write(1, b, l); }
unsigned td_millis(void) { return nt_millis(); }
#include "src/kernel/telnetd.c"
static int k_telnetd(int port, const char *pw, char *ip) {
  if (port == -1) { if (ip) strcpy(ip, td_live_ip()); return td_port ? td_port | TELNETD_LISTENING | (td_live() ? TELNETD_LIVE : 0) : 0; }
  if (port == 0) { td_stop("telnetd stopped"); if (td_lfd >= 0) close(td_lfd); td_lfd = -1; td_port = 0; return 0; }
  if (port < 0 || port > 65535 || !pw || !*pw || strlen(pw) > TD_PASSMAX) return TELNETD_BADARG;
  if (port != td_port) {             // a new port: a new listener, the live session stays
    int fd = socket(AF_INET, SOCK_STREAM, 0), one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(port); a.sin_addr.s_addr = INADDR_ANY;
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 2)) { close(fd); return TELNETD_NOSAVE; }
    if (td_lfd >= 0) close(td_lfd);
    td_lfd = fd; td_port = port;
  }
  td_start(pw);
  return 0;
}
// the console: stdin, and the telnet session when telnetd is on
static int con_getc(void) {
  for (;;) {
    td_flush(); td_poll();
    if (td_avail()) return td_getc();
    if (td_lfd < 0 || kbhit()) { unsigned char c; if (read(0, &c, 1) != 1) exit(0); return c; }
    struct pollfd p = { 0, POLLIN, 0 }; poll(&p, 1, 20);
  }
}
static int con_kbhit(void) { td_flush(); td_poll(); return kbhit() || td_avail(); }
static int nfs_owner[NFS_NFD] = { -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1 };
static int nfs_dir_owner[4] = { -1, -1, -1, -1 };
// a rename that is not inside one mount (SD and a mount, or two mounts): copy, then remove the
// old one (like k_rename)
static int nfs_cross_rename(const char *ra, const char *rb) {
  char pa[300], pb[300], buf[4096]; int na = nfs_path(ra), nb = nfs_path(rb), in, out, k, ok = 1; struct stat st;
  if (na) { in = nfs_open(ra, NFS_READ); if (in >= 0 && nfs_isdir(in)) { nfs_close(in); return -1; } }
  else { if (stat(hostpath(ra, pa), &st) || S_ISDIR(st.st_mode)) return -1; in = open(pa, O_RDONLY); }
  if (in < 0) return -1;
  out = nb ? nfs_open(rb, NFS_WRITE) : open(hostpath(rb, pb), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (out < 0) { if (na) nfs_close(in); else close(in); return -1; }
  while ((k = na ? nfs_read(in, buf, sizeof buf) : read(in, buf, sizeof buf)) > 0)
    if ((nb ? nfs_write(out, buf, k) : write(out, buf, k)) != k) { ok = 0; break; }
  if (k < 0) ok = 0;
  if (na) nfs_close(in); else close(in);
  if ((nb ? nfs_close(out) : close(out)) < 0) ok = 0;
  if (!ok) { if (!nb) unlink(pb); return -1; }
  return na ? nfs_remove(ra) : unlink(pa);
}
// /mnt/name on the root, so ls /mnt shows the mounts
static void mnt_dir(const char *name) {
  char p[300]; snprintf(p, sizeof p, "%s/mnt", root); mkdir(p, 0755);
  snprintf(p, sizeof p, "%s/mnt/%s", root, name); mkdir(p, 0755);
}
// after an unmount (or umount of a name not mounted): removes /mnt/name if it is empty
static void mnt_undir(const char *name) {
  if (!*name || !strcmp(name, ".") || !strcmp(name, "..") || strchr(name, '/') || strlen(name) > 15) return;
  char p[300]; snprintf(p, sizeof p, "%s/mnt/%s", root, name);
  rmdir(p);                          // fails, and keeps it, unless empty
}
static int nfs_mode(int fl) { return fl == OPEN_APPEND ? NFS_APPEND : (fl & 1) ? NFS_WRITE : NFS_READ; }
// reads /etc/mount and mounts every line without asking the server; a line that cannot be
// mounted (bad name, too long, no free mount) is skipped
static void nfs_boot(void) {
  char p[300], buf[2048]; snprintf(p, sizeof p, "%s/etc/mount", root);
  FILE *f = fopen(p, "r"); if (!f) return;
  int n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n > 0 ? n : 0] = 0;
  nfs_boot_text(buf);
}

static int k_write(int fd, const char *buf, int n) {
  if (fd == 1) fd = stdout_fd;
  if (nfs_fd(fd) >= 0) return nfs_write(nfs_fd(fd), buf, n);
  if (fd >= PIPE_FD && fd < PIPE_FD + MAX_PIPE_FDS) return pipe_write(pfd_find(fd), buf, n);
  if (fd == 1) { for (int i = 0; i < n; i++) td_putc(buf[i]); td_flush(); }
  return write(fd, buf, n);
}

// maths calls: a float argument or result is the float's bit pattern in an int word
static float wf(int w) { union { int i; float v; } u; u.i = w; return u.v; }
static int fw(float v) { union { int i; float v; } u; u.v = v; return u.i; }

int sys(int num, int n, int *argv) {
  char p1[300], p2[300];
  switch (num) {
    case SYS_exit: exit_codes[nest] = A(0); longjmp(jbs[nest], 1);
    case SYS_exec: return run_bin((char *)A(0), A(1), (char **)A(2));
    case SYS_millis: return millis_();
    case SYS_sleep: usleep(A(0) * 1000); return 0;
    case SYS_sysinfo: return A(0) == INFO_CONSOLE ? (td_size() ? td_size() : 80 | 24 << 16) : A(0) == INFO_PREEMPTS ? 0 : 8 << 20;
    case SYS_spawn: return SPAWN_UNSUPPORTED;   // background jobs are a board feature
    case SYS_yield: return 0;                   // one task on the host: nothing else to run
    case SYS_jobs: return 0;                    // no background jobs
    case SYS_kill: return -1;                   // no such running job
    case SYS_reboot: return -1;                 // a board feature
    case SYS_putchar: { char c = A(0); k_write(1, &c, 1); return c; }
    case SYS_getchar: return con_getc();
    case SYS_kbhit: return con_kbhit();
    case SYS_conctl: {                          // the board's SerialConsole::ansi codes, to stdout and the session
      char e[24]; e[0] = 0;
      if (A(0) == CON_CLEAR) strcpy(e, "\033[2J\033[H");
      else if (A(0) == CON_GOTO) snprintf(e, sizeof e, "\033[%d;%dH", A(2) + 1, A(1) + 1);
      else if (A(0) == CON_COLOR) snprintf(e, sizeof e, "\033[%d;%dm", 30 + (A(1) & 7), 40 + (A(2) & 7));
      if (e[0]) k_write(1, e, strlen(e));
      return 0; }
    case SYS_redirect: { int old = stdout_fd; stdout_fd = A(0) ? A(0) : 1; return old; }
    case SYS_open: { const char *in = (const char *)A(0); int fl = A(1);
      { char rp[300]; const char *pn = pipe_path(in, rp, sizeof rp); if (pn) { int i = find_pipe(pn); if (i < 0) return -1;
        return pfd_alloc(i, fl == OPEN_READ ? 0 : 1); } }
      { char r[160]; resolve(in, r, sizeof r); if (nfs_path(r)) { int h = nfs_open(r, nfs_mode(fl)); if (h < 0) return -1; nfs_owner[h] = nest; return NFS_FD + h; } }
      int f = fl == OPEN_WRITE ? O_WRONLY | O_CREAT | O_TRUNC : fl == OPEN_APPEND ? O_WRONLY | O_CREAT | O_APPEND : (fl & 1) ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY;
      return open(hostpath((char *)A(0), p1), f, 0644); }
    case SYS_close: { int fd = A(0);
      if (fd >= PIPE_FD && fd < PIPE_FD + MAX_PIPE_FDS) { int e = pfd_find(fd);
        if (e >= 0) { int i = pfd[e].pipe;
          int live = pfd_live(e), wr = pfd[e].mode == 1;
          pfd[e].pipe = -1;
          if (live && wr) {
            if (pipes[i].nbounds == 8) return -1;     // one task: nobody could drain it
            pipes[i].bounds[pipes[i].nbounds++] = pipes[i].total_written; } }
        return 0; }
      if (nfs_fd(fd) >= 0) { nfs_owner[nfs_fd(fd)] = -1; return nfs_close(nfs_fd(fd)); }
      return close(fd); }
    case SYS_read: { int fd = A(0), cnt = A(2); char *buf = (char *)A(1);
      if (fd >= PIPE_FD && fd < PIPE_FD + MAX_PIPE_FDS) { int e = pfd_find(fd);
        if (e < 0 || pfd[e].mode != 0) return -1;
        int i = pfd[e].pipe; if (!pfd_live(e)) return 0;
        long pos = pipes[i].total_read; int b = next_bound(i, pos);
        if (b == pos) { bound_drop(i, b); return 0; }   // at a message end: 0 once, whatever the count
        if (pipes[i].count == 0) return 0;              // no other task could fill it
        int avail = pipes[i].count; if (b >= 0 && b - pos < avail) avail = b - pos;   // never cross the next boundary
        if (cnt > avail) cnt = avail;
        if (cnt < 0) cnt = 0;
        for (int k = 0; k < cnt; k++) buf[k] = pipes[i].buf[(pipes[i].head + k) % PIPE_SIZE];
        pipes[i].head = (pipes[i].head + cnt) % PIPE_SIZE; pipes[i].count -= cnt;
        pipes[i].total_read += cnt;
        return cnt; }
      if (nfs_fd(fd) >= 0) return nfs_read(nfs_fd(fd), buf, cnt);
      if (fd == 0 && td_lfd >= 0) { int i = 0; while (i < cnt) { buf[i++] = con_getc(); if (!con_kbhit()) break; } return i; }
      return read(fd, buf, cnt); }
    case SYS_write: { int fd = A(0), cnt = A(2); const char *buf = (const char *)A(1);
      return k_write(fd, buf, cnt); }
    case SYS_seek: { int h = nfs_fd(A(0));
      if (h >= 0) return nfs_seek(h, A(2) == SEEK_BEGIN ? A(1) : A(2) == SEEK_CURRENT ? nfs_tell(h) + A(1) : nfs_size(h) + A(1));
      return lseek(A(0), A(1), A(2)); }
    case SYS_stat: { const char *in = (const char *)A(0);
      { char rp[300]; const char *pn = pipe_path(in, rp, sizeof rp); if (pn) { int i = find_pipe(pn); if (i < 0) return -1;
        struct stat_t *o = (struct stat_t *)A(1); o->size = pipes[i].count; o->isdir = 0; o->mtime = pipes[i].nbounds; /* pipes have no time: mtime = complete messages queued */ return 0; } }
      { char r[160]; resolve(in, r, sizeof r); if (nfs_path(r)) { int sz, d; if (nfs_stat(r, &sz, &d)) return -1;
        struct stat_t *o = (struct stat_t *)A(1); o->size = sz; o->isdir = d; o->mtime = 0; return 0; } }
      struct stat st; if (stat(hostpath((char *)A(0), p1), &st)) return -1;
      struct stat_t *o = (struct stat_t *)A(1); o->size = st.st_size; o->isdir = S_ISDIR(st.st_mode); o->mtime = st.st_mtime; return 0; }
    case SYS_unlink: { const char *in = (char *)A(0);
      { char rp[300]; if (pipe_path(in, rp, sizeof rp)) return -1; }   // only the owner's exit removes a pipe
      { char r[160]; resolve(in, r, sizeof r); if (nfs_path(r)) return nfs_remove(r); }
      return unlink(hostpath((char *)A(0), p1)); }
    case SYS_rename: { char ra[160], rb[160]; resolve((char *)A(0), ra, sizeof ra); resolve((char *)A(1), rb, sizeof rb);
      if (nfs_same(ra, rb)) return nfs_rename(ra, rb);
      if (nfs_path(ra) || nfs_path(rb)) return nfs_cross_rename(ra, rb);
      return rename(hostpath((char *)A(0), p1), hostpath((char *)A(1), p2)); }
    case SYS_mkdir: { char r[160]; resolve((char *)A(0), r, sizeof r); if (nfs_path(r)) return nfs_mkdir(r); }
      return mkdir(hostpath((char *)A(0), p1), 0755);
    case SYS_rmdir: { char r[160]; resolve((char *)A(0), r, sizeof r); if (nfs_path(r)) return nfs_rmdir(r); }
      return rmdir(hostpath((char *)A(0), p1));
    case SYS_opendir: { char r[160]; resolve((char *)A(0), r, sizeof r); if (nfs_path(r)) { int d = nfs_opendir(r); if (d < 0) return -1; nfs_dir_owner[d] = nest; return 4 + d; } }
      for (int i = 0; i < 4; i++) if (!dirs[i]) { dirs[i] = opendir(hostpath((char *)A(0), dirpaths[i])); return dirs[i] ? i : -1; } return -1;
    case SYS_readdir: { int d = A(0);
      if (d >= 4 && d < 8) { struct dirent_t *o = (struct dirent_t *)A(1); int sz, isd;
        if (!nfs_readdir(d - 4, o->name, sizeof o->name, &sz, &isd)) return 0; o->size = sz; o->isdir = isd; return 1; }
      if (d < 0 || d > 3 || !dirs[d]) return 0; struct dirent *e;
      while ((e = readdir(dirs[d])) && (e->d_name[0] == '.')) {}
      if (!e) return 0;
      struct dirent_t *o = (struct dirent_t *)A(1); strncpy(o->name, e->d_name, 63); o->name[63] = 0;
      char full[600]; struct stat st; snprintf(full, sizeof full, "%s/%s", dirpaths[d], e->d_name); stat(full, &st);
      o->size = st.st_size; o->isdir = S_ISDIR(st.st_mode); return 1; }
    case SYS_closedir: { int d = A(0); if (d >= 4 && d < 8) { nfs_dir_owner[d - 4] = -1; nfs_closedir(d - 4); return 0; } if (d >= 0 && d < 4 && dirs[d]) { closedir(dirs[d]); dirs[d] = 0; } return 0; }
    case SYS_chdir: { char r[160]; resolve((char *)A(0), r, sizeof r);
      if (nfs_path(r)) { int sz, d; if (strlen(r) >= sizeof cwd || nfs_stat(r, &sz, &d) || !d) return -1; strcpy(cwd, r); return 0; } }
      { struct stat st; if (stat(hostpath((char *)A(0), p1), &st) || !S_ISDIR(st.st_mode)) return -1; resolve((char *)A(0), cwd, sizeof cwd); return 0; }
    case SYS_getcwd: strncpy((char *)A(0), cwd, A(1)); return A(0);
    case SYS_mount: { const char *h = A(0) ? (const char *)A(0) : "";   // a null host unmounts too
      int r = nfs_mount(h, A(1), (const char *)A(2), 1);
      if (r == 0 && *h) mnt_dir("fs");
      if ((r == 0 || r == MOUNT_BADNAME) && !*h) mnt_undir("fs");
      return r; }
    case SYS_mountat: { const char *nm = A(0) ? (const char *)A(0) : "", *h = A(1) ? (const char *)A(1) : "";
      int r = nfs_mountat(nm, h, A(2), (const char *)A(3), 1);
      if (r == 0 && *h) mnt_dir(nm);
      if ((r == 0 || r == MOUNT_BADNAME) && !*h) mnt_undir(nm);
      return r; }
    case SYS_mkpipe: { const char *nm = (const char *)A(0);   // the part after /pipe/
      if (!nm[0] || strlen(nm) > 31 || strchr(nm, '/')) return -1;
      if (find_pipe(nm) >= 0) return -1;
      for (int i = 0; i < MAX_PIPES; i++) if (!pipes[i].used) { memset(&pipes[i], 0, sizeof pipes[i]); pipes[i].used = 1; pipe_gen[i]++; strcpy(pipes[i].name, nm); pipes[i].owner = nest; return 0; }
      return -1; }
    case SYS_malloc: return (int)calloc(1, A(0));   // zeroed, as the kernel does (cc and basic rely on it)
    case SYS_free: free((void *)A(0)); return 0;
    case SYS_memset: return (int)memset((void *)A(0), A(1), A(2));
    case SYS_memcmp: return memcmp((void *)A(0), (void *)A(1), A(2));
    case SYS_memcpy: return (int)memcpy((void *)A(0), (void *)A(1), A(2));
    case SYS_strlen: return strlen((char *)A(0));
    case SYS_strcmp: return strcmp((char *)A(0), (char *)A(1));
    case SYS_strcpy: return (int)strcpy((char *)A(0), (char *)A(1));
    case SYS_printf: { int l = k_format(fmtbuf, sizeof fmtbuf, (char *)A(0), argv, n, 1); k_write(1, fmtbuf, l < (int)sizeof fmtbuf ? l : sizeof fmtbuf - 1); return l; }
    case SYS_sprintf: return k_format((char *)A(0), 1 << 30, (char *)A(1), argv, n, 2);
    case SYS_atoi: return atoi((char *)A(0));
    case SYS_strncmp: return strncmp((char *)A(0), (char *)A(1), A(2));
    case SYS_strchr: return (int)strchr((char *)A(0), A(1));
    case SYS_net_join: return 0;
    case SYS_net_status: return NET_UP;
    case SYS_wifi_save: return A(0) ? -1 : 0;   // no key store on the host; nothing to forget
    case SYS_wifi_saved: return 0;
    case SYS_telnetd: return k_telnetd(A(0), (const char *)A(1), (char *)A(1));
    case SYS_net_ip: return (int)ipbuf;
    case SYS_connect: return sock_connect((char *)A(0), A(1));
    case SYS_send: { int r = send(A(0), (void *)A(1), A(2), MSG_NOSIGNAL); return r > 0 ? r : -1; }  // a closed peer: -1, not SIGPIPE
    case SYS_recv: return sock_recv(A(0), (char *)A(1), A(2), A(3));
    case SYS_listen: { if (listen_fd >= 0) close(listen_fd); listen_fd = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
      struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(A(0)); a.sin_addr.s_addr = INADDR_ANY;
      if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) || listen(listen_fd, 2)) return -1; return 0; }
    case SYS_accept: { if (listen_fd < 0) return -1; struct pollfd p = { listen_fd, POLLIN, 0 }; int r = poll(&p, 1, A(0)); if (r == 0) return 0; if (r < 0) return -1; return accept(listen_fd, 0, 0); }
    case SYS_ping: return host_ping((const char *)A(0), (char *)A(1));
    case SYS_sock_avail: { struct pollfd p = { A(0), POLLIN, 0 }; return poll(&p, 1, 0) > 0; }
    case SYS_gpio_mode: case SYS_gpio_write: case SYS_pwm_write: case SYS_delay_us: case SYS_spi_transfer: return 0;
    case SYS_gpio_read: case SYS_analog_read: case SYS_i2c_write: case SYS_i2c_read: return 0;
    case SYS_time: { if ((int *)A(0)) *(int *)A(0) = 1; return (int)time(NULL); }  // the host clock is always valid
    case SYS_localtime: { if ((int *)A(0)) *(int *)A(0) = 1; return (int)time(NULL) + tz_now(time(NULL)); }
    case SYS_tzset: { const char *s = (const char *)A(0);
      if (!s) return tz_now(time(NULL));
      tz_rule r; if (tz_parse(s, &r)) return -1; tz_load(&r);   // the engine is under test, not libc; no file persistence here
      return tz_now(time(NULL)); }
    case SYS_tzlocal: { long t = (long)A(0); return (int)(t + tz_now(t)); }   // local epoch for the instant t (tzlocal(utc) -> utc + tz_now(utc))
    // ---- maths
    case SYS_sin: return fw(sinf(wf(A(0))));
    case SYS_cos: return fw(cosf(wf(A(0))));
    case SYS_tan: return fw(tanf(wf(A(0))));
    case SYS_asin: return fw(asinf(wf(A(0))));
    case SYS_acos: return fw(acosf(wf(A(0))));
    case SYS_atan: return fw(atanf(wf(A(0))));
    case SYS_atan2: return fw(atan2f(wf(A(0)), wf(A(1))));
    case SYS_sinh: return fw(sinhf(wf(A(0))));
    case SYS_cosh: return fw(coshf(wf(A(0))));
    case SYS_tanh: return fw(tanhf(wf(A(0))));
    case SYS_exp: return fw(expf(wf(A(0))));
    case SYS_log: return fw(logf(wf(A(0))));
    case SYS_log10: return fw(log10f(wf(A(0))));
    case SYS_pow: return fw(powf(wf(A(0)), wf(A(1))));
    case SYS_sqrt: return fw(sqrtf(wf(A(0))));
    case SYS_fabs: return fw(fabsf(wf(A(0))));
    case SYS_floor: return fw(floorf(wf(A(0))));
    case SYS_ceil: return fw(ceilf(wf(A(0))));
    case SYS_fmod: return fw(fmodf(wf(A(0)), wf(A(1))));
  }
  fprintf(stderr, "bad syscall %d\n", num); exit(99);
}

__asm__(
  ".syntax unified\n.thumb\n.thumb_func\n.global run\n"
  "run:\n"
  "  push {r4-r11, lr}\n"
  "  ldr r5, [sp, #36]\n"
  "  ldr r6, [sp, #40]\n"
  "  mov r8, r1\n"
  "  mov r9, r2\n"
  "  mov r4, sp\n"
  "  mov sp, r3\n"
  "  push {r5}\n"
  "  push {r6}\n"
  "  blx r0\n"
  "  add sp, #8\n"
  "  mov sp, r4\n"
  "  pop {r4-r11, pc}\n");
int run(void *entry, void *sys, void *data, void *stacktop, int argc, char **argv);

static int run_bin(const char *name, int argc, char **argv) {
  if (nest + 1 >= MAX_NEST) return -1;
  char p[300]; FILE *f;
  { char r[160]; resolve(name, r, sizeof r);
    if (nfs_path(r)) {                 // a program on /mnt/fs: read it whole, then load it from memory
      int h = nfs_open(r, NFS_READ); if (h < 0 || nfs_isdir(h)) { if (h >= 0) nfs_close(h); return -1; }
      int sz = nfs_size(h); char *b = malloc(sz + 1); int got = nfs_read(h, b, sz); nfs_close(h);
      if (got != sz) { free(b); return -1; }
      f = fmemopen(b, sz, "rb"); if (!f) { free(b); return -1; }
      // the buffer lives as long as the FILE: freed after fclose below
      netfs_loadbuf = b;
    } else f = fopen(hostpath(name, p), "rb"); }
  if (!f) return -1;
  int hdr[5]; if (fread(hdr, 4, 5, f) != 5 || hdr[0] != 0x43433254) { fclose(f); free(netfs_loadbuf); netfs_loadbuf = 0; return -2; }
  char *code = mmap(0, hdr[1] + 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  char *data = malloc(hdr[2] + hdr[3] + 16);
  fread(code, 1, hdr[1], f); fread(data, 1, hdr[2], f); fclose(f);
  free(netfs_loadbuf); netfs_loadbuf = 0;
  memset(data + hdr[2], 0, hdr[3]);
  char *stack = malloc(1 << 20);
  nest++;
  int saved_stdout = stdout_fd, ret;
  if (setjmp(jbs[nest]) == 0) ret = run(code + hdr[4] + 1, (void *)sys, data, stack + (1 << 20) - 64, argc, argv);
  else ret = exit_codes[nest];
  owner_pipes_remove(nest);          // this task's pipes end with it; its pipe fds die too
  for (int i = 0; i < MAX_PIPE_FDS; i++) if (pfd[i].owner == nest) pfd[i].pipe = -1;   // fds opened by this level
  for (int i = 0; i < NFS_NFD; i++) if (nfs_owner[i] == nest) { nfs_owner[i] = -1; nfs_close(i); }   // saves what it wrote
  for (int i = 0; i < 4; i++) if (nfs_dir_owner[i] == nest) { nfs_dir_owner[i] = -1; nfs_closedir(i); }
  stdout_fd = saved_stdout; nest--;
  munmap(code, hdr[1] + 4096); free(data); free(stack);
  return ret;
}

int main(int argc, char **argv) {
  for (int i = 0; i < MAX_PIPE_FDS; i++) pfd[i].pipe = -1;
  if (argc < 2) { fprintf(stderr, "usage: host_linux [-r rootdir] prog.bin [args]\n"); return 1; }
  if (!strcmp(argv[1], "-r")) { strcpy(root, argv[2]); argv += 2; argc -= 2; }
  { tz_rule r; char *tzenv = getenv("TZ");   // the standard PC-side knob, read once at startup (UTC when unset)
    if (tzenv && !tz_parse(tzenv, &r)) tz_load(&r); }
  nfs_boot();
  if (isatty(0)) { struct termios t; tcgetattr(0, &t); t.c_lflag &= ~(ICANON | ECHO); tcsetattr(0, TCSANOW, &t); }
  int r;
  do r = run_bin(argv[1], argc - 1, argv + 1); while (td_logout("logged out"));   // as the board's loop()
  fflush(stdout);
  return r;
}
