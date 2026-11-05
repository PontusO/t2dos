// ar - T2DOS object archives
//   ar rc ARCHIVE OBJ...        create ARCHIVE, or replace the members of the same name (in place) and append new ones
//   ar t ARCHIVE                list the member names
//   ar x ARCHIVE [MEMBER...]    extract the members (all if none are named) into the current directory
// A member is named after its object file's base name. The archive's index lists every global
// defined symbol of every member, in member order (see lib/t2obj.h for the layout).
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "t2obj.c"

// the members: name, bytes, size
char **mname; char **mbuf; int *msize; int nmem;

int usage() { printf("usage: ar rc|t|x ARCHIVE [FILE...]\n"); exit(1); return 0; }
int notarchive(char *path) { printf("ar: %s: not a T2DOS archive\n", path); exit(1); return 0; }

char *basename_of(char *path) {
  char *b; b = path;
  while (*path) { if (*path == '/') b = path + 1; path++; }
  return b;
}

// read an archive file, checking its structure and that every member is an object
char *load_archive(char *path, int *size, struct t2ar *a) {
  char *buf; struct t2o o; int i;
  buf = t2_readfile(path, size);
  if (!buf) { printf("ar: cannot read %s\n", path); exit(1); }
  if (t2ar_read(buf, *size, a) < 0) notarchive(path);
  for (i = 0; i < a->h.nmembers; i++) if (t2ar_member(a, i, &o) < 0) notarchive(path);
  return buf;
}

int find(char *name) {
  int i;
  for (i = 0; i < nmem; i++) if (strcmp(mname[i], name) == 0) return i;
  return -1;
}

int pad4(int fd, int pos) {
  int z; z = 0;
  if (pos & 3) write(fd, &z, 4 - (pos & 3));
  return (pos + 3) & -4;
}

int create(char *path) {
  struct t2o o; struct t2ar_hdr h; struct t2ar_index *ix; struct t2ar_member *mt; char *str;
  int i; int j; int ni; int ss; int fd; int pos; int n; int ok; struct t2sym *s; char *nm;
  // pass 1: sizes
  ni = 0; ss = 0;
  for (i = 0; i < nmem; i++) {
    ss = ss + strlen(mname[i]) + 1;
    t2o_read(mbuf[i], msize[i], &o);
    for (j = 0; j < o.h.nsyms; j++) {
      s = o.syms + j;
      if ((s->flags & SYM_GLOBAL) && s->sect != SEC_UNDEF) { ni++; ss = ss + strlen(t2o_name(&o, j)) + 1; }
    }
  }
  ix = malloc((ni + 1) * sizeof(struct t2ar_index));
  mt = malloc((nmem + 1) * sizeof(struct t2ar_member));
  str = malloc(ss + 1);
  // pass 2: the tables
  ni = 0; ss = 0;
  for (i = 0; i < nmem; i++) {
    n = strlen(mname[i]) + 1;
    memcpy(str + ss, mname[i], n);
    mt[i].name = ss; mt[i].size = msize[i];
    ss = ss + n;
  }
  for (i = 0; i < nmem; i++) {
    t2o_read(mbuf[i], msize[i], &o);
    for (j = 0; j < o.h.nsyms; j++) {
      s = o.syms + j;
      if (!((s->flags & SYM_GLOBAL) && s->sect != SEC_UNDEF)) continue;
      nm = t2o_name(&o, j);
      n = strlen(nm) + 1;
      memcpy(str + ss, nm, n);
      ix[ni].name = ss; ix[ni].member = i;
      ss = ss + n; ni++;
    }
  }
  pos = sizeof(struct t2ar_hdr) + ni * sizeof(struct t2ar_index) + nmem * sizeof(struct t2ar_member) + ss;
  for (i = 0; i < nmem; i++) {
    pos = (pos + 3) & -4;
    mt[i].offset = pos;
    pos = pos + msize[i];
  }
  h.magic = T2AR_MAGIC; h.version = 1; h.nmembers = nmem; h.nindex = ni; h.str_size = ss;
  fd = open(path, 577, 420);    // O_WRONLY|O_CREAT|O_TRUNC, 0644
  if (fd < 0) { printf("ar: cannot write %s\n", path); return 1; }
  ok = write(fd, &h, sizeof(struct t2ar_hdr)) == sizeof(struct t2ar_hdr);
  if (ni) ok = ok && write(fd, ix, ni * sizeof(struct t2ar_index)) == ni * sizeof(struct t2ar_index);
  if (nmem) ok = ok && write(fd, mt, nmem * sizeof(struct t2ar_member)) == nmem * sizeof(struct t2ar_member);
  if (ss) ok = ok && write(fd, str, ss) == ss;
  pos = sizeof(struct t2ar_hdr) + ni * sizeof(struct t2ar_index) + nmem * sizeof(struct t2ar_member) + ss;
  for (i = 0; i < nmem && ok; i++) {
    pos = pad4(fd, pos);
    ok = write(fd, mbuf[i], msize[i]) == msize[i];
    pos = pos + msize[i];
  }
  close(fd);
  if (!ok) { printf("ar: cannot write %s\n", path); return 1; }
  return 0;
}

