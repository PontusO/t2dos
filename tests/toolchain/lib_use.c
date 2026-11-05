// lib_use.c - linked with -l tc against libtc.a (ar of lib_square.c and lib_cube.c,
// built with cc -c). Only lib_square.o is needed, so lib_cube.o is not linked.
extern int square(int x);
extern int squares;

int main() {
  printf("%d %d\n", square(7), square(12));
  printf("calls %d\n", squares);
  return square(3);
}
