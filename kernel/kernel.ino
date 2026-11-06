// kernel.ino - T2DOS: a DOS-class kernel for the RP2350 (arduino-pico).
//
// The kernel owns hardware and privileges only: syscall table (sys.h), program
// loader with nested exec, memory arenas, console and file drivers, board I/O.
// Everything else is a T2CC program on the SD card. Boot runs /bin/sh.bin.
//
// Compiled-code contract: r8 = sys(), r9 = data base, args on the stack
// (argv[n-1] is arg 0), result in r0. See compiler/cc.c and sys.h.

#include <SDFS.h>
#include <LittleFS.h>
#include <WiFi.h>
#include <Wire.h>
#include <SPI.h>
#include <setjmp.h>
#include "sys.h"

#define SD_CS SS
#define MAX_FD 16          // 0,1 console; 2.. files
#define MAX_DIR 4
#define MAX_NEST 4
#define MAX_SOCK 4
#define SOCK_FD 100
#define STACK_SIZE (256 * 1024)
#define JSTACK_SIZE (8 * 1024)    // kernel stack of a background task (SRAM). A job may exec, but
                                  // each level's syscall frames share these 8 KB: keep it shallow
#define MAX_ARGS 15               // spawn() keeps at most this many arguments...
#define ARG_BYTES 256             // ...in this many bytes, NUL bytes included
#define PIPE_FD 200               // pipe fds are PIPE_FD + an index into pfds
#define MAX_PIPES 8
#define MAX_PIPE_FDS 16
#define MAX_BOUNDS 8              // message ends a pipe holds that no reader has reached yet

// ============================================================ tasks
// Every malloc() a program makes is linked into its Proc, so whatever it did
// not free() is reclaimed when it exits. The 8-byte header keeps 8-byte alignment.
// The structs come before the first function: Arduino puts its generated
// prototypes there.
struct Blk { Blk *next, *prev; };
struct Proc { jmp_buf jb; int exit_code; uint16_t fd_mask; uint8_t sock_mask; Blk heap; void *code, *data, *stack; };

// Cooperative tasks: tasks[0] is the foreground (the shell and what it runs),
// tasks[1..MAX_JOBS] are background jobs. A task gives up the CPU only in
// k_yield(), called from the waiting system calls (getchar, kbhit, sleep,
// recv, accept, kill, yield, and a read of an empty pipe, a write to a full
// pipe or a pipe close with a full queue of message ends), never inside a
// file, flash or WiFi operation.
// cwd, stdout_fd and t2_sys_sp are globals that always hold the running
// task's values; k_yield() saves them into the Task it leaves and loads the
// next task's. A program and the programs it execs run nested in one task
// (procs[0..nest]), as the foreground always did.
enum { T_FREE = 0, T_RUN, T_DONE };
struct Task {
  int state, exit_code, killed, gen;    // gen counts spawns of this slot
  char name[32];
  uint32_t sp;                          // kernel sp while switched out (ctx_switch)
  uint32_t sys_sp;                      // t2_sys_sp while switched out
  char cwd[128];                        // cwd while switched out
  int stdout_fd;                        // stdout_fd while switched out
  int nest;                             // procs[nest] is the running program, -1 none
  Proc procs[MAX_NEST];
  jmp_buf jb;                           // task-level unwind (kill)
  uint8_t *code, *data, *stack; uint32_t entry;   // loaded by spawn, not yet started
  int argc; char *argv[MAX_ARGS + 1]; char args[ARG_BYTES];
  char fmtbuf[1024];                    // printf's buffer: a write to a full pipe switches mid-printf
};
static Task tasks[1 + MAX_JOBS];
static Task *cur = &tasks[0];
static int k_yield();
static void k_wait();

// Named pipes: /pipe/NAME, made by mkpipe, owned by the task (and nesting
// level) that made it and removed when that program ends. A writer's close
// ends one message: bounds[] holds the stream positions (in total_written
// terms) where messages end. A pipe fd is dead once its pipe is removed
// (gen differs): read 0, write -1, close frees it.
struct Pipe {
  int used, gen; char name[32]; int owner, level;
  char buf[PIPE_SIZE]; int head, count;
  int bounds[MAX_BOUNDS], nbounds;
  int total_read, total_written;
};
struct PipeFd { int used, pipe, mode, owner, level, wrote, gen, id; };   // mode 0 read, 1 write
static Pipe pipes[MAX_PIPES];
static PipeFd pfds[MAX_PIPE_FDS];

// Root filesystem: the SD card if it mounts, else LittleFS in on-board flash.
// The flash FS size comes from the board's flash layout (build with
// flash=8388608_7340032 on the CPico 2350: 1 MB kernel, 7 MB filesystem).
static FS *fsys = &SDFS;

// ============================================================ console drivers
struct Console {
  virtual void putc(int c) = 0;
  virtual int getc() = 0;          // blocking
  virtual int kbhit() = 0;
  virtual int peek() = 0;          // the next byte without taking it, -1 if none
  virtual void ctl(int cmd, int a, int b) {}
  virtual int width() { return 80; }
  virtual int height() { return 24; }
};

struct SerialConsole : Console {
  void putc(int c) override { if (c == '\n') Serial.write('\r'); Serial.write((char)c); }
  int getc() override { while (!Serial.available()) { k_wait(); } return Serial.read(); }
  int kbhit() override { return Serial.available(); }
  int peek() override { return Serial.available() ? Serial.peek() : -1; }
  void ctl(int cmd, int a, int b) override {
    if (cmd == CON_CLEAR) Serial.print("\033[2J\033[H");
    else if (cmd == CON_GOTO) Serial.printf("\033[%d;%dH", b + 1, a + 1);
    else if (cmd == CON_COLOR) Serial.printf("\033[%d;%dm", 30 + (a & 7), 40 + (b & 7));
  }
};

// A display + keyboard driver implements Console the same way; register it in setup().
static SerialConsole serial_console;
static Console *con = &serial_console;

