// test13.c - maths built-ins (sin to fmod). Standard C: gcc builds it natively (-lm) for
// reference output. T2CC's are float functions, gcc's are double: every argument is exact in
// binary and every result is printed to 5 digits, away from a rounding boundary.
#include <stdio.h>
#include <math.h>

int main() {
  float r; float x;

  printf("sin %.4e %.4e %.4e\n", sin(0.5), sin(1.25), sin(-2.0));
  printf("cos %.4e %.4e\n", cos(0.5), cos(2.0));
  printf("tan %.4e %.4e\n", tan(0.25), tan(1.0));
  printf("asin %.4e %.4e\n", asin(0.5), asin(-0.25));
  printf("acos %.4e %.4e\n", acos(0.5), acos(0.75));
  printf("atan %.4e %.4e\n", atan(0.5), atan(3.0));
  printf("atan2 %.4e %.4e %.4e\n", atan2(1.0, 2.0), atan2(-1.0, -1.0), atan2(3.0, -4.0));
  printf("sinh %.4e %.4e\n", sinh(0.5), sinh(2.0));
  printf("cosh %.4e %.4e\n", cosh(0.5), cosh(-1.5));
  printf("tanh %.4e %.4e\n", tanh(0.5), tanh(-1.25));
  printf("exp %.4e %.4e %.4e\n", exp(1.0), exp(-2.5), exp(10.0));
  printf("log %.4e %.4e %.4e\n", log(2.0), log(10.0), log(0.5));
  printf("log10 %.4e %.4e\n", log10(2.0), log10(0.125));
  printf("pow %.4e %.4e %.4e\n", pow(2.0, 0.5), pow(1.5, 3.0), pow(10.0, -2.0));
  printf("sqrt %.4e %.4e %.4e\n", sqrt(2.0), sqrt(9.0), sqrt(0.25));
  printf("fabs %.4e %.4e\n", fabs(-3.5), fabs(2.25));
  printf("floor %.4e %.4e %.4e\n", floor(2.5), floor(-2.5), floor(7.0));
  printf("ceil %.4e %.4e %.4e\n", ceil(2.5), ceil(-2.5), ceil(7.0));
  printf("fmod %.4e %.4e %.4e\n", fmod(7.5, 2.0), fmod(-7.5, 2.0), fmod(5.25, 0.5));

  printf("int arg %.4e\n", sqrt(2));
  r = pow(2, 10);
  printf("pow int %.1f\n", r);
  x = 0.5;
  printf("expr %.4f %.4f\n", 2 * sin(x) * cos(x), sin(1.0));
  return 0;
}
