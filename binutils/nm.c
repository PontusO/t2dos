// nm - list the symbols of T2DOS object files and archives
//   nm file.o|file.a...   prints "%08x %c name", sorted by name; T/D/A/U, lower case for local symbols
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "t2obj.c"

int nm_obj(struct t2o *o) {
  int *ord; int n; int i; int j; int k; char c; struct t2sym *s;
  n = o->h.nsyms;
  ord = malloc((n + 1) * sizeof(int));
  for (i = 0; i < n; i++) {                       // stable insertion sort by name
    j = i;
    while (j > 0 && strcmp(t2o_name(o, ord[j - 1]), t2o_name(o, i)) > 0) { ord[j] = ord[j - 1]; j--; }
    ord[j] = i;
  }
  for (k = 0; k < n; k++) {
    s = o->syms + ord[k];
    c = 0;
    if (s->sect == SEC_TEXT) c = 'T';
    if (s->sect == SEC_DATA) c = 'D';
    if (s->sect == SEC_ABS) c = 'A';
    if (s->sect == SEC_UNDEF) c = 'U';
    if (c == 0) c = '?';
    if (c != 'U' && c != '?' && !(s->flags & SYM_GLOBAL)) c = c + 32;
    if (s->sect == SEC_UNDEF) printf("         %c %s\n", c, t2o_name(o, ord[k]));
    else printf("%08x %c %s\n", s->value, c, t2o_name(o, ord[k]));
  }
  free(ord);
  return 0;
}

int nm_file(char *path, int heading) {
  char *buf; int size; struct t2o o; struct t2ar_hdr *ah; struct t2ar_member *m; char *astr; int i; int rc;
  buf = t2_readfile(path, &size);
  if (!buf) { printf("nm: cannot read %s\n", path); return 1; }
  if (size >= 4 && *(int *)buf == T2AR_MAGIC) {
    ah = (struct t2ar_hdr *)buf;
    m = (struct t2ar_member *)(buf + sizeof(struct t2ar_hdr) + ah->nindex * sizeof(struct t2ar_index));
    astr = (char *)(m + ah->nmembers);
    if (heading) printf("%s:\n", path);
    rc = 0;
    for (i = 0; i < ah->nmembers; i++) {
      if (m[i].offset < 0 || m[i].size < 0 || m[i].offset + m[i].size > size || t2o_read(buf + m[i].offset, m[i].size, &o) < 0) {
        printf("nm: %s: not a T2DOS object\n", path); return 1;
      }
      printf("\n%s:\n", astr + m[i].name);
      nm_obj(&o);
    }
    return rc;
  }
  if (t2o_read(buf, size, &o) < 0) { printf("nm: %s: not a T2DOS object\n", path); return 1; }
  if (heading) printf("%s:\n", path);
  return nm_obj(&o);
}

int main(int argc, char **argv) {
  int i; int rc;
  if (argc < 2) { printf("usage: nm file.o|file.a...\n"); return 1; }
  rc = 0;
  for (i = 1; i < argc; i++) if (nm_file(argv[i], argc > 2)) rc = 1;
  return rc;
}
