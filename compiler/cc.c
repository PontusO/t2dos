// cc.c - a tiny single-pass C compiler emitting Thumb-2 assembly for Cortex-M33
// (RP2350). Written in the subset of C it compiles, so it can build itself.
//
// Build from the repo root: make (gcc builds compiler/cc from compiler/*.c; cc
// builds everything else, itself included). The files and what they share: cc.h.
// Use:  cc [-o out] [-S|-c] [-O] [-mshort-data] [-I dir] [-D name[=v]] [-L dir] [-l name] file.c|file.s|file.o|file.a...
//   cc compiles each .c to assembly itself, then runs as on it and ld on all the objects
//   (/bin/as.bin and /bin/ld.bin on the board; on the PC build/host/as and build/host/ld,
//   or the ones in $T2_TOOLS). -S stops at the .s, -c at the .o; else the output is a .bin.
//
// Generated code model:
//   r7 = frame pointer, r8 = host syscall entry, r9 = data/bss base.
//   Expression results in r0.  All args passed on the stack, left to right
//   (first arg deepest), caller pops.  Position independent: data is addressed
//   from r9 (NAME+offset for a global of this file, .Ldata+offset for string
//   literals), function addresses PC relative (fnaddr).

#include "cc.h"

// ------------------------------------------------------------------ constant expressions
// Evaluated while parsing, for array sizes, case labels and enum values:
// numbers, enum and macro constants, sizeof(type), ( ), unary - + ~ !, binary
// operators and ?:.
int cexpr();
int cunary() {
  int v; int t;
  if (tk == Num) { if (numty == TFLOAT) die("integer constant expected"); v = ival; next(); return v; }
  if (tk == Id && id->class == CNum) { v = id->val; next(); return v; }
  if (tk == Id && pp_if) { next(); return 0; }          // #if FOO with FOO not a macro
  if (tk == '(') { next(); v = cexpr(); if (tk != ')') die(") expected"); next(); return v; }
  if (tk == Sub) { next(); return -cunary(); }
  if (tk == Add) { next(); return cunary(); }
  if (tk == '~') { next(); return ~cunary(); }
  if (tk == '!') { next(); return !cunary(); }
  if (tk == Sizeof) {
    next(); if (tk != '(') die("( expected after sizeof"); next();
    t = declarator(parse_type()); v = tysize(t); if (darr >= 0) v = v * darr;
    if (tk != ')') die(") expected"); next();
    return v;
  }
  die("constant expected"); return 0;
}
int cprec(int op) {
  if (op == Mul || op == Div || op == Mod) return 10;
  if (op == Add || op == Sub) return 9;
  if (op == Shl || op == Shr) return 8;
  if (op == Lt || op == Gt || op == Le || op == Ge) return 7;
  if (op == Eq || op == Ne) return 6;
  if (op == And) return 5;
  if (op == Xor) return 4;
  if (op == Or) return 3;
  if (op == Lan) return 2;
  if (op == Lor) return 1;
  return 0;
}
int cbin(int minp) {
  int v; int r; int op; int pr;
  v = cunary();
  while ((pr = cprec(tk)) && pr >= minp) {
    op = tk; next(); r = cbin(pr + 1);
    if ((op == Div || op == Mod) && !r) die("division by zero in a constant");
    if (op == Mul) v = v * r; else if (op == Div) v = v / r; else if (op == Mod) v = v % r;
    else if (op == Add) v = v + r; else if (op == Sub) v = v - r;
    else if (op == Shl) v = v << r; else if (op == Shr) v = v >> r;
    else if (op == Lt) v = v < r; else if (op == Gt) v = v > r; else if (op == Le) v = v <= r; else if (op == Ge) v = v >= r;
    else if (op == Eq) v = v == r; else if (op == Ne) v = v != r;
    else if (op == And) v = v & r; else if (op == Xor) v = v ^ r; else if (op == Or) v = v | r;
    else if (op == Lan) v = v && r; else v = v || r;
  }
  return v;
}
int cexpr() {
  int v; int a; int b;
  v = cbin(1);
  if (tk == Cond) {
    next(); a = cexpr(); if (tk != ':') die(": expected"); next(); b = cexpr();
    return v ? a : b;
  }
  return v;
}

