// pecho.c - named pipe test server (tools/check_jobs.py): pecho NAME
// Creates /pipe/NAME (requests) and /pipe/NAME.out (replies). Each message read from
// /pipe/NAME is written back uppercased as one message on /pipe/NAME.out. The message
// "quit" (a trailing newline allowed) is answered "BYE"; pecho then waits until that
// reply has been read (its pipes go away when it exits, unread data with them) and
// returns 3.
#include "sys.h"

char inpath[48];
char outpath[48];
char outname[40];
char msg[4096];
struct stat_t st;

int reply(char *buf, int len) {
  int fd; int w;
  fd = open(outpath, OPEN_WRITE);
  if (fd < 0) return -1;
  w = 0;
  if (len > 0) w = write(fd, buf, len);
  close(fd);
  return w;
}

int is_quit(int len) {
  if (len != 4 && len != 5) return 0;
  if (strncmp(msg, "quit", 4)) return 0;
  if (len == 5 && msg[4] != '\n') return 0;
  return 1;
}

int main(int argc, char **argv) {
  int fd; int len; int r; int i;
  if (argc < 2) { printf("usage: pecho NAME\n"); return 2; }
  if (strlen(argv[1]) > 27) { printf("pecho: name too long\n"); return 2; }
  sprintf(outname, "%s.out", argv[1]);
  if (mkpipe(argv[1])) { printf("pecho: mkpipe %s failed\n", argv[1]); return 1; }
  if (mkpipe(outname)) { printf("pecho: mkpipe %s failed\n", outname); return 1; }
  sprintf(inpath, "/pipe/%s", argv[1]);
  sprintf(outpath, "/pipe/%s", outname);
  fd = open(inpath, OPEN_READ);
  if (fd < 0) { printf("pecho: open %s failed\n", inpath); return 1; }
  while (1) {
    len = 0;
    r = read(fd, msg, 4095);
    while (r > 0) { len = len + r; r = read(fd, msg + len, 4095 - len); }
    if (is_quit(len)) {
      reply("BYE", 3);
      while (stat(outpath, &st) == 0 && st.size > 0) sleep(10);
      return 3;
    }
    i = 0;
    while (i < len) { if (msg[i] >= 'a' && msg[i] <= 'z') msg[i] = msg[i] - 32; i = i + 1; }
    reply(msg, len);
  }
  return 0;
}
