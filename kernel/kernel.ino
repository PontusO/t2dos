// kernel.ino - T2DOS: a DOS-class kernel for the RP2350 (arduino-pico).
//
// The kernel owns hardware and privileges only: syscall table (sys.h), program
// loader with nested exec, memory arenas, console and file drivers, board I/O.
// Everything else is a T2CC program on the SD card. Boot runs /bin/sh.bin.
//
// Compiled-code contract: r8 = sys(), r9 = data base, args on the stack
// (argv[n-1] is arg 0), result in r0. See cc.c and sys.h.

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

// Root filesystem: the SD card if it mounts, else LittleFS in on-board flash.
// The flash FS size comes from the board's flash layout (build with
// flash=8388608_7340032 on the CPico 2350: 1 MB kernel, 7 MB filesystem).
static FS *fsys = &SDFS;

// ============================================================ console drivers
struct Console {
  virtual void putc(int c) = 0;
  virtual int getc() = 0;          // blocking
  virtual int kbhit() = 0;
  virtual void ctl(int cmd, int a, int b) {}
  virtual int width() { return 80; }
  virtual int height() { return 24; }
};

struct SerialConsole : Console {
  void putc(int c) override { if (c == '\n') Serial.write('\r'); Serial.write((char)c); }
  int getc() override { while (!Serial.available()) { delay(1); } return Serial.read(); }
  int kbhit() override { return Serial.available(); }
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

// ============================================================ network
static WiFiClient socks[MAX_SOCK]; static bool sock_used[MAX_SOCK];
static WiFiServer *server = nullptr;
static char ipbuf[20];

static int sock_alloc() { for (int i = 0; i < MAX_SOCK; i++) if (!sock_used[i]) { sock_used[i] = true; return i; } return -1; }
static WiFiClient *sock_of(int fd) { int i = fd - SOCK_FD; return (i >= 0 && i < MAX_SOCK && sock_used[i]) ? &socks[i] : nullptr; }
static int sock_recv(int fd, char *buf, int n, int timeout) {
  WiFiClient *c = sock_of(fd); if (!c) return -1;
  uint32_t t0 = millis();
  while (!c->available()) {
    if (!c->connected()) return -1;
    if (timeout >= 0 && (int)(millis() - t0) >= timeout) return 0;
    delay(1);                                  // lets the network stack run
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

static int k_open(const char *name, int flags) {
  char path[160]; resolve(name, path, sizeof path);
  for (int i = 2; i < MAX_FD; i++) if (!fds[i]) {
    fds[i] = fsys->open(path, flags == OPEN_WRITE ? "w" : flags == OPEN_APPEND ? "a" : (flags & 1) ? "w" : "r");
    return fds[i] ? i : -1;
  }
  return -1;
}
static int k_read(int fd, char *buf, int n) {
  if (fd >= SOCK_FD) return sock_recv(fd, buf, n, -1);
  if (fd == 0) { int i = 0; while (i < n) { buf[i++] = con->getc(); if (!con->kbhit()) break; } return i; }
  if (fd < 2 || fd >= MAX_FD || !fds[fd]) return -1;
  return fds[fd].read((uint8_t *)buf, n);
}
static int k_write(int fd, const char *buf, int n) {
  if (fd == 1) fd = stdout_fd;
  if (fd == 1) { for (int i = 0; i < n; i++) con->putc(buf[i]); return n; }
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
  int o = 0, ai = first; char tmp[16];
  #define A(i) (argv[n - 1 - (i)])
  #define PUT(c) do { if (o < max - 1) out[o] = (c); o++; } while (0)
  for (const char *f = fmt; *f; f++) {
    if (*f != '%') { PUT(*f); continue; }
    f++;
    int left = 0, zero = 0, width = 0;
    if (*f == '-') { left = 1; f++; }
    if (*f == '0') { zero = 1; f++; }
    while (*f >= '0' && *f <= '9') width = width * 10 + *f++ - '0';
    const char *s = tmp; int len;
    if (*f == 'd') { int v = A(ai++); len = fmt_num(tmp, v < 0 ? -v : v, 10, v < 0); }
    else if (*f == 'u') { len = fmt_num(tmp, A(ai++), 10, 0); }
    else if (*f == 'x') { len = fmt_num(tmp, A(ai++), 16, 0); }
    else if (*f == 'c') { tmp[0] = A(ai++); len = 1; }
    else if (*f == 's') { s = (const char *)A(ai++); if (!s) s = "(null)"; len = strlen(s); }
    else if (*f == '%') { tmp[0] = '%'; len = 1; }
    else { PUT('%'); PUT(*f); continue; }
    if (!left) for (int i = len; i < width; i++) PUT(zero && *f != 's' ? '0' : ' ');
    for (int i = 0; i < len; i++) PUT(s[i]);
    if (left) for (int i = len; i < width; i++) PUT(' ');
  }
  out[o < max ? o : max - 1] = 0;
  return o;
  #undef PUT
}

// ============================================================ programs
// Every malloc() a program makes is linked into its Proc, so whatever it did
// not free() is reclaimed when it exits. The 8-byte header keeps 8-byte alignment.
struct Blk { Blk *next, *prev; };
struct Proc { jmp_buf jb; int exit_code; uint16_t fd_mask; uint8_t sock_mask; Blk heap; };
static Proc procs[MAX_NEST];
static int nest = -1;

static void *u_malloc(int n) {
  if (n < 0) return nullptr;
  Blk *b = (Blk *)big_alloc(sizeof(Blk) + n);
  if (!b) return nullptr;
  memset(b + 1, 0, n);                  // programs (cc's data segment, for one) rely on zeroed memory
  Blk *h = &procs[nest].heap;
  b->next = h->next; b->prev = h; h->next->prev = b; h->next = b;
  return b + 1;
}
static void u_free(void *p) {
  if (!p) return;
  Blk *b = (Blk *)p - 1;
  b->prev->next = b->next; b->next->prev = b->prev;
  free(b);
}
static void u_free_all(int level) {     // index, not Proc*: Arduino's generated prototypes precede struct Proc
  Blk *h = &procs[level].heap;
  while (h->next != h) { Blk *b = h->next; h->next = b->next; free(b); }
  h->prev = h;
}
static char fmtbuf[1024];

static int run_bin(const char *name, int argc, char **argv);

extern "C" int sys(int num, int n, int *argv) {
  #define A(i) (argv[n - 1 - (i)])
  switch (num) {
    // ---- process
    case SYS_exit: procs[nest].exit_code = A(0); longjmp(procs[nest].jb, 1);
    case SYS_exec: return run_bin((const char *)A(0), A(1), (char **)A(2));
    case SYS_millis: return millis();
    case SYS_sleep: delay(A(0)); return 0;
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
    case SYS_getchar: return con->getc();
    case SYS_kbhit: return con->kbhit();
    case SYS_conctl: con->ctl(A(0), A(1), A(2)); return 0;
    case SYS_redirect: { int old = stdout_fd; stdout_fd = A(0) ? A(0) : 1; return old; }
    // ---- files
    case SYS_open: return k_open((const char *)A(0), A(1));
    case SYS_close: { int fd = A(0); if (fd >= SOCK_FD) { sock_close(fd); return 0; } if (fd >= 2 && fd < MAX_FD && fds[fd]) { fds[fd].close(); fds[fd] = File(); } return 0; }
    case SYS_read: return k_read(A(0), (char *)A(1), A(2));
    case SYS_write: return k_write(A(0), (const char *)A(1), A(2));
    case SYS_seek: { int fd = A(0); if (fd < 2 || !fds[fd]) return -1;
      fds[fd].seek(A(1), A(2) == SEEK_BEGIN ? SeekSet : A(2) == SEEK_CURRENT ? SeekCur : SeekEnd); return fds[fd].position(); }
    case SYS_stat: { char path[160]; resolve((const char *)A(0), path, sizeof path);
      File f = fsys->open(path, "r"); if (!f) return -1;
      stat_t *st = (stat_t *)A(1); st->size = f.size(); st->isdir = f.isDirectory(); st->mtime = f.getLastWrite(); f.close(); return 0; }
    case SYS_unlink: { char path[160]; resolve((const char *)A(0), path, sizeof path); return fsys->remove(path) ? 0 : -1; }
    case SYS_rename: { char a[160], b[160]; resolve((const char *)A(0), a, sizeof a); resolve((const char *)A(1), b, sizeof b); return fsys->rename(a, b) ? 0 : -1; }
    case SYS_mkdir: { char path[160]; resolve((const char *)A(0), path, sizeof path); return fsys->mkdir(path) ? 0 : -1; }
    case SYS_rmdir: { char path[160]; resolve((const char *)A(0), path, sizeof path); return fsys->rmdir(path) ? 0 : -1; }
    case SYS_opendir: { char path[160]; resolve((const char *)A(0), path, sizeof path); if (!is_dir(path)) return -1;
      for (int i = 0; i < MAX_DIR; i++) if (!dir_used[i]) { dirs[i] = fsys->openDir(path); dir_used[i] = true; return i; } return -1; }
    case SYS_readdir: { int d = A(0); if (d < 0 || d >= MAX_DIR || !dir_used[d]) return 0;
      if (!dirs[d].next()) return 0;
      dirent_t *e = (dirent_t *)A(1); strlcpy(e->name, dirs[d].fileName().c_str(), sizeof e->name); e->size = dirs[d].fileSize(); e->isdir = dirs[d].isDirectory(); return 1; }
    case SYS_closedir: { int d = A(0); if (d >= 0 && d < MAX_DIR) dir_used[d] = false; return 0; }
    case SYS_chdir: { char path[160]; resolve((const char *)A(0), path, sizeof path); if (!is_dir(path)) return -1; strlcpy(cwd, path, sizeof cwd); return 0; }
    case SYS_getcwd: strlcpy((char *)A(0), cwd, A(1)); return A(0);
    // ---- memory and strings
    case SYS_malloc: return (int)u_malloc(A(0));
    case SYS_free: u_free((void *)A(0)); return 0;
    case SYS_memset: return (int)memset((void *)A(0), A(1), A(2));
    case SYS_memcmp: return memcmp((void *)A(0), (void *)A(1), A(2));
    case SYS_memcpy: return (int)memcpy((void *)A(0), (void *)A(1), A(2));
    case SYS_strlen: return strlen((const char *)A(0));
    case SYS_strcmp: return strcmp((const char *)A(0), (const char *)A(1));
    case SYS_strcpy: return (int)strcpy((char *)A(0), (const char *)A(1));
    case SYS_printf: { int l = k_format(fmtbuf, sizeof fmtbuf, (const char *)A(0), argv, n, 1); k_write(1, fmtbuf, l < (int)sizeof fmtbuf ? l : sizeof fmtbuf - 1); return l; }
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
    case SYS_listen: if (server) { server->stop(); delete server; } server = new WiFiServer(A(0)); server->begin(); return 0;
    case SYS_accept: { if (!server) return -1; uint32_t t0 = millis();
      for (;;) { WiFiClient c = server->accept(); if (c) { int i = sock_alloc(); if (i < 0) { c.stop(); return -1; } socks[i] = c; return SOCK_FD + i; }
        if (A(0) >= 0 && (int)(millis() - t0) >= A(0)) return 0; delay(1); } }
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
  procs[nest].exit_code = -1; longjmp(procs[nest].jb, 1);
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
static bool on_kstack(uint32_t sp) { return sp > (uint32_t)kstack && sp <= (uint32_t)(kstack + sizeof kstack / 8); }

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

static int run_bin(const char *name, int argc, char **argv) {
  if (nest + 1 >= MAX_NEST) { Serial.println("kernel: exec nesting too deep"); return -1; }
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

  nest++;
  Proc *pr = &procs[nest];
  pr->heap.next = pr->heap.prev = &pr->heap;
  pr->fd_mask = 0; for (int i = 2; i < MAX_FD; i++) if (fds[i]) pr->fd_mask |= 1 << i;   // fds that belong to the parent
  pr->sock_mask = 0; for (int i = 0; i < MAX_SOCK; i++) if (sock_used[i]) pr->sock_mask |= 1 << i;
  int saved_stdout = stdout_fd;
  int ret;
  // exec from inside a syscall: we are on the kernel stack, so the child's
  // syscalls must start below our frames (128 bytes covers run()'s pushes).
  uint32_t here; asm volatile("mov %0, sp" : "=r"(here));
  uint32_t saved_sys_sp = t2_sys_sp;
  if (on_kstack(here)) t2_sys_sp = (here - 128) & ~7u;
  if (setjmp(pr->jb) == 0) ret = run(code + hdr[4] + 1, (void *)sys_entry, data, stack + STACK_SIZE - 64, argc, argv);
  else ret = pr->exit_code;
  t2_sys_sp = saved_sys_sp;            // also after exit(), which longjmps past run()
  for (int i = 2; i < MAX_FD; i++) if (fds[i] && !(pr->fd_mask & (1 << i))) { fds[i].close(); fds[i] = File(); }  // leaked by the child
  for (int i = 0; i < MAX_SOCK; i++) if (sock_used[i] && !(pr->sock_mask & (1 << i))) sock_close(SOCK_FD + i);
  stdout_fd = saved_stdout;
  u_free_all(nest);                    // memory the child malloc()ed and never freed
  nest--;
  free(code); free(data); free(stack);
  return ret;
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
