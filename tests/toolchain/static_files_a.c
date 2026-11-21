// static_files_a.c - two objects define the same internal names: each uses its own
static int v = 1;
static int dup(void) { return v + 100; }
int other(void);
int main(void) { printf("%d %d\n", dup(), other()); return 0; }
