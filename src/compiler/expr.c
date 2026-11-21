// expr.c - expressions: expr() and its precedence ladder, assignment, calls and
// lvalues. See cc.h for what the files share.
#include "cc.h"

// ------------------------------------------------------------------ expressions
// Emit a user function call: args already pushed, n args.
int call_fn(struct Sym *f, int n) {
  if (n > 127) die("too many arguments");
  lnew(L_INS); ls("bl\t"); lsym(f); lend();    // as resolves it, or ld when f is in another file
  if (f->class == 0) { f->class = CFun; f->val = -1; f->type = TINT; }
  if (n) addsp(n);                              // pop the args
  return 0;
}
// r0 = address of function f (Thumb bit set), PC relative: fnaddr is movw/movt r0,#off ; add r0,pc
int fn_addr(struct Sym *f) {
  if (f->class == CSys) die("cannot take the address of a builtin");
  if (f->class == 0) { f->class = CFun; f->val = -1; f->type = TINT; }
  lnew(L_INS); ls("fnaddr\tr0, "); lsym(f); lend();
  return 0;
}
// Call through a function address in r0: keep it on the stack under the args,
// then ldr r1,[sp,#4n] ; blx r1 ; pop args and the address.
int call_ind(int t) {
  int n;
  push(); next(); n = 0;
  while (tk != ')') { expr(Assign); if (n < fnnp_of(t)) assignconv(fnparam_of(t, n)); push(); n = n + 1; if (tk == ',') next(); }
  next();
  if (n > 126) die("too many arguments");
  lnew(L_INS); ls("ldr\tr1, [sp, #"); ls(dec(4 * n)); ls("]"); lend();   // the address, under the n args
  ins("blx\tr1");
  addsp(n + 1);                                 // args and the address
  ty = fnret_of(t);
  return 0;
}
// Emit a host syscall: r0 = num, r1 = nargs, r2 = argv (argv[n-1] is arg 0)
int call_sys(int num, int n) {
  ins("mov\tr2, sp");                           // r2 = argv
  ins("push\t{r4}");
  ins("mov\tr4, sp");
  ins("mov\tr3, sp");
  ins("bic.w\tr3, r3, #7");                     // 8-byte aligned sp for the kernel
  ins("mov\tsp, r3");
  ins_ri("movs", 1, n);
  ins_ri("movs", 0, num);
  ins("blx\tr8");
  ins("mov\tsp, r4");
  ins("pop\t{r4}");
  if (n) addsp(n);                              // pop the args
  return 0;
}

// scale r0 by n (for pointer arithmetic)
int scale(int n) {
  if (n == 1) return 0;
  if (n == 4) return ins("lsls\tr0, r0, #2");
  imm(2, n); return ins("muls\tr0, r2");
}

// Roll back the last load so r0 holds an address, for lvalue use.
int lvalue() {
  int k;
  if (lastload) {                               // drop that many instruction lines (labels stay)
    k = nlines - 1;
    while (lastload > 0) { if (lkind[k] == L_INS) { lkind[k] = L_DEL; lastload = lastload - 1; } k = k - 1; }
  }
  else if (!(ty >= TSTRUCT && ty < PTR)) die("lvalue expected");
  return 0;
}
// scale r1 by n
int scale1(int n) {
  if (n == 1) return 0;
  if (n == 4) return ins("lsls\tr1, r1, #2");
  imm(2, n); return ins("muls\tr1, r2");
}

