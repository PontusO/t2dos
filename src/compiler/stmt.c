// stmt.c - initializers, statements and the top level: parse_global() opens functions
// and defines globals (.t2init carries run-before-main initializers).
// See cc.h for what the files share.
#include "cc.h"

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
int init_addr(int off) { if (ikind) lea_loc(ibase + off); else if (isym) lea_data(isym, off, ibase + off); else lea_data(0, ibase + off, ibase + off); return 0; }   // isym 0: a string literal or a static (.Ldata target)
int init_code() { if (!ikind) init_open = 1; return 0; }   // global initializer code: it goes to .Linit
int init_close() { if (init_open) { tobuf = 1; flush(); tobuf = 0; init_open = 0; } return 0; }
int init_splice(int from, int lpf) {   // a static local's coded init: move only the lines added since (from, lpf) to .Linit, keep the function's own lines (label numbers are unique per file, so the split is safe)
  int i; int k;
  if (!init_open) { nlines = from; lppos = lpf; return 0; }
  tobuf = 1;
  i = from;
  while (i < nlines) {
    k = lkind[i];
    if (k == L_INS) { oput("\t"); oput(lpool + ltext[i]); oput("\n"); }
    else if (k == L_LAB) { oput(".L"); oput(dec(llab[i])); oput(":\n"); }
    else if (k == L_BR) {
      if (llab[i] < 0) die("internal error: branch without a target");
      oput("\tb"); if (lcond[i] >= 0) oput(ccn(lcond[i])); oput(".w\t.L"); oput(dec(llab[i])); oput("\n");
    }
    i = i + 1;
  }
  tobuf = 0;
  nlines = from; lppos = lpf; init_open = 0;
  return 0;
}
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
int newstatic(struct Sym *s, int t, int size, int arr) {   // static local: file-scope storage, block-visible name (no slot, no label)
  s->hclass = s->class; s->htype = s->type; s->hval = s->val; s->hstatik = s->statik;
  dpos = (dpos + 3) & -4;
  s->class = arr ? CStaArr : CSta; s->type = arr ? t + PTR : t; s->val = dpos; s->statik = 0;
  dpos = dpos + size;
  return 0;
}