// ============================================================ memory
static void *big_alloc(size_t n) {       // PSRAM first, SRAM as fallback
  void *p = nullptr;
#if defined(PICO_RP2350)
  if (rp2040.getPSRAMSize() > 0) p = pmalloc(n);
#endif
  if (!p) p = malloc(n);
  return p;
}

// ============================================================ paths and files
static char cwd[128] = "/";
static File fds[MAX_FD];
static Dir dirs[MAX_DIR]; static bool dir_used[MAX_DIR];
static int stdout_fd = 1;                // redirect() target
// the task (index into tasks) that owns each fd, socket, dir and the listener:
// a task's leftovers are closed when it ends
static uint8_t fd_owner[MAX_FD], sock_owner[MAX_SOCK], dir_owner[MAX_DIR], server_owner;
static int cur_id() { return cur - tasks; }

// ============================================================ network
static WiFiClient socks[MAX_SOCK]; static bool sock_used[MAX_SOCK];
static WiFiServer *server = nullptr;
static char ipbuf[20];

static int sock_alloc() { for (int i = 0; i < MAX_SOCK; i++) if (!sock_used[i]) { sock_used[i] = true; sock_owner[i] = cur_id(); return i; } return -1; }
static WiFiClient *sock_of(int fd) { int i = fd - SOCK_FD; return (i >= 0 && i < MAX_SOCK && sock_used[i]) ? &socks[i] : nullptr; }
static int sock_recv(int fd, char *buf, int n, int timeout) {
  WiFiClient *c = sock_of(fd); if (!c) return -1;
  uint32_t t0 = millis();
  while (!c->available()) {
    if (!c->connected()) return -1;
    if (timeout >= 0 && (int)(millis() - t0) >= timeout) return 0;
    k_wait();                                  // other tasks, or delay(1): lets the network stack run
    if (!(c = sock_of(fd))) return -1;         // closed meanwhile (its owner ended)
  }
  return c->read((uint8_t *)buf, n);
}
static int sock_send(int fd, const char *buf, int n) { WiFiClient *c = sock_of(fd); if (!c || !c->connected()) return -1; int w = c->write((const uint8_t *)buf, n); return w > 0 ? w : -1; }
static void sock_close(int fd) { WiFiClient *c = sock_of(fd); if (c) { c->stop(); sock_used[fd - SOCK_FD] = false; } }

static const char *resolve(const char *in, char *out, int outsz) {
  char tmp[256];
  if (in[0] == '/') strlcpy(tmp, in, sizeof tmp);
  else { strlcpy(tmp, cwd, sizeof tmp); if (strcmp(cwd, "/")) strlcat(tmp, "/", sizeof tmp); strlcat(tmp, in, sizeof tmp); }
  int n = 0; out[n++] = '/'; out[n] = 0;
  for (char *t = strtok(tmp, "/"); t; t = strtok(nullptr, "/")) {
    if (!strcmp(t, ".") || !*t) continue;
    if (!strcmp(t, "..")) { while (n > 1 && out[n - 1] != '/') n--; if (n > 1) n--; out[n] = 0; continue; }
    if (n > 1) out[n++] = '/';
    int l = strlen(t); if (n + l + 1 >= outsz) break;
    memcpy(out + n, t, l); n += l; out[n] = 0;
  }
  return out;
}

static bool is_dir(const char *path) { File f = fsys->open(path, "r"); bool d = f && f.isDirectory(); if (f) f.close(); return d; }

// ============================================================ named pipes
static bool is_pipe_fd(int fd) { return fd >= PIPE_FD && fd < PIPE_FD + MAX_PIPE_FDS; }
// a resolved path's pipe name (the part after /pipe/), or nullptr if it is not under /pipe/
static const char *pipe_name(const char *path) { return !strncmp(path, "/pipe/", 6) && path[6] ? path + 6 : nullptr; }
static int find_pipe(const char *nm) { for (int i = 0; i < MAX_PIPES; i++) if (pipes[i].used && !strcmp(pipes[i].name, nm)) return i; return -1; }
static int pfd_entry(int fd) { int e = fd - PIPE_FD; return e >= 0 && e < MAX_PIPE_FDS && pfds[e].used ? e : -1; }
static bool pfd_live(int e) { int i = pfds[e].pipe; return pipes[i].used && pipes[i].gen == pfds[e].gen; }
// next message end at or after pos, or -1
static int next_bound(Pipe *p, int pos) { int b = -1; for (int k = 0; k < p->nbounds; k++) if (p->bounds[k] >= pos && (b < 0 || p->bounds[k] < b)) b = p->bounds[k]; return b; }
// drops one message end at `at` (two writers closing at the same position are two messages)
static void bound_drop(Pipe *p, int at) { for (int k = 0; k < p->nbounds; k++) if (p->bounds[k] == at) { p->bounds[k] = p->bounds[--p->nbounds]; return; } }