// array row type: n elements of type et (shared between declarations)
int mkrow(int et, int n) {
  int i;
  i = 0; while (i < nstructs) { if (starr[i] == et && stcnt[i] == n) return TSTRUCT + i; i = i + 1; }
  i = newstruct(); starr[i] = et; stcnt[i] = n; stsize[i] = tysize(et) * n;
  return TSTRUCT + i;
}
// [N][M]...: dims[0..ndims); only the first may be [] (-2)
int parse_dims() {
  ndims = 0;
  while (tk == Brak) {
    if (ndims >= 8) die("too many array dimensions");
    array_dim(); dims[ndims] = darr; ndims = ndims + 1;
  }
  return 0;
}
// element type of dims[from - 1]: rows of dims[from..ndims), innermost first
int rows_of(int t, int from) {
  int k;
  k = ndims - 1;
  while (k >= from) { if (dims[k] < 0) die("array size expected"); t = mkrow(t, dims[k]); k = k - 1; }
  return t;
}
// [N] or [] (size from the initializer: darr = -2)
int array_dim() {
  next();
  if (tk == ']') { darr = -2; next(); return 0; }
  darr = cexpr();
  if (darr < 0) die("negative array size");
  if (tk != ']') die("] expected"); next();
  return 0;
}
// ( types ) of a function pointer into pl[]; returns how many, -1 for ( )
int param_types(int *pl) {
  struct Sym *sv_name; int sv_arr; int n; int t; int isvoid;
  if (tk != '(') die("( expected");
  sv_name = dname; sv_arr = darr;           // declarator() is used for the parameters too
  next(); n = 0;
  if (tk == ')') { next(); dname = sv_name; darr = sv_arr; return -1; }
  while (tk != ')') {
    if (!is_type()) die("parameter type expected");
    isvoid = tk == Void;
    t = declarator(parse_type());
    if (darr != -1) t = t + PTR;
    if (isvoid && t == TCHAR && tk == ')' && !n) break;   // (void)
    if (n < 16) pl[n] = t;
    n = n + 1;
    if (tk == ',') next(); else if (tk != ')') die(", or ) expected");
  }
  next();
  dname = sv_name; darr = sv_arr;
  return n < 16 ? n : 16;
}
// Declarator after a base type: stars, then a name (or none, for casts and
// prototypes) and optional [N][M]..., or ( *name[N] ) followed by ( params ) for
// a function pointer or by [M]... for a pointer to an array.
// Returns the type (of an element, for arrays); sets dname (0 if no name) and
// darr (outermost length, -1 if not an array, -2 for []).
int declarator(int t) {
  int ps; int inner; int pl[16];
  while (tk == Mul) { next(); t = t + PTR; }
  dname = 0; darr = -1;
  if (tk == '(') {
    next(); ps = 0;
    while (tk == Mul) { next(); ps = ps + 1; }
    if (!ps) die("only function and array pointers are supported: (*name)");
    if (tk == Id) { dname = id; next(); }
    if (tk == Brak) array_dim();
    if (tk != ')') die(") expected in declarator"); next();
    if (tk == Brak) {                           // int (*p)[4]: pointer to array
      inner = darr; parse_dims(); darr = inner;
      return rows_of(t, 0) + ps * PTR;
    }
    inner = param_types(pl);
    return fntype_p(t, pl, inner) + ps * PTR;
  }
  if (tk == Id) { dname = id; next(); }
  if (tk == Brak) { parse_dims(); t = rows_of(t, 1); darr = dims[0]; }
  return t;
}

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
    else if (d->class == 9) { lea_ext(d); ty = d->type; }        // extern array: address
    else if (d->class == CFun || d->class == CSys) { fn_addr(d); ty = fntype_p(d->type, d->ptypes, d->ptypes ? d->nptypes : -1); }   // function as a value
    else {
      if (d->class == CLoc) lea_loc(d->val);
      else if (d->class == CGlo) lea_var(d);
      else if (d->class == 7) lea_ext(d);
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

// ------------------------------------------------------------------ initializers
// Globals: constants go straight into the data image. Anything else (strings for
// pointers, addresses, function pointers, expressions) becomes code in .Linit, the
// file's run-before-main function (.t2init; ld's entry stub calls it). Locals: code at
// the declaration; arrays and structs are zeroed first. Nested aggregates need
// their own braces; a char array also takes a string.
int save_lex() {
  sv_p = p; sv_tk = tk; sv_ival = ival; sv_id = id; sv_line = line; sv_dpos = dpos;
  sv_pb = pb; sv_pb_tk = pb_tk; sv_pb_ival = pb_ival; sv_pb_id = pb_id; sv_pb_numty = pb_numty; sv_numty = numty; sv_slen = slen;
  sv_fdepth = fdepth; memcpy(sv_fp, fstack_p, 4 * fdepth); memcpy(sv_fl, fstack_line, 4 * fdepth); memcpy(sv_fm, fstack_mac, 4 * fdepth);
  return 0;
}
int restore_lex() {                    // lexing strings wrote to data: clear what lookahead used
  if (dpos > sv_dpos) memset(data + sv_dpos, 0, dpos - sv_dpos);
  p = sv_p; tk = sv_tk; ival = sv_ival; id = sv_id; line = sv_line; dpos = sv_dpos;
  pb = sv_pb; pb_tk = sv_pb_tk; pb_ival = sv_pb_ival; pb_id = sv_pb_id; pb_numty = sv_pb_numty; numty = sv_numty; slen = sv_slen;
  fdepth = sv_fdepth; memcpy(fstack_p, sv_fp, 4 * fdepth); memcpy(fstack_line, sv_fl, 4 * fdepth); memcpy(fstack_mac, sv_fm, 4 * fdepth);
  return 0;
}
int init_addr(int off) { if (ikind) lea_loc(ibase + off); else lea_data(isym, off, ibase + off); return 0; }
int init_code() { if (!ikind) init_open = 1; return 0; }   // global initializer code: it goes to .Linit
int init_close() { if (init_open) { tobuf = 1; flush(); tobuf = 0; init_open = 0; } return 0; }
int poke(int off, int v, int t) {
  data[off] = v;
  if (tysize(t) >= 2) data[off + 1] = v >> 8;
  if (tysize(t) == 4) { data[off + 2] = v >> 16; data[off + 3] = v >> 24; }
  return 0;
}
// [-] number | enum or #define name, followed by , } or ;  ->  1, value in ival. Else 0, nothing consumed.
int const_init(int t) {
  int neg; int v; int vt;
  save_lex(); neg = 0;
  if (tk == Sub) { next(); neg = 1; }
  if (tk == Num) { v = ival; vt = numty; } else if (tk == Id && id->class == CNum) { v = id->val; vt = TINT; } else { restore_lex(); return 0; }
  next();
  if (tk != ',' && tk != '}' && tk != ';') { restore_lex(); return 0; }
  if (neg) { if (vt == TFLOAT) v = v ^ (1 << 31); else v = -v; }
  if (isflt(t) && vt != TFLOAT) v = int_to_float(v, vt == TUINT);   // float f = 3;
  else if (!isflt(t) && t < PTR && vt == TFLOAT) v = float_to_int(v);  // int i = 2.5;
  ival = v;
  return 1;
}
// number of elements in the { ... } list (or string) that starts here; consumes nothing
int count_init() {
  int depth; int paren; int n; int last;
  if (tk == Str) return slen;
  if (tk != '{') die("{ expected");
  save_lex();
  depth = 0; paren = 0; n = 0; last = 0;
  while (1) {
    if (!tk) die("unexpected eof in initializer");
    if (tk == '{') { depth = depth + 1; if (depth == 1) { next(); if (tk != '}') n = 1; continue; } }
    else if (tk == '}') { depth = depth - 1; if (!depth) break; }
    else if (tk == '(') paren = paren + 1;
    else if (tk == ')') paren = paren - 1;
    else if (tk == ',' && depth == 1 && !paren) n = n + 1;
    last = tk; next();
  }
  if (last == ',') n = n - 1;          // trailing comma
  restore_lex();
  return n;
}
int init_elem(int t, int off);
int init_scalar(int t, int off) {
  if (tk == '{') {                     // int x = { 5 };
    next(); init_scalar(t, off); if (tk == ',') next();
    if (tk != '}') die("} expected"); next();
    return 0;
  }
  if (!ikind && const_init(t)) { poke(ibase + off, ival, t); return 0; }
  init_code(); init_addr(off); push(); expr(Assign); assignconv(t); pop1(); store(t);
  return 0;
}
int init_array(int et, int cnt, int off) {  // returns the number of elements given
  int i; int es; int n;
  es = tysize(et);
  if (tk == Str && (et == TCHAR || et == TUCHAR)) {   // char s[N] = "..."
    n = slen;                               // with the terminating 0
    if (n - 1 > cnt) die("string longer than the array");
    if (n > cnt) n = cnt;                   // char s[3] = "abc": no terminator, as in C
    if (ikind) {                            // memcpy(dst, literal, n)
      init_addr(off); push(); lea_glo(ival); push(); imm(0, n); push(); call_sys(SYS_memcpy, 3);
    }
    else { i = 0; while (i < n) { data[ibase + off + i] = data[ival + i]; i = i + 1; } }
    next();
    return n;
  }
  if (tk != '{') die("{ expected for array initializer");
  next(); i = 0;
  while (tk != '}') {
    if (i >= cnt) die("too many initializers");
    init_elem(et, off + i * es);
    i = i + 1;
    if (tk == ',') next(); else if (tk != '}') die(", or } expected");
  }
  next();
  return i;
}
int init_struct(int t, int off) {
  int order[256]; int n; int m; int mt;
  m = stfirst[t - TSTRUCT]; n = 0;            // members are listed last first
  while (m >= 0) { if (n >= 256) die("struct too big to initialize"); order[n] = m; n = n + 1; m = mems[m].next; }
  if (tk != '{') die("{ expected for struct initializer");
  next();
  while (tk != '}') {
    if (n <= 0) die("too many initializers");
    n = n - 1; m = order[n]; mt = mems[m].type;
    if (mt & ARR) init_array((mt & ~ARR) - PTR, mems[m].cnt, off + mems[m].off);
    else init_elem(mt, off + mems[m].off);
    if (tk == ',') next(); else if (tk != '}') die(", or } expected");
  }
  next();
  return 0;
}
int init_elem(int t, int off) {
  if (isrow(t)) { init_array(rowelem(t), rowcnt(t), off); return 0; }
  if (t >= TSTRUCT && t < PTR) return init_struct(t, off);
  return init_scalar(t, off);
}
int init_zero(int n) {                       // local aggregate: memset(storage, 0, n)
  init_addr(0); push(); imm(0, 0); push(); imm(0, n); push(); call_sys(SYS_memset, 3);
  return 0;
}

// ------------------------------------------------------------------ statements
int newlocal(struct Sym *s, int t, int size) {
  s->hclass = s->class; s->htype = s->type; s->hval = s->val;
  loc = loc + ((size + 3) & -4);
  if (loc > maxloc) maxloc = loc;
  s->class = CLoc; s->type = t; s->val = -loc;
  if (optim) {                              // for promote(): words of arrays and structs
    if (t >= PTR && !(t & ARR) && size > 4) markbad(-loc + 4, -loc + size);   // array: its pointer word, then the storage
    else if (size != 4 || (t & ARR) || (t < PTR && t != TINT && t != TUINT && t != TFLOAT)) markbad(-loc, -loc + size);
    else if (t == TFLOAT && slotix(-loc) >= 0) slotf[slotix(-loc)] = 1;
  }
  return 0;
}

int stmt() {
  int at; int at2; int sv_brk; int sv_cont; int sv_tgt; int body; int t; int bt; int n; int hi; struct Sym *d;
  if (tk == If) {
    next(); if (tk != '(') die("( expected"); next();
    expr_comma(); if (tk != ')') die(") expected"); next();
    test0(); at = bc_w(CC_EQ);                  // false: to the else
    stmt();
    if (tk == Else) { next(); at2 = b_w(-1); patch_here(at); stmt(); patch_here(at2); }
    else patch_here(at);
  }
  else if (tk == While) {
    next(); if (tk != '(') die("( expected"); next();
    sv_brk = brk_fix; sv_cont = cont_fix; sv_tgt = cont_tgt;
    brk_fix = -1; cont_fix = -1; cont_tgt = here();
    expr_comma(); if (tk != ')') die(") expected"); next();
    test0(); at = bc_w(CC_EQ);
    nbrk = nbrk + 1; ncont = ncont + 1; stmt(); nbrk = nbrk - 1; ncont = ncont - 1;
    b_w(cont_tgt); patch_here(at);
    chain_here(brk_fix);
    brk_fix = sv_brk; cont_fix = sv_cont; cont_tgt = sv_tgt;
  }
  else if (tk == For) {
    next(); if (tk != '(') die("( expected"); next();
    if (tk != ';') expr_comma();
    if (tk != ';') die("; expected"); next();
    sv_brk = brk_fix; sv_cont = cont_fix; sv_tgt = cont_tgt;
    brk_fix = -1; cont_fix = -1;
    at = here();                                // condition
    if (tk != ';') { expr_comma(); test0(); at2 = bc_w(CC_EQ); } else at2 = -1;
    if (tk != ';') die("; expected"); next();
    body = b_w(-1);                             // skip increment first time
    cont_tgt = here();                          // increment
    if (tk != ')') expr_comma();
    if (tk != ')') die(") expected"); next();
    b_w(at);
    patch_here(body);
    nbrk = nbrk + 1; ncont = ncont + 1; stmt(); nbrk = nbrk - 1; ncont = ncont - 1;
    b_w(cont_tgt);
    if (at2 >= 0) patch_here(at2);
    chain_here(brk_fix);
    brk_fix = sv_brk; cont_fix = sv_cont; cont_tgt = sv_tgt;
  }
  else if (tk == Switch) {
    next(); if (tk != '(') die("( expected"); next();
    expr_comma(); if (tk != ')') die(") expected"); next();
    if (tk != '{') die("{ expected"); next();
    sv_brk = brk_fix; brk_fix = -1;
    loc = loc + 4; if (loc > maxloc) maxloc = loc; n = -loc;        // temp slot for the switch value
    push(); lea_loc(n); pop1(); ins("str\tr1, [r0]");
    at = b_w(-1);                                                 // -> first test (case chain head)
    at2 = -1;                                                     // default position
    body = -1;                                                    // fallthrough skip
    while (tk != '}') {
      if (tk == Case) {
        next();
        t = cexpr();
        if (tk != ':') die(": expected"); next();
        body = b_w(-1);                                           // previous body falls through over the test
        patch_here(at);                                           // failed previous test lands here
        lea_loc(n); ins("ldr\tr0, [r0]"); imm(1, t); ins("cmp\tr0, r1");
        at = bc_w(CC_NE);                                         // -> next test
        patch_here(body);
      }
      else if (tk == Default) { next(); if (tk != ':') die(": expected"); next(); at2 = here(); }
      else { if (!tk) die("unexpected eof"); nbrk = nbrk + 1; stmt(); nbrk = nbrk - 1; }
    }
    next();
    if (at2 >= 0) patch_b(at, at2); else patch_here(at);
    chain_here(brk_fix);
    brk_fix = sv_brk;
  }
  else if (tk == Break) { if (!nbrk) die("break outside a loop or switch"); next(); if (tk != ';') die("; expected"); next(); brk_fix = chain_add(brk_fix, b_w(-1)); }
  else if (tk == Continue) { if (!ncont) die("continue outside a loop"); next(); if (tk != ';') die("; expected"); next(); b_w(cont_tgt); }
  else if (tk == Return) {
    next();
    if (tk != ';') { expr_comma(); castto(fnrty); }   // unsigned char f() { return 300; } gives 44
    if (tk != ';') die("; expected"); next();
    ret_fix = chain_add(ret_fix, b_w(-1));
  }
  else if (tk == '{') {
    next();
    while (tk != '}') { if (!tk) die("unexpected eof"); stmt(); }
    next();
  }
  else if (tk == ';') { next(); }
  else if (is_type()) {                       // local declaration
    bt = parse_type();
    while (tk != ';') {
      t = declarator(bt);
      if (!dname) die("local name expected");
      d = dname; hi = 0;
      if (tk == Assign) { next(); hi = 1; }
      if (darr == -2) { if (!hi) die("array size expected"); darr = count_init(); }
      n = tysize(t);
      if (darr >= 0) {                          // local array: slot holds pointer to the storage that follows it
        n = n * darr;
        newlocal(d, t + PTR, n + 4);
        lea_loc(d->val); addi12(1, 0, 4); ins("str\tr1, [r0]");   // slot = slot + 4
        if (hi) { ikind = 1; ibase = d->val + 4; init_zero(n); init_array(t, darr, 0); }
      } else {
        newlocal(d, t, n);
        if (hi) {
          ikind = 1; ibase = d->val;
          if (t >= TSTRUCT && t < PTR) { init_zero(n); init_struct(t, 0); } else init_scalar(t, 0);
        }
      }
      if (tk == ',') next();
    }
    next();
  }
  else { expr_comma(); if (tk != ';') die("; expected"); next(); }
  return 0;
}

// ------------------------------------------------------------------ top level
int parse_global() {
  int bt; int t; struct Sym *s; int n; int i; int frame_at; int sz; int hi; int ext;
  if (tk == Enum) {
    next(); if (tk == Id) next();
    if (tk == '{') {
      next(); i = 0;
      while (tk != '}') {
        if (tk != Id) die("enum name expected");
        s = id; next();
        if (tk == Assign) { next(); i = cexpr(); }
        s->class = CNum; s->type = TINT; s->val = i; i = i + 1;
        if (tk == ',') next();
      }
      next();
    }
    if (tk != ';') die("; expected"); next();
    return 0;
  }
  if (tk == Typedef) {
    next(); bt = declarator(parse_type());
    if (!dname || darr >= 0) die("typedef name expected");
    dname->class = 8; dname->type = bt;
    if (tk != ';') die("; expected"); next();
    return 0;
  }
  ext = 0;
  if (tk == Extern) { ext = 1; next(); }        // a declaration: the storage is in another file
  bt = parse_type();
  while (tk != ';') {
    t = declarator(bt);
    if (!dname) die("global name expected");
    s = dname;
    if (tk == '(' && darr < 0) {                // function (a function pointer's params were consumed)
      if (s->class == CFun && s->val >= 0) die("duplicate function");
      s->type = t; next();
      // prototype or definition? parse params either way
      n = 0; loc = 0; maxloc = 0;
      if (optim) {
        if (!slotk) { slotk = malloc(512 * 4); slotf = malloc(512 * 4); }
        i = 0; while (i < 512) { slotk[i] = 0; slotf[i] = 0; i = i + 1; }
      }
      if (!s->ptypes) s->ptypes = malloc(32 * 4);
      while (tk != ')') {
        bt = declarator(parse_type());
        if (darr != -1) bt = bt + PTR;          // int a[N] or a[] as a parameter is int *a
        if (dname) {
          if (dname->class == CLoc) die("duplicate param");
          dname->hclass = dname->class; dname->htype = dname->type; dname->hval = dname->val;
          dname->class = CLoc; dname->type = bt; dname->val = n;   // index, fixed up below
        }
        if (n < 32) s->ptypes[n] = bt;
        n = n + 1;
        if (tk == ',') next();
      }
      next();
      s->nptypes = n < 32 ? n : 32;
      if (tk == ';') {                          // prototype
        next();
        i = 0; while (i < nsyms) { if (syms[i].class == CLoc) { syms[i].class = syms[i].hclass; syms[i].type = syms[i].htype; syms[i].val = syms[i].hval; } i = i + 1; }
        if (s->class != CFun) { s->class = CFun; s->val = -1; }
        return 0;
      }
      if (tk != '{') die("{ expected");
      // fix param offsets: param i at r7 + 8 + 4*(n-1-i)
      i = 0; while (i < nsyms) { if (syms[i].class == CLoc) syms[i].val = 8 + 4 * (n - 1 - syms[i].val); i = i + 1; }
      if (optim) { i = 0; while (i < nsyms) { if (syms[i].class == CLoc && syms[i].type == TFLOAT && slotix(syms[i].val) >= 0) slotf[slotix(syms[i].val)] = 1; i = i + 1; } }
      s->class = CFun; s->val = 0; nparams = n; fnrty = t;   // val >= 0: defined
      oput("\t.global\t"); oname(s); oput("\n"); oname(s); oput(":\n");
      ins("push\t{r7, lr}");
      if (optim) pvpush = lnew(L_DEL);         // promote()'s vpush
      ins("mov\tr7, sp");
      frame_at = nlines; imm2(1, 0);            // movw+movt r1,#frame (filled in below)
      ins("sub.w\tsp, sp, r1");
      phold = -1;
      if (optim) { phold = nlines; i = 0; while (i < 21) { lnew(L_DEL); i = i + 1; } }  // promote()'s parameter loads
      ret_fix = -1;
      stmt();
      chain_here(ret_fix);
      ins("mov\tsp, r7");
      if (optim) pvpop = lnew(L_DEL);          // promote()'s vpop
      ins("pop\t{r7, pc}");
      sz = (maxloc + 7) & -8;
      i = nlines; imm2(1, sz); ltext[frame_at] = ltext[i]; ltext[frame_at + 1] = ltext[i + 1]; nlines = i;
      if (optim) { prepro = 1; peep(); licm_loops(); prepro = 0; peep(); promote(); sprel(frame_at, sz); frameless(frame_at, sz); trimframe(frame_at, sz); }
      flush();
      npro = 0; phold = -1;
      // restore shadowed symbols
      i = 0; while (i < nsyms) { if (syms[i].class == CLoc) { syms[i].class = syms[i].hclass; syms[i].type = syms[i].htype; syms[i].val = syms[i].hval; } i = i + 1; }
      return 0;
    }
    if (ext) {                                  // extern variable or array
      if (tk == Assign) die("extern with an initializer");
      if (s->class == 0) {
        if (darr != -1) { s->class = 9; s->type = t + PTR; } else { s->class = 7; s->type = t; }
      }
      else if (s->class != CGlo && s->class != 6 && s->class != 7 && s->class != 9) die("duplicate global");
      if (tk == ',') next();
      continue;
    }
    // global variable (declared extern before, it is now defined here: later references are normal)
    if (s->class && s->class != 7 && s->class != 9) die("duplicate global");
    if (nglob >= 8192) die("too many globals");
    hi = 0;
    if (tk == Assign) { next(); hi = 1; }
    if (darr == -2) { if (!hi) die("array size expected"); darr = count_init(); }
    n = tysize(t);
    if (darr >= 0) {
      n = n * darr;
      // global array: the symbol is the storage itself; treat name as pointer constant
      dpos = (dpos + 3) & -4;
      s->class = 6; s->type = t + PTR; s->val = dpos;     // val = address of storage
      gsym[nglob] = s; goff[nglob] = dpos; nglob = nglob + 1;
      dpos = dpos + n;
    } else {
      dpos = (dpos + 3) & -4;
      s->class = CGlo; s->type = t; s->val = dpos;
      gsym[nglob] = s; goff[nglob] = dpos; nglob = nglob + 1;
      dpos = dpos + n;
    }
    if (hi) {
      ikind = 0; ibase = s->val; isym = s;
      if (darr >= 0) init_array(t, darr, 0); else init_elem(t, 0);
      init_close();
    }
    if (tk == ',') next();
  }
  next();
  return 0;
}

// the data image: .Ldata at its start, a global label at each variable, 16 bytes a line
int write_data() {
  int i; int g; int n;
  oput("\t.data\n.Ldata:\n");
  i = 0; g = 0; n = 0;
  while (1) {
    while (g < nglob && goff[g] <= i) {
      if (n) { oput("\n"); n = 0; }
      oput("\t.global\t"); oname(gsym[g]); oput("\n"); oname(gsym[g]); oput(":\n");
      g = g + 1;
    }
    if (i >= dpos) break;
    if (n) oput(", "); else oput("\t.byte\t");
    oput(dec(data[i] & 255)); n = n + 1;
    if (n == 16) { oput("\n"); n = 0; }
    i = i + 1;
  }
  if (n) oput("\n");
  return 0;
}


// ------------------------------------------------------------------ one C file to assembly
char **dnames; char **dvals; int ndefs;          // -D NAME[=value]

// Compile src to the assembly file out. Every table is allocated here and freed at the
// end, so the driver can compile several files in one run and as and ld get the memory.
int compile(char *src, char *out) {
  int i; struct Sym *s;
  tk = 0; ival = 0; line = 0; id = 0; nsyms = 0; nmems = 0; nstructs = 0; ndims = 0; dpos = 0; lastload = 0;
  ty = 0; loc = 0; maxloc = 0; ret_fix = 0; brk_fix = 0; cont_fix = 0; cont_tgt = 0; nbrk = 0; ncont = 0; nparams = 0; fnrty = 0;
  nfn = 0; dname = 0; darr = 0; pb = 0; numty = 0; slen = 0; ikind = 0; ibase = 0; init_open = 0;
  fdepth = 0; ifdepth = 0; pp_if = 0; tdepth = 0; nsrcbuf = 0;
  srcname = src; outname = out; ofd = -1;
  syms = malloc(8192 * sizeof(struct Sym)); nsyms = 0;
  hhead = malloc(4096 * 4); hnext = malloc(8192 * 4);
  i = 0; while (i < 4096) { hhead[i] = 0; i = i + 1; }
  mems = malloc(2048 * sizeof(struct Mem)); nmems = 0;
  stsize = malloc(256 * 4); stfirst = malloc(256 * 4); nstructs = 0;
  starr = malloc(256 * 4); stcnt = malloc(256 * 4); dims = malloc(8 * 4);
  data = malloc(1048576); dpos = 0;
  memset(data, 0, 1048576);                     // the data image starts zeroed (malloc may reuse memory)
  fstack_p = malloc(64 * 4); fstack_line = malloc(64 * 4); fstack_mac = malloc(64 * 4); fdepth = 0;
  sv_fp = malloc(64 * 4); sv_fl = malloc(64 * 4); sv_fm = malloc(64 * 4); mbuf = malloc(65536);
  tstack = malloc(64 * 4); tdepth = 0; bn = malloc(256 * 4); bnl = 1;
  fnret = malloc(512 * 4); fnparams = malloc(512 * 4); fnnp = malloc(512 * 4); nfn = 0;
  lpool = malloc(262144); lppos = 0; nlines = 0; nlabels = 0;
  lkind = malloc(16384 * 4); ltext = malloc(16384 * 4); llab = malloc(16384 * 4); lcond = malloc(16384 * 4); lnext = malloc(16384 * 4);
  obuf = malloc(65536); opos = 0; ibuf = malloc(262144); ipos = 0; tobuf = 0;
  numbuf = malloc(16); ccbuf = malloc(4); ccname = "eqnecsccmiplvsvchilsgeltgtle";
  gsym = malloc(8192 * sizeof(struct Sym *)); goff = malloc(8192 * 4); nglob = 0;
  srcbuf = malloc(256 * sizeof(char *));
  memset(syms, 0, 8192 * sizeof(struct Sym));

  // keywords, in token enum order
  p = "break case char continue default else enum for if int return sizeof struct switch typedef void while long short signed unsigned float double";
  i = Break; while (i <= Double) { next(); id->tk = i; i = i + 1; }
  p = "extern"; next(); id->tk = Extern;
  // builtin host functions: name, syscall number (see sys.h)
  p = "exit 0 exec 1 millis 2 sleep 3 sysinfo 4 spawn 5 yield 6 jobs 7 kill 8 reboot 9 putchar 16 getchar 17 kbhit 18 conctl 19 redirect 20 open 32 close 33 read 34 write 35 seek 36 stat 37 unlink 38 rename 39 mkdir 40 rmdir 41 opendir 42 readdir 43 closedir 44 chdir 45 getcwd 46 mkpipe 47 mount 48 mountat 49 malloc 64 free 65 memset 66 memcmp 67 memcpy 68 strlen 69 strcmp 70 strcpy 71 printf 72 sprintf 73 atoi 74 strncmp 75 strchr 76 net_join 80 net_status 81 net_ip 82 connect 83 send 84 recv 85 listen 86 accept 87 sock_avail 88 wifi_save 89 wifi_saved 90 ping 91 telnetd 92 gpio_mode 96 gpio_write 97 gpio_read 98 analog_read 99 pwm_write 100 i2c_write 101 i2c_read 102 spi_transfer 103 delay_us 104 sin 112 cos 113 tan 114 asin 115 acos 116 atan 117 atan2 118 sinh 119 cosh 120 tanh 121 exp 122 log 123 log10 124 pow 125 sqrt 126 fabs 127 floor 128 ceil 129 fmod 130";
  next();
  while (tk == Id) {
    s = id; next(); s->class = CSys; s->val = ival; s->type = TINT;
    if (ival == SYS_malloc || ival == SYS_strcpy || ival == SYS_strchr || ival == SYS_getcwd || ival == SYS_net_ip) s->type = TCHAR + PTR;
    if (ival >= SYS_sin && ival <= SYS_fmod) {     // maths: float f(float) or float f(float, float)
      s->type = TFLOAT; s->ptypes = malloc(32 * 4); s->ptypes[0] = TFLOAT; s->ptypes[1] = TFLOAT;
      s->nptypes = 1; if (ival == SYS_atan2 || ival == SYS_pow || ival == SYS_fmod) s->nptypes = 2;
    }
    next();
  }
  // predefined macro and -D NAME[=value]
  define_macro("__T2CC__", "1");
  i = 0; while (i < ndefs) { define_macro(dnames[i], dvals[i]); i = i + 1; }

  p = readfile(srcname); line = 1;
  ofd = open(outname, 577, 420);                // O_WRONLY|O_CREAT|O_TRUNC, 0644
  if (ofd < 0) { printf("cannot write %s\n", outname); quit(); }
  oput("\t.syntax unified\n\t.thumb\n\t.text\n");
  next();
  while (tk) parse_global();
  if (ifdepth) die("#endif missing");

  // global initializer code: one run-before-main function (ld calls it from its entry stub)
  if (ipos) {
    oput(".Linit:\n\tpush\t{r4, lr}\n"); oflush();
    if (write(ofd, ibuf, ipos) != ipos) { printf("cannot write %s\n", outname); quit(); }
    oput("\tpop\t{r4, pc}\n\t.t2init\t.Linit\n");
  }
  write_data();
  oflush();
  if (close(ofd) < 0) { ofd = -1; printf("cannot write %s\n", outname); unlink(outname); cleanup(); exit(1); }
  ofd = -1;

  i = 0; while (i < nsyms) { if (syms[i].ptypes) free(syms[i].ptypes); i = i + 1; }
  i = 0; while (i < nfn) { if (fnparams[i]) free(fnparams[i]); i = i + 1; }
  i = 0; while (i < nsrcbuf) { free(srcbuf[i]); i = i + 1; }
  free(srcbuf); free(syms); free(hhead); free(hnext); free(mems); free(stsize); free(stfirst); free(starr); free(stcnt); free(dims); free(data);
  free(fstack_p); free(fstack_line); free(fstack_mac); free(sv_fp); free(sv_fl); free(sv_fm); free(mbuf);
  free(tstack); free(bn); free(fnret); free(fnparams); free(fnnp); free(lpool);
  free(lkind); free(ltext); free(llab); free(lcond); free(lnext); free(obuf); free(ibuf); free(numbuf); free(ccbuf);
  free(gsym); free(goff);
  return 0;
}

// ------------------------------------------------------------------ running as and ld
// tmp_id(): the number in the temporary names, make_tmpdir(), init_tools(argv0), and
// run_tool(name, argc, argv): the tool's exit code (argv[0] is its name).
#ifdef __T2CC__
int tmp_id() { return millis(); }
int make_tmpdir() { mkdir("/tmp"); return 0; }   // an error means it is there already
int init_tools(char *argv0) { return 0; }
int run_tool(char *name, int argc, char **argv) {
  char *path; int r;
  path = malloc(strlen(name) + 10); sprintf(path, "/bin/%s.bin", name);
  r = exec(path, argc, argv);
  if (r < 0) { printf("cc: cannot run %s (%d)\n", path, r); r = 1; }
  free(path);
  return r;
}
#endif
#ifndef __T2CC__
// The PC (gcc) build: as and ld from $T2_TOOLS, else build/host/ under the directory that
// holds the directory of this executable (compiler/cc -> build/host/), run by system().
// Temporary names use the process id: make -j starts several cc in the same millisecond.
#include <sys/wait.h>
char *toolsdir;
int tmp_id() { return getpid(); }
int make_tmpdir() { return 0; }
int init_tools(char *argv0) {
  char buf[4096]; ssize_t k; int j; char *sl; char *e;
  e = getenv("T2_TOOLS");
  if (e && *e) { toolsdir = e; return 0; }
  k = readlink("/proc/self/exe", buf, sizeof buf - 1);
  if (k > 0) buf[k] = 0; else { strncpy(buf, argv0, sizeof buf - 1); buf[sizeof buf - 1] = 0; }
  for (j = 0; j < 2; j++) { sl = strrchr(buf, '/'); if (sl) *sl = 0; else strcpy(buf, "."); }
  toolsdir = malloc(strlen(buf) + 16); sprintf(toolsdir, "%s/build/host", buf);
  return 0;
}
static char *shquote(char *o, const char *s) {
  *o++ = '\'';
  for (; *s; s++) { if (*s == '\'') { strcpy(o, "'\\''"); o += 4; } else *o++ = *s; }
  *o++ = '\''; *o = 0;
  return o;
}
int run_tool(char *name, int argc, char **argv) {
  size_t n; int i; int st; char *cmd; char *o; char *path;
  path = malloc(strlen(toolsdir) + strlen(name) + 2); sprintf(path, "%s/%s", toolsdir, name);
  n = 4 * strlen(path) + 8;
  for (i = 1; i < argc; i++) n += 4 * strlen(argv[i]) + 3;
  cmd = malloc(n); o = shquote(cmd, path);
  for (i = 1; i < argc; i++) { *o++ = ' '; o = shquote(o, argv[i]); }
  fflush(stdout);
  st = system(cmd);
  free(cmd);
  if (st == -1 || !WIFEXITED(st)) { printf("cc: cannot run %s\n", path); free(path); return 1; }
  free(path);
  return WEXITSTATUS(st);
}
#endif

// ------------------------------------------------------------------ the driver
char *tmppre; int ntmpn;                         // temporary files: /tmp/cc<id & 0xffff>_<n>_<base>.s/.o

int endswith(char *s, char *suf) { int n; int k; n = strlen(s); k = strlen(suf); return n > k && !strcmp(s + n - k, suf); }
// a new temporary file name for input n (from 1), named after its source src (the base name
// without the .c/.s, so ld's messages point at the source); removed again by cleanup()
char *newtmp(char *src, int n, char *ext) {
  char *t; char *b; char *q; int k;
  if (!ntmpn) make_tmpdir();
  ntmpn = 1;
  b = src; q = src; while (*q) { if (*q == '/') b = q + 1; q = q + 1; }
  k = strlen(b) - 2;
  t = malloc(strlen(tmppre) + k + 24); sprintf(t, "%s%d_", tmppre, n);
  q = t + strlen(t); memcpy(q, b, k); sprintf(q + k, ".%s", ext);
  tmpf[ntmp] = t; ntmp = ntmp + 1;
  return t;
}
// the base name of path (a .c or .s) in the current directory, with ext (".o" or ".s") for its suffix
char *outfile(char *path, char *ext) {
  char *b; char *q; char *r; int n;
  b = path; q = path; while (*q) { if (*q == '/') b = q + 1; q = q + 1; }
  n = strlen(b); r = malloc(n + 4); strcpy(r, b); strcpy(r + n - 2, ext);
  return r;
}
// as -o obj src; a failure ends cc with as's exit code
int assemble(char *src, char *obj) {
  char **av; int r;
  av = malloc(5 * sizeof(char *)); av[0] = "as"; av[1] = "-o"; av[2] = obj; av[3] = src; av[4] = 0;
  r = run_tool("as", 4, av);
  free(av);
  if (r) { cleanup(); exit(r); }
  return 0;
}
int exists(char *path) { int fd; fd = open(path, 0, 0); if (fd < 0) return 0; close(fd); return 1; }
// the data size in the header of the object obj (t2obj_hdr.data_size, bytes 12..15);
// 0 if it cannot be read (ld then reports the file)
int objdsize(char *obj) {
  int fd; int v; char *b;
  b = malloc(16); memset(b, 0, 16); v = 0;
  fd = open(obj, 0, 0);
  if (fd >= 0) {
    if (read(fd, b, 16) == 16) v = (b[12] & 255) | ((b[13] & 255) << 8) | ((b[14] & 255) << 16) | ((b[15] & 255) << 24);
    close(fd);
  }
  free(b);
  if (v < 0) v = 0;
  return v;
}
// libNAME.a in the -L directories, the current directory, /lib; 0 if not found
char *findlib(char *name, char **ldirs, int nldir) {
  char *path; int i;
  i = 0;
  while (i <= nldir + 1) {
    path = malloc(strlen(name) + 16 + (i < nldir ? strlen(ldirs[i]) : 0));
    if (i < nldir) sprintf(path, "%s/lib%s.a", ldirs[i], name);
    else if (i == nldir) sprintf(path, "lib%s.a", name);
    else sprintf(path, "/lib/lib%s.a", name);
    if (exists(path)) return path;
    free(path);
    i = i + 1;
  }
  return 0;
}

int main(int argc, char **argv) {
  int i; int mode; int nin; char **in; int nlib; char **libs; char **ldirs; int nldir; char *a; char *dn; char *dv;
  char *out; char *s; char *o; char **av; int nav; int r; int shortdata; int dend;
  ofd = -1; outname = 0; incdir = 0; ntmp = 0; ntmpn = 0; out = 0;
  in = malloc(64 * sizeof(char *)); libs = malloc(64 * sizeof(char *)); ldirs = malloc(64 * sizeof(char *));
  dnames = malloc(64 * sizeof(char *)); dvals = malloc(64 * sizeof(char *));
  tmpf = malloc(160 * sizeof(char *)); nin = 0; nlib = 0; nldir = 0; ndefs = 0;
  mode = 0;                                     // 0: link a .bin, 1: -c, 2: -S
  shortdata = 0; dbase = 0; dlong = 0;
  i = 1;
  while (i < argc) {
    a = argv[i];
    if (nin >= 64 || nlib >= 64 || nldir >= 64 || ndefs >= 64) { printf("cc: too many arguments\n"); return 1; }
    if (a[0] == '-' && i + 1 >= argc && (!strcmp(a, "-o") || !strcmp(a, "-I") || !strcmp(a, "-D") || !strcmp(a, "-L") || !strcmp(a, "-l"))) {
      printf("cc: %s needs an argument\n", a); return 1;
    }
    if (!strcmp(a, "-o")) { out = argv[i + 1]; i = i + 2; }
    else if (!strcmp(a, "-S")) { mode = 2; i = i + 1; }
    else if (!strcmp(a, "-c")) { if (mode != 2) mode = 1; i = i + 1; }
    else if (!strcmp(a, "-mshort-data")) { shortdata = 1; i = i + 1; }
    else if (!strcmp(a, "-O")) { optim = 1; i = i + 1; }
    else if (!strcmp(a, "-I")) { incdir = argv[i + 1]; i = i + 2; }
    else if (!strcmp(a, "-D")) {                // a copy: every compile() defines it again
      dn = malloc(strlen(argv[i + 1]) + 1); strcpy(dn, argv[i + 1]); dv = strchr(dn, '=');
      if (dv) { *dv = 0; dv = dv + 1; } else dv = "1";
      dnames[ndefs] = dn; dvals[ndefs] = dv; ndefs = ndefs + 1; i = i + 2;
    }
    else if (!strcmp(a, "-L")) { ldirs[nldir] = argv[i + 1]; nldir = nldir + 1; i = i + 2; }
    else if (!strcmp(a, "-l")) { libs[nlib] = argv[i + 1]; nlib = nlib + 1; i = i + 2; }
    else if (a[0] == '-' && a[1] == 'L' && a[2]) { ldirs[nldir] = a + 2; nldir = nldir + 1; i = i + 1; }
    else if (a[0] == '-' && a[1] == 'l' && a[2]) { libs[nlib] = a + 2; nlib = nlib + 1; i = i + 1; }
    else if (a[0] == '-') { printf("cc: unknown option %s\n", a); return 1; }
    else { in[nin] = a; nin = nin + 1; i = i + 1; }
  }
  if (!nin) { printf("usage: cc [-o out] [-S|-c] [-O] [-mshort-data] [-I dir] [-D name[=v]] [-L dir] [-l name] file.c|file.s|file.o|file.a...\n"); return 1; }
  i = 0;
  while (i < nin) {
    s = in[i];
    if (!endswith(s, ".c") && !endswith(s, ".s") && !endswith(s, ".o") && !endswith(s, ".a")) { printf("cc: %s: unknown file type\n", s); return 1; }
    if (mode == 2 && !endswith(s, ".c")) { printf("cc: -S needs .c inputs\n"); return 1; }
    if (mode == 1 && !endswith(s, ".c") && !endswith(s, ".s")) { printf("cc: -c needs .c or .s inputs\n"); return 1; }
    i = i + 1;
  }
  if (mode == 1 && out && nin != 1) { printf("cc: -o with -c needs exactly one input\n"); return 1; }
  if (mode == 2 && out && nin != 1) { printf("cc: -o with -S needs exactly one input\n"); return 1; }
  init_tools(argv[0]);
  if (mode && !shortdata) dlong = 1;            // -S, -c: where the data lands is unknown
  tmppre = malloc(32); sprintf(tmppre, "/tmp/cc%x_", tmp_id() & 65535);

  if (mode == 2) {                              // -S: NAME.s, or out
    i = 0;
    while (i < nin) { o = out; if (!o) o = outfile(in[i], ".s"); compile(in[i], o); i = i + 1; }
    return 0;
  }
  if (mode == 1) {                              // -c: NAME.o, or out
    i = 0;
    while (i < nin) {
      o = out; if (!o) o = outfile(in[i], ".o");
      if (endswith(in[i], ".c")) { s = newtmp(in[i], i + 1, "s"); compile(in[i], s); assemble(s, o); }
      else assemble(in[i], o);
      i = i + 1;
    }
    cleanup();
    return 0;
  }
  // link: objects in command-line order, then the -l archives
  av = malloc((nin + nlib + 4) * sizeof(char *)); av[0] = "ld"; av[1] = "-o"; av[2] = out; if (!out) av[2] = "a.bin"; nav = 3;
  i = 0;
  while (i < nlib) {                            // look for them first: no work wasted on a typo
    s = findlib(libs[i], ldirs, nldir);
    if (!s) { printf("cc: library %s not found\n", libs[i]); return 1; }
    libs[i] = s; i = i + 1;
  }
  // each file's data goes where ld puts it: after the data of the objects before it, aligned
  // to 4 when it has any; archive members come after all objects, so .a inputs move nothing
  i = 0; dend = 0;
  while (i < nin) {
    s = in[i];
    dbase = (dend + 3) & -4;
    if (endswith(s, ".c")) { o = newtmp(in[i], i + 1, "s"); compile(s, o); s = o; }
    if (endswith(s, ".s")) { o = newtmp(in[i], i + 1, "o"); assemble(s, o); s = o; }
    if (endswith(s, ".o")) { r = objdsize(s); if (r > 0) dend = dbase + r; }
    av[nav] = s; nav = nav + 1;
    i = i + 1;
  }
  i = 0; while (i < nlib) { av[nav] = libs[i]; nav = nav + 1; i = i + 1; }
  av[nav] = 0;
  r = run_tool("ld", nav, av);
  cleanup();
  return r;
}
