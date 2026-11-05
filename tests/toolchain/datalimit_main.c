// the data limit across files (README, Toolchain): linked after a file with 5000 bytes of data,
// this file's addw reference to counter would be past 4095; linked first (as here) it works
int counter;
int fill();
int unused_proto(int x);
extern int unused_var;
int main() { counter = fill(); printf("%d\n", counter); return 0; }
