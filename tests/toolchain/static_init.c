// static_init.c - computed initializers for statics (a cc extension beyond C, the .Linit
// path): coded, once, before main - for a file-scope static AND a static local, whose
// init lines splice out of the function body (init_splice).
int gside;
int base(void) { gside = gside + 1; return 40; }
static int gc = 2 * base();
int f(void) { static int c = base() + 1; return c; }
int main(void) { printf("%d %d %d side%d\n", gc, f(), f(), gside); return 0; }