static int k_mkpipe(const char *nm) {
  if (!nm || !nm[0] || strlen(nm) > 31 || strchr(nm, '/') || find_pipe(nm) >= 0) return -1;
  for (int i = 0; i < MAX_PIPES; i++) if (!pipes[i].used) {
    Pipe *p = &pipes[i];
    int g = p->gen + 1;
    memset(p, 0, sizeof *p);
    p->used = 1; p->gen = g; strcpy(p->name, nm); p->owner = cur_id(); p->level = cur->nest;
    return 0;
  }
  return -1;
}
// Console bytes fg_break took off the console while looking for a Ctrl-C. The
// foreground reads them before the console's own, so no keystroke is lost.
#define TYPEAHEAD 64
static uint8_t tahead[TYPEAHEAD];
static int ta_head, ta_n;
static int con_kbhit() { return ta_n + con->kbhit(); }
static int con_getc() {
  if (!ta_n) return con->getc();
  int c = tahead[ta_head]; ta_head = (ta_head + 1) % TYPEAHEAD; ta_n--;
  return c;
}
// Ctrl-C ends a foreground pipe wait: true if the foreground task is running and
// a 3 is among the pending console bytes. The bytes before it are kept for the
// program (beyond TYPEAHEAD they are dropped), the 3 is taken, the bytes after it
// stay unread. Jobs never read keys.
static bool fg_break() {
  if (cur != tasks) return false;
  while (con->kbhit()) {
    int c = con->getc();
    if (c == 3) return true;
    if (ta_n < TYPEAHEAD) tahead[(ta_head + ta_n++) % TYPEAHEAD] = c;
  }
  return false;
}
static int pipe_open(int i, int mode) {
  static int next_id;
  for (int e = 0; e < MAX_PIPE_FDS; e++) if (!pfds[e].used) {
    PipeFd *f = &pfds[e];
    f->used = 1; f->pipe = i; f->mode = mode; f->owner = cur_id(); f->level = cur->nest;
    f->wrote = 0; f->gen = pipes[i].gen; f->id = ++next_id;
    return PIPE_FD + e;
  }
  return -1;
}
// Up to cnt bytes, never across the next message end; at a message end 0,
// once. Empty: wait for a writer. A removed pipe reads 0.
static int pipe_read(int fd, char *buf, int cnt) {
  int e = pfd_entry(fd);
  if (e < 0 || pfds[e].mode != 0) return -1;
  int id = pfds[e].id;
  for (;;) {
    if (!pfds[e].used || pfds[e].id != id || !pfd_live(e)) return 0;   // closed or removed while we waited
    Pipe *p = &pipes[pfds[e].pipe];
    int pos = p->total_read, b = next_bound(p, pos);
    if (b == pos) { bound_drop(p, b); return 0; }
    if (p->count == 0) { if (fg_break()) return 0; k_wait(); continue; }
    int avail = p->count; if (b >= 0 && b - pos < avail) avail = b - pos;
    if (cnt > avail) cnt = avail;
    if (cnt < 0) cnt = 0;
    for (int k = 0; k < cnt; k++) buf[k] = p->buf[(p->head + k) % PIPE_SIZE];
    p->head = (p->head + cnt) % PIPE_SIZE; p->count -= cnt; p->total_read += cnt;
    return cnt;
  }
}
// All n bytes, waiting while the pipe is full; -1 if it is (or gets) removed.
static int pipe_write(int fd, const char *buf, int cnt) {
  int e = pfd_entry(fd);
  if (e < 0 || pfds[e].mode != 1 || cnt < 0) return -1;
  int id = pfds[e].id, done = 0;
  for (;;) {
    if (!pfds[e].used || pfds[e].id != id || !pfd_live(e)) return -1;
    Pipe *p = &pipes[pfds[e].pipe];
    int k = PIPE_SIZE - p->count; if (k > cnt - done) k = cnt - done;
    for (int j = 0; j < k; j++) p->buf[(p->head + p->count + j) % PIPE_SIZE] = buf[done + j];
    p->count += k; p->total_written += k; done += k; pfds[e].wrote = 1;
    if (done >= cnt) return cnt;
    if (fg_break()) return -1;             // what was copied stays in the pipe
    k_wait();
  }
}
// A writer's close ends a message (an empty one if it wrote nothing). With
// wait, a full bounds queue is waited out; cleanup (wait false) must not
// switch, so there the message end is dropped instead.
static void pipe_close(int e, bool wait) {
  int id = pfds[e].id;
  if (pfds[e].mode == 1) {
    while (wait && pfds[e].used && pfds[e].id == id && pfd_live(e) && pipes[pfds[e].pipe].nbounds == MAX_BOUNDS) {
      if (fg_break()) { pfds[e].used = 0; return; }   // Ctrl-C: freed, the message end is dropped
      k_wait();
    }
    if (!pfds[e].used || pfds[e].id != id) return;                      // freed meanwhile
    if (pfd_live(e)) { Pipe *p = &pipes[pfds[e].pipe]; if (p->nbounds < MAX_BOUNDS) p->bounds[p->nbounds++] = p->total_written; }
  }
  pfds[e].used = 0;
}
// A program ends: close the pipe fds it opened and remove the pipes it made
// (level -1: every level, the task ends). Fds first: closing a writer ends
// a message on another task's pipe.
static void pipes_release(int task, int level) {
  for (int e = 0; e < MAX_PIPE_FDS; e++) if (pfds[e].used && pfds[e].owner == task && (level < 0 || pfds[e].level == level)) pipe_close(e, false);
  for (int i = 0; i < MAX_PIPES; i++) if (pipes[i].used && pipes[i].owner == task && (level < 0 || pipes[i].level == level)) pipes[i].used = 0;
}

static int k_open(const char *name, int flags) {
  char path[160]; resolve(name, path, sizeof path);
  if (const char *pn = pipe_name(path)) { int i = find_pipe(pn); return i < 0 ? -1 : pipe_open(i, flags == OPEN_READ ? 0 : 1); }
  for (int i = 2; i < MAX_FD; i++) if (!fds[i]) {
    fds[i] = fsys->open(path, flags == OPEN_WRITE ? "w" : flags == OPEN_APPEND ? "a" : (flags & 1) ? "w" : "r");
    fd_owner[i] = cur_id();
    return fds[i] ? i : -1;
  }
  return -1;
}
// Only the foreground reads keys: in a background task getchar waits forever
// (other tasks run meanwhile, and kill ends it) and kbhit is 0.
static int k_getc() { if (cur != tasks) for (;;) k_wait(); return con_getc(); }
static int k_kbhit() {
  if (cur != tasks) { k_yield(); return 0; }
  int k = con_kbhit(); if (!k) { k_yield(); k = con_kbhit(); }
  return k;
}
static int k_read(int fd, char *buf, int n) {
  if (is_pipe_fd(fd)) return pipe_read(fd, buf, n);
  if (fd >= SOCK_FD) return sock_recv(fd, buf, n, -1);
  if (fd == 0) { int i = 0; while (i < n) { buf[i++] = k_getc(); if (!con_kbhit()) break; } return i; }
  if (fd < 2 || fd >= MAX_FD || !fds[fd]) return -1;
  return fds[fd].read((uint8_t *)buf, n);
}
static int k_write(int fd, const char *buf, int n) {
  if (fd == 1) fd = stdout_fd;
  if (fd == 1) { for (int i = 0; i < n; i++) con->putc(buf[i]); return n; }
  if (is_pipe_fd(fd)) return pipe_write(fd, buf, n);
  if (fd >= SOCK_FD) return sock_send(fd, buf, n);
  if (fd < 2 || fd >= MAX_FD || !fds[fd]) return -1;
  return fds[fd].write((const uint8_t *)buf, n);
}

