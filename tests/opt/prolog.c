// prolog.c - shapes for call liveness and frame-pointer use (tools/check_prolog.py).
// ext/cfext are calls whose body cc cannot see, so promote() must reason about them.
int g;
#ifdef NATIVE
int ext(int a) { return a + 1; }         // reference build links the stubs
float cfext(float x) { return x + 0.25; }
#endif
int ext(int a);                          // prototype only: forces real calls
int csc(int n) {                         // s live across the call: its register stays saved
  int s;
  s = n;
  g = ext(s);
  return s + g;
}
int cleaf(int n) {                       // no calls, one local, one promoted param
  int t;
  t = n * 3;
  return t + 1;
}
int cmixed(int n) {                      // i dead across the call (it is the arg), s live after it
  int i; int s;
  s = 0; i = 0;
  while (i < n) { g = ext(i); s = s + i; i = i + 1; }
  return s;
}
int clocals(int n) {                     // two locals, no calls: frame stays (r7 reads)
  int a; int b;
  a = n + 1; b = a * 2;
  return b - n;
}
int rec(int n) {                           // self-recursion: the accumulator lives across
  int a;                                   // the call, the shifted argument does not
  if (n <= 0) return 0;
  a = rec(n / 2);
  return a + n;
}
float cfext(float x);
float cfloat(float x) {                  // float local: vpush/vpop keep the frame pointer
  float y;
  y = x + 1.5;
  return y * 2.0;
}
int cret(int n) {                        // early return: trimframe copies the pop
  if (n > 0) return 7;
  return ext(n);
}
int csys(char *p, int n) {               // a real syscall (the read builtin): the kernel
  return read(0, p, n);                  // switches tasks inside it, so r7 must stay put
}
int main() {
  int a; int b; int c; int d; float e;
  a = csc(4); b = cleaf(5); c = cmixed(3); d = clocals(6); e = cfloat(1.5);
  printf("check %d %d %d %d %d %d\n", a, b, c, d, (int)e, rec(9));   // gcc reference pins the numbers
  return 0;
}
