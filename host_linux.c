// host_linux.c - T2DOS kernel contract on Linux/ARM (qemu-arm), for testing userland on a PC.
//   qemu-arm ./host_linux /bin/sh.bin         (paths are relative to a root dir, default ./root)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <setjmp.h>
#include <dirent.h>
#include <poll.h>
#include <termios.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <netdb.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include "sys.h"

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
static int millis_() { struct timeval tv; gettimeofday(&tv, 0); return tv.tv_sec * 1000 + tv.tv_usec / 1000; }
static int kbhit() { struct pollfd p = { 0, POLLIN, 0 }; return poll(&p, 1, 0) > 0; }
static int k_write(int fd, const char *buf, int n) { if (fd == 1) fd = stdout_fd; return write(fd, buf, n); }
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

// named pipes under /pipe/ (one task on the host: nest == owner)
#define PIPE_FD 200
#define MAX_PIPES 8
#define MAX_PIPE_FDS 16
static struct { int used; char name[32]; int owner; char buf[PIPE_SIZE]; int head, count; int bounds[8], nbounds; long total_read, total_written; } pipes[MAX_PIPES];
static struct { int pipe, mode, owner, wrote; } pfd[MAX_PIPE_FDS];

static int is_pipename(const char *p) { return strncmp(p, "/pipe/", 6) == 0 && p[6] != 0; }
static int find_pipe(const char *name) { for (int i = 0; i < MAX_PIPES; i++) if (pipes[i].used && !strcmp(pipes[i].name, name)) return i; return -1; }
static void pipe_remove(int i) { pipes[i].used = 0; }
static void owner_pipes_remove(int owner) { for (int i = 0; i < MAX_PIPES; i++) if (pipes[i].used && pipes[i].owner == owner) pipe_remove(i); }
// next message boundary at or after pos, or -1
static int next_bound(int i, long pos) { int b = -1; for (int k = 0; k < pipes[i].nbounds; k++) if (pipes[i].bounds[k] >= pos && (b < 0 || pipes[i].bounds[k] < b)) b = pipes[i].bounds[k]; return b; }
static void bound_drop(int i, long at) { for (int k = 0; k < pipes[i].nbounds; k++) if (pipes[i].bounds[k] == at) pipes[i].bounds[k] = pipes[i].bounds[--pipes[i].nbounds]; }
static int pfd_find(int fd) { for (int i = 0; i < MAX_PIPE_FDS; i++) if (pfd[i].pipe >= 0 && pfd[i].owner == nest && pfd[i].mode == fd - PIPE_FD) return i; return -1; }
static int pfd_alloc(int pipe, int mode) { for (int i = 0; i < MAX_PIPE_FDS; i++) if (pfd[i].pipe < 0) { pfd[i].pipe = pipe; pfd[i].mode = mode; pfd[i].owner = nest; pfd[i].wrote = 0; return PIPE_FD + mode; } return -1; }