// r0 = r1 op r0 where r1 has type t and r0 has type ty; sets ty to the result type
// r0 = r1 op r0 for == != < > <= >= (left type t, right type ty)
int relop(int op, int t) {
  if ((isflt(t) || isflt(ty)) && t < PTR && ty < PTR) {          // vcmp; with a NaN only != is true
    fpair(t); ins("vcmp.f32\ts0, s1"); ins("vmrs\tAPSR_nzcv, fpscr");
    if (op == Eq) itcc(CC_EQ); else if (op == Ne) itcc(CC_NE); else if (op == Lt) itcc(CC_MI);
    else if (op == Gt) itcc(CC_GT); else if (op == Le) itcc(CC_LS); else itcc(CC_GE);
    return 0;
  }
  if (op == Eq) setcc(CC_EQ); else if (op == Ne) setcc(CC_NE);
  else if (op == Lt) setcc(isuns(t, ty) ? CC_LO : CC_LT); else if (op == Gt) setcc(isuns(t, ty) ? CC_HI : CC_GT);
  else if (op == Le) setcc(isuns(t, ty) ? CC_LS : CC_LE); else setcc(isuns(t, ty) ? CC_HS : CC_GE);
  return 0;
}
// s0 = r0 + 1.0 or r0 - 1.0 (float ++ and --)
int fstep(int up) {
  ins("vmov\ts0, r0"); imm(2, 1065353216); ins("vmov\ts1, r2");   // 1.0f
  if (up) ins("vadd.f32\ts0, s0, s1"); else ins("vsub.f32\ts0, s0, s1");
  return 0;
}
int binop(int op, int t) {
  int u;
  if ((isflt(t) || isflt(ty)) && t < PTR && ty < PTR) {          // float arithmetic
    fpair(t);
    if (op == Add) ins("vadd.f32\ts0, s0, s1"); else if (op == Sub) ins("vsub.f32\ts0, s0, s1");
    else if (op == Mul) ins("vmul.f32\ts0, s0, s1"); else if (op == Div) ins("vdiv.f32\ts0, s0, s1");
    else die("invalid operator for float");
    ins("vmov\tr0, s0"); ty = TFLOAT;
    return 0;
  }
  u = isuns(t, ty);
  if (op == Or)  { ins("orrs\tr0, r1"); ty = arith(t, ty); }
  else if (op == Xor) { ins("eors\tr0, r1"); ty = arith(t, ty); }
  else if (op == And) { ins("ands\tr0, r1"); ty = arith(t, ty); }
  else if (op == Shl) { ins("lsl.w\tr0, r1, r0"); ty = arith(t, t); }
  else if (op == Shr) {                                            // the left operand decides
    if (t == TUINT) ins("lsr.w\tr0, r1, r0"); else ins("asr.w\tr0, r1, r0");
    ty = arith(t, t);
  }
  else if (op == Add) {
    if (t >= PTR && ty < PTR) { scale(tysize(t - PTR)); ty = t; }
    else if (ty >= PTR && t < PTR) scale1(tysize(ty - PTR));
    else if (t < PTR && ty < PTR) ty = arith(t, ty);
    ins("adds\tr0, r1, r0");
  }
  else if (op == Sub) {
    if (t >= PTR && ty >= PTR) { ins("subs\tr0, r1, r0"); imm(2, tysize(t - PTR)); ins("sdiv\tr0, r0, r2"); ty = TINT; }   // pointer difference
    else { if (t >= PTR) scale(tysize(t - PTR)); ins("subs\tr0, r1, r0"); ty = t < PTR ? arith(t, ty) : t; }
  }
  else if (op == Mul) { ins("muls\tr0, r1"); ty = arith(t, ty); }
  else if (op == Div) { if (u) ins("udiv\tr0, r1, r0"); else ins("sdiv\tr0, r1, r0"); ty = arith(t, ty); }
  else if (op == Mod) {                                            // r1 - (r1 / r0) * r0
    if (u) ins("udiv\tr2, r1, r0"); else ins("sdiv\tr2, r1, r0");
    ins("mls\tr0, r2, r0, r1"); ty = arith(t, ty);
  }
  return 0;
}