int main(int argc, char **argv) {
  struct t2ar a; struct t2o o; char *abuf; int asize; char *buf; int size; char *name; int i; int j; int fd; int rc; int cap;
  if (argc < 3) usage();
  if (strcmp(argv[1], "rc") == 0) {
    if (argc < 4) usage();
    nmem = 0; cap = argc;
    abuf = t2_readfile(argv[2], &asize);
    if (abuf) {                                    // an existing archive: keep its members
      if (t2ar_read(abuf, asize, &a) < 0) notarchive(argv[2]);
      cap = cap + a.h.nmembers;
    }
    mname = malloc(cap * sizeof(char *)); mbuf = malloc(cap * sizeof(char *)); msize = malloc(cap * sizeof(int));
    if (abuf) {
      for (i = 0; i < a.h.nmembers; i++) {
        if (t2ar_member(&a, i, &o) < 0) notarchive(argv[2]);
        mname[nmem] = a.str + a.members[i].name;
        mbuf[nmem] = a.base + a.members[i].offset;
        msize[nmem] = a.members[i].size;
        nmem++;
      }
    }
    for (i = 3; i < argc; i++) {
      buf = t2_readfile(argv[i], &size);
      if (!buf) { printf("ar: cannot read %s\n", argv[i]); return 1; }
      if (t2o_read(buf, size, &o) < 0) { printf("ar: %s: not a T2DOS object\n", argv[i]); return 1; }
      name = basename_of(argv[i]);
      j = find(name);
      if (j < 0) { j = nmem; nmem++; }
      mname[j] = name; mbuf[j] = buf; msize[j] = size;
    }
    return create(argv[2]);
  }
  if (strcmp(argv[1], "t") == 0 || strcmp(argv[1], "x") == 0) {
    if (strcmp(argv[1], "t") == 0 && argc != 3) usage();
    abuf = load_archive(argv[2], &asize, &a);
    if (argv[1][0] == 't') {
      for (i = 0; i < a.h.nmembers; i++) printf("%s\n", a.str + a.members[i].name);
      return 0;
    }
    for (j = 3; j < argc; j++) {
      for (i = 0; i < a.h.nmembers; i++) if (strcmp(a.str + a.members[i].name, argv[j]) == 0) break;
      if (i == a.h.nmembers) { printf("ar: %s: no such member\n", argv[j]); return 1; }
    }
    rc = 0;
    for (i = 0; i < a.h.nmembers; i++) {
      name = a.str + a.members[i].name;
      if (argc > 3) {
        for (j = 3; j < argc; j++) if (strcmp(name, argv[j]) == 0) break;
        if (j == argc) continue;
      }
      if (name[0] == 0 || strchr(name, '/')) { printf("ar: %s: bad member name\n", name); rc = 1; continue; }
      fd = open(name, 577, 420);
      if (fd < 0 || write(fd, a.base + a.members[i].offset, a.members[i].size) != a.members[i].size) {
        printf("ar: cannot write %s\n", name); rc = 1;
      }
      if (fd >= 0) close(fd);
    }
    return rc;
  }
  usage();
  return 1;
}
