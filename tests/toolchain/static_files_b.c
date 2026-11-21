// static_files_b.c - the other half: same static names, different values
static int v = 2;
static int dup(void) { return v + 200; }
int other(void) { return dup(); }
