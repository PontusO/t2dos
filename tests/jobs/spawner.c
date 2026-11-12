// spawner.c - foreground driver for the background job tests (tools/check_jobs.py).
// spawner CASE runs one case, prints one line per check ("ok ..." or "FAIL ...")
// and then PASS or FAIL. Cases: basic errors sleeping getchar redirect exec listen preempt
// spinkill.
// Every case leaves the job table empty (all jobs ended and reported by jobs()).
#include "sys.h"

#define TICK "/src/jobs/tick.bin"
#define HOG "/src/jobs/hog.bin"
#define SPIN "/src/jobs/spin.bin"
#define SPIN_N 300000

struct job_t jb[8];
char *av[8];
char line[64];
char msg[128];
int fails;

int check(int cond) {
  if (cond) printf("ok %s\n", msg); else { printf("FAIL %s\n", msg); fails = fails + 1; }
  return cond;
}

// spawn TICK with arguments n and ms
int tick(char *n, char *ms) {
  av[0] = "tick"; av[1] = n; av[2] = ms; av[3] = 0;
  return spawn(TICK, 3, av);
}

// exactly one jobs() entry: job id, done with exit code code
int expect_done(int id, int code) {
  int n;
  n = jobs(jb, 8);
  sprintf(msg, "jobs: %d entries, id %d state %d exit %d (want 1, %d, %d, %d)", n, jb[0].id, jb[0].state, jb[0].exit_code, id, JOB_DONE, code);
  return check(n == 1 && jb[0].id == id && jb[0].state == JOB_DONE && jb[0].exit_code == code);
}

int empty() {
  int n;
  n = jobs(jb, 8);
  sprintf(msg, "job table empty (%d entries)", n);
  return check(n == 0);
}

int case_basic() {
  int id;
  id = tick("3", "100");
  sprintf(msg, "spawn tick 3 100 = %d", id);
  check(id >= 1 && id <= MAX_JOBS);
  sleep(1000);
  expect_done(id, 7);
  sprintf(msg, "job name \"%s\"", jb[0].name);
  check(!strcmp(jb[0].name, "tick"));
  empty();
  return 0;
}

int case_errors() {
  int r; int i; int n; int good; int ids[5];
  av[0] = "x"; av[1] = 0;
  r = spawn("/nosuch.bin", 1, av);
  sprintf(msg, "spawn /nosuch.bin = %d (want %d)", r, SPAWN_NOTFOUND);
  check(r == SPAWN_NOTFOUND);
  r = spawn("/src/jobs/notprog.txt", 1, av);
  sprintf(msg, "spawn notprog.txt = %d (want %d)", r, SPAWN_BADPROG);
  check(r == SPAWN_BADPROG);
  i = 0;
  while (i < 5) { ids[i] = tick("0", "1000"); i = i + 1; }
  sprintf(msg, "four spawns = %d %d %d %d", ids[0], ids[1], ids[2], ids[3]);
  check(ids[0] > 0 && ids[1] > 0 && ids[2] > 0 && ids[3] > 0);
  sprintf(msg, "fifth spawn = %d (want %d)", ids[4], SPAWN_FULL);
  check(ids[4] == SPAWN_FULL);
  n = jobs(jb, 8);
  good = n == 4;
  i = 0;
  while (i < n) { if (jb[i].state != JOB_RUNNING) good = 0; i = i + 1; }
  sprintf(msg, "jobs: %d running", n);
  check(good);
  i = 0;
  while (i < 4) {
    r = kill(ids[i]);
    sprintf(msg, "kill %d = %d", ids[i], r);
    check(r == 0);
    i = i + 1;
  }
  r = kill(ids[0]);
  sprintf(msg, "kill %d again = %d (want -1)", ids[0], r);
  check(r == -1);
  n = jobs(jb, 8);
  good = n == 4;
  i = 0;
  while (i < n) { if (jb[i].state != JOB_DONE || jb[i].exit_code != JOB_KILLED) good = 0; i = i + 1; }
  sprintf(msg, "jobs: %d done, killed", n);
  check(good);
  empty();
  return 0;
}

int case_sleeping() {
  int id; int r;
  id = tick("0", "5000");
  sleep(200);                          // the job is now inside sleep(5000)
  r = kill(id);
  sprintf(msg, "kill sleeping job %d = %d", id, r);
  check(r == 0);
  expect_done(id, JOB_KILLED);
  empty();
  return 0;
}

