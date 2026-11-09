// fmt.c - numbers to and from text, the QBasic way, with the interpreter's own digit
// generation (never printf's %f/%e/%g), so the PC build and the board print the same.
// Floats are converted exactly: dec_exact() writes out m * 2^e in decimal with a small
// big-number multiply, which makes both directions correctly rounded.
#include "basic.h"

int fbits(float f) { return *(int *)&f; }
float bitsf(int b) { return *(float *)&b; }

// The decimal digits (values 0..9, most significant first) of the integer N, where
// m * 2^e2 = N * 10^p10 exactly; 0 < m < 2^27. Returns the digit count (at most 160).
int dec_exact(int m, int e2, char *d, int *p10) {
  char t[200]; int n; int i; int k; int c; int v; int f; int mul; int j; int batch;
  n = 0;
  while (m > 0) { t[n] = m % 10; n++; m = m / 10; }
  if (e2 >= 0) { *p10 = 0; f = 2; k = e2; batch = 20; }       // N = m * 2^e2
  else { *p10 = e2; f = 5; k = -e2; batch = 9; }              // m / 2^k = m * 5^k / 10^k
  while (k > 0) {
    mul = 1; j = 0;
    while (j < k && j < batch) { mul = mul * f; j++; }
    k = k - j;
    c = 0;
    for (i = 0; i < n; i++) { v = t[i] * mul + c; t[i] = v % 10; c = v / 10; }
    while (c > 0) { t[n] = c % 10; n++; c = c / 10; }
  }
  for (i = 0; i < n; i++) d[i] = t[n - 1 - i];
  return n;
}

int fmt_int(char *buf, int v) {
  char t[12]; int n; int k; unsigned u;
  n = 0; u = v;
  if (v < 0) { buf[n] = '-'; n++; u = 0; u = u - v; }
  k = 0;
  while (1) { t[k] = '0' + u % 10; k++; u = u / 10; if (!u) break; }
  while (k > 0) { k--; buf[n] = t[k]; n++; }
  buf[n] = 0;
  return n;
}

// QBasic's SINGLE output: at most 7 significant digits, correctly rounded (ties to even),
// trailing zeros and a leading 0 dropped; the E form (1E+07, 1.5E-05) when the decimal
// exponent is 7 or more, or -5 or less.
int fmt_sng(char *buf, float f) {
  char d[200]; int b; int ex; int m; int e2; int nd; int p10; int e; int i; int up; int n;
  b = fbits(f); n = 0;
  if ((b & 0x7FFFFFFF) == 0) b = 0;          // -0 prints as 0
  if (b < 0) { buf[n] = '-'; n++; b = b & 0x7FFFFFFF; }
  ex = (b >> 23) & 255; m = b & 0x7FFFFF;
  if (ex == 255) { if (m) strcpy(buf + n, "NAN"); else strcpy(buf + n, "INF"); return n + 3; }
  if (ex == 0 && m == 0) { buf[n] = '0'; n++; buf[n] = 0; return n; }
  if (ex == 0) e2 = -149; else { m = m | 0x800000; e2 = ex - 150; }
  nd = dec_exact(m, e2, d, &p10);
  e = nd - 1 + p10;                         // value = d[0].d[1]d[2]... * 10^e
  if (nd > 7) {
    up = 0;
    if (d[7] > 5) up = 1;
    else if (d[7] == 5) { up = d[6] & 1; for (i = 8; i < nd; i++) if (d[i]) up = 1; }
    nd = 7;
    if (up) {
      i = 6;
      while (i >= 0 && d[i] == 9) { d[i] = 0; i--; }
      if (i < 0) { d[0] = 1; e++; } else d[i]++;
    }
  }
  while (nd > 1 && d[nd - 1] == 0) nd--;
  if (e >= 7 || e <= -5) {
    buf[n] = '0' + d[0]; n++;
    if (nd > 1) { buf[n] = '.'; n++; for (i = 1; i < nd; i++) { buf[n] = '0' + d[i]; n++; } }
    buf[n] = 'E'; n++;
    if (e < 0) { buf[n] = '-'; e = -e; } else buf[n] = '+';
    n++;
    buf[n] = '0' + e / 10; n++; buf[n] = '0' + e % 10; n++;
  }
  else if (e >= 0) {
    for (i = 0; i <= e; i++) { if (i < nd) buf[n] = '0' + d[i]; else buf[n] = '0'; n++; }
    if (nd > e + 1) { buf[n] = '.'; n++; for (i = e + 1; i < nd; i++) { buf[n] = '0' + d[i]; n++; } }
  }
  else {
    buf[n] = '.'; n++;
    for (i = -1; i > e; i--) { buf[n] = '0'; n++; }
    for (i = 0; i < nd; i++) { buf[n] = '0' + d[i]; n++; }
  }
  buf[n] = 0;
  return n;
}

// Compare the decimal number d[0..nd) * 10^(e - nd + 1) (d[0] != 0) with the point halfway
// between the float with bit pattern b and its neighbour above (dir 1) or below (dir -1).
int cmp_mid(char *d, int nd, int e, int b, int dir) {
  char md[200]; int ex; int mant; int m; int e2; int mm; int ee; int mn; int p; int me; int i; int x; int y;
  ex = (b >> 23) & 255; mant = b & 0x7FFFFF;
  if (ex == 0) { m = mant; e2 = -149; } else { m = mant | 0x800000; e2 = ex - 150; }
  if (dir > 0) { mm = 2 * m + 1; ee = e2 - 1; }
  else if (ex > 1 && mant == 0) { mm = 4 * m - 1; ee = e2 - 2; }   // the gap below a power of 2 is half as wide
  else { mm = 2 * m - 1; ee = e2 - 1; }
  mn = dec_exact(mm, ee, md, &p);
  me = mn - 1 + p;
  if (e != me) { if (e > me) return 1; return -1; }
  i = 0;
  while (i < nd || i < mn) {
    x = 0; y = 0;
    if (i < nd) x = d[i];
    if (i < mn) y = md[i];
    if (x != y) { if (x > y) return 1; return -1; }
    i++;
  }
  return 0;
}