int expr(int lev) {
  int t; int n; int at; int at2; int m; int sz; int tmp; struct Sym *d;

  if (!tk) die("unexpected eof in expression");
  else if (tk == Num) { imm(0, ival); ty = numty; next(); }
  else if (tk == Str) { lea_glo(ival); next(); ty = TCHAR + PTR; }
  else if (tk == Sizeof) {
    next(); if (tk != '(') die("( expected after sizeof"); next();
    t = declarator(parse_type());
    if (tk != ')') die(") expected"); next();
    n = tysize(t); if (darr >= 0) n = n * darr;
    imm(0, n); ty = TINT;
  }
  else if (tk == Id) {
    d = id; next();
    if (tk == '(' && (d->class == CSys || d->class == CFun || d->class == 0)) {   // direct call
      next(); n = 0;
      while (tk != ')') { expr(Assign); if (n < d->nptypes) assignconv(d->ptypes[n]); push(); n = n + 1; if (tk == ',') next(); }
      next();
      if (d->class == CSys) call_sys(d->val, n);
      else if (d->class == CFun || d->class == 0) call_fn(d, n);
      else die("bad function call");
      ty = d->type;
    }
    else if (d->class == CNum) { imm(0, d->val); ty = d->type; }
    else if (d->class == 6) { lea_var(d); ty = d->type; }   // global array: address
    else if (d->class == CStaArr) { lea_data(0, d->val, d->val); ty = d->type; }   // static array: .Ldata address (with s 0 the printed n is the whole offset)
    else if (d->class == 9) { lea_ext(d); ty = d->type; }        // extern array: address
    else if (d->class == CFun || d->class == CSys) { fn_addr(d); ty = fntype_p(d->type, d->ptypes, d->ptypes ? d->nptypes : -1); }   // function as a value
    else {
      if (d->class == CLoc) lea_loc(d->val);
      else if (d->class == CGlo) lea_var(d);
      else if (d->class == 7) lea_ext(d);
      else if (d->class == CSta) lea_data(0, d->val, d->val);   // static: .Ldata + off, no label (with s 0 the printed n is the whole offset)
      else die("undefined variable");
      ty = d->type;
      load(ty);
    }
  }
  else if (tk == '(') {
    next();
    if (is_type()) {                          // cast
      t = declarator(parse_type());
      if (tk != ')') die(") expected in cast"); next();
      expr(Inc); castto(t); ty = t;
    } else { expr_comma(); if (tk != ')') die(") expected"); next(); }
  }
  else if (tk == Mul) {                         // dereference
    next(); expr(Inc);
    if (isfndes(ty)) {}                         // *f is f
    else {
      if (ty < PTR) die("bad dereference");
      ty = ty - PTR; load(ty); decay();
    }
  }
  else if (tk == And) {                         // address-of
    next(); expr(Inc);
    if (isfndes(ty)) ty = ty + PTR;             // &f: the address is already in r0
    else { lvalue(); ty = ty + PTR; }
  }
  else if (tk == '!') { next(); expr(Inc); test0(); itcc(CC_EQ); ty = TINT; }
  else if (tk == '~') { next(); expr(Inc); if (isflt(ty)) die("~ on a float"); ins("mvns\tr0, r0"); ty = arith(ty, ty); }
  else if (tk == Add) { next(); expr(Inc); ty = arith(ty, ty); }
  else if (tk == Sub) {
    next();
    if (tk == Num) { imm(0, numty == TFLOAT ? ival ^ (1 << 31) : -ival); ty = numty; next(); }   // a float flips its sign bit
    else {
      expr(Inc);
      if (isflt(ty)) { ins("vmov\ts0, r0"); ins("vneg.f32\ts0, s0"); ins("vmov\tr0, s0"); }
      else { ins("negs\tr0, r0"); ty = arith(ty, ty); }
    }
  }
  else if (tk == Inc || tk == Dec) {            // prefix
    t = tk; next(); expr(Inc); lvalue();
    if (isflt(ty)) { ins("mov\tr1, r0"); load_r1(ty); fstep(t == Inc); ins("vmov\tr0, s0"); store(ty); }
    else {
      sz = ty >= PTR ? tysize(ty - PTR) : 1;
      ins("mov\tr1, r0");
      load_r1(ty);
      if (t == Dec) sz = -sz;
      if (sz >= 0 && sz < 4096) addi12(0, 0, sz); else if (sz < 0 && -sz < 4096) subi12(0, 0, -sz); else { imm(2, sz); ins("adds\tr0, r0, r2"); }
      store(ty);
    }
  }
  else die("bad expression");

  // binary operators, precedence climbing
  while (tk >= lev || (tk == '(' && isfn(ty))) {   // a call binds tightest, like [ and ->
    t = ty;
    if (tk == '(') call_ind(t);
    else if (tk == Assign) {
      if (t >= TSTRUCT && t < PTR) die("struct assignment is not supported");
      next(); lvalue(); push(); expr(Assign); assignconv(t); pop1(); store(t); ty = t;
    }
    else if (tk == Cond) {
      next(); test0(); at = bc_w(CC_EQ);
      expr_comma();
      if (tk != ':') die(": expected"); next();
      t = ty; at2 = b_w(-1); patch_here(at);
      expr(Cond);
      if ((isflt(t) || isflt(ty)) && t < PTR && ty < PTR && t != ty) {   // the int branch becomes float
        if (!isflt(ty)) cvt(ty, TFLOAT);
        if (!isflt(t)) { m = b_w(-1); patch_here(at2); cvt(t, TFLOAT); at2 = m; }
        ty = TFLOAT;
      }
      patch_here(at2);
    }
    else if (tk == Lor) { next(); truth(); at = bc_w(CC_NE); expr(Lan); truth(); patch_here(at); itcc(CC_NE); ty = TINT; }
    else if (tk == Lan) { next(); truth(); at = bc_w(CC_EQ); expr(Or); truth(); patch_here(at); itcc(CC_NE); ty = TINT; }
    else if (tk >= AddA && tk <= ShrA) {        // compound assignment
      n = tk; next(); lvalue(); push(); load(t); push(); expr(Assign); pop1();
      if (n == AddA) n = Add; else if (n == SubA) n = Sub; else if (n == MulA) n = Mul; else if (n == DivA) n = Div; else if (n == ModA) n = Mod;
      else if (n == AndA) n = And; else if (n == OrA) n = Or; else if (n == XorA) n = Xor; else if (n == ShlA) n = Shl; else n = Shr;
      binop(n, t);                              // r0 = old op rhs
      assignconv(t); pop1(); store(t); ty = t;
    }
    else if (tk == Or)  { next(); push(); expr(Xor); pop1(); binop(Or, t); }
    else if (tk == Xor) { next(); push(); expr(And); pop1(); binop(Xor, t); }
    else if (tk == And) { next(); push(); expr(Eq);  pop1(); binop(And, t); }
    else if (tk >= Eq && tk <= Ge) { n = tk; next(); push(); expr(n <= Ne ? Lt : Shl); pop1(); relop(n, t); ty = TINT; }
    else if (tk == Shl) { next(); push(); expr(Add); pop1(); binop(Shl, t); }
    else if (tk == Shr) { next(); push(); expr(Add); pop1(); binop(Shr, t); }
    else if (tk == Add) { next(); push(); expr(Mul); pop1(); binop(Add, t); }
    else if (tk == Sub) { next(); push(); expr(Mul); pop1(); binop(Sub, t); }
    else if (tk == Mul) { next(); push(); expr(Inc); pop1(); binop(Mul, t); }
    else if (tk == Div) { next(); push(); expr(Inc); pop1(); binop(Div, t); }
    else if (tk == Mod) { next(); push(); expr(Inc); pop1(); binop(Mod, t); }
    else if ((tk == Inc || tk == Dec) && isflt(ty)) {   // postfix on a float: r0 keeps the old value
      lvalue(); ins("mov\tr1, r0"); load_r1(ty); fstep(tk == Inc); ins("vmov\tr2, s0"); store_r2(ty); next();
    }
    else if (tk == Inc || tk == Dec) {          // postfix
      lvalue();
      sz = ty >= PTR ? tysize(ty - PTR) : 1;
      if (tk == Dec) sz = -sz;
      ins("mov\tr1, r0");
      load_r1(ty);
      if (sz >= 0 && sz < 4096) addi12(2, 0, sz); else if (sz < 0 && -sz < 4096) subi12(2, 0, -sz); else { imm(2, sz); ins("adds\tr2, r0, r2"); }
      store_r2(ty);
      next();
    }
    else if (tk == Dot || tk == Arrow) {
      if (tk == Arrow) { if (ty < PTR) die("-> on non-pointer"); ty = ty - PTR; }
      next(); if (tk != Id) die("member expected");
      m = find_mem(ty); next();
      if (mems[m].off) addi12(0, 0, mems[m].off);
      ty = mems[m].type;
      lastload = 0;                             // a struct or array member emits no load: do not let lvalue() undo the pointer's
      if (ty & ARR) ty = ty & ~ARR; else load(ty);
    }
    else if (tk == Brak) {
      next(); push(); expr_comma(); pop1();
      if (tk != ']') die("] expected"); next();
      if (t < PTR) die("indexing a non-pointer");
      scale(tysize(t - PTR));
      ins("adds\tr0, r1, r0");
      ty = t - PTR; load(ty); decay();
    }
    else die("bad operator");
  }
  return 0;
}

// a, b, c: evaluate left to right, the value is the last one. Used where C takes a
// full expression; arguments and initializers use expr(Assign), so a comma there
// still separates them.
int expr_comma() {
  expr(Assign);
  while (tk == ',') { next(); expr(Assign); }
  return 0;
}
