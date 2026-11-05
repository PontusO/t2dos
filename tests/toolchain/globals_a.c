// globals_a.c - with globals_b.c: globals defined in one file and used in the other
// through extern (int, arrays, a pointer), modified from both files
int shared = 10;                        // defined here, also changed by globals_b.c
int table[4] = { 5, 6, 7, 8 };          // used there as extern int table[4]
extern int counter;                     // defined in globals_b.c (after 70000 bytes of data)
extern char name[];                     // a char array defined there
extern char *greeting;                  // a pointer with an initializer there
extern int bump(int k);
extern int late;                        // declared extern, defined further down in this file
int read_late() { return late; }        // this reference uses the extern form
int late = 77;
int read_late2() { return late; }       // and this one the normal one

int main() {
  int r;
  shared = shared + 1;                  // 11
  r = bump(2);                          // shared 13, counter 1, table[2]
  printf("bump %d shared %d counter %d\n", r, shared, counter);
  counter = counter + 40;               // written from this file
  r = bump(3);                          // shared 16, counter 42
  printf("bump %d shared %d counter %d\n", r, shared, counter);
  printf("%s %s %d\n", greeting, name, name[1]);
  name[0] = 'B';
  printf("%s %d %d\n", name, read_late(), read_late2());
  late = late + 1;
  printf("late %d %d\n", read_late(), read_late2());
  return counter;
}
