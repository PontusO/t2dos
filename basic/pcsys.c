// pcsys.c - PC build of BASIC only: the T2DOS library calls of pcsys.h, done with libc.
// Console control writes the same ANSI sequences as the kernel's SerialConsole. Calls that
// have no meaning on the PC (jobs, pipes, pins) answer like a board where they fail or do
// nothing. Not part of the board build (the Makefile leaves this file out).
#include <stdarg.h>
#include <time.h>
#include <poll.h>
#include <sys/wait.h>
#include "sys.h"
#undef exit
#undef open
#undef close
#undef read
#undef write
#undef stat
#undef unlink
#undef rename
#undef mkdir
#undef rmdir
#undef opendir
#undef readdir
#undef closedir
#undef chdir
#undef getcwd
#undef malloc
#undef free
#undef sleep
#undef kill

int t2_start_ms;

int t2_now_ms(void) {
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return (int)(ts.tv_sec * 1000 + ts.tv_nsec / 1000000);
}

// /bin/NAME.bin is the T2DOS program NAME; on the PC it is the host's /bin/NAME (SHELL tests)
char *t2_hostpath(char *path, char *buf, int n) {
  int len = strlen(path);
  if (!strncmp(path, "/bin/", 5) && len > 9 && len < n && !strcmp(path + len - 4, ".bin")) {
    memcpy(buf, path, len - 4);
    buf[len - 4] = 0;
    return buf;
  }
  return path;
}

void t2_exit(int code) { exit(code); }

int t2_exec(char *path, int argc, char **argv) {
  // runs a PC program: the T2DOS paths do not exist here, so this is for tests that need it
  pid_t pid; int st; char **av; int i; char hp[256];
  path = t2_hostpath(path, hp, sizeof(hp));
  av = calloc(argc + 1, sizeof(char *));
  for (i = 0; i < argc; i++) av[i] = argv[i];
  pid = fork();
  if (pid == 0) { execvp(path, av); _exit(127); }
  free(av);
  if (pid < 0 || waitpid(pid, &st, 0) < 0) return -1;
  if (WIFEXITED(st) && WEXITSTATUS(st) == 127) return -1;
  return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
}

int t2_millis(void) {
  if (!t2_start_ms) t2_start_ms = t2_now_ms() - 1;
  return t2_now_ms() - t2_start_ms;
}

int t2_sleep(int ms) { if (ms > 0) usleep(ms * 1000); return 0; }

// Memory as on the board: PSRAM of BASIC_PSRAM bytes (8 MB if not set in the environment), of
// which what the interpreter holds is not free; malloc fails beyond BASIC_MALLOC_CAP bytes
// held (no limit if not set: set below the PSRAM size it tests malloc returning 0).
int t2_live;                     // bytes the interpreter holds (t2_malloc less t2_free)
int t2_psram = -1;
int t2_cap = -1;

void t2_mem_env(void) {
  char *e;
  if (t2_psram >= 0) return;
  e = getenv("BASIC_PSRAM"); t2_psram = e ? atoi(e) : 8 * 1024 * 1024;
  e = getenv("BASIC_MALLOC_CAP"); t2_cap = e ? atoi(e) : 0x7fffffff;
}

int t2_sysinfo(int kind) {
  t2_mem_env();
  if (kind == INFO_PSRAM_TOTAL) return t2_psram;
  if (kind == INFO_PSRAM_FREE) return t2_psram > t2_live ? t2_psram - t2_live : 0;
  if (kind == INFO_SRAM_FREE) return 370 * 1024;
  if (kind == INFO_CONSOLE) return 1;
  return 0;
}

int t2_spawn(char *path, int argc, char **argv) { return SPAWN_UNSUPPORTED; }
int t2_yield(void) { return 0; }
int t2_jobs(struct job_t *buf, int max) { return 0; }
int t2_kill(int job) { return -1; }

int t2_putchar(int c) { unsigned char b = c; write(1, &b, 1); return c; }

int t2_getchar(void) {
  unsigned char b;
  if (read(0, &b, 1) != 1) return -1;
  return b;
}

int t2_kbhit(void) {
  struct pollfd p;
  p.fd = 0; p.events = POLLIN; p.revents = 0;
  return poll(&p, 1, 0) > 0 && (p.revents & POLLIN) ? 1 : 0;
}

int t2_conctl(int cmd, int a, int b) {
  char s[32];
  if (cmd == CON_CLEAR) strcpy(s, "\033[2J\033[H");
  else if (cmd == CON_GOTO) sprintf(s, "\033[%d;%dH", b + 1, a + 1);
  else if (cmd == CON_COLOR) sprintf(s, "\033[%d;%dm", 30 + (a & 7), 40 + (b & 7));
  else return 0;
  write(1, s, strlen(s));
  return 0;
}