// ============================================================ printf
static int fmt_num(char *out, unsigned v, int base, int neg) {
  char t[12]; int n = 0;
  do { int d = v % base; t[n++] = d < 10 ? '0' + d : 'a' + d - 10; v /= base; } while (v);
  int k = 0; if (neg) out[k++] = '-';
  while (n) out[k++] = t[--n];
  return k;
}
// %[-][0][width]d x c s %  ->  returns length written (buffer is limited to max)
static int k_format(char *out, int max, const char *fmt, int *argv, int n, int first) {
  int o = 0, ai = first; char tmp[64];
  #define A(i) (argv[n - 1 - (i)])
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
  #undef PUT
}

// ============================================================ programs
#define PROC (cur->procs[cur->nest])     // the running program

static void *u_malloc(int n) {
  if (n < 0) return nullptr;
  Blk *b = (Blk *)big_alloc(sizeof(Blk) + n);
  if (!b) return nullptr;
  memset(b + 1, 0, n);                  // programs (cc's data segment, for one) rely on zeroed memory
  Blk *h = &PROC.heap;
  b->next = h->next; b->prev = h; h->next->prev = b; h->next = b;
  return b + 1;
}
static void u_free(void *p) {
  if (!p) return;
  Blk *b = (Blk *)p - 1;
  b->prev->next = b->next; b->next->prev = b->prev;
  free(b);
}
static void u_free_all(int level) {     // the running task's procs[level]
  Blk *h = &cur->procs[level].heap;
  while (h->next != h) { Blk *b = h->next; h->next = b->next; free(b); }
  h->prev = h;
}

static int run_bin(const char *name, int argc, char **argv);
static int k_spawn(const char *name, int argc, char **argv);
static int k_kill(int j);
static int k_jobs(job_t *jb, int max);

