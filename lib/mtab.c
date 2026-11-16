// mtab.c - /etc/mount for mount and umount: one "name host[:port] dir" line per mount (dir is
// the rest of the line, so it may hold spaces).  The old format, "host[:port]" then "dir" (one
// mount at /mnt/fs), is read as name fs.  The kernel reads the file at boot.  Include it after
// sys.h.
char mt_buf[2048];
char mt_out[2048];
char mt_a[300];
char mt_b[300];

// reads /etc/mount into mt_buf, in the new format; its length (0 when there is none)
int mt_load() {
  int fd; int n; char *nl; char *sp; char *e;
  mt_buf[0] = 0;
  fd = open("/etc/mount", OPEN_READ);
  if (fd < 0) return 0;
  n = read(fd, mt_buf, 2047); close(fd);
  if (n < 0) n = 0;
  mt_buf[n] = 0;
  nl = strchr(mt_buf, '\n');
  sp = strchr(mt_buf, ' ');
  if (nl && (sp == 0 || sp > nl)) {           // the old format
    *nl = 0;
    e = strchr(nl + 1, '\n'); if (e) *e = 0;
    if (strlen(mt_buf) > 250 || strlen(nl + 1) > 250) { mt_buf[0] = 0; return 0; }
    mt_copy(mt_a, mt_buf, '\n', 300); mt_copy(mt_b, nl + 1, '\n', 300);
    if (mt_b[0] == 0) strcpy(mt_b, "/");
    sprintf(mt_buf, "fs %s %s\n", mt_a, mt_b);
  }
  return strlen(mt_buf);
}

// copies at most max - 1 bytes of s, up to stop (or the end of s), into out; the bytes used
int mt_copy(char *out, char *s, int stop, int max) {
  int n; int k;
  n = 0; k = 0;
  while (s[n] && s[n] != stop && s[n] != '\n') {
    if (k < max - 1 && s[n] != '\r') { out[k] = s[n]; k = k + 1; }   // a CR from a file edited on Windows
    n = n + 1;
  }
  out[k] = 0;
  return n;
}

// the line of mt_buf at offset at, split into name, host and dir; the offset of the next line,
// or -1 at the end
int mt_get(int at, char *name, char *host, char *dir) {
  char *p;
  if (at < 0 || mt_buf[at] == 0) return -1;
  p = mt_buf + at;
  p = p + mt_copy(name, p, ' ', 16); if (*p == ' ') p = p + 1;
  p = p + mt_copy(host, p, ' ', 64); if (*p == ' ') p = p + 1;
  p = p + mt_copy(dir, p, '\n', 200);
  if (*p == '\n') p = p + 1;
  return p - mt_buf;
}

// mt_buf without name's line; line (a whole "name host dir\n") takes its place, or goes at the
// end when name had none, unless line is 0.  Writes /etc/mount (or removes it when it ends up
// empty).  1 if name had a line, 0 if not, -1 if the file cannot be written.
int mt_put(char *name, char *line) {
  int at; int next; int found; int fd; int n;
  char nm[16]; char host[64]; char dir[200];
  found = 0; at = 0; mt_out[0] = 0;
  while ((next = mt_get(at, nm, host, dir)) >= 0) {
    if (strcmp(nm, name) == 0) {
      if (line && !found) {
        if (strlen(mt_out) + strlen(line) >= 2047) return -1;
        strcpy(mt_out + strlen(mt_out), line);
      }
      found = 1;
    } else if (strlen(mt_out) + next - at < 2047) {
      n = strlen(mt_out);
      memcpy(mt_out + n, mt_buf + at, next - at); mt_out[n + next - at] = 0;
      if (mt_out[strlen(mt_out) - 1] != '\n') strcpy(mt_out + strlen(mt_out), "\n");
    }
    at = next;
  }
  if (line && !found) {
    if (strlen(mt_out) + strlen(line) >= 2047) return -1;   // no room: /etc/mount is left as it was
    strcpy(mt_out + strlen(mt_out), line);
  }
  strcpy(mt_buf, mt_out);
  if (mt_out[0] == 0) { unlink("/etc/mount"); return found; }
  mkdir("/etc");
  fd = open("/etc/mount", OPEN_WRITE);
  if (fd < 0) return -1;
  n = strlen(mt_out);
  if (write(fd, mt_out, n) != n) { close(fd); return -1; }
  if (close(fd) < 0) return -1;
  return found;
}

// name without a leading /mnt/
char *mt_name(char *s) {
  if (strncmp(s, "/mnt/", 5) == 0) return s + 5;
  return s;
}
