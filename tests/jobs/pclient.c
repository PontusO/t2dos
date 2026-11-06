// pclient.c - named pipe test driver (tools/check_jobs.py). pclient CASE [NAME [TEXT]]
// prints one line per check ("ok ..." or "FAIL ...") and then PASS or FAIL. Cases:
//   start NAME       spawn pecho NAME as a job and wait until its pipes exist
//   finish NAME      send "quit", expect "BYE"; pecho ends with exit 3 (reported by jobs)
//   send NAME TEXT   write TEXT to /pipe/NAME as one message, print the reply message
//   writer NAME TEXT (run as a job by early) sleep 300 ms, then send TEXT, no reply read
//   early NAME       spawn `pclient writer NAME abc`, read /pipe/NAME.out at once: it
//                    blocks until pecho has replied, prints the reply
//   large NAME       a 2000-byte message round trips intact
//   orphan NAME      read /pipe/NAME.out while pecho quits: BYE, end of message, then 0
//                    once pecho is done; its pipes are gone
//   killer NAME      spawn pecho NAME, kill it while it waits on its empty pipe
//   mkpipe           mkpipe errors, single-task pipe I/O, stat and unlink on a pipe
//   nopipe NAME      /pipe/NAME does not exist (a foreground program's pipes end with it)
#include "sys.h"

#define PECHO "/src/jobs/pecho.bin"
#define PCLIENT "/src/jobs/pclient.bin"

struct job_t jb[8];
struct stat_t st;
char *av[8];
char path[48];
char outpath[48];
char buf[4096];
char big[2048];
char msg[160];
int fails;

int check(int cond) {
  if (cond) printf("ok %s\n", msg); else { printf("FAIL %s\n", msg); fails = fails + 1; }
  return cond;
}

int paths(char *name) {
  sprintf(path, "/pipe/%s", name);
  sprintf(outpath, "/pipe/%s.out", name);
  return 0;
}

// write len bytes of s to /pipe/NAME as one message
int send(char *s, int len) {
  int fd; int w; int c;
  fd = open(path, OPEN_WRITE);
  sprintf(msg, "open %s for write = %d", path, fd);
  if (!check(fd >= 0)) return -1;
  w = write(fd, s, len);
  sprintf(msg, "write %d bytes = %d", len, w);
  check(w == len);
  c = close(fd);
  sprintf(msg, "close = %d", c);
  check(c == 0);
  return w;
}

// read fd to the end of one message into buf (NUL-terminated), return its length
int readmsg(int fd) {
  int len; int r;
  len = 0;
  r = read(fd, buf, 4095);
  while (r > 0) { len = len + r; r = read(fd, buf + len, 4095 - len); }
  buf[len] = 0;
  return len;
}

// read one reply message from /pipe/NAME.out
int getreply() {
  int fd; int len;
  fd = open(outpath, OPEN_READ);
  sprintf(msg, "open %s for read = %d", outpath, fd);
  if (!check(fd >= 0)) return -1;
  len = readmsg(fd);
  close(fd);
  return len;
}

// wait (up to 3 s) until jobs() reports a done job named name; its exit code, or -100
int wait_done(char *name) {
  int t; int n; int i;
  t = 0;
  while (t < 300) {
    n = jobs(jb, 8);
    i = 0;
    while (i < n) {
      if (jb[i].state == JOB_DONE && !strcmp(jb[i].name, name)) return jb[i].exit_code;
      i = i + 1;
    }
    sleep(10);
    t = t + 1;
  }
  return -100;
}

// spawn pecho name and wait until both its pipes exist; the job id
int start(char *name) {
  int id; int t;
  av[0] = "pecho"; av[1] = name; av[2] = 0;
  id = spawn(PECHO, 2, av);
  sprintf(msg, "spawn pecho %s = %d", name, id);
  if (!check(id >= 1 && id <= MAX_JOBS)) return id;
  t = 0;
  while (t < 200 && stat(outpath, &st) != 0) { sleep(10); t = t + 1; }
  sprintf(msg, "%s and %s exist", path, outpath);
  check(stat(path, &st) == 0 && stat(outpath, &st) == 0);
  return id;
}