int newlocal(struct Sym *s, int t, int size) {
  s->hclass = s->class; s->htype = s->type; s->hval = s->val; s->hstatik = s->statik;
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
  int at; int at2; int sv_brk; int sv_cont; int sv_tgt; int body; int t; int bt; int n; int hi; int st; int ln0; int lp0; struct Sym *d;
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
  else if (tk == Static || is_type()) {         // local declaration (maybe static: data image, no slot)
    st = 0;
    if (tk == Static) { st = 1; next(); if (!is_type()) die("type expected after static"); }
    bt = parse_type();
    while (tk != ';') {
      t = declarator(bt);
      if (!dname) die("local name expected");
      d = dname; hi = 0;
      if (tk == Assign) { next(); hi = 1; }
      if (darr == -2) { if (!hi) die("array size expected"); darr = count_init(); }
      n = tysize(t);
      if (st) {                               // global-init semantics on the .Ldata/.Linit paths
        ln0 = nlines; lp0 = lppos;
        if (darr >= 0) {
          n = n * darr;
          newstatic(d, t, n, 1);
          if (hi) { ikind = 0; isym = 0; ibase = d->val; init_array(t, darr, 0); init_splice(ln0, lp0); }
        } else {
          newstatic(d, t, n, 0);
          if (hi) { ikind = 0; isym = 0; ibase = d->val; init_elem(t, 0); init_splice(ln0, lp0); }
        }
      }
      else if (darr >= 0) {                          // local array: slot holds pointer to the storage that follows it
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
  int bt; int t; struct Sym *s; int n; int i; int frame_at; int sz; int hi; int ext; int st;
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
  ext = 0; st = 0;
  if (tk == Static) { st = 1; next(); }             // internal linkage: no .global, no data label
  if (tk == Extern) { ext = 1; next(); }            // a declaration: the storage is in another file
  if (tk == Static) { if (st) die("duplicate static"); st = 1; next(); }   // extern static
  if (st && ext) die("static and extern");
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
          dname->hclass = dname->class; dname->htype = dname->type; dname->hval = dname->val; dname->hstatik = dname->statik;
          dname->class = CLoc; dname->type = bt; dname->val = n;   // index, fixed up below
        }
        if (n < 32) s->ptypes[n] = bt;
        n = n + 1;
        if (tk == ',') next();
      }
      next();
      s->nptypes = n < 32 ? n : 32;
      if (st && s->class == CFun && !s->statik) die("static mismatch");   // declared plain, defined static: the prototype promised external linkage
      if (st) s->statik = 1;
      if (s->statik && s->len == 4 && strncmp(s->name, "main", 4) == 0) die("static main");   // strncmp: names are not NUL-terminated
      if (tk == ';') {                          // prototype
        next();
        i = 0; while (i < nsyms) { if (syms[i].class == CLoc || syms[i].class == CSta || syms[i].class == CStaArr) { syms[i].class = syms[i].hclass; syms[i].type = syms[i].htype; syms[i].val = syms[i].hval; syms[i].statik = syms[i].hstatik; } i = i + 1; }
        if (s->class != CFun) { s->class = CFun; s->val = -1; }
        return 0;
      }
      if (tk != '{') die("{ expected");
      // fix param offsets: param i at r7 + 8 + 4*(n-1-i)
      i = 0; while (i < nsyms) { if (syms[i].class == CLoc) syms[i].val = 8 + 4 * (n - 1 - syms[i].val); i = i + 1; }
      if (optim) { i = 0; while (i < nsyms) { if (syms[i].class == CLoc && syms[i].type == TFLOAT && slotix(syms[i].val) >= 0) slotf[slotix(syms[i].val)] = 1; i = i + 1; } }
      if (s->statik && !st) die("static mismatch");   // declared static, defined plain
      s->class = CFun; s->val = 0; nparams = n; fnrty = t;   // val >= 0: defined
      if (!s->statik) { oput("\t.global\t"); oname(s); oput("\n"); }
      oname(s); oput(":\n");
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
      i = 0; while (i < nsyms) { if (syms[i].class == CLoc || syms[i].class == CSta || syms[i].class == CStaArr) { syms[i].class = syms[i].hclass; syms[i].type = syms[i].htype; syms[i].val = syms[i].hval; syms[i].statik = syms[i].hstatik; } i = i + 1; }
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
    if (st && (s->class == 7 || s->class == 9)) die("static after extern");
    if (nglob >= 8192) die("too many globals");
    hi = 0;
    if (tk == Assign) { next(); hi = 1; }
    if (darr == -2) { if (!hi) die("array size expected"); darr = count_init(); }
    n = tysize(t);
    if (darr >= 0) {
      n = n * darr;
      // global array: the symbol is the storage itself; treat name as pointer constant
      dpos = (dpos + 3) & -4;
      s->class = st ? CStaArr : 6; s->type = t + PTR; s->val = dpos;     // val = address of storage
      if (st) { s->hclass = s->class; s->htype = s->type; s->hval = s->val; s->hstatik = s->statik; }   // restore loops then no-op on file-scope statics (shadowing saves overwrite these)
      if (!st) { gsym[nglob] = s; goff[nglob] = dpos; nglob = nglob + 1; }   // static: no label, .Ldata refs
      dpos = dpos + n;
    } else {
      dpos = (dpos + 3) & -4;
      s->class = st ? CSta : CGlo; s->type = t; s->val = dpos;
      if (st) { s->hclass = s->class; s->htype = s->type; s->hval = s->val; s->hstatik = s->statik; }
      if (!st) { gsym[nglob] = s; goff[nglob] = dpos; nglob = nglob + 1; }
      dpos = dpos + n;
    }
    if (hi) {
      ikind = 0; ibase = s->val; isym = st ? 0 : s;
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
