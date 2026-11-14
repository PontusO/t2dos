// native.h - lets gcc build the benchmarks on the PC for the reference checksums
// (tools/bench.py passes it with -include; T2DOS has millis() as a builtin)
#include <time.h>
static int millis(void) { return (int)(clock() / (CLOCKS_PER_SEC / 1000)); }
