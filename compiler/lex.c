// lex.c - the lexer: next() and the token machinery, the preprocessor (#include,
// macros, #if) and float literals. See cc.h for what the files share.
#include "cc.h"

// ------------------------------------------------------------------ lexer
int lookup(char *s, int len, int hash) {
  int i;
  i = hhead[hash & 4095] - 1;
  while (i >= 0) {
    if (syms[i].hash == hash && syms[i].len == len && !memcmp(syms[i].name, s, len)) return i;
    i = hnext[i] - 1;
  }
  return -1;
}

int idstart(int c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }
int idchar(int c) { return idstart(c) || (c >= '0' && c <= '9'); }
// the symbol for identifier s[0..n), created if new
int intern(char *s, int n) {
  int hash; int i; int k;
  hash = s[0]; k = 1;
  while (k < n) { hash = hash * 147 + s[k]; k = k + 1; }
  hash = (hash & 1073741823) | n << 6;
  i = lookup(s, n, hash);
  if (i < 0) {
    if (nsyms >= 8192) die("too many symbols");
    i = nsyms; nsyms = nsyms + 1;
    syms[i].name = s; syms[i].len = n; syms[i].hash = hash; syms[i].tk = Id;
    syms[i].class = 0;
    hnext[i] = hhead[hash & 4095]; hhead[hash & 4095] = i + 1;
  }
  return i;
}
char *sdup(char *a, char *b) {          // copy of a..b, newlines turned into spaces
  char *r; int n; int i;
  n = b - a; r = malloc(n + 1);
  i = 0; while (i < n) { r[i] = a[i] == '\n' ? ' ' : a[i]; i = i + 1; }
  r[n] = 0;
  return r;
}
char *trim(char *s) {
  int n;
  while (*s == ' ' || *s == '\t') s = s + 1;
  n = strlen(s); while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r')) n = n - 1;
  s[n] = 0;
  return s;
}
int push_src(char *np, struct Sym *m) {
  if (fdepth >= 64) die("macro or include nesting too deep");
  fstack_p[fdepth] = p; fstack_line[fdepth] = line; fstack_mac[fdepth] = m; fdepth = fdepth + 1;
  p = np;
  return 0;
}
int expanding(struct Sym *m) {         // a macro is not expanded again inside its own expansion
  int k;
  k = 0; while (k < fdepth) { if (fstack_mac[k] == m) return 1; k = k + 1; }
  return 0;
}
int param_index(struct Sym *m, char *s, int n) {
  int k;
  k = 0;
  while (k < m->mnargs) { if (strlen(m->margs[k]) == n && !memcmp(m->margs[k], s, n)) return k; k = k + 1; }
  return -1;
}
int mput(char *o, int c) { if (o - mbuf >= 65500) die("macro expansion too long"); *o = c; return 0; }
// expansion buffers: every subst() and expand_text() call has its own, freed when done
char *bput(char *b, char *o, int c) { if (o - b >= 65500) die("macro expansion too long"); *o = c; return o + 1; }
char *bcat(char *b, char *o, char *s) { while (*s) { o = bput(b, o, *s); s = s + 1; } return o; }
char *blit(char *b, char *o, char **ps) {   // copy the literal at *ps ("..." or '...')
  char *s; int q;
  s = *ps; q = *s; o = bput(b, o, *s); s = s + 1;
  while (*s && *s != q) { if (*s == '\\' && s[1]) { o = bput(b, o, *s); s = s + 1; } o = bput(b, o, *s); s = s + 1; }
  if (*s) { o = bput(b, o, *s); s = s + 1; }
  *ps = s;
  return o;
}
struct Sym *find_sym(char *s, int n) {      // like intern() but never creates one (s may be a temporary)
  int hash; int i; int k;
  hash = s[0]; k = 1;
  while (k < n) { hash = hash * 147 + s[k]; k = k + 1; }
  i = lookup(s, n, (hash & 1073741823) | n << 6);
  if (i < 0) return 0;
  return syms + i;
}
// arguments of a call of m; *pq points at its '(' and ends after the ')'.
// nl: count newlines into line (reading the source itself)
char **collect_args(struct Sym *m, char **pq, int nl) {
  char *q; char **args; int n; int depth; char *start; int c;
  q = *pq + 1;
  args = malloc(4 * (m->mnargs + 1)); n = 0; depth = 0; start = q;
  while (1) {
    c = *q;
    if (!c) die("unterminated macro call");
    if (c == '"' || c == '\'') {
      q = q + 1;
      while (*q && *q != c) { if (*q == '\\' && q[1]) q = q + 1; q = q + 1; }
      if (*q) q = q + 1;
      continue;
    }
    if (c == '\n' && nl) line = line + 1;
    if (c == '(') depth = depth + 1;
    else if (c == ')') { if (!depth) break; depth = depth - 1; }
    else if (c == ',' && !depth && !(m->mvar && n == m->mnargs - 1)) {   // the ... argument keeps its commas
      if (n >= m->mnargs) die("too many macro arguments");
      args[n] = trim(sdup(start, q)); n = n + 1; start = q + 1;
    }
    q = q + 1;
  }
  if (n >= m->mnargs && !(m->mnargs == 0 && !*trim(sdup(start, q)))) die("too many macro arguments");
  if (m->mnargs > 0) { args[n] = trim(sdup(start, q)); n = n + 1; }
  if (n == m->mnargs - 1 && m->mvar) { args[n] = ""; n = n + 1; }   // f(x) for f(x, ...)
  if (n != m->mnargs) die("wrong number of macro arguments");
  *pq = q + 1;
  return args;
}
struct Sym **tstack; int tdepth;            // macros being expanded by expand_text()
int tactive(struct Sym *m) { int k; k = 0; while (k < tdepth) { if (tstack[k] == m) return 1; k = k + 1; } return 0; }
char *subst(struct Sym *m, char **args);
// s with all macros in it expanded: what C does to an argument before substituting it
char *expand_text(char *s) {
  char *b; char *o; char *w; char *r; char *q; char **args; struct Sym *m;
  b = malloc(65536); o = b;
  while (*s) {
    if (*s == '"' || *s == '\'') o = blit(b, o, &s);
    else if (*s >= '0' && *s <= '9') { while (idchar(*s)) { o = bput(b, o, *s); s = s + 1; } }   // 0x1f, 10u
    else if (idstart(*s)) {
      w = s; while (idchar(*s)) s = s + 1;
      m = find_sym(w, s - w); r = 0;
      if (m && m->mdef && !expanding(m) && !tactive(m)) {
        if (m->mnargs < 0) r = m->mbody;
        else {
          q = s; while (*q == ' ' || *q == '\t') q = q + 1;
          if (*q == '(') { args = collect_args(m, &q, 0); s = q; r = subst(m, args); }
        }
      }
      if (r) {                               // rescan the replacement with m disabled
        if (tdepth >= 64) die("macro nesting too deep");
        tstack[tdepth] = m; tdepth = tdepth + 1;
        r = expand_text(r);
        tdepth = tdepth - 1;
        o = bcat(b, o, r); free(r);
      }
      else while (w < s) { o = bput(b, o, *w); w = w + 1; }
    }
    else { o = bput(b, o, *s); s = s + 1; }
  }
  bput(b, o, 0);
  r = sdup(b, o); free(b);
  return r;
}
// body of m with params replaced by args; #x makes a string, a ## b joins.
// Arguments are macro-expanded first, except next to # and ##, as in C.
char *subst(struct Sym *m, char **args) {
  char *b; char *o; char *s; char *a; char *w; char *t; int k; int raw;
  b = malloc(65536); o = b; s = m->mbody; raw = 0;
  while (*s) {
    if (*s == '"' || *s == '\'') { o = blit(b, o, &s); raw = 0; }   // literals are copied as they are
    else if (*s == '#' && s[1] == '#') {
      while (o > b && (*(o - 1) == ' ' || *(o - 1) == '\t')) o = o - 1;
      s = s + 2; while (*s == ' ' || *s == '\t') s = s + 1;
      raw = 1;                                // the next parameter is pasted as written
    }
    else if (*s == '#') {
      s = s + 1; while (*s == ' ' || *s == '\t') s = s + 1;
      w = s; while (idchar(*s)) s = s + 1;
      k = param_index(m, w, s - w);
      if (k < 0) die("# must be followed by a macro parameter");
      o = bput(b, o, '"');
      a = args[k];
      while (*a) {                            // runs of white space become one space, as in C
        if (*a == ' ' || *a == '\t') { o = bput(b, o, ' '); while (*a == ' ' || *a == '\t') a = a + 1; continue; }
        if (*a == '"' || *a == '\\') o = bput(b, o, '\\');
        o = bput(b, o, *a); a = a + 1;
      }
      o = bput(b, o, '"'); raw = 0;
    }
    else if (idstart(*s)) {
      w = s; while (idchar(*s)) s = s + 1;
      k = param_index(m, w, s - w);
      if (k >= 0) {
        t = s; while (*t == ' ' || *t == '\t') t = t + 1;
        if (raw || (t[0] == '#' && t[1] == '#')) o = bcat(b, o, args[k]);
        else { a = expand_text(args[k]); o = bcat(b, o, a); free(a); }
      }
      else while (w < s) { o = bput(b, o, *w); w = w + 1; }
      raw = 0;
    }
    else if (*s >= '0' && *s <= '9') { while (idchar(*s)) { o = bput(b, o, *s); s = s + 1; } raw = 0; }
    else { if (*s != ' ' && *s != '\t') raw = 0; o = bput(b, o, *s); s = s + 1; }
  }
  bput(b, o, 0);
  a = sdup(b, o); free(b);
  return a;
}
// expand macro m whose name was just read: 0 if it is function-like and not called
int expand(struct Sym *m) {
  char *q; char **args; int nl; int lv;
  if (m->mnargs < 0) { push_src(m->mbody, m); return 1; }
  // look for the '(', also past the end of finished macro expansions: F(x)(y)
  q = p; nl = 0; lv = fdepth;
  while (1) {
    while (*q == ' ' || *q == '\t' || *q == '\n' || *q == '\r') { if (*q == '\n') nl = nl + 1; q = q + 1; }
    if (*q || lv == 0 || !fstack_mac[lv - 1]) break;
    lv = lv - 1; q = fstack_p[lv]; nl = 0;
  }
  if (*q != '(') return 0;
  if (lv < fdepth) { line = fstack_line[lv]; fdepth = lv; }   // those expansions are done
  line = line + nl;
  args = collect_args(m, &q, 1);
  p = q;
  push_src(subst(m, args), m);
  return 1;
}
// ------------------------------------------------------------------ float literals
// Decimal to IEEE single, correctly rounded, in integer arithmetic only (cc.c
// itself cannot use float). The digits go into a big number of 16-bit limbs.
int *bn; int bnl;
int bn_muladd(int m, int a) {             // bn = bn * m + a, m and a small
  int i; int c;
  c = a; i = 0;
  while (i < bnl) { c = bn[i] * m + c; bn[i] = c & 65535; c = c >> 16; i = i + 1; }
  while (c) { if (bnl >= 250) die("float literal too long"); bn[bnl] = c & 65535; c = c >> 16; bnl = bnl + 1; }
  return 0;
}
int bn_div10() {                          // bn = bn / 10, returns the remainder
  int i; int r;
  r = 0; i = bnl - 1;
  while (i >= 0) { r = r * 65536 + bn[i]; bn[i] = r / 10; r = r % 10; i = i - 1; }
  while (bnl > 1 && !bn[bnl - 1]) bnl = bnl - 1;
  return r;
}
int bn_bit(int k) { if (k < 0 || k / 16 >= bnl) return 0; return (bn[k / 16] >> (k % 16)) & 1; }
int bn_top() {                            // index of the highest set bit, -1 for zero
  int i; int b;
  i = bnl - 1; while (i >= 0 && !bn[i]) i = i - 1;
  if (i < 0) return -1;
  b = 15; while (!((bn[i] >> b) & 1)) b = b - 1;
  return i * 16 + b;
}
int bn_below(int k) { int i; i = 0; while (i < k) { if (bn_bit(i)) return 1; i = i + 1; } return 0; }
int bn_field(int at, int n) { int v; int i; v = 0; i = n - 1; while (i >= 0) { v = v * 2 + bn_bit(at + i); i = i - 1; } return v; }
// bn * 2^-k (plus a nonzero tail if sticky) as float bits, rounded to nearest even
int bn_float(int k, int sticky) {
  int h; int at; int mant; int rnd; int e2; int ef;
  h = bn_top();
  if (h < 0) return 0;
  at = h - 23;                            // lowest bit that fits in the 24-bit mantissa
  if (at - k < -149) at = k - 149;        // subnormal: no bits below 2^-149
  if (at > 0) {
    mant = h - at + 1 > 0 ? bn_field(at, h - at + 1) : 0;
    rnd = bn_bit(at - 1); if (bn_below(at - 1)) sticky = 1;
  }
  else { mant = bn_field(0, h + 1) << -at; rnd = 0; }
  if (rnd && (sticky || (mant & 1))) mant = mant + 1;
  e2 = at - k;                            // value = mant * 2^e2
  if (mant == 16777216) { mant = mant >> 1; e2 = e2 + 1; }
  if (mant >= 8388608) {
    ef = e2 + 23 + 127;
    if (ef >= 255) return 2139095040;     // infinity
    return ef << 23 | (mant - 8388608);
  }
  return mant;                            // subnormal or 0
}
// the float literal at s: ival = its bits; returns the end (after any f/F/l/L)
char *fl_parse(char *s) {
  int e10; int nd; int sticky; int ex; int neg; int k; int n; int c;
  bn[0] = 0; bnl = 1; e10 = 0; nd = 0; sticky = 0;
  while (*s >= '0' && *s <= '9') {        // integer part; 200 digits decide even exact halfway cases
    c = *s - '0';
    if (nd < 200) { bn_muladd(10, c); if (nd || c) nd = nd + 1; } else { e10 = e10 + 1; if (c) sticky = 1; }
    s = s + 1;
  }
  if (*s == '.') {
    s = s + 1;
    while (*s >= '0' && *s <= '9') {
      c = *s - '0';
      if (nd < 200) { bn_muladd(10, c); e10 = e10 - 1; if (nd || c) nd = nd + 1; } else if (c) sticky = 1;
      s = s + 1;
    }
  }
  if (*s == 'e' || *s == 'E') {
    s = s + 1; neg = 0; ex = 0;
    if (*s == '+') s = s + 1; else if (*s == '-') { neg = 1; s = s + 1; }
    if (*s < '0' || *s > '9') die("bad float exponent");
    while (*s >= '0' && *s <= '9') { if (ex < 10000) ex = ex * 10 + *s - '0'; s = s + 1; }
    e10 = neg ? e10 - ex : e10 + ex;
  }
  while (*s == 'f' || *s == 'F' || *s == 'l' || *s == 'L') s = s + 1;
  if (bn_top() < 0 || e10 + nd < -50) { ival = 0; return s; }
  if (e10 + nd > 40) { ival = 2139095040; return s; }
  k = 0;
  if (e10 >= 0) { n = 0; while (n < e10) { bn_muladd(10, 0); n = n + 1; } }
  else {                                  // shift left so the quotient keeps 26+ bits, then divide
    n = -e10;
    k = 28 + (n * 3322 + 999) / 1000 - bn_top(); if (k < 0) k = 0;
    c = 0; while (c < k) { bn_muladd(2, 0); c = c + 1; }
    while (n > 0) { if (bn_div10()) sticky = 1; n = n - 1; }
  }
  ival = bn_float(k, sticky);
  return s;
}
int is_float_lit(char *s) {               // digits followed by . or an exponent (not hex)
  if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X')) return 0;
  while (*s >= '0' && *s <= '9') s = s + 1;
  return *s == '.' || *s == 'e' || *s == 'E';
}
// compile time conversions, for constant initializers
int int_to_float(int v, int uns) {
  int neg; int b;
  neg = 0;
  if (!uns && v < 0) { neg = 1; v = -v; }   // -2^31 stays 0x80000000: as 16-bit limbs that is 2^31
  bn[0] = v & 65535; bn[1] = (v >> 16) & 65535; bnl = 2;
  b = bn_float(0, 0);
  return neg ? b | (1 << 31) : b;
}
int float_to_int(int b) {                 // toward zero, like vcvt
  int e; int m; int neg;
  neg = b < 0; e = (b >> 23) & 255; m = (b & 8388607) | 8388608;
  if (e < 127) return 0;
  if (e > 157) return neg ? -2147483647 - 1 : 2147483647;
  e = e - 150;
  if (e >= 0) m = m << e; else m = m >> -e;
  return neg ? -m : m;
}
int lex() {
  char *pp; int i; int c;
  while ((tk = *p)) {
    p = p + 1;
    if (tk == '\n') { line = line + 1; }
    else if (tk == '#') { preprocess(); }
    else if (idstart(tk)) {
      pp = p - 1;
      while (idchar(*p)) p = p + 1;
      i = intern(pp, p - pp);
      if (syms[i].mdef && !expanding(syms + i) && expand(syms + i)) continue;   // read the expansion
      id = syms + i; tk = id->tk;
      return 0;
    }
    else if (tk >= '0' && tk <= '9') {
      if (is_float_lit(p - 1)) { p = fl_parse(p - 1); numty = TFLOAT; tk = Num; return 0; }
      if ((ival = tk - '0')) { while (*p >= '0' && *p <= '9') { ival = ival * 10 + *p - '0'; p = p + 1; } }
      else if (*p == 'x' || *p == 'X') {
        p = p + 1;
        while ((tk = *p) && ((tk >= '0' && tk <= '9') || (tk >= 'a' && tk <= 'f') || (tk >= 'A' && tk <= 'F'))) {
          ival = ival * 16 + (tk & 15) + (tk >= 'A' ? 9 : 0); p = p + 1;
        }
      }
      else { while (*p >= '0' && *p <= '7') { ival = ival * 8 + *p - '0'; p = p + 1; } }
      numty = TINT;                          // 10u 10L 10UL; 0x80000000 and up do not fit an int
      while (*p == 'u' || *p == 'U' || *p == 'l' || *p == 'L') { if (*p == 'u' || *p == 'U') numty = TUINT; p = p + 1; }
      if (ival < 0) numty = TUINT;
      tk = Num; return 0;
    }
    else if (tk == '/') {
      if (*p == '/') { while (*p && *p != '\n') p = p + 1; }
      else if (*p == '*') { p = p + 1; while (*p && !(*p == '*' && p[1] == '/')) { if (*p == '\n') line = line + 1; p = p + 1; } if (*p) p = p + 2; }
      else if (*p == '=') { p = p + 1; tk = DivA; return 0; }
      else { tk = Div; return 0; }
    }
    else if (tk == '"' || tk == '\'') {
      pp = data + dpos;
      while (*p && *p != tk) {
        if ((c = *p) == '\\') {
          p = p + 1; c = *p;
          // numeric values only: this file must compile with a cc that lacks these escapes
          if (c == 'n') c = 10; else if (c == 't') c = 9; else if (c == 'r') c = 13;
          else if (c == 'b') c = 8; else if (c == 'a') c = 7; else if (c == 'f') c = 12; else if (c == 'v') c = 11;
          else if (c == 'e') c = 27;
          else if (c >= '0' && c <= '7') {     // \0 \33 \033: up to 3 octal digits
            c = c - '0'; i = 1;
            while (i < 3 && p[1] >= '0' && p[1] <= '7') { p = p + 1; c = c * 8 + *p - '0'; i = i + 1; }
          }
          else if (c == 'x') {                 // \x1b: hex digits
            c = 0;
            while ((p[1] >= '0' && p[1] <= '9') || (p[1] >= 'a' && p[1] <= 'f') || (p[1] >= 'A' && p[1] <= 'F')) {
              p = p + 1; c = c * 16 + (*p & 15) + (*p >= 'A' ? 9 : 0);
            }
          }
        }
        p = p + 1;
        if (tk == '"') { data[dpos] = c; dpos = dpos + 1; } else ival = c;
      }
      p = p + 1;
      if (tk == '"') { data[dpos] = 0; dpos = dpos + 1; ival = pp - data; slen = dpos - ival; tk = Str; } else { tk = Num; numty = TINT; }
      return 0;
    }
    else if (tk == '=') { if (*p == '=') { p = p + 1; tk = Eq; } else tk = Assign; return 0; }
    else if (tk == '+') { if (*p == '+') { p = p + 1; tk = Inc; } else if (*p == '=') { p = p + 1; tk = AddA; } else tk = Add; return 0; }
    else if (tk == '-') { if (*p == '-') { p = p + 1; tk = Dec; } else if (*p == '>') { p = p + 1; tk = Arrow; } else if (*p == '=') { p = p + 1; tk = SubA; } else tk = Sub; return 0; }
    else if (tk == '!') { if (*p == '=') { p = p + 1; tk = Ne; } return 0; }
    else if (tk == '<') { if (*p == '=') { p = p + 1; tk = Le; } else if (*p == '<') { p = p + 1; if (*p == '=') { p = p + 1; tk = ShlA; } else tk = Shl; } else tk = Lt; return 0; }
    else if (tk == '>') { if (*p == '=') { p = p + 1; tk = Ge; } else if (*p == '>') { p = p + 1; if (*p == '=') { p = p + 1; tk = ShrA; } else tk = Shr; } else tk = Gt; return 0; }
    else if (tk == '|') { if (*p == '|') { p = p + 1; tk = Lor; } else if (*p == '=') { p = p + 1; tk = OrA; } else tk = Or; return 0; }
    else if (tk == '&') { if (*p == '&') { p = p + 1; tk = Lan; } else if (*p == '=') { p = p + 1; tk = AndA; } else tk = And; return 0; }
    else if (tk == '^') { if (*p == '=') { p = p + 1; tk = XorA; } else tk = Xor; return 0; }
    else if (tk == '%') { if (*p == '=') { p = p + 1; tk = ModA; } else tk = Mod; return 0; }
    else if (tk == '*') { if (*p == '=') { p = p + 1; tk = MulA; } else tk = Mul; return 0; }
    else if (tk == '[') { tk = Brak; return 0; }
    else if (tk == '?') { tk = Cond; return 0; }
    else if (tk == '.') {
      if (*p >= '0' && *p <= '9') { p = fl_parse(p - 1); numty = TFLOAT; tk = Num; return 0; }   // .5
      tk = Dot; return 0;
    }
    else if (tk == '~' || tk == ';' || tk == '{' || tk == '}' || tk == '(' || tk == ')' || tk == ']' || tk == ',' || tk == ':') return 0;
  }
  // end of this file or macro expansion: pop the input stack
  if (fdepth > 0) { fdepth = fdepth - 1; p = fstack_p[fdepth]; line = fstack_line[fdepth]; return lex(); }
  return 0;
}
// next token. Adjacent string literals ("a" "b", also from macros) become one:
// after a string, peek at the next token and push it back if it is not a string.
int next() {
  int s_ival; int s_slen; int i;
  if (pb) { pb = 0; tk = pb_tk; ival = pb_ival; id = pb_id; numty = pb_numty; return 0; }
  lex();
  if (tk != Str) return 0;
  while (1) {
    s_ival = ival; s_slen = slen;
    lex();
    if (tk != Str) { pb = 1; pb_tk = tk; pb_ival = ival; pb_id = id; pb_numty = numty; tk = Str; ival = s_ival; slen = s_slen; return 0; }
    if (ival != s_ival + s_slen) die("string literals not adjacent");
    i = ival; while (i < dpos) { data[i - 1] = data[i]; i = i + 1; }   // drop the first literal's 0
    dpos = dpos - 1; slen = s_slen + slen - 1; ival = s_ival;
  }
}

