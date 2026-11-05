// 5000 bytes of data: link this file last (see datalimit_main.c)
char buf[5000];
int fill() { buf[0] = 1; return buf[0]; }
