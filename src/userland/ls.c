#include "sys.h"
int list(char *dir) {
  struct dirent_t e; int d; int files; int bytes; struct stat_t st;
  if (stat(dir, &st) == 0 && !st.isdir) { printf("%8d  %s\n", st.size, dir); return 0; }
  d = opendir(dir);
  if (d < 0) { printf("ls: %s: not found\n", dir); return 1; }
  files = 0; bytes = 0;
  while (readdir(d, &e)) {
    if (e.isdir) printf("   <dir>  %s/\n", e.name);
    else { printf("%8d  %s\n", e.size, e.name); files++; bytes = bytes + e.size; }
  }
  closedir(d);
  printf("%d files, %d bytes\n", files, bytes);
  return 0;
}
int main(int argc, char **argv) {
  int i;
  if (argc < 2) return list(".");
  for (i = 1; i < argc; i++) { if (argc > 2) printf("%s:\n", argv[i]); list(argv[i]); }
  return 0;
}
