// pcsys.h - PC build of BASIC only (gcc -m32 -include basic/pcsys.h basic/*.c): the
// T2DOS library on top of libc, so the interpreter's own sources build unchanged.
// Calls whose name or meaning differs from libc's are renamed to t2_* (implemented in
// pcsys.c); the maths built-ins map to libm's single-precision functions, as on the board.
// The board build (compiler/cc) never sees this file.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/stat.h>
#include <dirent.h>

struct stat_t;
struct dirent_t;
struct job_t;

#define exit t2_exit
#define exec t2_exec
#define millis t2_millis
#define sleep t2_sleep
#define sysinfo t2_sysinfo
#define spawn t2_spawn
#define yield t2_yield
#define jobs t2_jobs
#define kill t2_kill
#define putchar t2_putchar
#define getchar t2_getchar
#define kbhit t2_kbhit
#define conctl t2_conctl
#define redirect t2_redirect
#define open t2_open
#define close t2_close
#define read t2_read
#define write t2_write
#define seek t2_seek
#define stat t2_stat
#define unlink t2_unlink
#define rename t2_rename
#define mkdir t2_mkdir
#define rmdir t2_rmdir
#define opendir t2_opendir
#define readdir t2_readdir
#define closedir t2_closedir
#define chdir t2_chdir
#define getcwd t2_getcwd
#define mkpipe t2_mkpipe
#define malloc t2_malloc
#define printf t2_printf
#define gpio_mode t2_gpio_mode
#define gpio_write t2_gpio_write
#define gpio_read t2_gpio_read
#define analog_read t2_analog_read
#define pwm_write t2_pwm_write
#define delay_us t2_delay_us

void t2_exit(int code);
int t2_exec(char *path, int argc, char **argv);
int t2_millis(void);
int t2_sleep(int ms);
int t2_sysinfo(int kind);
int t2_spawn(char *path, int argc, char **argv);
int t2_yield(void);
int t2_jobs(struct job_t *buf, int max);
int t2_kill(int job);
int t2_putchar(int c);
int t2_getchar(void);
int t2_kbhit(void);
int t2_conctl(int cmd, int a, int b);
int t2_redirect(int fd);
int t2_open(char *path, int mode);
int t2_close(int fd);
int t2_read(int fd, void *buf, int n);
int t2_write(int fd, void *buf, int n);
int t2_seek(int fd, int offset, int whence);
int t2_stat(char *path, struct stat_t *st);
int t2_unlink(char *path);
int t2_rename(char *from, char *to);
int t2_mkdir(char *path);
int t2_rmdir(char *path);
int t2_opendir(char *path);
int t2_readdir(int d, struct dirent_t *e);
int t2_closedir(int d);
int t2_chdir(char *path);
char *t2_getcwd(char *buf, int n);
int t2_mkpipe(char *name);
void *t2_malloc(int n);
int t2_printf(char *fmt, ...);
int t2_gpio_mode(int pin, int mode);
int t2_gpio_write(int pin, int v);
int t2_gpio_read(int pin);
int t2_analog_read(int pin);
int t2_pwm_write(int pin, int v);
int t2_delay_us(int us);

// maths: T2DOS's are single precision (float in, float out)
#define sin(x) sinf(x)
#define cos(x) cosf(x)
#define tan(x) tanf(x)
#define asin(x) asinf(x)
#define acos(x) acosf(x)
#define atan(x) atanf(x)
#define atan2(y, x) atan2f(y, x)
#define sinh(x) sinhf(x)
#define cosh(x) coshf(x)
#define tanh(x) tanhf(x)
#define exp(x) expf(x)
#define log(x) logf(x)
#define log10(x) log10f(x)
#define pow(x, y) powf(x, y)
#define sqrt(x) sqrtf(x)
#define fabs(x) fabsf(x)
#define floor(x) floorf(x)
#define ceil(x) ceilf(x)
#define fmod(x, y) fmodf(x, y)