int sys(int num, int n, int *argv) {
  char p1[300], p2[300];
  switch (num) {
    case SYS_exit: exit_codes[nest] = A(0); longjmp(jbs[nest], 1);
    case SYS_exec: return run_bin((char *)A(0), A(1), (char **)A(2));
    case SYS_millis: return millis_();
    case SYS_sleep: usleep(A(0) * 1000); return 0;
    case SYS_sysinfo: return A(0) == INFO_CONSOLE ? 80 | 24 << 16 : 8 << 20;
    case SYS_spawn: return SPAWN_UNSUPPORTED;   // background jobs are a board feature
    case SYS_yield: return 0;                   // one task on the host: nothing else to run
    case SYS_jobs: return 0;                    // no background jobs
    case SYS_kill: return -1;                   // no such running job
    case SYS_putchar: { char c = A(0); k_write(1, &c, 1); return c; }
    case SYS_getchar: { unsigned char c; if (read(0, &c, 1) != 1) exit(0); return c; }
    case SYS_kbhit: return kbhit();
    case SYS_conctl: return 0;
    case SYS_redirect: { int old = stdout_fd; stdout_fd = A(0) ? A(0) : 1; return old; }
    case SYS_open: { const char *in = (const char *)A(0); int fl = A(1);
      if (is_pipename(in)) { int i = find_pipe(in); if (i < 0) return -1;
        return pfd_alloc(i, fl == OPEN_READ ? 0 : 1); }
      int f = fl == OPEN_WRITE ? O_WRONLY | O_CREAT | O_TRUNC : fl == OPEN_APPEND ? O_WRONLY | O_CREAT | O_APPEND : (fl & 1) ? O_WRONLY | O_CREAT | O_TRUNC : O_RDONLY;
      return open(hostpath((char *)A(0), p1), f, 0644); }
    case SYS_close: { int fd = A(0);
      if (fd >= PIPE_FD && fd < PIPE_FD + MAX_PIPE_FDS) { int e = pfd_find(fd);
        if (e >= 0) { int i = pfd[e].pipe;
          if (pipes[i].used && pfd[e].mode == 1) { while (pipes[i].nbounds == 8) ;   // one task: never blocks
            pipes[i].bounds[pipes[i].nbounds++] = pipes[i].total_written; }
          pfd[e].pipe = -1; }
        return 0; }
      return close(fd); }
    case SYS_read: { int fd = A(0), n = A(2); char *buf = (char *)A(1);
      if (fd >= PIPE_FD && fd < PIPE_FD + MAX_PIPE_FDS) { int e = pfd_find(fd);
        if (e < 0 || pfd[e].mode != 0) return -1;
        int i = pfd[e].pipe; if (!pipes[i].used) return 0;
        if (pipes[i].count == 0) { int b = next_bound(i, pipes[i].total_read);
          if (b < 0) return 0;                       // no other task could fill it
          bound_drop(i, b); return 0; }
        long pos = pipes[i].total_read; int avail = pipes[i].count;
        int b = next_bound(i, pos); if (b >= 0 && b - pos < avail) avail = b - pos;
        if (n < avail) n = avail;
        memcpy(buf, pipes[i].buf + pipes[i].head, n);
        pipes[i].head = (pipes[i].head + n) % PIPE_SIZE; pipes[i].count -= n;
        pipes[i].total_read += n;
        if (pipes[i].total_read == b) bound_drop(i, b);
        return n; }
      return read(fd, (void *)A(1), n); }
    case SYS_write: { int fd = A(0), n = A(2); const char *buf = (const char *)A(1);
      if (fd == 1) fd = stdout_fd;
      if (fd >= PIPE_FD && fd < PIPE_FD + MAX_PIPE_FDS) { int e = pfd_find(fd);
        if (e < 0 || pfd[e].mode != 1) return -1;
        int i = pfd[e].pipe; if (!pipes[i].used) return -1;
        while (pipes[i].count == PIPE_SIZE) ;         // one task: never blocks
        memcpy(pipes[i].buf + (pipes[i].head + pipes[i].count) % PIPE_SIZE, buf, n);
        pipes[i].count += n; pipes[i].total_written += n; pfd[e].wrote = 1;
        return n; }
      return k_write(fd, buf, n); }
    case SYS_seek: return lseek(A(0), A(1), A(2));
    case SYS_stat: { const char *in = (const char *)A(0);
      if (is_pipename(in)) { int i = find_pipe(in); if (i < 0) return -1;
        struct stat_t *o = (struct stat_t *)A(1); o->size = pipes[i].count; o->isdir = 0; o->mtime = 0; return 0; }
      struct stat st; if (stat(hostpath((char *)A(0), p1), &st)) return -1;
      struct stat_t *o = (struct stat_t *)A(1); o->size = st.st_size; o->isdir = S_ISDIR(st.st_mode); o->mtime = st.st_mtime; return 0; }
    case SYS_unlink: { const char *in = (char *)A(0);
      if (is_pipename(in)) return -1;                // only the owner's exit removes a pipe
      return unlink(hostpath((char *)A(0), p1)); }
    case SYS_rename: return rename(hostpath((char *)A(0), p1), hostpath((char *)A(1), p2));
    case SYS_mkdir: return mkdir(hostpath((char *)A(0), p1), 0755);
    case SYS_rmdir: return rmdir(hostpath((char *)A(0), p1));
    case SYS_opendir: for (int i = 0; i < 4; i++) if (!dirs[i]) { dirs[i] = opendir(hostpath((char *)A(0), dirpaths[i])); return dirs[i] ? i : -1; } return -1;
    case SYS_readdir: { int d = A(0); if (d < 0 || d > 3 || !dirs[d]) return 0; struct dirent *e;
      while ((e = readdir(dirs[d])) && (e->d_name[0] == '.')) {}
      if (!e) return 0;
      struct dirent_t *o = (struct dirent_t *)A(1); strncpy(o->name, e->d_name, 63); o->name[63] = 0;
      char full[600]; struct stat st; snprintf(full, sizeof full, "%s/%s", dirpaths[d], e->d_name); stat(full, &st);
      o->size = st.st_size; o->isdir = S_ISDIR(st.st_mode); return 1; }
    case SYS_closedir: { int d = A(0); if (d >= 0 && d < 4 && dirs[d]) { closedir(dirs[d]); dirs[d] = 0; } return 0; }
    case SYS_chdir: { struct stat st; if (stat(hostpath((char *)A(0), p1), &st) || !S_ISDIR(st.st_mode)) return -1; resolve((char *)A(0), cwd, sizeof cwd); return 0; }
    case SYS_getcwd: strncpy((char *)A(0), cwd, A(1)); return A(0);
    case SYS_mkpipe: { const char *nm = (const char *)A(0) + 6;   // the part after /pipe/
      if (!is_pipename((const char *)A(0)) || strlen(nm) > 31 || strchr(nm, '/')) return -1;
      if (find_pipe((const char *)A(0)) >= 0) return -1;
      for (int i = 0; i < MAX_PIPES; i++) if (!pipes[i].used) { memset(&pipes[i], 0, sizeof pipes[i]); pipes[i].used = 1; strcpy(pipes[i].name, (const char *)A(0)); pipes[i].owner = nest; return 0; }
      return -1; }
    case SYS_malloc: return (int)malloc(A(0));
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
    case SYS_net_ip: return (int)ipbuf;
    case SYS_connect: return sock_connect((char *)A(0), A(1));
    case SYS_send: { int r = write(A(0), (void *)A(1), A(2)); return r > 0 ? r : -1; }
    case SYS_recv: return sock_recv(A(0), (char *)A(1), A(2), A(3));
    case SYS_listen: { if (listen_fd >= 0) close(listen_fd); listen_fd = socket(AF_INET, SOCK_STREAM, 0); int one = 1; setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
      struct sockaddr_in a = {0}; a.sin_family = AF_INET; a.sin_port = htons(A(0)); a.sin_addr.s_addr = INADDR_ANY;
      if (bind(listen_fd, (struct sockaddr *)&a, sizeof a) || listen(listen_fd, 2)) return -1; return 0; }
    case SYS_accept: { if (listen_fd < 0) return -1; struct pollfd p = { listen_fd, POLLIN, 0 }; int r = poll(&p, 1, A(0)); if (r == 0) return 0; if (r < 0) return -1; return accept(listen_fd, 0, 0); }
    case SYS_sock_avail: { struct pollfd p = { A(0), POLLIN, 0 }; return poll(&p, 1, 0) > 0; }
    case SYS_gpio_mode: case SYS_gpio_write: case SYS_pwm_write: case SYS_delay_us: case SYS_spi_transfer: return 0;
    case SYS_gpio_read: case SYS_analog_read: case SYS_i2c_write: case SYS_i2c_read: return 0;
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
  char p[300]; FILE *f = fopen(hostpath(name, p), "rb"); if (!f) return -1;
  int hdr[5]; if (fread(hdr, 4, 5, f) != 5 || hdr[0] != 0x43433254) { fclose(f); return -2; }
  char *code = mmap(0, hdr[1] + 4096, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  char *data = malloc(hdr[2] + hdr[3] + 16);
  fread(code, 1, hdr[1], f); fread(data, 1, hdr[2], f); fclose(f);
  memset(data + hdr[2], 0, hdr[3]);
  char *stack = malloc(1 << 20);
  nest++;
  int saved_stdout = stdout_fd, ret;
  if (setjmp(jbs[nest]) == 0) ret = run(code + hdr[4] + 1, (void *)sys, data, stack + (1 << 20) - 64, argc, argv);
  else ret = exit_codes[nest];
  owner_pipes_remove(nest);          // this task's pipes end with it; its pipe fds die too
  stdout_fd = saved_stdout; nest--;
  munmap(code, hdr[1] + 4096); free(data); free(stack);
  return ret;
}

int main(int argc, char **argv) {
  if (argc < 2) { fprintf(stderr, "usage: host_linux [-r rootdir] prog.bin [args]\n"); return 1; }
  if (!strcmp(argv[1], "-r")) { strcpy(root, argv[2]); argv += 2; argc -= 2; }
  if (isatty(0)) { struct termios t; tcgetattr(0, &t); t.c_lflag &= ~(ICANON | ECHO); tcsetattr(0, TCSANOW, &t); }
  int r = run_bin(argv[1], argc - 1, argv + 1);
  fflush(stdout);
  return r;
}