int case_finish() {
  int len; int code;
  send("quit", 4);
  len = getreply();
  sprintf(msg, "reply to quit \"%s\" (%d bytes)", buf, len);
  check(len == 3 && !strcmp(buf, "BYE"));
  code = wait_done("pecho");
  sprintf(msg, "pecho done, exit %d (want 3)", code);
  check(code == 3);
  return 0;
}

int case_send(char *text) {
  int len;
  send(text, strlen(text));
  len = getreply();
  printf("%s\n", buf);
  sprintf(msg, "reply %d bytes", len);
  check(len == strlen(text));
  return 0;
}

int case_writer(char *text) {
  sleep(300);
  send(text, strlen(text));
  return 0;
}

int case_early(char *name) {
  int id; int t0; int len; int dt; int code;
  av[0] = "pclient"; av[1] = "writer"; av[2] = name; av[3] = "abc"; av[4] = 0;
  t0 = millis();
  id = spawn(PCLIENT, 4, av);
  sprintf(msg, "spawn pclient writer = %d", id);
  check(id >= 1 && id <= MAX_JOBS);
  len = getreply();                    // the request is not even written yet
  dt = millis() - t0;
  printf("%s\n", buf);
  sprintf(msg, "reply %d bytes after %d ms (want 3, at least 250)", len, dt);
  check(len == 3 && dt >= 250);
  code = wait_done("pclient");
  sprintf(msg, "writer done, exit %d (want 0)", code);
  check(code == 0);
  return 0;
}

int case_large() {
  int i; int len; int good;
  i = 0;
  while (i < 2000) { big[i] = 'a' + i % 26; i = i + 1; }
  send(big, 2000);                     // blocks on the full pipe while pecho drains it
  len = getreply();
  good = len == 2000;
  i = 0;
  while (good && i < len) { if (buf[i] != 'A' + i % 26) good = 0; i = i + 1; }
  sprintf(msg, "2000-byte message came back as %d bytes, uppercased: %d", len, good);
  check(good);
  return 0;
}

int case_orphan() {
  int rfd; int wfd; int len; int r; int n; int i; int code;
  rfd = open(outpath, OPEN_READ);
  sprintf(msg, "open %s for read = %d", outpath, rfd);
  check(rfd >= 0);
  wfd = open(path, OPEN_WRITE);        // a second writer, still open when pecho ends
  sprintf(msg, "open %s for write = %d", path, wfd);
  check(wfd >= 0);
  send("quit", 4);
  len = readmsg(rfd);
  sprintf(msg, "reply \"%s\" (%d bytes), then end of message", buf, len);
  check(len == 3 && !strcmp(buf, "BYE"));
  r = read(rfd, buf, 100);             // blocks until pecho has ended and removed its pipes
  sprintf(msg, "read after the owner ended = %d", r);
  check(r == 0);
  n = jobs(jb, 8);
  code = -100;
  i = 0;
  while (i < n) { if (jb[i].state == JOB_DONE && !strcmp(jb[i].name, "pecho")) code = jb[i].exit_code; i = i + 1; }
  sprintf(msg, "pecho already done when the read returned, exit %d (want 3)", code);
  check(code == 3);
  r = read(rfd, buf, 100);
  sprintf(msg, "read again = %d", r);
  check(r == 0);
  r = write(wfd, "x", 1);
  sprintf(msg, "write on the removed pipe = %d", r);
  check(r == -1);
  r = close(wfd);
  sprintf(msg, "close the dead writer = %d", r);
  check(r == 0);
  close(rfd);
  r = open(path, OPEN_WRITE);
  sprintf(msg, "open %s after the owner ended = %d", path, r);
  check(r == -1);
  r = stat(outpath, &st);
  sprintf(msg, "stat %s = %d", outpath, r);
  check(r == -1);
  return 0;
}

int case_killer(char *name) {
  int id; int r; int n;
  id = start(name);
  sleep(100);                          // pecho is blocked reading its empty pipe
  r = kill(id);
  sprintf(msg, "kill pecho %d = %d", id, r);
  check(r == 0);
  n = jobs(jb, 8);
  sprintf(msg, "jobs: %d entries, state %d exit %d", n, jb[0].state, jb[0].exit_code);
  check(n == 1 && jb[0].id == id && jb[0].state == JOB_DONE && jb[0].exit_code == JOB_KILLED);
  r = stat(path, &st);
  sprintf(msg, "stat %s = %d (want -1)", path, r);
  check(r == -1);
  r = stat(outpath, &st);
  sprintf(msg, "stat %s = %d (want -1)", outpath, r);
  check(r == -1);
  return 0;
}

