// licm.c - -O loop-invariant code motion: hoist what the loop head leaves unchanged
// out of the loop. See cc.h for what the files share.
#include "cc.h"

// ---------------------------------------------------------------- licm(): loop-invariant code motion
// the ALU lines licm() may hoist. ldr and friends are never candidates: no alias analysis
// exists, so a store through a pointer may change what a load would return. cmp/tst and the
// branch forms stay put too: they set flags other lines read, and moving a compare across a
// loop head is not licm()'s job.
// the instructions licm() may move at all: only address and value math whose result a later
// line reads through a register. A load's value depends on memory the loop may write (no
// alias analysis: ldr is no candidate), and cmp/tst and the branch forms write flags the
// loop head owns, so they stay put too
int licm_mnem() {
  return mnis("mov") || mnis("movw") || mnis("movt")
    || mnis("movw.16")
    || mnis("add") || mnis("adds") || mnis("add.w") || mnis("addw")
    || mnis("sub") || mnis("subs") || mnis("subw")
    || mnis("mul") || mnis("muls") || mnis("lsls") || mnis("lsrs");
}
// the text of line j with every r9 spelled "rB" (a name no real register reaches: idchar('B')
// is 0), saved in licm_save; the number of r9s, or -1 if the line does not read r9 at all.
// Only such lines move: licm() rewrites every placeholder in the body's copy, so a line with no
// r9 has nothing to say here and answers -1 (a symbol reference is immovable by itself). The
// copy lives outside lpool on purpose: a candidate
// at the pool's tail would lose its own bytes to its placeholders (ls() appends), and a rejected
// line must stand exactly as it was for opinfo(), dead() and the output. licm_to() writes the
// saved text into the pool when the line really moves, and licm_ok() reads the answer right
// after this: opinfo() runs again then, because scanning resets opmn. One candidate is live at
// a time: licm_rtext fills the buffer, licm_ok reads it, licm_to writes it out.
char *licm_save; int licm_savelen;
char licm_buf[600];                        // (licm_to(): a copy of the saved text, see there)
int outl[64];                              // (licm(): labels outside the loop a body branch reaches)
int licm_rtext(int j) {
  char *t; int prev; int c; int e;
  if (!licm_save) licm_save = malloc(512);
  t = lpool + ltext[j]; prev = 0; c = 0; e = 0;
  { char *tt = t;                            // a line licm() moved carries the mark where its
    while (*tt) {                            // base stood: it reads r9 no more, and moving one
      if (!*tt && !tt[1]) {                  // again would rewrite the placeholder in its own
        opinfo(j); return -1;                // text. Refuse it here (opinfo: the scan below
      }                                      // never runs then, so opmn must be set)
      tt = tt + 1;
    }
  }
  while (*t) {                               // (the mark is two NULs, so this scan stops at it:
    if (!t[1]) break;                        // a line that carries one never gets this far)
    if (idstart(*t)) {
      if (!prev || !idchar(prev)) {
        if (t[0] == 'r' && t[1] == '9' && !idchar(t[2])) {   // the data base: a placeholder,
          licm_save[e] = 0; licm_save[e + 1] = 0;            // marked with two NULs: idchar(0)
          e = e + 2; t = t + 2; c = c + 1; prev = '9'; continue;   // is 0, so no line's own bytes
        }                                           // can spell the mark by accident
      }
      while (*t && idchar(*t)) { licm_save[e] = *t; e = e + 1; t = t + 1; }
    }
    else { licm_save[e] = *t; e = e + 1; t = t + 1; }
    prev = *(t - 1);
  }
  licm_savelen = e;                          // (the text, without its terminator)
  licm_save[e] = 0;                          // (a string: probes and later reads stop at it)
  if (c) return c;                           // it reads r9: the saved text is what moves
  opinfo(j);                                 // (scanning the copy reset opmn: licm_ok() reads
  return -1;                                 // the mnemonic, and lpool never saw a byte)
}
// the hoisted text of line j at the end of the pool (rB for every base register), its r9
// count, or -1 if licm() may not move the line. Only a line that reads r9 moves: its saved
// text becomes the body's copy with the placeholder, and the insertion point's copy with r9.
// A symbol's movw/movt pair stays put: hoisting one half alone would split the chain, and ld
// resolves the symbol from each line's own text.
int licm_ok(int j) {
  int c;
  c = licm_rtext(j);                         // (it re-reads the line into opinfo: its scan
  if (c > 0) return c;                       // reset opmn, and licm() reads the mnemonic after)
  return -1;                                 // only a line that reads the data base moves: its
}                                            // marked text is saved for licm_to(). A symbol's
                                             // movw/movt half stays put: hoisting one half alone
                                             // splits the chain, and ld reads the symbol from
                                             // each line's own text