extern "C" int sys(int num, int n, int *argv) {
  #define A(i) (argv[n - 1 - (i)])
  switch (num) {
    // ---- process
    case SYS_exit: PROC.exit_code = A(0); longjmp(PROC.jb, 1);
    case SYS_exec: return run_bin((const char *)A(0), A(1), (char **)A(2));
    case SYS_millis: return millis();
    case SYS_sleep: { int ms = A(0); uint32_t t0 = millis(); while ((int)(millis() - t0) < ms) k_wait(); return 0; }
    case SYS_spawn: return k_spawn((const char *)A(0), A(1), (char **)A(2));
    case SYS_yield: return k_yield();
    case SYS_jobs: return k_jobs((job_t *)A(0), A(1));
    case SYS_kill: return k_kill(A(0));
    case SYS_sysinfo: {
      int k = A(0);
      if (k == INFO_SRAM_FREE) return rp2040.getFreeHeap();
#if defined(PICO_RP2350)
      if (k == INFO_PSRAM_TOTAL) return rp2040.getPSRAMSize();
      if (k == INFO_PSRAM_FREE) return rp2040.getFreePSRAMHeap();   // 0 when no PSRAM
#endif
      if (k == INFO_DISK_TOTAL_KB || k == INFO_DISK_USED_KB) { FSInfo fi; fsys->info(fi); return (k == INFO_DISK_TOTAL_KB ? fi.totalBytes : fi.usedBytes) / 1024; }
      if (k == INFO_CONSOLE) return con->width() | con->height() << 16;
      return -1;
    }
    // ---- console
    case SYS_putchar: { char c = A(0); k_write(1, &c, 1); return A(0); }
    case SYS_getchar: return k_getc();
    case SYS_kbhit: return k_kbhit();
    case SYS_conctl: con->ctl(A(0), A(1), A(2)); return 0;
    case SYS_redirect: { int old = stdout_fd; stdout_fd = A(0) ? A(0) : 1; return old; }
    // ---- files
    case SYS_open: return k_open((const char *)A(0), A(1));
    case SYS_close: { int fd = A(0);
      if (is_pipe_fd(fd)) { int e = pfd_entry(fd); if (e >= 0) pipe_close(e, true); return 0; }
      if (fd >= SOCK_FD) { sock_close(fd); return 0; } if (fd >= 2 && fd < MAX_FD && fds[fd]) { fds[fd].close(); fds[fd] = File(); } return 0; }
    case SYS_read: return k_read(A(0), (char *)A(1), A(2));
    case SYS_write: return k_write(A(0), (const char *)A(1), A(2));
    case SYS_seek: { int fd = A(0); if (fd < 2 || fd >= MAX_FD || !fds[fd]) return -1;
      fds[fd].seek(A(1), A(2) == SEEK_BEGIN ? SeekSet : A(2) == SEEK_CURRENT ? SeekCur : SeekEnd); return fds[fd].position(); }
    case SYS_stat: { char path[160]; resolve((const char *)A(0), path, sizeof path);
      if (const char *pn = pipe_name(path)) { int i = find_pipe(pn); if (i < 0) return -1;
        stat_t *st = (stat_t *)A(1); st->size = pipes[i].count; st->isdir = 0; st->mtime = pipes[i].nbounds; /* pipes have no time: mtime = complete messages queued */ return 0; }
      File f = fsys->open(path, "r"); if (!f) return -1;
      stat_t *st = (stat_t *)A(1); st->size = f.size(); st->isdir = f.isDirectory(); st->mtime = f.getLastWrite(); f.close(); return 0; }
    case SYS_unlink: { char path[160]; resolve((const char *)A(0), path, sizeof path);
      if (pipe_name(path)) return -1;      // only the owner's end removes a pipe
      return fsys->remove(path) ? 0 : -1; }
    case SYS_rename: { char a[160], b[160]; resolve((const char *)A(0), a, sizeof a); resolve((const char *)A(1), b, sizeof b); return fsys->rename(a, b) ? 0 : -1; }
    case SYS_mkdir: { char path[160]; resolve((const char *)A(0), path, sizeof path); return fsys->mkdir(path) ? 0 : -1; }
    case SYS_rmdir: { char path[160]; resolve((const char *)A(0), path, sizeof path); return fsys->rmdir(path) ? 0 : -1; }
    case SYS_opendir: { char path[160]; resolve((const char *)A(0), path, sizeof path); if (!is_dir(path)) return -1;
      for (int i = 0; i < MAX_DIR; i++) if (!dir_used[i]) { dirs[i] = fsys->openDir(path); dir_used[i] = true; dir_owner[i] = cur_id(); return i; } return -1; }
    case SYS_readdir: { int d = A(0); if (d < 0 || d >= MAX_DIR || !dir_used[d]) return 0;
      if (!dirs[d].next()) return 0;
      dirent_t *e = (dirent_t *)A(1); strlcpy(e->name, dirs[d].fileName().c_str(), sizeof e->name); e->size = dirs[d].fileSize(); e->isdir = dirs[d].isDirectory(); return 1; }
    case SYS_closedir: { int d = A(0); if (d >= 0 && d < MAX_DIR) dir_used[d] = false; return 0; }
    case SYS_chdir: { char path[160]; resolve((const char *)A(0), path, sizeof path); if (!is_dir(path)) return -1; strlcpy(cwd, path, sizeof cwd); return 0; }
    case SYS_getcwd: strlcpy((char *)A(0), cwd, A(1)); return A(0);
    case SYS_mkpipe: return k_mkpipe((const char *)A(0));
    // ---- memory and strings
    case SYS_malloc: return (int)u_malloc(A(0));
    case SYS_free: u_free((void *)A(0)); return 0;
    case SYS_memset: return (int)memset((void *)A(0), A(1), A(2));
    case SYS_memcmp: return memcmp((void *)A(0), (void *)A(1), A(2));
    case SYS_memcpy: return (int)memcpy((void *)A(0), (void *)A(1), A(2));
    case SYS_strlen: return strlen((const char *)A(0));
    case SYS_strcmp: return strcmp((const char *)A(0), (const char *)A(1));
    case SYS_strcpy: return (int)strcpy((char *)A(0), (const char *)A(1));
    case SYS_printf: { char *fb = cur->fmtbuf; int l = k_format(fb, sizeof cur->fmtbuf, (const char *)A(0), argv, n, 1); k_write(1, fb, l < (int)sizeof cur->fmtbuf ? l : sizeof cur->fmtbuf - 1); return l; }
    case SYS_sprintf: return k_format((char *)A(0), 1 << 30, (const char *)A(1), argv, n, 2);
    case SYS_atoi: return atoi((const char *)A(0));
    case SYS_strncmp: return strncmp((const char *)A(0), (const char *)A(1), A(2));
    case SYS_strchr: return (int)strchr((const char *)A(0), A(1));
    // ---- network
    case SYS_net_join: WiFi.mode(WIFI_STA); WiFi.begin((const char *)A(0), (const char *)A(1)); {
      uint32_t t0 = millis(); while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(50); }
      return WiFi.status() == WL_CONNECTED ? 0 : -1;
    case SYS_net_status: { int st = WiFi.status(); return st == WL_CONNECTED ? NET_UP : st == WL_IDLE_STATUS ? NET_CONNECTING : NET_DOWN; }
    case SYS_net_ip: strlcpy(ipbuf, WiFi.localIP().toString().c_str(), sizeof ipbuf); return (int)ipbuf;
    case SYS_connect: { int i = sock_alloc(); if (i < 0) return -1;
      if (!socks[i].connect((const char *)A(0), A(1))) { sock_used[i] = false; return -1; }
      return SOCK_FD + i; }
    case SYS_send: return sock_send(A(0), (const char *)A(1), A(2));
    case SYS_recv: return sock_recv(A(0), (char *)A(1), A(2), A(3));
    case SYS_listen:                      // a running task's listener is its own: only it may replace it
      if (server && server_owner != cur_id() && tasks[server_owner].state == T_RUN) return -1;
      if (server) { server->stop(); delete server; } server = new WiFiServer(A(0)); server->begin(); server_owner = cur_id(); return 0;
    case SYS_accept: { if (!server) return -1; int to = A(0); uint32_t t0 = millis();
      for (;;) {
        { WiFiClient c = server->accept(); if (c) { int i = sock_alloc(); if (i < 0) { c.stop(); return -1; } socks[i] = c; return SOCK_FD + i; } }  // c is gone before k_wait (kill unwinds past it)
        if (to >= 0 && (int)(millis() - t0) >= to) return 0;
        k_wait();
        if (!server) return -1;           // its owner ended meanwhile
      } }
    case SYS_sock_avail: { WiFiClient *c = sock_of(A(0)); return c ? c->available() : -1; }
    // ---- board
    case SYS_gpio_mode: pinMode(A(0), A(1) == PIN_OUTPUT ? OUTPUT : A(1) == PIN_INPUT_PULLUP ? INPUT_PULLUP : INPUT); return 0;
    case SYS_gpio_write: digitalWrite(A(0), A(1)); return 0;
    case SYS_gpio_read: return digitalRead(A(0));
    case SYS_analog_read: return analogRead(A(0));
    case SYS_pwm_write: analogWrite(A(0), A(1)); return 0;
    case SYS_i2c_write: Wire.beginTransmission(A(0)); Wire.write((const uint8_t *)A(1), A(2)); return Wire.endTransmission();
    case SYS_i2c_read: { int got = Wire.requestFrom((int)A(0), (int)A(2)); uint8_t *b = (uint8_t *)A(1); int i = 0; while (i < got && Wire.available()) b[i++] = Wire.read(); return i; }
    case SYS_spi_transfer: SPI.transfer((void *)A(0), A(1)); return 0;
    case SYS_delay_us: delayMicroseconds(A(0)); return 0;
  }
  con->putc('\n'); Serial.printf("kernel: bad syscall %d\r\n", num);
  PROC.exit_code = -1; longjmp(PROC.jb, 1);
  #undef A
}