int case_mkpipe() {
  int r; int i; int good; int fd; int w; int len;
  r = mkpipe("m0");
  sprintf(msg, "mkpipe m0 = %d", r);
  check(r == 0);
  r = mkpipe("m0");
  sprintf(msg, "mkpipe m0 again = %d (want -1)", r);
  check(r == -1);
  good = 1;
  i = 1;
  while (i < 8) { sprintf(buf, "m%d", i); if (mkpipe(buf) != 0) good = 0; i = i + 1; }
  sprintf(msg, "mkpipe m1..m7 (eight pipes)");
  check(good);
  r = mkpipe("m8");
  sprintf(msg, "ninth pipe = %d (want -1)", r);
  check(r == -1);
  r = stat("/pipe/m7", &st);
  sprintf(msg, "stat /pipe/m7 = %d", r);
  check(r == 0);
  r = mkpipe("a/b");
  sprintf(msg, "mkpipe a/b = %d (want -1)", r);
  check(r == -1);
  r = mkpipe("");
  sprintf(msg, "mkpipe \"\" = %d (want -1)", r);
  check(r == -1);
  r = mkpipe("abcdefghijklmnopqrstuvwxyz0123456");
  sprintf(msg, "mkpipe of a 33-char name = %d (want -1)", r);
  check(r == -1);
  r = open("/pipe/nosuch", OPEN_READ);
  sprintf(msg, "open /pipe/nosuch = %d (want -1)", r);
  check(r == -1);
  r = unlink("/pipe/m0");
  sprintf(msg, "unlink /pipe/m0 = %d (want -1)", r);
  check(r == -1);
  fd = open("/pipe/m0", OPEN_WRITE);
  w = write(fd, "abcd", 4);
  r = stat("/pipe/m0", &st);
  sprintf(msg, "write fd %d = %d, stat = %d size %d isdir %d", fd, w, r, st.size, st.isdir);
  check(fd >= 200 && w == 4 && r == 0 && st.size == 4 && st.isdir == 0);
  close(fd);
  fd = open("/pipe/m0", OPEN_READ);
  len = readmsg(fd);
  sprintf(msg, "read back \"%s\" (%d bytes)", buf, len);
  check(len == 4 && !strcmp(buf, "abcd"));
  close(fd);
  return 0;
}

int case_nopipe() {
  int r; int i; int good;
  r = stat(path, &st);
  sprintf(msg, "stat %s = %d (want -1)", path, r);
  check(r == -1);
  good = 1;
  i = 0;
  while (i < 8) { sprintf(buf, "n%d", i); if (mkpipe(buf) != 0) good = 0; i = i + 1; }
  sprintf(msg, "eight new pipes (the table is free again)");
  check(good);
  return 0;
}

int main(int argc, char **argv) {
  char *c;
  fails = 0;
  if (argc < 2) { printf("usage: pclient start|finish|send|writer|early|large|orphan|killer|mkpipe|nopipe [NAME [TEXT]]\n"); return 2; }
  c = argv[1];
  if (argc > 2) paths(argv[2]); else paths("p1");
  if (!strcmp(c, "start")) start(argv[2]);
  else if (!strcmp(c, "finish")) case_finish();
  else if (!strcmp(c, "send") && argc > 3) case_send(argv[3]);
  else if (!strcmp(c, "writer") && argc > 3) { case_writer(argv[3]); return 0; }
  else if (!strcmp(c, "early")) case_early(argv[2]);
  else if (!strcmp(c, "large")) case_large();
  else if (!strcmp(c, "orphan")) case_orphan();
  else if (!strcmp(c, "killer")) case_killer(argv[2]);
  else if (!strcmp(c, "mkpipe")) case_mkpipe();
  else if (!strcmp(c, "nopipe")) case_nopipe();
  else { printf("unknown case %s\n", c); return 2; }
  if (fails) printf("FAIL\n"); else printf("PASS\n");
  return 0;
}