// line j written at the pool's tail, every base register spelled `to` (the caller's ls()/
// lend() terminate it). licm_ok() saved this line's text with its r9s marked by two NULs:
int licm_to(int j, char *to, int nr) {       // that is what moves, the mark spelled `to` here.
  char *t; int e;                            // A line licm_ok kept no text for copies its own
                                             // bytes verbatim: it reads no base, so there is
  if (nr > 0 && licm_savelen) {              // nothing to replace. One byte past the saved text:
    e = licm_savelen + 1;                    // its NUL, so a mark at the very end does not
    while (e) { e = e - 1; licm_buf[e] = licm_save[e]; }   // clobber text this writes. The copy
    t = licm_buf; e = 0;                     // goes into a buffer of its own first: nothing may
    while (e < licm_savelen) {               // write licm_save between licm_ok() and here. The
      if (!t[0] && !t[1]) {                  // walk is by position: the old one ran while (e) on
        lpool[lppos] = to[0]; lpool[lppos + 1] = to[1];     // a countdown, and a mark at the very
        lppos = lppos + 2; t = t + 2; e = e + 2;           // end drove its counter negative and
      }                                      // copied bytes past the copy into the line's text
      else { lpool[lppos] = t[0]; lppos = lppos + 1; t = t + 1; e = e + 1; }
    }
    return 0;
  }
  t = lpool + ltext[j];                      // (the line's own text up to its NUL; lend() ends it)
  while (*t) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  return 0;
}
// licm(): move loop-invariant lines out of loops. A branch back to a label at or before it
// makes a loop, the same shape promote() reads. licm(hl) scans the loop forward: a known ALU
// line is invariant when every register it reads was written before the loop or by an earlier
// invariant line. That covers chains: in matmul's inner loop today the movw/movt pair for a
// global's offset and the add.w r9 that turns it into an address rebuild on every iteration,
// and all three lines move because each reads only what the previous one wrote (the symbol
// half of the pair moves when dead() says its value dies at once). The original goes away
// (delete-only, per the design note): sound because nothing in the loop writes the
// destination, no branch enters the body below the head (the back edge aside), and rB names a
// register only licm() writes. A line that reads r9 keeps it at the insertion point and
// becomes rB in the body: the data base survives calls, peep() runs again after licm_loops()
// so peep_tohi folds "add.w r0, r9, rB ; mov rP, r0" into "add.w rP, r9, rB", and promote()
// gives the hoisted base a callee-saved register. A loop that makes a call hoists nothing
// else: bl clobbers r0-r3 and r12, so a line writing one of those moves out on its own only
// when the body makes no call at all. Flags need no care: licm_mnem's lines never read them,
// so moving one keeps every flag use in place. A loop whose body holds an unknown line, an it
// block, stack traffic, or any v line is skipped whole. Inserting a line shifts the line
// numbers after it, so licm_loops() rescans from the top and hoists loops out one at a time:
// the innermost first, because the scan meets its back edge first.
// livo[n]: line n carries the text licm() moved out of a loop: its tail holds the NUL mark
// where its base register stood. Nothing may move twice, so licm() refuses a loop whose lines
// livo already marks (the body scan in pass 2 skips such a line, but one between c and hl
// would hide from it, and the guard after licm_ok() catches what is left)
int *livo;               // livo[n]: line n is invariant in the loop being hoisted
int licm(int h);
int licm_loops() {
  int i; int k; int round; int moved;
  if (!livo) { livo = malloc(16400 * 4); }
  round = 0;
  i = 0; while (i < 16400) { livo[i] = 0; i = i + 1; }   // (marks stand across flushes: cc.c
                                           // compiles itself, and a new function's lines must
                                           // not inherit one. licm() sets them as it moves)
  while (round < 512) {                    // one move per pass: line numbers shift under it
    round = round + 1;
    moved = 0;                             // (a loop that stays put must not spin the rounds:
                                           // the scan breaks only when a licm() call moves one)
    i = 0; while (i < nlines) { if (lkind[i] == L_LAB && llab[i] >= 0 && llab[i] < 65536) labline[llab[i]] = i; i = i + 1; }   // (a hoist's insert shifts every label line after it: the
                                           // scan reads labline, so it is rebuilt from the lines
                                           // each round instead of tracked through the shifts)
    i = 0;
    while (i < nlines) {                   // a branch back to label t makes a loop at line t
      if (lkind[i] == L_BR) { k = labat(llab[i]); if (k >= 0 && k < i) { if (licm(k)) { moved = 1; break; } } }
      i = i + 1;
    }
    if (!moved) break;                     // nothing moved anywhere: the work is done
  }
  return 0;
}
// licm(hl): hoist what is invariant in the loop whose head label's line is hl, and answer 1
// if anything moved (the caller rescans: the insertion shifted every line number after it).
// peep() has rewritten the branches: a conditional branch to the head sits above the label and
// an unconditional one below it. The loop runs from the first of those, "c", through the body
// and the back edge b; the lines between c and hl are part of its test, not a preheader.
int licm(int hl) {
  int i; int j; int k; int m; int n; int r; int moved; int wmask; int dmask; int nr; int b; int calls; int c; int nout; int changed; int t; int cstack; int pool_at; int sl; char *t2; char *w;
  if (lkind[hl] != L_LAB) return 0;
  c = hl;                                  // c: the loop's first line, a branch back to the head
  while (c > 0) { k = pv(c);               // (a branch may spell the head's number differently,
    if (lkind[k] != L_BR) break;           // peep() has rewritten some of them: labat says where
    if (labat(llab[k]) != hl) break;       // a branch really lands)
    c = k;
  }
  b = -1;                                  // the back branch: the last line that reaches hl again
  i = nx(hl);
  while (i < nlines) {                     // labat, not llab: peep() rewrites some branches so
    if (lkind[i] == L_BR && labat(llab[i]) == hl) b = i;   // they no longer spell the head's own
    i = nx(i);                             // number. Every back edge counts: see pass 1
  }
  if (b < 0 || b <= hl) return 0;          // no back edge, or an empty body
  dmask = 0; calls = 0; cstack = 0;
  nout = 0;                                // outl[] starts with the head's own label: a branch
  m = llab[hl];                            // from the body to it would enter below the hoisted
  if (m >= 0) {                            // line (and its flags, which the head sets). The
    outl[nout] = m; nout = nout + 1;       // walk below tests every branch's target against it
  }                                        // so a label two lines reach is no entry either
  i = c;                                   // pass 1: what the loop writes, and whether it is safe at all
  while (i < b) {
    if (lkind[i] == L_INS) {
      opinfo(i);
      if (opkind == 1) calls = 1;          // a call: r0-r3 and r12 do not survive it
      else if (opkind != 0) return 0;                   // an it block or unknown line: skip the loop
                                           // (a cmp in the body sets no flags a later line
                                           // reads: peep() leaves only dead ones. licm() moves
                                           // one line per round, and pass 2 below marks lines
                                           // in order, so what reaches the head's test stands
                                           // above every hoisted line already)
      if (mnis("pop")) {                                // a pop writes its list: those registers
        t2 = lpool + ltext[i];                          // are not candidates. A push only reads
        while (*t2) {                                   // its source and stores to the stack
          if (!*t2 && t2[1] == 0) break;                // (opinfo() leaves a pop's list in opmask
          if (idstart(*t2)) {                           // only for its pc case: read the tokens.
            w = t2;                                     // A hoisted line's NUL mark ends the
            while (idchar(*t2)) t2 = t2 + 1;            // scan: regword sees no register in it)
            while (idchar(*t2)) t2 = t2 + 1;
            r = regword(w, t2 - w);
            if (r >= 0) dmask = dmask | (1 << r);
          }
          else t2 = t2 + 1;
        }
      }
      if (lpool[ltext[i]] == 'v') return 0;                             // float code stays put
      if (oprd >= 0) dmask = dmask | (1 << oprd);   // (an unknown destination writes no register
                                                    // name opinfo() knows: str, it blocks)
    }
    i = nx(i);
  }
  // licm() inserts above the head label, so a line that reaches the body without passing the
  // head would miss the hoisted line. The walk starts with every label outside c..b that a
  // branch from hl to b reaches (the loop's own exits: cc builds loops whose exit test branches
  // back into this very body, and licm_loops() accepts the innermost back edge first, so an
  // outer loop is only walked once every loop inside it has been hoisted from or rejected).
  // It grows two ways: fall-through from a reached label, and what a line at or after a reached
  // label branches to. A label two lines reach is no entry either: nothing below it runs except
  // from inside the body.
  i = hl;
  while (i <= b) {                          // inclusive: b itself branches back to hl, and a
    if (lkind[i] == L_BR && llab[i] >= 0) { // branch at b lands on the head like any other
      m = labat(llab[i]);
      if (m < c || m > b) {                 // a target outside the loop: where an entry would be
        if (nout < 64) outl[nout] = llab[i];
        nout = nout + 1;
      }
    }
    i = nx(i);
  }
  changed = 1;
  while (changed) {                         // follow the entry labels to fixpoint
    changed = 0;
    r = 0;
    while (r < nout && r < 64) {
      m = outl[r]; r = r + 1;
      t = labat(m);
      if (t < 0) return 0;                  // no line carries the label: fail closed
      i = c;                                   // (the walk starts at the region's first line: a
      while (i < b) {                          // branch in c..hl lands on the head's test, which
        k = nx(i);                             // runs after it. A body line only falls through
        if (lkind[i] == L_INS && t == k) return 0;             // into the reached label only by
        i = k;                                 // next line is at or before t)
      }
      n = -1; i = c;                           // what a line at or after the region's first line
      while (i < nlines) {                     // branches to is reachable too: from inside c..b it
        if (lkind[i] == L_BR && llab[i] >= 0 && labat(llab[i]) == m) {   // would enter below the
          if (i > hl) return 0;                // head: its flags are set there, and a branch in
          if (n < 0) n = i;                    // c..hl lands on the head's test after the hoist
        }
        i = nx(i);
      }
      if (n >= 0) {                         // its own target joins the walk (m itself: reached)
        if (nout < 64) outl[nout] = llab[n];
        nout = nout + 1;
        changed = 1;
      }
    }
  }
  wmask = 1 << 9; moved = 0; cstack = 0;    // pass 2: mark the invariant lines, in order. The
  i = c;                                    // walk above proved no body branch reaches a label
                                            // that falls into the body, so only fall-through
                                            // enters it: r9 (the data base) and every register
                                            // the loop never writes reach the head unchanged
                                            // from outside. wmask grows with each line's dest;
                                            // a source outside it is set inside the loop, and
                                            // a chain through that one stays put
  while (i < b) {                           // from c: a branch to the head above the label reads
                                            // its flags, and they are set inside the loop
    if (lkind[i] == L_INS) {
      opinfo(i);
      if (opkind == 1) calls = 1;           // a call: r0-r3 and r12 do not survive it
      if (mnis("push")) cstack = cstack | opmask;   // a push reads its source here and hands the
      if (mnis("pop")) {                    // value to the matching pop: neither register may be
        t2 = lpool + ltext[i];              // hoisted, nor anything in between (the stack model
        while (*t2) {                       // is one push per register). A pop whose register no
          if (idstart(*t2)) {               // push here wrote reads a value from outside the
            w = t2;                         // loop: nothing above it moved by mistake, so its
            while (idchar(*t2)) t2 = t2 + 1;   // candidates may still move. dmask in pass 1
            r = regword(w, t2 - w);         // keeps such a register away from the candidates
            if (r >= 0 && (cstack & (1 << r))) cstack = cstack & ~(1 << r);   // of later rounds
          }
          else t2 = t2 + 1;
        }
      }
      // dmask plays no part here: a register the loop writes every round (the counter, r0-r2)
      // is still a constant source for a line whose own value is fixed at the head. wmask says
      // which sources are read before the loop writes them, and that is the test pass 2 needs.
      // cstack guards both ways: a candidate may not write a register whose value is on the
      // stack (its pop would read the hoist's result), nor read one (hoisting it above the
      // push would pass the push's earlier value by). The second test holds for chains too:
      // every line between the pushed line and this one is a candidate, so the stack sits
      // above all of them once they move.
      opinfo(i);
      t2 = lpool + ltext[i];                // (a line licm() moved before carries the NUL mark
      n = 0;                                // where its base stood: it stands for r9, so the
      while (*t2) {                         // walk stops: never move a line twice. Scanning
        if (!*t2 && t2[1] == 0) { n = 1; break; }   // for an "rB" token would not do: a moved
        t2 = t2 + 1; }                      // chain's movw names symbols that could spell one)
      if (n) { i = nx(i); continue; }
      n = 0; t2 = lpool + ltext[i];          // (only a line that reads the data base is worth
      while (*t2) {                          // hoisting: address math on r9 repeats every round
        if (idstart(*t2)) {                  // with the same result. A bare constant like
          w = t2;                            // movw r0, #1 moves forever for nothing: it reads
          while (idchar(*t2)) t2 = t2 + 1;   // no base, and a moved line's mark stops its
          if (t2 - w == 2 && w[0] == 'r' && w[1] == '9') n = 1;   // chain's add.w from moving
          continue;                          // again, so the halves stay where they are)
        }
        t2 = t2 + 1;
      }
      if (!n) { i = nx(i); continue; }       // (no base read: leave the line where it is)
      if (licm_mnem() && oprd >= 0 && oprd <= 12 && !(cstack & ((1 << oprd) | opmask))) {
        m = opmask; n = 0;
        while (n < 16) { if ((m & (1 << n)) && !(wmask & (1 << n))) break; n = n + 1; }   // (not
        if (n == 16 && licm_ok(i) >= 0) { livo[i] = 1; moved = moved + 1;   // dmask: a line may
          wmask = wmask | (1 << oprd);      // the line moves too: later lines may still read it
        }
        else wmask = wmask | (1 << oprd);   // its value is fixed at the head: later lines may read it
      }
    }
    i = nx(i);
  }
  if (!moved) return 0;
  i = c;                                    // (a line an earlier round moved carries the mark in
  while (i < b) {                           // its text, and pass 2 left livo standing for it:
    if (livo[i]) return 0;                  // refuse a second move before touching anything.
    i = nx(i);                              // The guard after licm_ok() catches the rest)
  }
  j = -1; i = b - 1;                        // the bottommost candidate: a chain moves as add.w,
  pool_at = lppos;                          // movt, movw, in order. The pool's tail: licm_to
                                            // appends from here, and a late licm_ok failure
                                            // rewinds it (the line's own text stands until then)
  while (i > c) { if (livo[i]) { j = i; break; } i = pv(i); }   // movt, movw, in order
  if (j < 0) return 0;
  nr = licm_ok(j);                          // > 0: the hoisted text, marked at every base
  sl = licm_savelen;                        // (a symbol's movw/movt keeps licm_save from an
                                           // earlier line: pin this line's own length now)
  if (nr <= 0) { lppos = pool_at; return 0; }   // (the candidate died at the last look: undo
                                           // anything its check appended to the pool)
  opinfo(j);                                 // (licm_rtext's scan reset opmn)
  if (calls && oprd <= 3) { lppos = pool_at; return 0; }   // bl clobbers r0-r3 and r12: its
                                           // value would not live past the call (rewind too)
  k = pv(hl);                               // an empty line before the head label, or insert one
  licm_savelen = sl;                        // (dead() above ran licm_ok() for another line:
                                           // licm_to() reads licm_save for this one)
  m = nlines - 1;                           // nothing may move twice. livo is the record of
  while (m > k) { if (livo[m]) { lppos = pool_at; return 0; }   // what moved: its shift above
    m = m - 1; }                            // runs after this point, so a marked line can only
                                           // stand below the head now. Pass 2 skips marks in the
                                           // body, but one between c and hl would hide from it
  if (k >= 0 && lkind[k] == L_DEL) {       // the line above the head was deleted: its text is
    pool_at = lppos;                        // free pool space. Write the hoist's r9 copy there:
    licm_to(j, "r9", nr); lend();     // the pool only grows, and this space would go unused
                                           // anyway (no live line points at it: a hoisted line
                                           // points at its own text, written below)
    lkind[k] = L_INS; ltext[k] = pool_at;   // (pset after lend left k at the NEXT line's text:
                                           // every round rewrote that line with this one's copy)
  }
  else {
    m = nlines; nlines = nlines + 1;        // shift the lines from hl up by one and make room
    while (m > hl) { lkind[m] = lkind[m - 1]; ltext[m] = ltext[m - 1]; llab[m] = llab[m - 1]; lcond[m] = lcond[m - 1]; m = m - 1; }
    pool_at = lppos;                        // (the new line's text starts here: lend() moves the
                                           // tail, and licm_to() writes from this point)
    licm_to(j, "r9", nr); lend();     // at the head: the mark becomes r9 again. The pool
    ltext[hl] = pool_at;                    // write comes first: a line's text must never point
                                           // at another line's text (ltext[j] still does here)
    lkind[hl] = L_INS; llab[hl] = -1; lcond[hl] = -1;
    k = hl;
  }
  i = k;                                    // livo follows the shift (labline: licm_loops()
  m = nlines - 1;                           // rebuilds it from the lines each round)
  while (m > i) { livo[m] = livo[m - 1]; m = m - 1; }
  if (i >= 0) livo[i] = 0;                  // (an insert before line 0 shifts it all)
  licm_savelen = sl;                        // (the head's licm_to() may have rewritten it: the
                                           // body's copy of this line goes in now)
  pool_at = lppos;                          // (the body's text starts here: lend() moves the
                                           // tail past it. The walk in licm_to() is by
                                           // position: its old countdown ran past the end when
                                           // a mark stood at the very tail of the line)
  licm_to(j, "rB", nr); lend();       // the body keeps the mark (a name no
  lkind[j] = L_INS; ltext[j] = pool_at;     // real register reaches) and points at its own text
  livo[j] = 1;                              // (the shift above copied livo with the lines: this
                                           // body now carries moved text, so it must say so. It
                                           // stands below k, where that scan cannot see it)
  return 1;
}
// the instructions that take r4-r6, r10, r11 in any register operand (the wide encodings)
int hiok() {
  return mnis("ldr") || mnis("ldrb") || mnis("ldrh") || mnis("ldrsb.w") || mnis("ldrsh.w") || mnis("str") || mnis("strb")
    || mnis("strh") || mnis("cmp") || mnis("mov") || mnis("movw") || mnis("addw") || mnis("subw") || mnis("add.w")
    || mnis("adds") || mnis("subs");
}
// line i with register rf (r0-r3) spelled to, the first operand left alone if skip
int renreg(int i, int rf, char *to, int skip) {
  char *t; int prev;
  t = lpool + ltext[i]; pset(i); prev = 0;
  while (*t && *t != 9) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  if (*t) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  if (skip) { lpool[lppos] = t[0]; lpool[lppos + 1] = t[1]; lppos = lppos + 2; prev = t[1]; t = t + 2; }
  while (*t) {
    if (*t == '#') { while (*t && *t != ',' && *t != ']') { lpool[lppos] = *t; lppos = lppos + 1; prev = *t; t = t + 1; } }
    else if (t[0] == 'r' && t[1] == '0' + rf && !idchar(t[2]) && !idchar(prev)) { ls(to); t = t + 2; prev = '0'; }
    else { lpool[lppos] = *t; lppos = lppos + 1; prev = *t; t = t + 1; }
  }
  lend();
  return 0;
}
// mov rA, rP ; X -> X reading rP in place of rA (rP a promoted register, rA r0-r3 not read after X)
int peep_hsrc(int a) {
  int j; int ra; int w; char *t;
  t = lpool + ltext[a];
  if (t[5] < '0' || t[5] > '3' || t[6] != ',' || t[7] != ' ') return 0;
  ra = t[5] - '0'; w = regword(t + 8, strlen(t + 8));
  if (!ispro(w)) return 0;
  j = nx(a); if (j >= nlines || lkind[j] != L_INS) return 0;
  opinfo(j);
  if (opkind != 0 || !hiok() || !(opmask & (1 << ra))) return 0;
  if (oprd == ra) { if (!oppw) return 0; renreg(j, ra, t + 8, 1); }
  else { if (!dead(ra, nx(j), 0)) return 0; renreg(j, ra, t + 8, 0); }
  pdel(a);
  return 1;
}
// X rA, ... ; mov rP, rA -> X rP, ... (rP a promoted register, rA r0-r3 not read afterwards)
int peep_tohi(int a) {
  int b; int rp; int ra; char *u; char *c;
  b = nx(a); if (b >= nlines || lkind[b] != L_INS) return 0;
  u = lpool + ltext[b];
  if (strncmp(u, "mov\tr", 5)) return 0;
  c = strchr(u, ','); if (!c || c[1] != ' ') return 0;
  rp = regword(u + 4, c - u - 4); ra = regword(c + 2, strlen(c + 2));
  if (!ispro(rp) || ra < 0 || ra > 3) return 0;
  opinfo(a);
  if (opkind != 0 || oprd != ra || !oppw || !hiok()) return 0;
  if (!dead(ra, nx(b), 0)) return 0;
  if (rp >= 10) rp = rp - 7; else rp = rp - 4;   // r4 r5 r6 r10 r11 -> 0..4
  redest(a, regnm(rp)); pdel(b);
  return 1;
}
// subw/addw r0, r7, #N ; push {r0} ; X ; pop {r1} ; ST r0, [r1] -> X ; ST r0, [r7, #off]
// (an assignment to a local whose right side uses r1 and r2). X must leave the pushed word alone:
// stack references only to words it pushed itself, a syscall's argv only over its own args.
int peep_astore(int a) {
  int off; int b; int j; int dep; int n; int k; int sz; char *mn; char *t;
  off = froff(a); b = nx(a);
  if (off == 1 || !isins(b, "push\t{r0}")) return 0;
  if (!dead(0, nx(b), 0)) return 0;          // X does not read the address
  j = nx(b); dep = 0; n = 0;
  while (j < nlines && !(dep == 0 && isins(j, "pop\t{r1}"))) {
    if (lkind[j] != L_INS) return 0;
    n = n + 1; if (n > 200) return 0;
    t = lpool + ltext[j];
    if (!strcmp(t, "push\t{r0}")) dep = dep + 1;
    else if (!strcmp(t, "pop\t{r1}")) dep = dep - 1;
    else if (!strncmp(t, "add\tsp, #", 9)) { dep = dep - pnum(j, "add\tsp, #") / 4; if (dep < 0) return 0; }
    else if (!strncmp(t, "ldr\tr1, [sp, #", 14)) { if (atoi(t + 14) >= 4 * dep) return 0; }   // a call's function address
    else if (!strcmp(t, "mov\tr2, sp")) {   // a syscall: its args are the top movs r1, #n words
      k = 0; while (k < 8 && !preins(j, "movs\tr1, #")) { j = nx(j); k = k + 1; }
      if (k >= 8 || pnum(j, "movs\tr1, #") > dep) return 0;
      while (j < nlines && !isins(j, "pop\t{r4}")) { if (lkind[j] != L_INS) return 0; j = nx(j); }
      if (j >= nlines) return 0;
    }
    else {
      opinfo(j);
      if (opkind == 4 || opkind == 2 || oprd == 13 || (opmask & 8192) || mnis("push") || mnis("pop")) return 0;
    }
    j = nx(j);
  }
  if (j >= nlines) return 0;
  k = nx(j); mn = 0;
  if (isins(k, "str\tr0, [r1]")) { mn = "str"; sz = 4; }
  if (isins(k, "strb\tr0, [r1]")) { mn = "strb"; sz = 1; }
  if (isins(k, "strh\tr0, [r1]")) { mn = "strh"; sz = 2; }
  if (!mn || !offok(off, sz) || !dead(1, nx(k), 0)) return 0;
  pset(k); lsoff(mn, "r0", off);
  pdel(a); pdel(b); pdel(j);
  return 1;
}
// add.w rA, r9, rA ; (up to 3 lines without rA) ; LD/ST rX, [rA] -> ... ; LD/ST rX, [r9, rA]
int peep_gidx(int a) {
  int j; int n; int ra; char *t; char *u; char *v;
  if (isins(a, "add.w\tr1, r9, r1")) ra = 1; else if (isins(a, "add.w\tr2, r9, r2")) ra = 2; else return 0;
  j = nx(a); n = 0;
  while (j < nlines && lkind[j] == L_INS && n < 4) {
    t = lpool + ltext[j]; u = t + strlen(t);
    if (u - t > 6 && *(u - 4) == '[' && *(u - 3) == 'r' && *(u - 2) == '0' + ra && *(u - 1) == ']'
      && (preins(j, "ldr\t") || preins(j, "ldrb\t") || preins(j, "ldrh\t") || preins(j, "ldrsb.w\t") || preins(j, "ldrsh.w\t")
        || preins(j, "str\t") || preins(j, "strb\t") || preins(j, "strh\t"))) {
      opinfo(j);
      if (oprd == ra) { if (!(oppw)) return 0; }    // ldr r1, [r1]: fine, the load replaces the address
      else if (!dead(ra, nx(j), 0)) return 0;
      v = strchr(t, 9);                     // str r1, [r1] stores the address itself
      if (preins(j, "st") && v[1] == 'r' && v[2] == '0' + ra && v[3] == ',') return 0;
      pset(j);
      while (t < u - 3) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
      ls("r9, r"); ls(dec(ra)); ls("]"); lend();
      pdel(a);
      return 1;
    }
    opinfo(j);
    if (opkind != 0 || oprd == ra || (opmask & (1 << ra))) return 0;
    j = nx(j); n = n + 1;
  }
  return 0;
}
// push {r0} ; ldr r1, [sp, #0] ; blx r1 ; add sp, #4 -> blx r0 (a call through a pointer, no args)
int peep_icall(int a) {
  int b; int c; int d;
  b = nx(a); c = nx(b); d = nx(c);
  if (!isins(b, "ldr\tr1, [sp, #0]") || !isins(c, "blx\tr1") || !isins(d, "add\tsp, #4")) return 0;
  pset(c); ls("blx\tr0"); lend();
  pdel(a); pdel(b); pdel(d);
  return 1;
}
int peep() {
  int i; int round; int k;
  if (!labline) { labline = malloc(65536 * 4); opmn = malloc(64); labref = malloc(65536 * 4); dvis = malloc(16400 * 4);
    i = 0; while (i < 65536) { labline[i] = -1; i = i + 1; }   // (a label no line carries points nowhere)
    i = 0; while (i < 16400) { dvis[i] = 0; i = i + 1; }   // stamps start at 1: no line is visited yet
  }
  i = 0; while (i < nlines) { if (lkind[i] == L_LAB && llab[i] >= 0 && llab[i] < 65536) labline[llab[i]] = i; i = i + 1; }
  pchg = 1; round = 0;
  while (pchg && round < 16) {
    pchg = 0;
    i = 0; while (i < nlines) { if (lkind[i] == L_LAB && llab[i] >= 0 && llab[i] < 65536) labref[llab[i]] = 0; i = i + 1; }
    i = 0; while (i < nlines) { if (lkind[i] == L_BR && llab[i] >= 0 && llab[i] < 65536) labref[llab[i]] = labref[llab[i]] + 1; i = i + 1; }
    i = 0; while (i < nlines) { if (lkind[i] == L_LAB && llab[i] >= 0 && llab[i] < 65536 && !labref[llab[i]]) pdel(i); i = i + 1; }
    i = 0;
    while (i < nlines) {
      k = lkind[i];
      if (k == L_BR) peep_br(i);
      else if (k == L_INS) {
        if (npro && preins(i, "mov\tr") && peep_hsrc(i)) ;
        else if (npro && peep_tohi(i)) ;
        else if (lpool[ltext[i]] == 'v') peep_fp(i);
        else if (preins(i, "mov\tr") && peep_dce(i)) ;
        else if (isins(i, "push\t{r0}")) { if (!peep_icall(i) && !peep_fpush(i)) peep_push(i); }
        else if (preins(i, "mov\tr1, r")) peep_mov(i);
        else if (preins(i, "ite\t")) peep_ite(i);
        else if (froff(i) != 1) { if (!peep_astore(i) && !peep_store(i)) { if (!peep_ld(i)) peep_tor1(i); } }
        else if (preins(i, "add.w\tr0, r9, r")) { if (!peep_ld(i)) peep_tor1(i); }
        else if (preins(i, "add.w\tr1, r9, r") || preins(i, "add.w\tr2, r9, r")) peep_gidx(i);
        else if (preins(i, "subw\tr1, r7, #") || preins(i, "addw\tr1, r7, #")) peep_r1fr(i);
        else if (isins(i, "adds\tr0, r1, r0") || isins(i, "adds\tr0, r2, r0")) { if (!peep_idx(i)) peep_tor1(i); }
        else if (preins(i, "str\tr0, [r7, #")) peep_reload(i);
        else peep_tor1(i);
      }
      i = i + 1;
    }
    round = round + 1;
  }
  return 0;
}
