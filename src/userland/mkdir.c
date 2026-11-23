// mkdir [-p] dir... : create directories; -p creates every parent and
// tolerates directories that already exist.
#include "sys.h"

// -p on one path: walk a copy; at each interior '/' NUL-terminate the prefix
// and create it unless stat says it is already a directory, then restore the
// byte; the final component last. A failed mkdir (an existing FILE on the
// path) names the failing prefix and returns 1.
int mkp(char *arg) {
  char p[200]; struct stat_t st; int k; int c;
  if (strlen(arg) >= 200) { printf("mkdir: path too long: %s\n", arg); return 1; }   // rm.c's cap: sh words reach ~255
  strcpy(p, arg);
  k = 0;
  while (p[k]) {
    if (p[k] == '/' && k > 0 && p[k + 1]) {   // interior separator only
      c = p[k]; p[k] = 0;
      if (stat(p, &st) < 0 || !st.isdir) {
        if (mkdir(p) < 0) { printf("mkdir: %s failed\n", p); return 1; }
      }
      p[k] = c;
    }
    k++;
  }
  if (stat(p, &st) < 0 || !st.isdir) {
    if (mkdir(p) < 0) { printf("mkdir: %s failed\n", p); return 1; }
  }
  return 0;
}

int main(int argc, char **argv) {
  int i; int j; int pflag; char *a;
  pflag = 0;
  for (i = 1; i < argc; i++) {
    a = argv[i];
    if (a[0] == '-' && a[1]) {
      for (j = 1; a[j]; j++) {
        if (a[j] == 'p') pflag = 1;
        else { printf("usage: mkdir [-p] dir...\n"); return 1; }
      }
    } else if (pflag) {
      if (mkp(a)) return 1;
    } else if (mkdir(a) < 0) { printf("mkdir: %s failed\n", a); return 1; }
  }
  return 0;
}