// Syscalls run on a kernel stack in SRAM, never on the program's stack. The
// program stack lives in PSRAM, which shares the QSPI controller with flash:
// while LittleFS erases or programs flash, PSRAM is unreachable, so touching
// that stack inside a flash write hangs the core.
// t2_sys_sp is where the next syscall's stack starts. A nested exec moves it
// below the parent's in-use kernel frames (see run_bin).
static uint64_t kstack[16 * 1024 / 8];
extern "C" { __attribute__((used)) volatile uint32_t t2_sys_sp = (uint32_t)(kstack + sizeof kstack / 8); }
// A background task's kernel stack holds its task_start() frames and, below
// them, its syscall frames (also SRAM, for the same reason).
static uint64_t jstack[MAX_JOBS][JSTACK_SIZE / 8];
static bool on_kstack(uint32_t sp) {      // on the running task's kernel stack
  uint32_t lo = cur == tasks ? (uint32_t)kstack : (uint32_t)jstack[cur_id() - 1];
  uint32_t sz = cur == tasks ? sizeof kstack : JSTACK_SIZE;
  return sp > lo && sp <= lo + sz;
}

extern "C" __attribute__((naked)) int sys_entry(int num, int n, int *argv) {
  asm volatile(
    "push {r4, lr}\n"
    "mov r4, sp\n"               // program sp (argv points into the program's stack, still readable)
    "movw r3, #:lower16:t2_sys_sp\n"
    "movt r3, #:upper16:t2_sys_sp\n"
    "ldr r3, [r3]\n"
    "mov sp, r3\n"
    "bl sys\n"
    "mov sp, r4\n"
    "pop {r4, pc}\n");
}

// Switch to the program's stack, set r8/r9, push argc/argv, call entry.
__attribute__((naked)) static int run(void *entry, void *sysfn, void *data, void *stacktop, int argc, char **argv) {
  asm volatile(
    "push {r4-r11, lr}\n"
    "ldr r5, [sp, #36]\n"        // argc
    "ldr r6, [sp, #40]\n"        // argv
    "mov r8, r1\n"
    "mov r9, r2\n"
    "mov r4, sp\n"
    "mov sp, r3\n"
    "push {r5}\n"                // arg 0 deepest
    "push {r6}\n"
    "blx r0\n"
    "add sp, #8\n"
    "mov sp, r4\n"
    "pop {r4-r11, pc}\n");
}

// Load a T2CC file: code in SRAM (PSRAM if SRAM is short), data and the
// program stack in PSRAM. Returns 0, -1 (not found), -2 (not a T2CC program)
// or -3 (out of memory): the SPAWN_ error codes.
static int load_bin(const char *name, uint8_t **pcode, uint8_t **pdata, uint8_t **pstack, uint32_t *pentry) {
  char path[160]; resolve(name, path, sizeof path);
  File f = fsys->open(path, "r");
  if (!f) return -1;
  uint32_t hdr[5];
  if (f.read((uint8_t *)hdr, 20) != 20 || hdr[0] != 0x43433254) { f.close(); return -2; }
  uint8_t *code = (uint8_t *)malloc(hdr[1] + 8);                 // SRAM: fast
  if (!code) code = (uint8_t *)big_alloc(hdr[1] + 8);            // PSRAM: works, slower
  uint8_t *data = (uint8_t *)big_alloc(hdr[2] + hdr[3] + 16);
  uint8_t *stack = (uint8_t *)big_alloc(STACK_SIZE);
  if (!code || !data || !stack) { f.close(); free(code); free(data); free(stack); Serial.println("kernel: out of memory"); return -3; }
  f.read(code, hdr[1]); f.read(data, hdr[2]); f.close();
  memset(data + hdr[2], 0, hdr[3]);
  asm volatile("dsb\n isb");
  *pcode = code; *pdata = data; *pstack = stack; *pentry = hdr[4];
  return 0;
}

// Run a loaded program as the running task's next nesting level; frees it after.
// A task's code runs only while it is cur, so cur is the same before and after run().
static int run_loaded(uint8_t *code, uint8_t *data, uint8_t *stack, uint32_t entry, int argc, char **argv) {
  Task *t = cur;
  int self = cur_id();
  int level = t->nest + 1;
  Proc *pr = &t->procs[level];
  pr->heap.next = pr->heap.prev = &pr->heap;
  pr->code = code; pr->data = data; pr->stack = stack;       // freed by task_end if the task is killed
  // what this task's parent level already holds; other tasks' slots are left out, so a slot
  // another task frees and the child reuses is still the child's to close
  pr->fd_mask = 0; for (int i = 2; i < MAX_FD; i++) if (fds[i] && fd_owner[i] == self) pr->fd_mask |= 1 << i;
  pr->sock_mask = 0; for (int i = 0; i < MAX_SOCK; i++) if (sock_used[i] && sock_owner[i] == self) pr->sock_mask |= 1 << i;
  t->nest = level;
  int saved_stdout = stdout_fd;
  int ret;
  // exec from inside a syscall: we are on the kernel stack, so the child's
  // syscalls must start below our frames (128 bytes covers run()'s pushes).
  // A background task starts here on its own kernel stack, so the same holds.
  uint32_t here; asm volatile("mov %0, sp" : "=r"(here));
  uint32_t saved_sys_sp = t2_sys_sp;
  if (on_kstack(here)) t2_sys_sp = (here - 128) & ~7u;
  if (setjmp(pr->jb) == 0) ret = run(code + entry + 1, (void *)sys_entry, data, stack + STACK_SIZE - 64, argc, argv);
  else ret = pr->exit_code;
  t2_sys_sp = saved_sys_sp;            // also after exit(), which longjmps past run()
  // leaked by the child: opened at this level by this task (another task's are not ours to close)
  for (int i = 2; i < MAX_FD; i++) if (fds[i] && fd_owner[i] == self && !(pr->fd_mask & (1 << i))) { fds[i].close(); fds[i] = File(); }
  for (int i = 0; i < MAX_SOCK; i++) if (sock_used[i] && sock_owner[i] == self && !(pr->sock_mask & (1 << i))) sock_close(SOCK_FD + i);
  pipes_release(self, level);          // the pipe fds it opened and the pipes it made
  stdout_fd = saved_stdout;
  u_free_all(level);                   // memory the child malloc()ed and never freed
  t->nest = level - 1;
  free(code); free(data); free(stack);
  return ret;
}