// The float nearest to d[0..nd) * 10^e10 (digit values, d[0] != 0), ties to even, as a bit
// pattern; -1 if it is too large for a float.
int dec2f(char *d, int nd, int e10) {
  int e; int k; int mt; int x; float g; int b; int it; int c;
  if (nd == 0) return 0;
  e = nd - 1 + e10;
  if (e > 38) return -1;
  if (e < -46) return 0;
  // a first guess from the leading 9 digits, then step to the right float
  mt = 0; k = 0;
  while (k < nd && k < 9) { mt = mt * 10 + d[k]; k++; }
  x = e10 + nd - k;
  g = mt;
  while (x > 0) { g = g * 10; x--; }
  while (x < 0) { g = g / 10; x++; }
  b = fbits(g);
  if (b >= 0x7F800000) b = 0x7F7FFFFF;
  it = 0;
  while (it < 400) {
    it++;
    c = cmp_mid(d, nd, e, b, 1);
    if (c > 0) { if (b == 0x7F7FFFFF) return -1; b++; continue; }
    if (c == 0) { if (b & 1) b++; break; }
    if (b > 0) {
      c = cmp_mid(d, nd, e, b, -1);
      if (c < 0) { b--; continue; }
      if (c == 0 && (b & 1)) b--;
    }
    break;
  }
  if (b >= 0x7F800000) return -1;
  return b;
}

int is_digit(int c) { return c >= '0' && c <= '9'; }

// A number literal at s (n chars available, no sign): 12, 1.5, .5, 1E3, 1.5D-3, &HFF, &O17,
// &17. Returns the chars used (0 if s does not start a number); *type is T_INT (an integer
// literal that fits in 32 bits, and every &H/&O one), T_SNG, or T_NONE for an overflow.
// Type suffixes are the caller's business. VAL (Task 4) can use it too.
int num_scan(char *s, int n, int *type, int *val) {
  char d[130]; int i; int c; int nd; int e10; int sticky; int seen; int ex; int esign; int j;
  int base; int dig; unsigned u; unsigned lim; int ndig; int b; int isint;
  *type = T_NONE; *val = 0;
  if (n <= 0) return 0;
  if (s[0] == '&') {
    i = 1; base = 8;
    if (n > 1 && (s[1] == 'H' || s[1] == 'h')) { base = 16; i = 2; }
    else if (n > 1 && (s[1] == 'O' || s[1] == 'o')) i = 2;
    u = 0; ndig = 0; lim = 0xFFFFFFFF;
    lim = lim / base;
    while (i < n) {
      c = s[i];
      dig = -1;
      if (c >= '0' && c <= '9') dig = c - '0';
      else if (base == 16 && c >= 'A' && c <= 'F') dig = c - 'A' + 10;
      else if (base == 16 && c >= 'a' && c <= 'f') dig = c - 'a' + 10;
      if (dig < 0 || dig >= base) break;
      if (u > lim) ndig = -1000;            // too many digits
      u = u * base + dig; ndig++; i++;
    }
    if (ndig == 0) return 0;
    if (ndig > 0) { *type = T_INT; *val = u; }
    return i;
  }
  i = 0; nd = 0; e10 = 0; sticky = 0; seen = 0; isint = 1;
  while (i < n && is_digit(s[i])) {          // integer part
    seen = 1;
    if (nd == 0 && s[i] == '0') { i++; continue; }
    if (nd < 120) { d[nd] = s[i] - '0'; nd++; }
    else { e10++; if (s[i] != '0') sticky = 1; }
    i++;
  }
  if (i < n && s[i] == '.') {               // fraction
    i++; isint = 0;
    while (i < n && is_digit(s[i])) {
      seen = 1;
      if (nd == 0 && s[i] == '0') { e10--; i++; continue; }
      if (nd < 120) { d[nd] = s[i] - '0'; nd++; e10--; }
      else if (s[i] != '0') sticky = 1;
      i++;
    }
  }
  if (!seen) return 0;
  ex = 0; esign = 1;
  if (i < n && (s[i] == 'E' || s[i] == 'e' || s[i] == 'D' || s[i] == 'd')) {
    j = i + 1;
    if (j < n && (s[j] == '+' || s[j] == '-')) { if (s[j] == '-') esign = -1; j++; }
    if (j < n && is_digit(s[j])) {           // else the E belongs to what follows
      while (j < n && is_digit(s[j])) { if (ex < 10000) ex = ex * 10 + s[j] - '0'; j++; }
      i = j; isint = 0;
    }
  }
  if (isint && nd <= 10 && e10 == 0) {       // digits only: an integer if it fits
    u = 0; c = 1;
    for (j = 0; j < nd && c; j++) {
      if (u > 214748364) c = 0;
      else { u = u * 10 + d[j]; if (u > 2147483647) c = 0; }
    }
    if (c) { *type = T_INT; *val = u; return i; }
  }
  if (sticky) { d[nd] = 1; nd++; e10--; }
  b = dec2f(d, nd, e10 + esign * ex);
  if (b == -1) return i;                     // overflow: T_NONE
  *type = T_SNG; *val = b;
  return i;
}