int case_getchar() {
  int id; int r; int c; int len;
  av[0] = "hog"; av[1] = 0;
  id = spawn(HOG, 1, av);
  sprintf(msg, "spawn hog = %d", id);
  check(id >= 1 && id <= MAX_JOBS);
  sleep(100);                          // the hog is now blocked in getchar()
  printf("ready\n");
  len = 0;
  c = getchar();
  while (c != '\r' && c != '\n') { if (len < 63) { line[len] = c; len = len + 1; } c = getchar(); }
  line[len] = 0;
  sprintf(msg, "foreground read \"%s\"", line);
  check(!strcmp(line, "hello jobs"));
  r = kill(id);
  sprintf(msg, "kill hog = %d", r);
  check(r == 0);
  expect_done(id, JOB_KILLED);
  empty();
  return 0;
}

int case_redirect() {
  int id; int fd; int old; int n;
  id = tick("3", "100");
  fd = open("/tmp/r.txt", OPEN_WRITE);
  old = redirect(fd);
  printf("fg");
  sleep(500);                          // the job ticks meanwhile, to the console
  redirect(old);
  close(fd);
  fd = open("/tmp/r.txt", OPEN_READ);
  n = read(fd, line, 63);
  close(fd);
  if (n < 0) n = 0;
  line[n] = 0;
  sprintf(msg, "/tmp/r.txt holds \"%s\" (%d bytes)", line, n);
  check(n == 2 && !strcmp(line, "fg"));
  expect_done(id, 7);
  empty();
  return 0;
}

// run in a copy of the tests/toolchain globals case: the board's cc builds globals.bin
// (cc execs as twice and ld) while a job runs, then a nested program sleeps
int case_exec() {
  int id; int r;
  id = tick("0", "300");
  sleep(400);
  av[0] = "cc"; av[1] = "-o"; av[2] = "globals.bin"; av[3] = "globals_a.c"; av[4] = "globals_b.c"; av[5] = 0;
  r = exec("/bin/cc.bin", 5, av);
  sprintf(msg, "cc = %d", r);
  check(r == 0);
  av[0] = "globals"; av[1] = 0;
  r = exec("globals.bin", 1, av);
  printf("exit %d\n", r);
  av[0] = "tick"; av[1] = "2"; av[2] = "400"; av[3] = 0;
  r = exec(TICK, 3, av);               // a nested program that sleeps: the job runs while it waits
  sprintf(msg, "nested tick = %d", r);
  check(r == 7);
  r = kill(id);
  sprintf(msg, "kill tick = %d", r);
  check(r == 0);
  expect_done(id, JOB_KILLED);
  empty();
  return 0;
}

// a listener belongs to its task: while job A listens, the foreground's and job B's
// listen() return -1 (B prints "listen -1"); killing A frees it (no listener is left)
int case_listen() {
  int a; int b; int r; int n; int i; int good;
  av[0] = "listener"; av[1] = "7001"; av[2] = 0;
  a = spawn("/src/jobs/listener.bin", 2, av);
  sprintf(msg, "spawn listener 7001 = %d", a);
  check(a >= 1 && a <= MAX_JOBS);
  sleep(300);                          // A prints "listen 0"
  r = listen(7002);
  sprintf(msg, "foreground listen while job %d listens = %d (want -1)", a, r);
  check(r == -1);
  av[1] = "7003";
  b = spawn("/src/jobs/listener.bin", 2, av);
  sprintf(msg, "spawn listener 7003 = %d", b);
  check(b >= 1 && b <= MAX_JOBS);
  sleep(300);                          // B prints "listen -1"
  r = kill(a);
  sprintf(msg, "kill %d = %d", a, r);
  check(r == 0);
  r = kill(b);
  sprintf(msg, "kill %d = %d", b, r);
  check(r == 0);
  n = jobs(jb, 8);
  good = n == 2;
  i = 0;
  while (i < n) { if (jb[i].state != JOB_DONE || jb[i].exit_code != JOB_KILLED) good = 0; i = i + 1; }
  sprintf(msg, "jobs: %d done, killed", n);
  check(good);
  empty();
  return 0;
}