static int run_bin(const char *name, int argc, char **argv) {
  if (cur->nest + 1 >= MAX_NEST) { Serial.println("kernel: exec nesting too deep"); return -1; }
  uint8_t *code, *data, *stack; uint32_t entry;
  int r = load_bin(name, &code, &data, &stack, &entry);
  if (r) return r;
  return run_loaded(code, data, stack, entry, argc, argv);
}

// ============================================================ task switching
// Saves the callee-saved registers (r4-r11, lr, s16-s31) on the current
// stack, stores sp in *save_sp, loads new_sp and pops the same frame there.
// User r8/r9 are among them: inside a syscall the kernel C code keeps them
// in callee-saved registers. A new task's frame (see k_spawn) is zeros with
// lr = task_start.
extern "C" __attribute__((naked, noinline)) void ctx_switch(uint32_t *save_sp, uint32_t new_sp) {
  asm volatile(
    "push {r4-r11, lr}\n"
    "vpush {s16-s31}\n"
    "mov r2, sp\n"
    "str r2, [r0]\n"
    "mov sp, r1\n"
    "vpop {s16-s31}\n"
    "pop {r4-r11, pc}\n");
}
#define CTX_FRAME 100                    // bytes ctx_switch pushes: 16 s + 8 r + lr

// Switch to the next runnable task, round robin; 1 if one ran (we are back
// now), 0 if there was none. A killed task does not come back from here: it
// unwinds to task_start. The recovery prompt (task 0 running no program) is
// not a switch point, so jobs stay frozen while it runs.
static int k_yield() {
  if (cur == tasks && cur->nest < 0) return 0;
  int self = cur_id();
  for (int k = 1; k <= MAX_JOBS; k++) {
    Task *t = &tasks[(self + k) % (1 + MAX_JOBS)];
    if (t->state != T_RUN) continue;
    Task *old = cur;
    old->sys_sp = t2_sys_sp; old->stdout_fd = stdout_fd; memcpy(old->cwd, cwd, sizeof cwd);
    cur = t;
    t2_sys_sp = t->sys_sp; stdout_fd = t->stdout_fd; memcpy(cwd, t->cwd, sizeof cwd);
    ctx_switch(&old->sp, t->sp);
    if (cur->killed && cur != tasks) longjmp(cur->jb, 1);
    return 1;
  }
  return 0;
}
static void k_wait() { if (!k_yield()) delay(1); }

// A background task ends: free what its programs left (all nesting levels,
// it may have been killed deep inside), close what it owns, become DONE
// and switch away for good. Its slot is FREE once jobs() has reported it.
static void task_end(int code) {
  Task *t = cur;
  int self = cur_id();
  for (int l = t->nest; l >= 0; l--) { u_free_all(l); free(t->procs[l].code); free(t->procs[l].data); free(t->procs[l].stack); }
  t->nest = -1;
  free(t->code); free(t->data); free(t->stack);          // killed before it started
  t->code = t->data = t->stack = nullptr;
  for (int i = 2; i < MAX_FD; i++) if (fds[i] && fd_owner[i] == self) { fds[i].close(); fds[i] = File(); }
  for (int i = 0; i < MAX_SOCK; i++) if (sock_used[i] && sock_owner[i] == self) sock_close(SOCK_FD + i);
  for (int i = 0; i < MAX_DIR; i++) if (dir_used[i] && dir_owner[i] == self) dir_used[i] = false;
  if (server && server_owner == self) { server->stop(); delete server; server = nullptr; }
  pipes_release(self, -1);               // all levels: it may have been killed deep inside
  t->exit_code = code;
  t->state = T_DONE;
  for (;;) k_yield();                    // task 0 is always runnable
}

// A new task's first switch-in lands here, on its own kernel stack.
static void task_start() {
  Task *t = cur;
  int code;
  if (setjmp(t->jb) == 0) {
    if (t->killed) code = JOB_KILLED;
    else {
      uint8_t *c = t->code, *d = t->data, *s = t->stack;
      t->code = t->data = t->stack = nullptr;             // run_loaded owns them now
      code = run_loaded(c, d, s, t->entry, t->argc, t->argv);
    }
  } else code = JOB_KILLED;              // kill: k_yield longjmps here
  task_end(code);
}

// spawn(path, argc, argv): load in the caller's context (errors return at
// once), copy the arguments, inherit cwd and the current stdout (whose fd
// moves to the job), and make the job runnable. It first runs at the
// caller's next switch point.
static int k_spawn(const char *name, int argc, char **argv) {
  int j = 1;
  while (j <= MAX_JOBS && tasks[j].state != T_FREE) j++;
  if (j > MAX_JOBS) return SPAWN_FULL;
  uint8_t *code, *data, *stack; uint32_t entry;
  int r = load_bin(name, &code, &data, &stack, &entry);
  if (r) return r;
  Task *t = &tasks[j];
  if (argc < 0) argc = 0;
  if (argc > MAX_ARGS) argc = MAX_ARGS;
  int o = 0, k = 0;
  for (; k < argc; k++) {                // arguments that do not fit in ARG_BYTES are dropped
    const char *a = argv[k] ? argv[k] : "";
    int l = strlen(a) + 1;
    if (o + l > ARG_BYTES) break;
    memcpy(t->args + o, a, l); t->argv[k] = t->args + o; o += l;
  }
  t->argc = k; t->argv[k] = nullptr;
  const char *base = k > 0 ? t->argv[0] : name;           // the job's name: argv[0] without directory and .bin
  const char *sl = strrchr(base, '/'); if (sl) base = sl + 1;
  strlcpy(t->name, base, sizeof t->name);
  char *dot = strrchr(t->name, '.'); if (dot && !strcmp(dot, ".bin")) *dot = 0;
  t->code = code; t->data = data; t->stack = stack; t->entry = entry;
  t->nest = -1; t->killed = 0; t->exit_code = 0; t->gen++;
  memcpy(t->cwd, cwd, sizeof cwd);
  t->stdout_fd = stdout_fd;
  if (stdout_fd >= 2 && stdout_fd < MAX_FD) fd_owner[stdout_fd] = j;
  if (stdout_fd >= SOCK_FD && stdout_fd < SOCK_FD + MAX_SOCK) sock_owner[stdout_fd - SOCK_FD] = j;
  if (pfd_entry(stdout_fd) >= 0) { pfds[stdout_fd - PIPE_FD].owner = j; pfds[stdout_fd - PIPE_FD].level = 0; }   // closed when the job ends
  uint32_t top = (uint32_t)(jstack[j - 1] + JSTACK_SIZE / 8);
  t->sys_sp = top;                       // run_loaded moves it below its own frames
  uint32_t *f = (uint32_t *)(top - CTX_FRAME);
  memset(f, 0, CTX_FRAME);
  f[CTX_FRAME / 4 - 1] = (uint32_t)task_start;           // popped into pc (Thumb bit set)
  t->sp = (uint32_t)f;
  t->state = T_RUN;
  return j;
}

