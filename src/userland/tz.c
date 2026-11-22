// tz : show, set (POSIX string or +N/-N shorthand) and clear the time zone.
// This is the tool, not the engine: the POSIX TZ parser and DST rules live in
// the kernel (src/kernel/tz.c); tz only talks to it through the tzset syscall
// and reads/removes /etc/tz. Do not confuse the two tz.c files.
#include "sys.h"
char file[128];  // contents of /etc/tz with trailing CR/LF stripped; empty when the file is absent
char want[16];   // the POSIX string the +N/-N shorthand maps to

int is_alpha(int c) { return c >= 'A' && c <= 'Z' || c >= 'a' && c <= 'z'; }

// read /etc/tz into file; 1 when the file is there, 0 when it is not
int read_file() {
  int fd; int n;
  fd = open("/etc/tz", OPEN_READ);
  if (fd < 0) { file[0] = 0; return 0; }
  n = read(fd, file, 127);
  close(fd);
  if (n < 0) n = 0;
  file[n] = 0;
  while (n > 0 && (file[n - 1] == '\r' || file[n - 1] == '\n')) { file[n - 1] = 0; n = n - 1; }
  return 1;
}

// the rule names a DST side when a second alpha token appears before the comma
int dst_named(char *rule) {
  int i; int names;
  i = 0; names = 0;
  while (rule[i] && rule[i] != ',') {
    if (is_alpha(rule[i])) {
      names = names + 1;
      while (is_alpha(rule[i])) i = i + 1;
    } else i = i + 1;
  }
  return names > 1;
}

// the std offset of rule in seconds east of UTC: the signed number after the std
// name (-H, +H, -H:MM, +H:MM, unsigned counts as +), negated because POSIX
// counts west-positive. Only called when dst_named(rule), so name and number exist.
int std_seconds(char *rule) {
  int i; int sign; int h; int m;
  i = 0;
  while (rule[i] && !is_alpha(rule[i])) i = i + 1;
  while (is_alpha(rule[i])) i = i + 1;
  sign = 1;
  if (rule[i] == '-') { sign = -1; i = i + 1; }
  if (rule[i] == '+') i = i + 1;
  h = 0;
  while (rule[i] >= '0' && rule[i] <= '9') { h = h * 10 + rule[i] - '0'; i = i + 1; }
  m = 0;
  if (rule[i] == ':') {
    i = i + 1;
    while (rule[i] >= '0' && rule[i] <= '9') { m = m * 10 + rule[i] - '0'; i = i + 1; }
  }
  return -sign * (h * 3600 + m * 60);
}

// one state line: rule <name> offset +hh:mm, plus std/dst when the rule text names
// a DST side (live offset equals the parsed std offset -> std), plus an optional note
int print_state(char *name, char *rule, int off, char *note) {
  int a;
  a = off < 0 ? -off : off;
  printf("rule %s offset %c%02d:%02d", name, off < 0 ? '-' : '+', a / 3600, a / 60 % 60);
  if (rule && dst_named(rule)) {
    if (off == std_seconds(rule)) printf(" std"); else printf(" dst");
  }
  if (note) printf(" %s", note);
  printf("\n");
  return 0;
}

// apply the POSIX rule s; on success print the new state (re-queried, not assumed)
// and say so when the kernel did not persist it (the host harness never does)
int set_zone(char *s) {
  int off;
  if (tzset(s) == -1) { printf("tz: %s: rejected, previous rule kept\n", s); return 1; }   // -1 exactly: a west-side zone returns a legal negative offset (GMT+3 -> -10800)
  off = tzset(0);
  read_file();
  if (file[0] && !strcmp(file, s)) return print_state(s, s, off, 0);
  return print_state(s, s, off, "live only, not saved");
}

int main(int argc, char **argv) {
  int found; int off; int i; int h;
  if (argc < 2) {
    found = read_file();
    off = tzset(0);   // queries never return -1
    if (found) return print_state(file, file, off, 0);
    if (off) return print_state("UTC", 0, off, "live only, not saved");
    return print_state("UTC", 0, off, 0);
  }
  if (!strcmp(argv[1], "UTC")) { tzset("UTC0"); unlink("/etc/tz"); return print_state("UTC", 0, tzset(0), 0); }
  if ((argv[1][0] == '+' || argv[1][0] == '-') && argv[1][1] >= '0' && argv[1][1] <= '9') {
    i = 1;
    while (argv[1][i] >= '0' && argv[1][i] <= '9') i = i + 1;
    if (!argv[1][i]) {
      h = 0;
      i = 1;
      while (argv[1][i] >= '0' && argv[1][i] <= '9') { h = h * 10 + argv[1][i] - '0'; i = i + 1; }
      if (argv[1][0] == '-') h = -h;
      // POSIX TZ counts west-positive while the shorthand means "add hours" east,
      // so the sign inverts here: tz +2 applies UTC-2, tz -3 applies UTC+3.
      sprintf(want, "UTC%d", -h);
      return set_zone(want);
    }
  }
  return set_zone(argv[1]);
}