// the same as spin() in spin.c: integer and float work with no syscalls inside the loop
int spin(int n) {
  int i; int a; int b; int c; float x; float y; float z;
  a = 1; b = 7; c = 0; x = 1.0; y = 0.5; z = 0.0;
  i = 0;
  while (n == 0 || i < n) {
    a = a * 1103515245 + 12345;
    b = b ^ (a >> 7);
    c = b & 255;
    z = c;
    x = x * 1.0001 + y;
    if (x > 1000.0) x = x - 999.0;
    y = y * 0.999 + z * 0.001;
    i = i + 1;
  }
  return a ^ b ^ (int)(x * 1000.0) ^ (int)(y * 1000.0);
}

// two spin jobs and the foreground compute at once: a foreground sleep(100) returns on
// time, every result equals the one computed alone, the tick preempted
int case_preempt() {
  int want; int t0; int t1; int dt; int got; int p0; int n; int i; int good; int ids[2];
  char nbuf[16]; char wbuf[16];
  p0 = sysinfo(INFO_PREEMPTS);
  t0 = millis();
  want = spin(SPIN_N);
  t1 = millis() - t0;
  sprintf(msg, "spin %d alone: %d ms", SPIN_N, t1);
  check(t1 >= 300);
  sprintf(nbuf, "%d", SPIN_N); sprintf(wbuf, "%d", want);
  av[0] = "spin"; av[1] = nbuf; av[2] = wbuf; av[3] = 0;
  ids[0] = spawn(SPIN, 3, av);
  ids[1] = spawn(SPIN, 3, av);
  sprintf(msg, "spawn spin twice = %d %d", ids[0], ids[1]);
  check(ids[0] >= 1 && ids[1] >= 1);
  t0 = millis();
  sleep(100);
  dt = millis() - t0;
  sprintf(msg, "sleep(100) next to two spinning jobs took %d ms (want < 300)", dt);
  check(dt < 300);
  got = spin(SPIN_N);
  sprintf(msg, "foreground spin while preempted = %d (want %d)", got, want);
  check(got == want);
  n = 0; i = 0;
  while (n < 2 && i < 600) { n = n + jobs(jb + n, 8 - n); if (n < 2) sleep(50); i = i + 1; }
  good = n == 2;
  i = 0;
  while (i < n) { if (jb[i].state != JOB_DONE || jb[i].exit_code != 1) good = 0; i = i + 1; }
  sprintf(msg, "both jobs done with the right result: %d done, exits %d %d (want 1 1)", n, jb[0].exit_code, jb[1].exit_code);
  check(good);
  sprintf(msg, "preemptions %d", sysinfo(INFO_PREEMPTS) - p0);
  check(sysinfo(INFO_PREEMPTS) - p0 > 10);
  empty();
  return 0;
}

// a job that never makes a syscall still ends when killed
int case_spinkill() {
  int id; int r; int t0;
  av[0] = "spin"; av[1] = "0"; av[2] = 0;
  id = spawn(SPIN, 2, av);
  sprintf(msg, "spawn spin 0 = %d", id);
  check(id >= 1);
  sleep(200);
  t0 = millis();
  r = kill(id);
  sprintf(msg, "kill of the spinning job = %d in %d ms", r, millis() - t0);
  check(r == 0);
  expect_done(id, JOB_KILLED);
  empty();
  return 0;
}

int main(int argc, char **argv) {
  fails = 0;
  if (argc < 2) { printf("usage: spawner basic|errors|sleeping|getchar|redirect|exec|listen|preempt|spinkill\n"); return 2; }
  if (!strcmp(argv[1], "basic")) case_basic();
  else if (!strcmp(argv[1], "errors")) case_errors();
  else if (!strcmp(argv[1], "sleeping")) case_sleeping();
  else if (!strcmp(argv[1], "getchar")) case_getchar();
  else if (!strcmp(argv[1], "redirect")) case_redirect();
  else if (!strcmp(argv[1], "exec")) case_exec();
  else if (!strcmp(argv[1], "listen")) case_listen();
  else if (!strcmp(argv[1], "preempt")) case_preempt();
  else if (!strcmp(argv[1], "spinkill")) case_spinkill();
  else { printf("unknown case %s\n", argv[1]); return 2; }
  if (fails) printf("FAIL\n"); else printf("PASS\n");
  return 0;
}