int t2_redirect(int fd) {
  // fd 1 becomes fd; returns a descriptor that redirect() takes back to restore the old one
  int old = dup(1);
  dup2(fd, 1);
  return old;
}

int t2_open(char *path, int mode) {
  if (mode == OPEN_WRITE) return open(path, O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (mode == OPEN_APPEND) return open(path, O_WRONLY | O_CREAT | O_APPEND, 0644);
  return open(path, O_RDONLY);
}

int t2_close(int fd) { return close(fd); }
int t2_read(int fd, void *buf, int n) { return read(fd, buf, n); }
int t2_write(int fd, void *buf, int n) { return write(fd, buf, n); }

int t2_seek(int fd, int offset, int whence) {
  int w = whence == SEEK_CURRENT ? SEEK_CUR : whence == SEEK_FINISH ? SEEK_END : SEEK_SET;
  return (int)lseek(fd, offset, w);
}

int t2_stat(char *path, struct stat_t *st) {
  struct stat s; char hp[256];
  if (stat(t2_hostpath(path, hp, sizeof(hp)), &s) < 0) return -1;
  st->size = (int)s.st_size;
  st->isdir = S_ISDIR(s.st_mode) ? 1 : 0;
  st->mtime = (int)s.st_mtime;
  return 0;
}

int t2_unlink(char *path) { return unlink(path); }
int t2_rename(char *from, char *to) { return rename(from, to); }
int t2_mkdir(char *path) { return mkdir(path, 0777); }
int t2_rmdir(char *path) { return rmdir(path); }
int t2_chdir(char *path) { return chdir(path); }
char *t2_getcwd(char *buf, int n) { return getcwd(buf, n); }

// directories: T2DOS hands out small int handles
DIR *t2_dirs[8];
char t2_dirpath[8][512];

int t2_opendir(char *path) {
  int i;
  for (i = 0; i < 8; i++)
    if (!t2_dirs[i]) {
      t2_dirs[i] = opendir(path);
      if (!t2_dirs[i]) return -1;
      snprintf(t2_dirpath[i], 512, "%s", path);
      return i;
    }
  return -1;
}

int t2_readdir(int d, struct dirent_t *e) {
  struct dirent *de; struct stat s; char p[1100];
  if (d < 0 || d >= 8 || !t2_dirs[d]) return 0;
  for (;;) {
    de = readdir(t2_dirs[d]);
    if (!de) return 0;
    if (strcmp(de->d_name, ".") && strcmp(de->d_name, "..")) break;
  }
  snprintf(e->name, sizeof(e->name), "%s", de->d_name);
  snprintf(p, sizeof(p), "%s/%s", t2_dirpath[d], de->d_name);
  e->size = 0; e->isdir = 0;
  if (stat(p, &s) == 0) { e->size = (int)s.st_size; e->isdir = S_ISDIR(s.st_mode) ? 1 : 0; }
  return 1;
}

int t2_closedir(int d) {
  if (d < 0 || d >= 8 || !t2_dirs[d]) return -1;
  closedir(t2_dirs[d]); t2_dirs[d] = 0;
  return 0;
}

int t2_mkpipe(char *name) { return -1; }

// T2DOS malloc zeroes. 8 bytes in front of each block hold its size, for t2_free.
void *t2_malloc(int n) {
  char *p;
  t2_mem_env();
  if (n < 0 || n > t2_cap - t2_live) return 0;
  p = calloc(1, n + 8);
  if (!p) return 0;
  *(int *)p = n; t2_live += n;
  return p + 8;
}

void t2_free(void *b) {
  char *p;
  if (!b) return;
  p = (char *)b - 8;
  t2_live -= *(int *)p;
  free(p);
}

int t2_printf(char *fmt, ...) {
  char buf[4096]; va_list ap; int n;
  va_start(ap, fmt);
  n = vsnprintf(buf, sizeof(buf), fmt, ap);
  va_end(ap);
  if (n > (int)sizeof(buf) - 1) n = sizeof(buf) - 1;
  write(1, buf, n);
  return n;
}

int t2_gpio_mode(int pin, int mode) { return 0; }
int t2_gpio_write(int pin, int v) { return 0; }
int t2_gpio_read(int pin) { return 0; }
int t2_analog_read(int pin) { return 0; }
int t2_pwm_write(int pin, int v) { return 0; }
int t2_delay_us(int us) { if (us > 0) usleep(us); return 0; }
