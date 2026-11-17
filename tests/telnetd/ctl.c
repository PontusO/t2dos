// conctl codes must reach a telnet session (check_telnetd.py --pc builds this as /tmp/tdctl.bin)
#include "sys.h"
int main() { conctl(CON_GOTO, 2, 3); conctl(CON_COLOR, 1, 4); printf("ctl\n"); return 0; }