// kill(job): mark it, then wait until it has ended (it unwinds at its next
// switch-in, see k_yield). A task killing itself ends here.
static int k_kill(int j) {
  if (j < 1 || j > MAX_JOBS || tasks[j].state != T_RUN) return -1;
  Task *t = &tasks[j];
  t->killed = 1;
  if (t == cur) longjmp(cur->jb, 1);
  int g = t->gen;
  while (t->state == T_RUN && t->gen == g) k_wait();
  return 0;
}

// jobs(buf, max): RUN and DONE jobs; a DONE one is reported once, then its slot is free
static int k_jobs(job_t *jb, int max) {
  int cnt = 0;
  for (int i = 1; i <= MAX_JOBS && cnt < max; i++) {
    Task *t = &tasks[i];
    if (t->state == T_FREE) continue;
    jb[cnt].id = i;
    jb[cnt].state = t->state == T_RUN ? JOB_RUNNING : JOB_DONE;
    jb[cnt].exit_code = t->state == T_DONE ? t->exit_code : 0;
    strlcpy(jb[cnt].name, t->name, sizeof jb[cnt].name);
    if (t->state == T_DONE) t->state = T_FREE;
    cnt++;
  }
  return cnt;
}

// ============================================================ recovery prompt
// Only enough to get files onto the card when /bin/sh.bin is missing or broken.
static void recv_file(const char *name, int size) {
  char path[160]; resolve(name, path, sizeof path);
  File f = fsys->open(path, "w");
  if (!f) { Serial.printf("cannot write %s\r\n", path); return; }
  Serial.printf("receiving %d bytes\r\n", size);
  uint8_t buf[512]; int got = 0; uint32_t last = millis();
  while (got < size && millis() - last < 5000) {
    int k = Serial.readBytes((char *)buf, min((int)sizeof buf, size - got));
    if (k > 0) { f.write(buf, k); got += k; last = millis(); }
  }
  f.close();
  Serial.printf(got == size ? "ok\r\n" : "timeout after %d bytes\r\n", got);
}

static void recovery(char *line) {
  char *argv[8]; int argc = 0;
  for (char *t = strtok(line, " "); t && argc < 8; t = strtok(nullptr, " ")) argv[argc++] = t;
  if (!argc) return;
  if (!strcmp(argv[0], "put") && argc > 2) recv_file(argv[1], atoi(argv[2]));
  else if (!strcmp(argv[0], "ls")) { Dir d = fsys->openDir(argc > 1 ? argv[1] : "/"); while (d.next()) Serial.printf("%8d  %s\r\n", (int)d.fileSize(), d.fileName().c_str()); }
  else if (!strcmp(argv[0], "run") && argc > 1) Serial.printf("[exit %d]\r\n", run_bin(argv[1], argc - 1, argv + 1));
  else if (!strcmp(argv[0], "mkdir") && argc > 1) fsys->mkdir(argv[1]);
  else Serial.println("recovery: put name size | ls [dir] | mkdir d | run prog.bin [args]");
}

void setup() {
  tasks[0].state = T_RUN; tasks[0].nest = -1;      // the foreground task, never killed
#if defined(PICO_RP2350)
  *(volatile uint32_t *)0xE000ED88 |= 0xF << 20;   // CPACR: programs use the FPU (CP10, CP11)
  asm volatile("dsb\n isb");
#endif
  Serial.begin(115200);
  while (!Serial) {}
  Wire.begin(); SPI.begin();
  SDFS.setConfig(SDFSConfig(SD_CS, SPI_FULL_SPEED));
  const char *fsname = "NONE";
  bool mounted = SDFS.begin();
  if (mounted) fsname = "sd";
  else if ((mounted = LittleFS.begin())) { fsys = &LittleFS; fsname = "flash"; }   // formats on first use
#if defined(PICO_RP2350)
  Serial.printf("T2DOS kernel  psram %d  sram free %d  fs %s\r\n", (int)rp2040.getPSRAMSize(), (int)rp2040.getFreeHeap(), fsname);
#endif
  if (mounted && !is_dir("/bin")) fsys->mkdir("/bin");
}

void loop() {
  if (!(bool)fsys->open("/bin/sh.bin", "r")) {
    Serial.println("no /bin/sh.bin, recovery prompt");
  } else {
    char *argv[] = { (char *)"sh" };
    int r = run_bin("/bin/sh.bin", 1, argv);
    Serial.printf("sh exited (%d), recovery prompt\r\n", r);
  }
  static char line[128];
  for (;;) {
    Serial.print("recovery> "); int len = 0;
    for (;;) { int c = serial_console.getc(); if (c == '\r' || c == '\n') break; if ((c == 8 || c == 127) && len) { len--; Serial.print("\b \b"); } else if (c >= 32 && len < 127) { line[len++] = c; Serial.write(c); } }
    line[len] = 0; Serial.println();
    if (!strcmp(line, "sh")) break;
    recovery(line);
  }
}
