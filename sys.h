// sys.h - T2DOS syscall ABI. Shared by the kernel (C++), cc.c (builtin table)
// and user programs. Numbers are frozen; add new ones in the gaps.
//
// Convention: all arguments are ints on the stack, result in r0.
// Builtins are called by name from C: open("x", 1), gpio_write(25, 1), ...

enum {
  // process
  SYS_exit = 0, SYS_exec = 1, SYS_millis = 2, SYS_sleep = 3, SYS_sysinfo = 4,
  SYS_spawn = 5, SYS_yield = 6, SYS_jobs = 7, SYS_kill = 8,
  // console (fd 0 = console in, fd 1 = console out)
  SYS_putchar = 16, SYS_getchar = 17, SYS_kbhit = 18, SYS_conctl = 19, SYS_redirect = 20,
  // files
  SYS_open = 32, SYS_close = 33, SYS_read = 34, SYS_write = 35, SYS_seek = 36, SYS_stat = 37,
  SYS_unlink = 38, SYS_rename = 39, SYS_mkdir = 40, SYS_rmdir = 41,
  SYS_opendir = 42, SYS_readdir = 43, SYS_closedir = 44, SYS_chdir = 45, SYS_getcwd = 46,
  SYS_mkpipe = 47,
  // memory and string helpers (done in the kernel: fast, saves code in every program)
  SYS_malloc = 64, SYS_free = 65, SYS_memset = 66, SYS_memcmp = 67, SYS_memcpy = 68,
  SYS_strlen = 69, SYS_strcmp = 70, SYS_strcpy = 71, SYS_printf = 72, SYS_sprintf = 73,
  SYS_atoi = 74, SYS_strncmp = 75, SYS_strchr = 76,
  // network. Socket fds are 100+ and also work with read()/write()/close().
  SYS_net_join = 80, SYS_net_status = 81, SYS_net_ip = 82,
  SYS_connect = 83, SYS_send = 84, SYS_recv = 85, SYS_listen = 86, SYS_accept = 87, SYS_sock_avail = 88,
  // board
  SYS_gpio_mode = 96, SYS_gpio_write = 97, SYS_gpio_read = 98, SYS_analog_read = 99, SYS_pwm_write = 100,
  SYS_i2c_write = 101, SYS_i2c_read = 102, SYS_spi_transfer = 103, SYS_delay_us = 104,
  // maths: float arguments and result travel as the float's bit pattern (float sin(float), ...)
  SYS_sin = 112, SYS_cos = 113, SYS_tan = 114, SYS_asin = 115, SYS_acos = 116, SYS_atan = 117, SYS_atan2 = 118,
  SYS_sinh = 119, SYS_cosh = 120, SYS_tanh = 121, SYS_exp = 122, SYS_log = 123, SYS_log10 = 124, SYS_pow = 125,
  SYS_sqrt = 126, SYS_fabs = 127, SYS_floor = 128, SYS_ceil = 129, SYS_fmod = 130
};

// open() flags
enum { OPEN_READ = 0, OPEN_WRITE = 1, OPEN_APPEND = 2 };
// seek() whence
enum { SEEK_BEGIN = 0, SEEK_CURRENT = 1, SEEK_FINISH = 2 };
// sysinfo() kinds
enum { INFO_SRAM_FREE = 0, INFO_PSRAM_TOTAL = 1, INFO_PSRAM_FREE = 2, INFO_DISK_TOTAL_KB = 3, INFO_DISK_USED_KB = 4, INFO_CONSOLE = 5 };
// conctl() commands
enum { CON_CLEAR = 0, CON_GOTO = 1, CON_COLOR = 2, CON_SIZE = 3 };
// gpio_mode()
enum { PIN_INPUT = 0, PIN_OUTPUT = 1, PIN_INPUT_PULLUP = 2 };

// net_status() results
enum { NET_DOWN = 0, NET_CONNECTING = 1, NET_UP = 2 };
// jobs() states
enum { JOB_RUNNING = 1, JOB_DONE = 2 };
// spawn() errors
enum { SPAWN_NOTFOUND = -1, SPAWN_BADPROG = -2, SPAWN_NOMEM = -3, SPAWN_FULL = -4, SPAWN_UNSUPPORTED = -5 };
#define MAX_JOBS 4
#define PIPE_SIZE 512
#define JOB_KILLED -9   // exit code of a killed job
// spawn(path, argc, argv): starts path as a background job; returns its id (1..MAX_JOBS) or a SPAWN_ error
// yield(): lets another runnable task run; returns 1 if one did, else 0
// jobs(buf, max): fills buf with the job table and returns how many it wrote
// kill(job): marks a running job to end (exit code JOB_KILLED); returns 0 once it has ended, -1 if no such running job
// mkpipe(name): name is WITHOUT the /pipe/ prefix (mkpipe("x") creates /pipe/x), owned by the caller; name is at most 31 chars with no '/'; returns 0, or -1 if it exists, the table is full or the name is invalid
// recv(fd, buf, n, timeout_ms): >0 bytes, 0 timeout, -1 closed.  timeout -1 = wait forever
// listen(port): 0, or -1 if another running job owns the listener or the port can't be bound (in use, e.g.
// for a while after this board closed a connection on that port, which then sits in TCP TIME_WAIT)
// accept(timeout_ms): >0 a connection's socket fd (they start at 100), 0 nobody connected within the timeout
// (accept(0) never waits), -1 no listener or no free socket.  timeout -1 = wait forever.  Test sock > 0, not >= 0
// jobs(buf, max) fills this
struct job_t { int id; int state; int exit_code; char name[32]; };
// stat() fills this. For a /pipe/ file, size is the bytes queued and mtime (pipes have no time) is the
// number of complete messages queued: when mtime > 0 a read of the next message will not wait
struct stat_t { int size; int isdir; int mtime; };
// readdir() fills this
struct dirent_t { char name[64]; int size; int isdir; };
