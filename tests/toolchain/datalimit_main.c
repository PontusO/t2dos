// the data limit across files (README, Toolchain): cc gives counter's reference the form that
// reaches its linked offset, so this links before (datalimit) and after (datalimit_rev) the 5000
// bytes of datalimit_big.c; only an object built with -mshort-data and linked after it fails
int counter;
int fill();
int unused_proto(int x);
extern int unused_var;
int main() { counter = fill(); printf("%d\n", counter); return 0; }
