// prints the console's width and height (sysinfo INFO_CONSOLE), for check_telnetd.py
#include "sys.h"
int main() { int k; k = sysinfo(INFO_CONSOLE); printf("size %d %d\n", k & 65535, k >> 16); return 0; }