// rest of the line, \-newline continuations joined: a macro body
char *line_body() {
  char *o; char *r;
  while (*p == ' ' || *p == '\t') p = p + 1;
  o = mbuf;
  while (*p && *p != '\n') {
    if (*p == '\\' && (p[1] == '\n' || (p[1] == '\r' && p[2] == '\n'))) {
      p = p + (p[1] == '\r' ? 3 : 2); line = line + 1; mput(o, ' '); o = o + 1;
    }
    else { mput(o, *p); o = o + 1; p = p + 1; }
  }
  mput(o, 0);
  r = sdup(mbuf, o);
  return trim(r);
}
// is the directive word w at p (and not just the start of a longer word)?
int pp_word(char *w) { int n; n = strlen(w); return !memcmp(p, w, n) && !idchar(p[n]); }
int pp_skip_ws() { while (*p == ' ' || *p == '\t') p = p + 1; return 0; }
// Skip a block whose condition is false: lines up to the #elif, #else or #endif of
// this level (only #endif if to_endif). Nested #if blocks are skipped whole.
// Returns 1 #elif, 2 #else, 3 #endif, with p after the word.
int skip_cond(int to_endif) {
  int depth;
  depth = 0;
  while (1) {
    while (*p && *p != '\n') { if (*p == '\\' && p[1] == '\n') { p = p + 1; line = line + 1; } p = p + 1; }
    if (!*p) die("#endif missing");
    p = p + 1; line = line + 1;
    pp_skip_ws();
    if (*p != '#') continue;
    p = p + 1; pp_skip_ws();
    if (pp_word("if") || pp_word("ifdef") || pp_word("ifndef")) depth = depth + 1;
    else if (pp_word("endif")) { if (!depth) { p = p + 5; return 3; } depth = depth - 1; }
    else if (!depth && !to_endif && pp_word("elif")) { p = p + 4; return 1; }
    else if (!depth && !to_endif && pp_word("else")) { p = p + 4; return 2; }
  }
}
// the value of an #if or #elif expression (the rest of the line)
int pp_eval() {
  char *t; char *b; char *o; char *w; struct Sym *m; int v; int paren;
  int s_tk; int s_ival; struct Sym *s_id; int s_numty; int s_slen; int s_pb;
  t = line_body();
  b = malloc(strlen(t) * 2 + 16); o = b;     // defined(X) and defined X become 1 or 0, before expansion
  while (*t) {
    if (t[0] == '/' && t[1] == '/') break;      // comments go
    if (t[0] == '/' && t[1] == '*') { t = t + 2; while (*t && !(t[0] == '*' && t[1] == '/')) t = t + 1; if (*t) t = t + 2; *o = ' '; o = o + 1; }
    else if (idstart(*t)) {
      w = t; while (idchar(*t)) t = t + 1;
      if (t - w == 7 && !memcmp(w, "defined", 7)) {
        while (*t == ' ' || *t == '\t') t = t + 1;
        paren = *t == '(';
        if (paren) { t = t + 1; while (*t == ' ' || *t == '\t') t = t + 1; }
        w = t; while (idchar(*t)) t = t + 1;
        if (t == w) die("defined needs a name");
        m = find_sym(w, t - w);
        *o = m && m->mdef ? '1' : '0'; o = o + 1;
        if (paren) { while (*t == ' ' || *t == '\t') t = t + 1; if (*t != ')') die(") expected after defined"); t = t + 1; }
      }
      else while (w < t) { *o = *w; o = o + 1; w = w + 1; }
    }
    else { *o = *t; o = o + 1; t = t + 1; }
  }
  *o = 0;
  t = expand_text(b); free(b);
  b = malloc(strlen(t) + 4); strcpy(b, t); strcpy(b + strlen(b), " ;"); free(t);   // ; ends it
  s_tk = tk; s_ival = ival; s_id = id; s_numty = numty; s_slen = slen; s_pb = pb;   // we are inside lex()
  push_src(b, 0); pb = 0; pp_if = 1;
  next(); v = cexpr();
  if (tk != ';') die("bad #if expression");
  pp_if = 0;
  fdepth = fdepth - 1; p = fstack_p[fdepth]; line = fstack_line[fdepth];
  tk = s_tk; ival = s_ival; id = s_id; numty = s_numty; slen = s_slen; pb = s_pb;
  return v;
}
// #define NAME body from C (-D and predefined macros)
int define_macro(char *name, char *body) {
  struct Sym *d;
  d = syms + intern(name, strlen(name));
  d->mdef = 1; d->mnargs = -1; d->mvar = 0; d->mbody = body;
  return 0;
}
int preprocess() {
  char *name; char *q; int n; struct Sym *d; char **names; int r;
  while (*p == ' ' || *p == '\t') p = p + 1;
  if (!memcmp(p, "include", 7)) {
    p = p + 7;
    while (*p == ' ' || *p == '\t') p = p + 1;
    if (*p == '"') {
      p = p + 1; name = p;
      while (*p && *p != '"') p = p + 1;
      n = p - name; q = malloc(n + 1); memcpy(q, name, n); q[n] = 0;
      if (*p) p = p + 1;
      while (*p && *p != '\n') p = p + 1;
      push_src(readfile(q), 0); line = 1;
      return 0;
    }
    // <system header>: ignored, builtins cover it
  }
  else if (!memcmp(p, "define", 6)) {
    p = p + 6;
    while (*p == ' ' || *p == '\t') p = p + 1;
    if (!idstart(*p)) die("bad #define");
    name = p; while (idchar(*p)) p = p + 1;
    d = syms + intern(name, p - name);
    d->mnargs = -1; d->mvar = 0;
    if (*p == '(') {                          // NAME( with no space: function-like
      p = p + 1; n = 0; names = malloc(32 * 4);
      while (*p == ' ' || *p == '\t') p = p + 1;
      while (*p != ')') {
        if (n >= 32) die("too many macro parameters");
        if (*p == '.' && p[1] == '.' && p[2] == '.') { names[n] = "__VA_ARGS__"; d->mvar = 1; p = p + 3; }
        else { if (!idstart(*p)) die("bad macro parameter"); q = p; while (idchar(*p)) p = p + 1; names[n] = sdup(q, p); }
        n = n + 1;
        while (*p == ' ' || *p == '\t') p = p + 1;
        if (*p == ',' && !d->mvar) { p = p + 1; while (*p == ' ' || *p == '\t') p = p + 1; }
        else if (*p != ')') die("bad macro parameter list");
      }
      p = p + 1;
      d->margs = names; d->mnargs = n;
    }
    d->mbody = line_body(); d->mdef = 1;   // the symbol keeps its class: a name can be a macro and a variable
    return 0;
  }
  else if (pp_word("ifdef") || pp_word("ifndef") || pp_word("if")) {
    if (pp_word("if")) { p = p + 2; n = pp_eval(); }
    else {
      r = pp_word("ifndef"); p = p + (r ? 6 : 5); pp_skip_ws();
      name = p; while (idchar(*p)) p = p + 1;
      if (p == name) die("#ifdef needs a name");
      d = find_sym(name, p - name);
      n = d && d->mdef; if (r) n = !n;
    }
    if (n) ifdepth = ifdepth + 1;
    else {
      while (1) {                               // find the branch to compile, if any
        r = skip_cond(0);
        if (r == 3) break;
        if (r == 2) { ifdepth = ifdepth + 1; break; }
        if (pp_eval()) { ifdepth = ifdepth + 1; break; }
      }
    }
  }
  else if (pp_word("elif") || pp_word("else")) {  // the branch before was taken: skip the rest
    if (!ifdepth) die("#elif or #else without #if");
    skip_cond(1); ifdepth = ifdepth - 1;
  }
  else if (pp_word("endif")) {
    if (!ifdepth) die("#endif without #if");
    ifdepth = ifdepth - 1;
  }
  else if (pp_word("error")) {
    p = p + 5; printf("%s:%d: #error %s\n", srcname, line, line_body()); quit();
  }
  else if (!memcmp(p, "undef", 5)) {
    p = p + 5;
    while (*p == ' ' || *p == '\t') p = p + 1;
    name = p; while (idchar(*p)) p = p + 1;
    if (p > name) {
      d = syms + intern(name, p - name);
      d->mdef = 0;
    }
  }
  while (*p && *p != '\n') {                // other directives are ignored, continuations too
    if (*p == '\\' && p[1] == '\n') { p = p + 1; line = line + 1; }
    p = p + 1;
  }
  return 0;
}
