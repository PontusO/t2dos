// telnetd : show the status.  telnetd on [port] : ask for a password, then let one telnet
// session at a time share the console with USB (port 23 by default); the board keeps the
// setting and starts it again at every boot.  telnetd off : stop it and forget the setting.
#include "sys.h"
int usage() { printf("usage: telnetd [on [port] | off]\n"); return 1; }
// reads a line without echo into b (size 34); 0, or -1 for Ctrl-C or a password too long
int getpw(char *msg, char *b) {
  int n; int c;
  printf("%s", msg);
  n = 0;
  for (;;) {
    c = getchar();
    if (c == 3) { printf("\n"); return -1; }
    if (c == '\r' || c == '\n') break;
    if (c == 8 || c == 127) { if (n > 0) n--; }
    else if (c >= 32 && n < 33) { b[n] = c; n++; }   // as at login: other control keys do nothing
  }
  b[n] = 0;
  printf("\n");
  if (n > 32) { printf("telnetd: at most 32 characters\n"); return -1; }
  return 0;
}
int main(int argc, char **argv) {
  int port; int st; char pw[34]; char pw2[34]; char ip[48]; int r;
  if (argc == 1) {
    st = telnetd(-1, ip);
    if (!st) { printf("telnetd is off\n"); return 0; }
    printf("telnetd on port %d", st & 65535);
    if (!(st & TELNETD_LISTENING)) printf(", waiting for the network");
    if (st & TELNETD_LIVE) printf(", %s logged in", ip);
    printf("\n");
    return 0;
  }
  if (!strcmp(argv[1], "off") && argc == 2) {
    if (telnetd(0, 0) < 0) { printf("telnetd: cannot remove /etc/telnetd\n"); return 1; }
    printf("telnetd is off\n");
    return 0;
  }
  if (strcmp(argv[1], "on") || argc > 3) return usage();
  port = 23;
  if (argc == 3) port = atoi(argv[2]);
  if (port < 1 || port > 65535) { printf("telnetd: bad port %s\n", argv[2]); return 1; }
  if (getpw("password: ", pw) < 0) return 1;
  if (!pw[0]) { printf("telnetd: the password is empty\n"); return 1; }
  if (getpw("again: ", pw2) < 0) return 1;
  if (strcmp(pw, pw2)) { printf("telnetd: the passwords differ\n"); return 1; }
  r = telnetd(port, pw);
  memset(pw, 0, 34); memset(pw2, 0, 34);
  if (r == TELNETD_NOSAVE) { printf("telnetd: cannot save the setting (no key store on this board?)\n"); return 1; }
  if (r < 0) { printf("telnetd: bad port or password\n"); return 1; }
  printf("telnetd on port %d\n", port);
  return 0;
}
