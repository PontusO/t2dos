// t2obj.c - read and write T2DOS object files, #include "t2obj.c" (found in /lib on the board)
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "t2obj.h"

// the tables point into the buffer given to t2o_read, which must stay alive
struct t2o { struct t2obj_hdr h; char *text; char *data; struct t2sym *syms; struct t2rel *rels; char *str; };

// 0 ok, -1 bad magic, truncated or inconsistent (the checks keep every table and name inside buf)
int t2o_read(char *buf, int size, struct t2o *o) {
  int need; int p; int i; struct t2sym *s; struct t2rel *r;
  if (size < 0 || size < sizeof(struct t2obj_hdr)) return -1;
  memcpy((char *)o, buf, sizeof(struct t2obj_hdr));   // h is the first member (compiler/cc mis-compiles &o->h)
  if (o->h.magic != T2OBJ_MAGIC || o->h.version != T2OBJ_VERSION) return -1;
  if (o->h.text_size < 0 || o->h.data_size < 0 || o->h.nsyms < 0 || o->h.nrels < 0 || o->h.str_size < 0) return -1;
  // bound each count by the file size before multiplying, and the running total after each addition
  if (o->h.text_size > size || o->h.data_size > size || o->h.str_size > size) return -1;
  if (o->h.nsyms > size / sizeof(struct t2sym) || o->h.nrels > size / sizeof(struct t2rel)) return -1;
  need = sizeof(struct t2obj_hdr);
  if (o->h.text_size > size - need) return -1;
  need = need + o->h.text_size;
  if (o->h.data_size > size - need) return -1;
  need = need + o->h.data_size;
  if (o->h.nsyms * sizeof(struct t2sym) > size - need) return -1;
  need = need + o->h.nsyms * sizeof(struct t2sym);
  if (o->h.nrels * sizeof(struct t2rel) > size - need) return -1;
  need = need + o->h.nrels * sizeof(struct t2rel);
  if (o->h.str_size > size - need) return -1;
  p = sizeof(struct t2obj_hdr);
  o->text = buf + p; p = p + o->h.text_size;
  o->data = buf + p; p = p + o->h.data_size;
  o->syms = (struct t2sym *)(buf + p); p = p + o->h.nsyms * sizeof(struct t2sym);
  o->rels = (struct t2rel *)(buf + p); p = p + o->h.nrels * sizeof(struct t2rel);
  o->str = buf + p;
  // string table: NUL terminated, empty only without symbols
  if (o->h.str_size == 0 && o->h.nsyms > 0) return -1;
  if (o->h.str_size > 0 && o->str[o->h.str_size - 1] != 0) return -1;
  for (i = 0; i < o->h.nsyms; i++) {
    s = o->syms + i;
    if (s->name < 0 || s->name >= o->h.str_size) return -1;
    if (s->sect < SEC_UNDEF || s->sect > SEC_ABS) return -1;
  }
  for (i = 0; i < o->h.nrels; i++) {
    r = o->rels + i;
    if (r->sym < 0 || r->sym >= o->h.nsyms) return -1;
    if (r->sect != SEC_TEXT && r->sect != SEC_DATA) return -1;
  }
  return 0;
}

// 0 ok, -1 on a short write
int t2o_write(int fd, struct t2o *o) {
  if (write(fd, (char *)o, sizeof(struct t2obj_hdr)) != sizeof(struct t2obj_hdr)) return -1;
  if (o->h.text_size && write(fd, o->text, o->h.text_size) != o->h.text_size) return -1;
  if (o->h.data_size && write(fd, o->data, o->h.data_size) != o->h.data_size) return -1;
  if (o->h.nsyms && write(fd, o->syms, o->h.nsyms * sizeof(struct t2sym)) != o->h.nsyms * sizeof(struct t2sym)) return -1;
  if (o->h.nrels && write(fd, o->rels, o->h.nrels * sizeof(struct t2rel)) != o->h.nrels * sizeof(struct t2rel)) return -1;
  if (o->h.str_size && write(fd, o->str, o->h.str_size) != o->h.str_size) return -1;
  return 0;
}

char *t2o_name(struct t2o *o, int sym) { return o->str + o->syms[sym].name; }

// whole file into a malloc'ed buffer (NUL after the data); 0 if it cannot be read
char *t2_readfile(char *path, int *size) {
  int fd; char *buf; char *nb; int cap; int total; int n;
  fd = open(path, 0, 0);
  if (fd < 0) return 0;
  cap = 4096; total = 0;
  buf = malloc(cap + 1); memset(buf, 0, cap + 1);
  while ((n = read(fd, buf + total, cap - total)) > 0) {
    total = total + n;
    if (total == cap) {
      nb = malloc(cap * 2 + 1); memset(nb, 0, cap * 2 + 1); memcpy(nb, buf, total);
      free(buf); buf = nb; cap = cap * 2;
    }
  }
  close(fd);
  buf[total] = 0;
  *size = total;
  return buf;
}
