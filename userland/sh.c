// sh.c - T2DOS shell. Builtins: cd pwd exit help. Everything else is /bin/name.bin or ./name.bin.
// Output redirection with "> file" and ">> file". "sh script" runs a script.
// Up and Down scroll through the last 16 command lines.
#include "sys.h"

char line[256];
char path[160];
char cwd[128];

// command history: the last HIST lines, hist[(hfirst + i) % HIST] for i < hcount, oldest first
#define HIST 16
char hist[HIST][256];
int hfirst; int hcount;
char draft[256];                       // what was typed before Up was first pressed

int hist_add(char *s) {
  if (!*s) return 0;
  if (hcount && !strcmp(hist[(hfirst + hcount - 1) % HIST], s)) return 0;   // same as the last one
  if (hcount == HIST) { hfirst = (hfirst + 1) % HIST; hcount--; }
  strcpy(hist[(hfirst + hcount) % HIST], s);
  hcount++;
  return 1;
}
// the next key; an escape sequence comes back as 'A' + 256 (Up), 'B' + 256 (Down) or 0 (others)
int waitkey(int ms) { int t; t = millis(); while (!kbhit()) { if (millis() - t > ms) return -1; } return getchar(); }
int getkey() {
  int c;
  c = getchar();
  if (c != 27) return c;
  c = waitkey(50);
  if (c != '[' && c != 'O') return 0;
  c = waitkey(50);
  while (c >= 0 && (c < 64 || c > 126)) c = waitkey(50);    // parameters, up to the final byte
  if (c == 'A' || c == 'B') return c + 256;
  return 0;
}
// replace the len characters on screen and in buf with s
int retype(char *buf, int len, char *s) {
  while (len) { printf("\b \b"); len--; }
  strcpy(buf, s); printf("%s", buf);
  return strlen(buf);
}

int readline(char *buf, int max) {
  int len; int c; int h;
  len = 0; h = hcount;                 // h: the history line shown, hcount = the line being typed
  while (1) {
    c = getkey();
    if (c == '\r' || c == '\n') { buf[len] = 0; putchar('\n'); hist_add(buf); return len; }
    if (c == 8 || c == 127) { if (len) { len--; printf("\b \b"); } }
    else if (c == 3) { buf[0] = 0; putchar('\n'); return 0; }
    else if (c == 'A' + 256) {         // Up: an older line
      if (h > 0) {
        if (h == hcount) { buf[len] = 0; strcpy(draft, buf); }
        h--; len = retype(buf, len, hist[(hfirst + h) % HIST]);
      }
    }
    else if (c == 'B' + 256) {         // Down: a newer line, then back to what was typed
      if (h < hcount) {
        h++;
        if (h == hcount) len = retype(buf, len, draft);
        else len = retype(buf, len, hist[(hfirst + h) % HIST]);
      }
    }
    else if (c >= 32 && c < 256 && len < max - 1) { buf[len++] = c; putchar(c); }
  }
}

int find_prog(char *name) {
  struct stat_t st;
  if (strchr(name, '.')) sprintf(path, "%s", name); else sprintf(path, "%s.bin", name);
  if (stat(path, &st) == 0 && !st.isdir) return 1;
  if (strchr(name, '.')) sprintf(path, "/bin/%s", name); else sprintf(path, "/bin/%s.bin", name);
  if (stat(path, &st) == 0 && !st.isdir) return 1;
  return 0;
}

int execute(char *cmd) {
  char *argv[16]; int argc; char *p; char *redir; int append; int fd; int old; int r; int t0;
  argc = 0; p = cmd; redir = 0; append = 0;
  while (*p) {
    while (*p == ' ') p++;
    if (!*p) break;
    if (*p == '>') { p++; if (*p == '>') { append = 1; p++; } while (*p == ' ') p++; redir = p; while (*p && *p != ' ') p++; if (*p) *p++ = 0; continue; }
    if (argc < 15) argv[argc++] = p;
    while (*p && *p != ' ') p++;
    if (*p) *p++ = 0;
  }
  if (!argc) return 0;
  argv[argc] = 0;
  if (!strcmp(argv[0], "cd")) { if (chdir(argc > 1 ? argv[1] : "/") < 0) printf("cd: no such directory\n"); return 0; }
  if (!strcmp(argv[0], "pwd")) { printf("%s\n", getcwd(cwd, 128)); return 0; }
  if (!strcmp(argv[0], "exit")) exit(argc > 1 ? atoi(argv[1]) : 0);
  if (!strcmp(argv[0], "help")) { printf("builtins: cd pwd exit help.  programs: ls /bin.  cmd args > file redirects output.\n"); return 0; }
  if (!find_prog(argv[0])) { printf("%s: not found\n", argv[0]); return -1; }
  fd = 0;
  if (redir) { fd = open(redir, append ? OPEN_APPEND : OPEN_WRITE); if (fd < 0) { printf("cannot write %s\n", redir); return -1; } old = redirect(fd); }
  t0 = millis();
  r = exec(path, argc, argv);
  if (redir) { redirect(old); close(fd); }
  if (r < -1) printf("%s: not a valid program (%d)\n", argv[0], r);
  else if (r) printf("[exit %d, %d ms]\n", r, millis() - t0);
  return r;
}

int run_script(char *name) {
  int fd; char *buf; int n; char *p; char *q;
  fd = open(name, OPEN_READ);
  if (fd < 0) { printf("sh: cannot open %s\n", name); return 1; }
  buf = malloc(65536); n = read(fd, buf, 65535); close(fd);
  if (n < 0) n = 0;
  buf[n] = 0; p = buf;
  while (*p) {
    q = p; while (*q && *q != '\n') q++;
    if (*q) *q++ = 0;
    if (*p && *p != '#') execute(p);
    p = q;
  }
  free(buf);
  return 0;
}

int main(int argc, char **argv) {
  struct stat_t st;
  if (argc > 1) return run_script(argv[1]);
  if (stat("/autoexec.sh", &st) == 0) run_script("/autoexec.sh");
  printf("T2DOS shell, type help\n");
  while (1) {
    printf("%s> ", getcwd(cwd, 128));
    if (readline(line, 256) > 0) execute(line);
  }
  return 0;
}
