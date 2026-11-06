// spawner.c - foreground driver for the background job tests (tools/check_jobs.py).
// spawner CASE runs one case, prints one line per check ("ok ..." or "FAIL ...")
// and then PASS or FAIL. Cases: basic errors sleeping getchar redirect exec.
// Every case leaves the job table empty (all jobs ended and reported by jobs()).
#include "sys.h"

#define TICK "/src/jobs/tick.bin"
#define HOG "/src/jobs/hog.bin"

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

int main(int argc, char **argv) {
  fails = 0;
  if (argc < 2) { printf("usage: spawner basic|errors|sleeping|getchar|redirect|exec\n"); return 2; }
  if (!strcmp(argv[1], "basic")) case_basic();
  else if (!strcmp(argv[1], "errors")) case_errors();
  else if (!strcmp(argv[1], "sleeping")) case_sleeping();
  else if (!strcmp(argv[1], "getchar")) case_getchar();
  else if (!strcmp(argv[1], "redirect")) case_redirect();
  else if (!strcmp(argv[1], "exec")) case_exec();
  else { printf("unknown case %s\n", argv[1]); return 2; }
  if (fails) printf("FAIL\n"); else printf("PASS\n");
  return 0;
}
