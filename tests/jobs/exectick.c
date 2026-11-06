// exectick.c - background job test program: exectick [dir]
// Changes to dir (if given), prints "exectick cwd DIR", then execs
// /src/jobs/tick.bin 0 1000, so the job spends its life one exec level down
// (killing it there unwinds both levels). Prints "exectick exec N" if the exec returns.
#include "sys.h"

char buf[128];
char *args[4];

int main(int argc, char **argv) {
  int r;
  if (argc > 1 && chdir(argv[1]) < 0) { printf("exectick: no directory %s\n", argv[1]); return 1; }
  printf("exectick cwd %s\n", getcwd(buf, 128));
  args[0] = "tick"; args[1] = "0"; args[2] = "1000"; args[3] = 0;
  r = exec("/src/jobs/tick.bin", 3, args);
  printf("exectick exec %d\n", r);
  return 0;
}
