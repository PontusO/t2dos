// regs.c - the -O register allocator for locals: promote() gives frame slots registers,
// sprel() rewrites what is left to sp-relative, frameless() drops frames nothing reads.
// See cc.h for what the files share.
#include "cc.h"

// ------------------------------------------------------------------ locals in registers (-O)
// After peep(), promote() moves up to 5 word-sized locals and parameters into r4-r6, r10 and
// r11: the ones only ever accessed as "ldr/str rX, [r7, #off]" (never their address, never as
// part of an array or struct), most used first, a use inside a loop counting 8 times per loop
// level. The prologue saves those registers with r7 and lr ("push {r4, r5, r7, lr}"), so the
// parameters move up by 4 bytes per register; a promoted parameter is loaded once, into one of
// the 5 empty lines parse_global() leaves after the prologue. peep() then runs again, and
// peep_hsrc()/peep_tohi() use the registers directly instead of copies in r0-r3. Float words
// go to s16-s31 instead (up to 16, "vmov rX, sN" and "vmov sN, rX"), saved by a vpush after the
// push (pvpush, an empty line) and restored by a vpop before the return (pvpop).
int *slotk;              // slotk[off / 4 + 256]: 1 when the frame word at off is part of an array or struct
int *slotw;              // promote(): use weight of each frame word; -1 not promotable, -2 - k in register k
int *pdep;               // promote(): loop depth of each line
int npro;                // registers promote() gave out in the function being compiled
int nfpro;               // s registers it gave out (s16 up)
int *slotf;              // slotf[off / 4 + 256]: 1 when the frame word at off is a float local or parameter
int pvpush; int pvpop;   // the empty lines for promote()'s vpush and vpop
int phold;               // the first of the empty lines after the prologue, -1 if none
int rfoff; int rfplain;  // r7ref(): the frame offset; 1 for a plain ldr/str of r0-r3

int slotix(int off) { off = (off & -4) / 4 + 256; if (off < 0 || off >= 512) return -1; return off; }
int markbad(int from, int to) {             // frame bytes [from, to) belong to an array or struct
  int k;
  while (from < to) { k = slotix(from); if (k >= 0) slotk[k] = 1; from = from + 4; }
  return 0;
}
char *regnm(int k) {
  if (k == 0) return "r4";
  if (k == 1) return "r5";
  if (k == 2) return "r6";
  if (k == 3) return "r10";
  return "r11";
}
int ispro(int r) { return r == 4 || r == 5 || r == 6 || r == 10 || r == 11; }
// line i's use of r7: 0 none (or the prologue and epilogue), 1 "..r7, #off.." (rfoff, rfplain),
// 2 a use promote() does not know
int fl_pc_end(int i);                     // defined below frameless(): line i pops pc
int r7ref(int i) {
  char *t; char *u; int v; int neg;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i]; u = t;
  while (*u) {
    if (u[0] == 'r' && u[1] == '7' && !idchar(u[2]) && (u == t || !idchar(*(u - 1)))) break;
    u = u + 1;
  }
  if (!*u) return 0;
  if (!strcmp(t, "push\t{r7, lr}") || !strcmp(t, "mov\tr7, sp") || !strcmp(t, "mov\tsp, r7") || !strcmp(t, "pop\t{r7, pc}")) return 0;
  if (!strncmp(t, "push\t{", 6) && inregs(t + 6, "r7")) return 0;   // promote's grown entry
  if (fl_pc_end(i)) return 0;                         // push {r4, r7, lr} and exit pop too
  if (strncmp(u, "r7, #", 5)) return 2;
  u = u + 5; neg = 0;
  if (*u == '-') { neg = 1; u = u + 1; }
  if (*u < '0' || *u > '9') return 2;
  v = 0;
  while (*u >= '0' && *u <= '9') { if (v > 100000) return 2; v = v * 10 + *u - '0'; u = u + 1; }
  if (neg) v = -v;
  if (!strncmp(t, "subw\t", 5)) v = -v;      // subw rX, r7, #N: the address r7 - N
  rfoff = v; rfplain = 0;
  if (*u == ']' && !u[1] && (!strncmp(t, "ldr\tr", 5) || !strncmp(t, "str\tr", 5)) && t[5] >= '0' && t[5] <= '3'
    && u > t + 6 && !strncmp(t + 6, ", [r7, #", 8)) rfplain = 1;   // (a plain word read or
                                                            // write of a frame slot)
  return 1;
}
int r7shift(int i, int off) {               // line i with its r7 offset (rfoff, positive) now off
  char *t;
  t = lpool + ltext[i]; pset(i);
  while (strncmp(t, "r7, #", 5)) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  ls("r7, #"); ls(dec(off)); t = t + 5;
  while (*t >= '0' && *t <= '9') t = t + 1;
  ls(t); lend();
  return 0;
}
// sprel(): -O, after promote() and before frameless(). Every frame slot is a constant
// offset from sp once the reservation is in, so a function whose r7 uses are all plain
// slot reads and whose body moves sp only in tracked ways rewrites them to [sp, #N] and
// lets frameless() drop the frame pointer - even with parameters and locals (which is
// what it used to bail on). Spec: docs/superpowers/specs/2026-11-21-cc-sp-relative-design.md.
// All-or-nothing: the screen either passes and every slot line converts, or nothing moves.
// The offset comes from each line's own literal, not rfoff: promote shifted those lines by
// sh for its grown save list, and frameless shrinks that list again (a hold load at its
// promoted [r7, #N] means the slot at N - sh, which is what the sp-relative form must be).
int spholdcvt;                           // 1 when the phold arm converted a
                                           // hold load: frameless must excuse those
                                           // lines (they read [sp, #N] by then)
int wstart[128], wend[128], nwin;          // the argv-window spans sprel()'s prescan skipped
                                           // (3d): fl_frame excuses every line inside one,
                                           // so the scan's window test IS the walk's span
                                           // instead of a second text match that can drift.
                                           // nwin resets at sprel entry; a full table
                                           // records nothing and the scan bails on the
                                           // window's mov r4, sp (the frame is kept).
                                           // 128: the census found sh's run_cmd at 65.
int spconv;                                // 1 when sprel() made this function's slots:
int sprel_ok() { return spconv; }          // frameless() may allow >1 exit. Sticky per
                                           // function: reset at the next prologue below.
// line i's own slot offset as written (subw negated), -1 if the line is not a plain
// memory read or write of [r7, #off] (lea subw/addw, bare-register uses, vstr: no).
// A line sprel already rewrote ([sp, #off], ldr/str/vldr only) answers its offset too:
// fl_frame asks this to excuse the opinfo flag every [sp, #..] operand carries.
// cc.c has no strstr; scan like r7ref does. Only a literal ", [r7, #" (or ", " plus a
// width letter before it: str.w r0, [r7, #-8], what promote's r7shift() emits) counts:
// the lea subw/addw forms end in ", r7, #N]" with no bracket and answer -1 here.
int split(int i) {
  char *t; char *u; int v; int neg; int o;
  if (lkind[i] != L_INS) return -2;           // a label/branch/deleted line is never a slot
  t = lpool + ltext[i];                       // read: -2, so callers tell it from -1 ("a real
                                              // instruction that is not one"), which the
                                              // sprewrite walk relies on to skip screened
                                              // non-slot lines without spinning on them
  if (!strncmp(t, "subw", 4) || !strncmp(t, "addw", 4) || !strncmp(t, "vstr", 4)) return -1;
  u = t;                                        // find ", [r7, #" or (rewritten) ", [sp, #":
  o = 2;                                        // the wide forms put a letter after the comma:
  while (*u) {                                  // str.w r0, [r7, #-8] (what r7shift() emits
    if (*u == ',' && (!strncmp(u + o, "[r7, #", 6) || !strncmp(u + o, "[sp, #", 6))) break;
    if (*u == ',' && u[2] == 'w' && (!strncmp(u + 3, "[r7, #", 6) || !strncmp(u + 3, "[sp, #", 6))) { o = 3; break; }
    u = u + 1;
  }
  if (!*u) return -1;
  u = u + o + 6; neg = 0;                       // the offset right after "[r7, #" / "[sp, #"
  if (*u == '-') { neg = 1; u = u + 1; }
  v = 0;
  while (*u >= '0' && *u <= '9') { if (v > 100000) return -1; v = v * 10 + *u - '0'; u = u + 1; }
  return neg ? -v : v;
}
// line i is cc's own single-register core push/pop spill ({rX}, no r7, not a pop of pc:
// those are the exit and the entry push's business). Task 3's depth walk uses this; A1
// bails on every push/pop line instead.
int spspill(char *t, int is_push) {
  char *u;
  if (is_push) { if (strncmp(t, "push\t{r", 7)) return 0; u = t + 7; }
  else { if (strncmp(t, "pop\t{r", 6)) return 0; u = t + 6; }
  return u[0] >= '0' && u[0] <= '9' && u[1] == '}' && u[2] == 0 && strcmp(u, "7}");
}
// the line's text with its r7 slot offset rewritten to sp-relative `new` (split's forms)
int sprewrite(int i, int new) {
  char *t;
  t = lpool + ltext[i]; pset(i);              // pchg = 1: frameless' scan must see the line
  while (strncmp(t, "r7, #", 5)) { lpool[lppos] = *t; lppos = lppos + 1; t = t + 1; }
  ls("sp, #"); ls(dec(new)); t = t + 5;       // past "r7, #": the offset may be negative, so
  if (*t == '-') t = t + 1;                   // skip sign and digits, not digits only (a bare
  while (*t >= '0' && *t <= '9') t = t + 1;   // loop re-reads the '-' as a stop and duplicates
  ls(t); lend();                              // the number's tail on [r7, #-8])
  return 0;
}
// the uniform mapping is NOT uniform in -4: the caller's dropped r7 word shifts
// the BAND (incoming params, k > 0) one word down, but our own slots (k < 0) keep
// the offset they had framed - the reservation hangs off sp, which moved with the
// push list. Mixing them up parks every local 4 low; the top of the array then
// stores over the saved lr (bigprom was that witness).
int spmap(int k, int sz, int d) {
  return k + sz + d - (k < 0 ? 0 : 4);
}
// the CONVERTED lea shape: "addw rD, sp, #N" (digits, nothing after). fl_frame's
// sp-move test flags it: opinfo reads the SOURCE operand sp (oprd == 13) although
// the line only READS sp and writes rD - with the frame gone that is harmless,
// so the arm excuses the shape.
int leasp(int i) {
  char *t; char *u;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (strncmp(t, "addw\t", 5)) return 0;
  u = t;
  while (*u && strncmp(u, ", sp, #", 7)) u = u + 1;
  if (!*u) return 0;
  u = u + 7;
  if (*u < '0' || *u > '9') return 0;
  while (*u >= '0' && *u <= '9') u = u + 1;
  return !*u;
}
// the shape of a convertible lea, no window test: "subw rD, r7, #N" or
// "addw rD, r7, #N" (digits after the #, nothing after them). Acceptance already
// ran the window (leaoff) before spconv is set, so fl_r7 can excuse on shape.
int leaform(int i) {
  char *t; char *u;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (strncmp(t, "subw\t", 5) && strncmp(t, "addw\t", 5)) return 0;
  u = t;
  while (*u && strncmp(u, ", r7, #", 7)) u = u + 1;
  if (!*u) return 0;
  u = u + 7;
  if (*u < '0' || *u > '9') return 0;
  while (*u >= '0' && *u <= '9') u = u + 1;
  return !*u;
}
// the lea form: "subw rD, r7, #N" (the address of word k = -N) or "addw rD, r7, #N"
// (k = +N). A lea line computes a WORD ADDRESS, and a word's address never moves:
// pushes grow DOWNWARD from sp, so everything inside the reservation - and the
// caller's band above r7 - sits at a fixed address until the exit drains. So the
// line maps exactly like a literal read of the same word: o = spmap(k, sz, d) at
// the line's own live depth d. 1 with *o the offset when the line is one and o
// lands in the window; 0 for the other -1 forms (vstr, bare-register r7) and for
// an out-of-window lea - all three keep the frame.
int leaoff(int i, int sz, int d, int *o) {
  char *t; char *u; int v; int sgn;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (!strncmp(t, "subw\t", 5)) sgn = -1;
  else if (!strncmp(t, "addw\t", 5)) sgn = 1;
  else return 0;
  u = t;
  while (*u && strncmp(u, ", r7, #", 7)) u = u + 1;   // base operand exactly r7
  if (!*u) return 0;
  u = u + 7;                                          // past ", r7, #"
  if (*u < '0' || *u > '9') return 0;
  v = 0;
  while (*u >= '0' && *u <= '9') { if (v > 100000) return 0; v = v * 10 + *u - '0'; u = u + 1; }
  if (*u) return 0;                                   // trailing text: not our form
  *o = spmap(sgn * v, sz, d);
  return *o >= 0 && *o <= 1020;
}
// the rewrite: "addw rD, sp, #o" - the same word's address, measured from sp.
// rD is the source text between the mnemonic's tab and the base operand; o is
// non-negative (leaoff's window), so one mnemonic covers both source forms.
int leawrite(int i, int o) {
  char *t; char *u; char *r;
  t = lpool + ltext[i]; r = t + 5;                    // past "subw\t" / "addw\t"
  u = t;
  while (strncmp(u, ", r7, #", 7)) u = u + 1;
  pset(i); ls("addw\t");
  while (r != u) { lpool[lppos] = *r; lppos = lppos + 1; r = r + 1; }
  ls(", sp, #"); ls(dec(o)); lend();
  return 0;
}
// the words reglist() grows the entry push with beyond its one word (r7's): every promoted
// word saves itself - a core register among promote's npro (fl_list only ever keeps r4-r6:
// r10 and r11 spell into the list but never encode in a narrow push, so at most 3) plus
// nfpro s registers. sprel() uses this to tell an un-promoted parameter read at its
// incoming offset (8..7+4n) from one promote shifted out of it (r7shift: rfoff + sh).
int spshift() { int k; if (!npro && !nfpro) return 0; k = npro < 3 ? npro : 3; return 4 * (k + nfpro); }
int inregs(char *t, char *nm);              // defined below frameless(): "r7" among the
                                           // registers of a push/pop list, whole words only
// call_sys() (cc.c's syscall trampoline, see its emits) is a fixed line sequence: it
// saves sp in r4, aligns sp for the kernel, runs the call and restores sp from r4. The
// depth walk cannot follow its movs of sp, so it recognises the whole shape line for
// line and treats it as one unit. Its net effect on sp is zero: the push {r4} pairs with
// the pop {r4}, and the mov sp,r3 / mov sp,r4 pair cancels. The `add sp, #4n` that pops
// the argv words lives OUTSIDE the unit (call_sys emits it after the pop), so the walk
// never reaches it inside: its own arm claims na for those words exactly as for a bl's
// post-call add - and if peep moved the add in front of a result save, fl_resultsave's
// pv test still sees the add. A trampoline with no arguments ends at the pop with no add
// at all. Anything that does not match line for line is not this shape: 0, and the
// normal screen bails on it (a stray push {r4} counts as a spill, its unbalanced pop
// then bails on d). The `movs rN, #imm` test is loose on purpose: only the trampoline's
// own movs can stand here, and the kernel never touches r4 across a call (its argv
// arrives by pointer in r2).
int sp_syscall_save(int i) {
  int j; char *t;
  if (lkind[i] != L_INS || strcmp(lpool + ltext[i], "mov\tr2, sp")) return 0;
  j = nx(i);
  while (j < nlines && lkind[j] == L_INS) {
    t = lpool + ltext[j];
    if (!strcmp(t, "pop\t{r4}")) return 1;        // the restore: end of the unit
    if ((!strncmp(t, "push\t{", 6) || !strncmp(t, "pop\t{", 5))   // a push/pop LIST that
      && inregs(t + (t[1] == 'u' ? 6 : 5), "r4")                 // names r4 but is NOT the
      && strcmp(t, "push\t{r4}")) return 0;       // trampoline's own save: another register
    if (strcmp(t, "push\t{r4}") && strcmp(t, "mov\tr4, sp") && strcmp(t, "mov\tr3, sp")
      && strncmp(t, "bic.w\tr3, r3, #7", 14) && strcmp(t, "mov\tsp, r3")
      && strncmp(t, "movs\tr", 6) && strcmp(t, "blx\tr8") && strcmp(t, "mov\tsp, r4")) return 0;
    j = nx(j);
  }
  return 0;
}
// 1 when line i sits inside a window span the prescan skipped (wstart/wend): the
// excuse fl_frame needs for every line but the window's first (sp_syscall_save
// matches its text only there). The table is fresh whenever sprel_ok() holds:
// sprel resets it at entry and fl_frame's two callers both run after sprel on
// the same function.
int in_window(int i) {
  int t;
  t = 0;
  while (t < nwin) { if (i >= wstart[t] && i <= wend[t]) return 1; t = t + 1; }
  return 0;
}
// line i moves sp by this much (Task 3's depth walk): a push of exactly one core
// register +4, a pop of exactly one core register -4, add sp, #N -N. A pop or push
// list with pc in it is not a spill (the exit pops frameless rewrites separately).
// 0 means the line is none of these shapes; the caller's opinfo test still catches
// every other sp mover. The list parse skips any s-register group: vpush/vpop spell
// sNN, so they never match a one-core-register shape and stay bailed by opinfo.
int spd(int i) {
  char *t; char *u; int n; int sh;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (!strncmp(t, "push\t{", 6) || !strncmp(t, "pop\t{", 5)) {
    u = t + (strncmp(t, "push", 4) ? 5 : 6);          // past the brace: "push\t{" is 6 chars,
                                                      // "pop\t{" is 5
    if (inregs(u, "pc")) return 0;                    // the exit pop: fl_pc_end's business
    sh = strncmp(t, "push", 4) ? -4 : 4;
    n = 0;
    while (*u) {                                      // count the core registers in the list:
      if (u[0] == 'r' && u[1] >= '0' && u[1] <= '9') { // a push of r7 saves no word frameless
        if (u[1] == '7') sh = 0;                      // will not save anyway; a pop of r7
        n = n + 1; u = u + 2;                         // unbalances the converted lists by one,
      }                                               // so it counts as the -4 it really is
      else if (u[0] == 's' && u[1] >= '0' && u[1] <= '9') { n = n + 1; u = u + 2; }
      else u = u + 1;
    }
    return n == 1 ? sh : 0;
  }
  if (!strncmp(t, "add\tsp, #", 9)) { n = pnum(i, "add\tsp, #"); return n < 0 ? 0 : -n; }
  return 0;
}
// a direct or indirect call (blx r8 is cc's syscall trampoline call: it takes no pushed
// args, the screen excuses it). A tail jump to a builtin ends the body.
int fl_bl(int i) {
  char *t;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  return !strncmp(t, "bl\t", 3) || (!strncmp(t, "blx\t", 4) && strcmp(t, "blx\tr8"))
    || (!strncmp(t, "b\t", 2) && strcmp(t, "b\tw"));
}
// lines that put a value where a push of r0-r3 can read it: cc builds args from these
int fl_argsetup(char *t) {
  return !strncmp(t, "mov\tr", 4) || !strncmp(t, "movw\tr", 5)   // of r0-r3 can read it: cc
    || !strncmp(t, "movt\tr", 5) || !strncmp(t, "muls\tr", 5)    // builds args from mov/movw/
    || !strncmp(t, "adds\tr", 5) || !strncmp(t, "subs\tr", 5)    // movt/mul/add/sub of r0-r3
    || !strncmp(t, "ldr\tr", 4) || !strncmp(t, "ldr\ts", 4)     // (loads included: an arg may
    || !strncmp(t, "vmov\ts", 5) || !strncmp(t, "vcvt", 4);      // be loaded on the way)
}
// line i is a call's result save: push {r0} right after the bl (cc saves r0 exactly this
// way when a call's value must live across more code, and r0 is dead at every bl) or right
// after that call's add sp, #N. The word it pushes sits below every later push until its
// own add or pop claims it: the depth walk counts it. A push feeding a bl is never one:
// that is an arg push (fl_argpush), and a save always stands at the top of its own window.
int fl_resultsave(int i) {
  int j; char *t;
  if (lkind[i] != L_INS || strncmp(lpool + ltext[i], "push\t{r0}", 10)) return 0;
  j = nx(i);                                          // a push {r0} feeding a call right
  while (j < nlines && lkind[j] == L_INS              // below, or any arg setups that lead to
    && fl_argsetup(lpool + ltext[j])                  // one, is that call's ARG push: its word
    && strncmp(lpool + ltext[j], "push\t{", 6))       // sits ON sp during the call and its add
    j = nx(j);                                        // pops it again, never read back as a
  if (j < nlines && lkind[j] == L_INS                 // saved result (this holds whether the
    && fl_bl(j))                                      // previous line is the bl or its
    return 0;                                         // post-call add)
  j = pv(i);                                          // a save stands right after the bl or
  if (j < 0 || lkind[j] != L_INS) return 0;           // its post-call add sp, #N
  t = lpool + ltext[j];
  if (!strncmp(t, "add\tsp, #", 9)) return 1;         // right after the post-call add: that
  if (strncmp(t, "bl\t", 3) && strncmp(t, "blx\tr8", 6)) return 0;   // call's result save
  j = nx(i);                                          // a str r0, [r7, #-N] right after the
  while (j < nlines && lkind[j] == L_INS              // push stores the answer in a slot
    && !strncmp(lpool + ltext[j], "str", 3))          // instead: r0 dies there and this push
    j = nx(j);                                        // feeds no call (fl_bl's walk above):
  if (j != nx(i) || split(j) < 0) return 0;           // dead code peep deletes. Either way it
  return 1;                                           // is no save: the str reads r0, not sp
}
// the str r0, [r7, #-N] that stands for a call result save fl_resultsave would otherwise
// claim at a push. Right below a bare bl it is the save when nothing sits between: the
// str's own slot read sees the result word still on sp, so the walk counts +4 HERE and
// r0 dies at the store. After an add sp, #N stand-in it is the save unless the str's
// value flows into a call (then the push below is fl_resultsave's).
int strsave(int i) {
  int j; char *t;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (strncmp(t, "str", 3)) return 0;
  j = pv(i);
  if (j < 0 || lkind[j] != L_INS) return 0;
  t = lpool + ltext[j];
  if (!strncmp(t, "add\tsp, #", 9)) {
    j = nx(i);                                        // the add stand-in: a push below that
    while (j < nlines && lkind[j] == L_INS            // leads to a call (through value setups)
      && fl_argsetup(lpool + ltext[j])                // is fl_resultsave's save; once peep's
      && strncmp(lpool + ltext[j], "push\t{", 6))     // dead-push shape leaves it feeding
      j = nx(j);                                      // nothing, the str stands for the save
    if (j < nlines && lkind[j] == L_INS               // HERE instead (its dead push below
      && !strncmp(lpool + ltext[j], "push\t{", 6)     // shifts sp right away: net zero)
      && fl_bl(nx(j)))
      return 0;
    return 1; }
  if (!strcmp(t, "pop\t{r4}"))                      // a builtin's window whose argv add peep
    return nx(pv(j)) == i;                          // moved in front of this str: the str is
                                                     // the save when it sits right below the
  if (strncmp(t, "bl\t", 3)                       // pop (its prev line is not the add).
    && strncmp(t, "blx\tr8", 6)) return 0;        // After a bare bl the str must sit right
  return nx(j) == i;                              // below it: a push {r0} in between is
                                                   // fl_resultsave's save (or an arg push)
}
// line i is a call's argument push (cc pushes each arg as a value setup into r0-r3 and a
// push {rX}): transparent to the depth walk, because a call sits ON these words and pops
// them again before the function's exits (the fib shape: push {r0}; bl; add sp, #4). The
// test walks forward over the run of value setups until it finds either the push (then
// every setup line in the run is an arg push too) or a line that cannot be a setup.
int fl_argpush(int i) {
  int j; char *t;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (fl_resultsave(i)) return 0;                   // a result save is not an arg push: the
                                                    // test runs first, so a push {r0} whose
                                                    // prev line is a bl or an add sp belongs
                                                    // to the PREVIOUS call, not this one
  if (!strncmp(t, "push\t{", 6)) {                 // the push itself: an arg push when it
    if (inregs(t + 6, "r7") || inregs(t + 6, "lr")) // saves a value, not r7 or lr. A push
      return 0;                                     // {r0} feeding the bl right below is
    return 1;                                       // that call's FIRST arg (cc emits it with
  }                                                 // no setup line)
  if (!fl_argsetup(t)) return 0;                    // a setup line: an arg push when the run
  j = nx(i);                                        // of setups it leads ends in a push
  while (j < nlines && lkind[j] == L_INS && fl_argsetup(lpool + ltext[j])) j = nx(j);
  return j < nlines && lkind[j] == L_INS && !strncmp(lpool + ltext[j], "push\t{", 6);
}
int sprel(int fa, int sz) {
  int i; int e; int j; int k; int d; int mv; int ps; int na; int isbl; int nconv; int o; char *t;
  spconv = 0;
  nwin = 0;                                         // window spans are per-function (3d)
  if (!isins(fa - 1, "mov\tr7, sp")) return 0;        // the prologue as flush() emits it
  if (pnum(fa, "movw\tr1, #") < 0 || pnum(fa + 1, "movt\tr1, #") < 0
    || !isins(fa + 2, "sub.w\tsp, sp, r1")) return 0;   // the reservation trio (sz may be 0)
  e = nlines - 1;
  while (e >= 0 && lkind[e] == L_DEL) e = e - 1;       // the last live line (the pop of pc)
  spholdcvt = 0;                              // band aliasing is a non-issue in the
                                                    // spmap model: every literal maps at
                                                    // its line's depth (band by k+sz+d-4,
                                                    // own-frame words by k+sz+d)
  i = fa + 3; nconv = 0; d = 0; ps = 0; na = 0;    // d: spill depth, words below the frame
                                                   // reservation (Task 3). The trio at
                                                   // fa..fa+2 is fl_frame's and trimframe's
                                                   // own work: always allowed
  if (!sp_syscall_save(fa + 3)) {                    // the line right after the trio sits
    opinfo(fa + 3);                                  // OUTSIDE the walk below: an unknown
    if (opmask & 8192 || oprd == 13) return 0;       // sp move there slips past both walks
  }                                                  // and leaves d wrong at entry (the first
                                                     // line of a function whose body opens
                                                     // with a syscall is its mov r2, sp)
  while (i <= e) {
    if (lkind[i] == L_INS && i != fa - 3) {
      t = lpool + ltext[i];
      if (i >= fa && i <= fa + 2) { i = nx(i); continue; }   // the reservation trio: not body
      if (phold >= 0 && i >= phold && i < phold + 21         // promote's hold loads sit right
        && r7ref(i) == 1 && split(i) >= 0) {         // after the trio and read the GROWN
        k = spmap(split(i), sz, d);                  // incoming band. frameless drops ONE
        if (k < 0 || k > 1020) return 0;             // word - r7's - from the entry list, so
        nconv = nconv + 1; spholdcvt = 1;            // a band word (split >= 0 here) lands
        i = nx(i); continue;                         // k + sz + d - 4 below sp; own-frame
      }                                              // slots keep their offset (spmap; the
                                                     // acceptance scan below proves the
                                                     // frame actually goes)
      opinfo(i); isbl = opkind == 1;                 // a bl stores the call result to a slot
      if (!strncmp(t, "push\t{", 6) && inregs(t + 6, "r7")) { i = nx(i); continue; }
                                                      // an entry push that saves r7 lists it
                                                      // as a register, not a slot (promote
                                                      // grows the list: r7ref's exact-text
                                                      // exemption missed {r4, r7, lr} and its
                                                      // answer 2 bailed every such function)
      if (fl_pc_end(i) && inregs(t + 5, "r7"))        // an exit pop that restores r7: the
        { i = nx(i); continue; }                      // line frameless' own rewrite strips
                                                      // (a bare pop {r7} mid-body still reads
                                                      // the frame: r7ref answers 2, bail)
      if (r7ref(i) == 2) return 0;                    // unknown r7 form
      k = split(i);                                   // split/r7ref parse this line only: read
      if (r7ref(i) == 1) {                            // the values right after their calls
        if (k == -1) {                                // the -1 forms: lea subw/addw convert
          if (!leaoff(i, sz, d, &o)) return 0;        // (leaoff/leawrite: a word's ADDRESS
            nconv = nconv + 1;                        // maps through the same spmap as a
            i = nx(i); continue;                      // read of the word, at this line's
          }                                           // depth, and the line moves nothing);
                                                      // vstr and bare-register r7 still bail.
                                                      // split parses the rest: a literal
                                                      // slot may sit below r7, an unpromoted
                                                      // local at [r7, #-N], and spmap maps
                                                      // it like any other word. The mapping
                                                      // is uniform with ONE split: dropping
                                                      // r7's word from the entry list shifts
                                                      // the CALLER's band down by one, so
                                                      // [r7, #k], k >= 0, reads [sp, #k+sz+d-4];
                                                      // our own words sit in the reservation
                                                      // that hangs off the moved sp, so
                                                      // [r7, #k], k < 0, reads [sp, #k+sz+d],
                                                      // at any spshift (parked registers
                                                      // shift band and frame alike, the walk
                                                      // counts the same words sprel drained).
        k = spmap(k, sz, d);                          // Task 3: the line's live spill depth
        if (k < 0 || k > 1020) return 0;              // out of ldr/str/vldr's immediate window
        nconv = nconv + 1;
      }
      else if (fl_pc_end(i)) {                        // an exit pop: every window must be
        if (na || ps || d) return 0;                  // drained above it. frameless rebuilds
                                                      // the pop from the entry list minus
                                                      // r7, so any word still live below sp
                                                      // would land in the return slot (a
                                                      // tail-call's arg window, with no
                                                      // post-call add before the pop, stops
                                                      // the conversion here).
        opinfo(i);
        if (opmask & 8192 || oprd == 13) return 0;    // the pop must not move sp otherwise
      }
      else {                                          // no r7: spills move sp, everything
        if (!strcmp(t, "mov\tr7, sp") || !strcmp(t, "mov\tsp, r7")) { i = nx(i); continue; }
                                                      // our own bookkeeping: both move sp by
                                                      // r7, which opinfo would flag; frameless
                                                      // deletes them once the function converts
        else if (fl_bl(i))                            // a call line: nothing moves - the
          { i = nx(i); continue; }                    // arg window sits on sp until the
                                                      // post-call add sp, #N claims it (the
                                                      // caller pops, not the callee), and a
                                                      // pending save stays live below (r0
                                                      // dies only at its own add or pop)
        else if (sp_syscall_save(i)) {                // a builtin's argv window: net zero on
          j = i;                                      // sp (see sp_syscall_save). Land just
          while (j < nlines                           // past the pop {r4}; the `add sp, #N`
              && strcmp(lpool + ltext[j], "pop\t{r4}")) j = nx(j);  // that pops the argv
          if (nwin < 128)                             // record the span: fl_frame excuses
            { wstart[nwin] = i; wend[nwin] = j;       // its lines for the acceptance scan
              nwin = nwin + 1; }                      // and frameless' range scan (3d)
          i = nx(j);                                  // words is next and its own arm below
          continue;                                   // claims na, as for a bl's post-call
        }                                             // add (a result save peep moved after
                                                      // the add: fl_resultsave's pv test)
        else if (strsave(i))                          // the str r0, [r7, #-N] right after a
          { ps = 1; d = d + 4; i = nx(i); continue; } // bare bl: stands for the call result
                                                      // save. The str's own slot read above
                                                      // saw the save word still on sp, so the
                                                      // depth grew at the store, not at a push
        else if (fl_resultsave(i))                    // push {r0} right after a bl, or right
          { ps = 1; d = d + 4; i = nx(i); continue; } // after the call's add sp, #N: the call
                                                      // result save. Its word sits below every
                                                      // later push until its own add or pop
                                                      // claims it (na stays: an arg window may
                                                      // already stand above)
        else if (fl_argpush(i)) {                     // an arg push: the word lands at the
          mv = spd(i);                                // push line itself. Its setups answer
          if (mv) { na = na + mv / 4; d = d + mv; }   // too, and add nothing - counting the
          i = nx(i); continue;                        // list keeps the walk independent of
        }                                             // which setup forms peep leaves
                                                      // (add.w's lea is no fl_argsetup)
        else if (!strncmp(t, "add\tsp, #", 9)) {      // the post-call cleanup: pops the
          j = pnum(i, "add\tsp, #") / 4;              // window's newest words first - the
          while (j > 0) {                             // counted arg words (each entered d
            if (na > 0) { na = na - 1; d = d - 4; }   // at its setup line), then a pending
            else if (ps)                              // save's own word, then plain spill
              { ps = 0; if (d > 0) d = d - 4; }       // words below. Args pushed without
            else if (d >= 4) d = d - 4;               // a count (register saves' windows)
            j = j - 1;                                // leave guard words for the pop arms
          }                                           // to claim - their own lines say
          i = nx(i); continue;                        // how many words they take.
        }
        else if (!strncmp(t, "pop\t{", 5) && spd(i) < 0) {  // popping spilled words or a
          j = -spd(i) / 4;                            // pending save: newest first (args
          while (j > 0) {                             // were above the save, the save
            if (na > 0) { na = na - 1; d = d - 4; }   // above plain spills). A pop that
            else if (ps)                              // feeds pc (spd 0) is fl_pc_end's
              { ps = 0; if (d > 0) d = d - 4; }       // own business, and r7 in a list
            else if (d >= 4) d = d - 4;               // only matters once the function is
            j = j - 1;                                // frameless: the pop keeps its real
          }                                           // word count, the conversion never
          i = nx(i); continue;                        // adds or drops pop words.
        }
        else mv = spd(i);                             // depth shrinks, newest first (the
                                                      // counted cases ran above)
        if (mv) { d = d + mv; i = nx(i); continue; }  // a plain spill push/pop shifts sp for
        opinfo(i);                                    // later reads. Anything else that moves
        if (opmask & 8192 || oprd == 13) return 0;    // sp - vpush, a syscall's mov sp, r3, a
        if (opkind == 4) return 0;                    // linked .s - the frameless screen bails
      }
    }
    i = nx(i);
  }
  if (!nconv) return 0;                               // no slot reads: frameless already fires
  if (d) return 0;                                    // the body must end at reservation depth
  if (phold < 0) return 0;                            // frameless strips nothing without
                                                      // promote's prologue (its gate here,
                                                      // BEFORE rewriting: the converted
                                                      // offsets bake in the dropped r7 word,
                                                      // so the frame MUST actually go)
  if (sz > 4095) return 0;                            // reserved frames convert since 3b-iii
                                                      // (frameless rewrites the exit's
                                                      // mov sp, r7 into add sp, #sz): past the
                                                      // add.w immediate the drain has no
                                                      // single form, so those keep the frame
  t = lpool + ltext[fa - 3];                          // frameless' entry test, verbatim
  if (strncmp(t, "push\t{", 6) || !inregs(t + 6, "lr")) return 0;
  spconv = 1;                                         // from here the scan judges the
                                                      // CONVERTED function; any bail below
                                                      // clears it again (line for line the
                                                      // frameless body, kept in step)
  i = nx(fa - 3);                                     // extra pops of pc are fine converted
  while (i < e) {
    if (fl_pc_end(i) && !sprel_ok()) { spconv = 0; return 0; }
    i = nx(i);
  }
  i = fa - 3;                                         // the decisive scan: fl_frame on every
  while (i <= e) {                                    // line but the entry push and the pops
    if (lkind[i] == L_INS) {                          // of pc (frameless rewrites those)
      if (i != fa - 3 && !fl_pc_end(i))
        if (fl_frame(i, fa)) { spconv = 0; return 0; }
    }
    i = nx(i);
  }
  if (fl_outside(e + 1, nlines)) { spconv = 0; return 0; }   // code after the exit pop
  i = fa + 3; d = 0; ps = 0; na = 0;                  // the rewrite walk repeats the screen's
  while (i <= e) {                                    // rules in the same order, so every
    if (lkind[i] == L_INS && i != fa - 3) {           // read lands on its OWN live depth
      if (i >= fa && i <= fa + 2) { i = nx(i); continue; }   // the reservation trio (see above)
      if (spholdcvt && r7ref(i) == 1 && split(i) >= 0)  // hold load the screen converted: the
        { sprewrite(i, spmap(split(i), sz, d));      // same line, same depth (the walk
          i = nx(i); continue; }                     // repeats the screen's rules in order)

      t = lpool + ltext[i];
      opinfo(i); isbl = opkind == 1;
      if (!strncmp(t, "push\t{", 6) && inregs(t + 6, "r7")) { i = nx(i); continue; }
      if (fl_pc_end(i) && inregs(t + 5, "r7")) { i = nx(i); continue; }   // see the screen
      if (r7ref(i) == 1 && split(i) == -1 && leaoff(i, sz, d, &o))
        { leawrite(i, o); i = nx(i); continue; }      // the lea form: the screen's predicate,
                                                      // the same depth, leaoff recomputes o
      if (r7ref(i) == 1 && split(i) != -1) {          // (r7ref answers 0 for non-instructions;
        sprewrite(i, spmap(split(i), sz, d));         // split != -1: a real slot offset, the
                                                      // sign only says whose word it is - own
                                                      // frame slots sit below r7. spmap drops
                                                      // the -4 for them: only the CALLER's
                                                      // dropped r7 word shifts the band)
      }
      else if (!strcmp(t, "mov\tr7, sp") || !strcmp(t, "mov\tsp, r7")) { }   // bookkeeping:
                                                      // nothing to track (the rewrite walk has
                                                      // no opinfo screen: frameless deletes
                                                      // both lines once the function converts)
      else if (fl_pc_end(i)) {                        // an exit pop with a window still
        if (na || ps || d) return 0;                  // open: the screen bailed on the same
      }                                               // line (r7 pops are exempt above)

      else if (strsave(i))                            // the bare-bl result save's store:
        { ps = 1; d = d + 4; i = nx(i); continue; }   // pending +4 below (the screen ran the
                                                      // same arm at this line)
      else {                                          // the screen's arms, same order:
        if (strsave(i))
          { ps = 1; d = d + 4; i = nx(i); continue; }
        if (fl_resultsave(i))
          { ps = 1; d = d + 4; i = nx(i); continue; }
        if (fl_bl(i))                                // nothing moves: the window waits
          { i = nx(i); continue; }                   // for its add sp, #N (the screen)

        if (sp_syscall_save(i))                       // the builtin's argv window: skip it
          { j = i;                                    // exactly as the screen did (net zero:
              while (j < nlines                       // d, ps and na unchanged; the argv pop
                  && strcmp(lpool + ltext[j], "pop\t{r4}")) j = nx(j);  // claims na on its own
            i = nx(j); continue; }                    // arm below
        if (fl_argpush(i)) {                         // arg push: same line-for-line rule
          mv = spd(i);                               // as the screen's arm above (the
          if (mv) { na = na + mv / 4; d = d + mv; }  // word lands at the push line)
          i = nx(i); continue;
        }
        if (!strncmp(t, "add\tsp, #", 9)) {           // the drain arm and the pop-claim
          j = pnum(i, "add\tsp, #") / 4;              // arm, identical to the screen's so
          while (j > 0) {                             // every line sees its own live depth
            if (na > 0) { na = na - 1; d = d - 4; }
            else if (ps)
              { ps = 0; if (d > 0) d = d - 4; }
            else if (d >= 4) d = d - 4;
            j = j - 1;
          }
          i = nx(i); continue;
        }
        if (!strncmp(t, "pop\t{", 5) && spd(i) < 0) {
          j = -spd(i) / 4;
          while (j > 0) {
            if (na > 0) { na = na - 1; d = d - 4; }
            else if (ps)
              { ps = 0; if (d > 0) d = d - 4; }
            else if (d >= 4) d = d - 4;
            j = j - 1;
          }
          i = nx(i); continue;
        }
        mv = spd(i);                                  // a plain spill push/pop moves sp for
        if (mv) d = d + mv;                           // the next slot read
      }
    }
    i = nx(i);
  }
  spconv = 1;
  return 0;
}
int reglist(char *mn, char *last) {         // "mn\t{r4, .., r7, last}" with the promoted registers
  int k;
  ls(mn); ls("\t{");
  k = 0; while (k < npro && k < 3) { ls(regnm(k)); ls(", "); k = k + 1; }
  ls("r7, ");
  while (k < npro) { ls(regnm(k)); ls(", "); k = k + 1; }
  ls(last); ls("}"); lend();
  return 0;
}
// -O, after promote(): a cheaper entry and exit.  The frame reservation (movw/movt r1 at line fa,
// then sub.w sp, sp, r1) of sz bytes becomes nothing or one sub sp, #sz.  With no frame, sp equals
// r7 at the exit, so mov sp, r7 goes, a return that branches to the exit pops right there, and a
// function that never reads r7 (no parameters, nothing promoted) does not set it up.
// frameless(): -O, after promote() and before trimframe(). When nothing between entry and
// exit reads r7 (calllive leaving every slot unpromoted is what makes this common), the
// frame pointer buys nothing: delete mov r7, sp / mov sp, r7 and take r7 out of the save
// lists. sp itself must then stand still between entry and exit, so any sub of sp (a
// by-value struct argument's space), any vpush/vpop (they move sp), and any instruction
// form opinfo does not know all keep the frame. promote parked a value in a register with
// a load at phold? Those lines read r7 too - but they sit inside the hold area, between
// the frame setup and the first body line, and their value dies against r7 before any
// other line reads it: a load of [r7, #off] needs no frame pointer of its own once sp
// stands still (the caller's r7 is at the same place as ours was). So the scan runs from
// after the hold area to the exit pop of pc. trimframe runs after this and copies the
// rewritten pop into the branches that jump straight to the exit.
int inregs(char *t, char *nm) {              // "r7" among the registers of a push/pop list
  int n;                                    // ("{r4, r7, lr}" and friends), whole words only
  int k; char *u;
  n = strlen(nm); u = t;
  while (*u) {
    if (!strncmp(u, nm, n)) {
      k = 0; while (k < n && u[k] == nm[k]) k = k + 1;
      if (k == n && !idchar(u[n])) return 1;
    }
    u = u + 1;
  }
  return 0;
}
int fl_pc_end(int i) {                      // line i is a pop of pc: the exit (a value pop
  char *t;                                  // ends in rX} and is not one)
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  return !strncmp(t, "pop\t{", 5) && !strcmp(t + strlen(t) - 3, "pc}");
}
int fl_r7(int i) {                          // line i needs the frame pointer: it reads r7,
  char *t;                                  // moves sp with vpush/vpop, or is a push/pop
  int k;                                    // that saves or restores r7. A value spill
  if (lkind[i] != L_INS) return 0;          // (push {r0} / pop {r1}) and a builtin's save
  t = lpool + ltext[i];                     // (push {r4}) do not mention r7. An sprel line
  if (sprel_ok() && (split(i) != -1 || leaform(i)))
    return 0;                               // (fl_frame's opinfo test runs before this
                                            // call and excuses the same lines itself; an
                                            // accepted lea is one too - the conversion
                                            // walk rewrites it to addw rD, sp, #o, which
                                            // mentions no r7 and needs no excuse.
                                            // split answers -1 ONLY for instruction
                                            // lines that are not a slot read (the -2
                                            // non-instruction answer is handled by the
                                            // lkind test above; a real offset is a
                                            // multiple of 4, negative for own-frame
                                            // slots): every one of those converts)
  if (!strncmp(t, "push\t{", 6) && inregs(t + 6, "r7"))   // a push that saves r7 keeps the
    return !sprel_ok();                     // frame (the entry line: fl_frame's range test
                                            // exempts it; once sprel converted, frameless
                                            // deletes it and rewrites the list without r7)
  k = r7ref(i);                             // FIRST: it excuses the exact entry and exit
  if (k == 0) return 0;                     // forms (push {r7, lr}, mov r7,sp, mov sp,r7,
                                            // pop {r7, pc}) as well as lines without r7.
  if (!strncmp(t, "push\t{", 6))             // A push with r7 AND registers (promote grew
    return !(!strcmp(t + 6, "r7, lr}"));    // it) is a save the frame still needs: bail.
  if (!strncmp(t, "pop\t{", 5))              // A pop of pc is an exit form (trimframe's
    return !fl_pc_end(i)                    // copied return included): fl_list strips its
      || !inregs(t + 5, "r7");              // r7 below (the grown list too: promote's
                                            // reglist rewrote it); any other list pop
                                            // restores a saved value and keeps the frame
                                            // Any [r7, #off] keeps the frame: once any
                                            // function in the call chain goes frameless it
                                            // saves one word less, so the caller's r7 and
                                            // ours no longer see the same words - an
                                            // incoming read names the wrong word
  if (!strncmp(t, "vpush", 5) || !strncmp(t, "vpop", 4)) return 1;   // they move sp
  return 1;                                 // any [r7, #off] or unknown r7 use: with our
                                            // frame pointer gone r7 holds the caller's fp,
                                            // a different base - the line would name a
                                            // different word
}
int fl_list(int i, char *t, char *mn, char *last) {   // push/pop line t without r7: the
  char *u; int first;                       // promoted registers left with npro = 0, so
  pset(i); ls(mn); ls("\t{");               // the list is whatever remains (lr or pc plus
  u = t; while (*u != '{') u = u + 1;       // any builtin's r4). lr/pc is always last:
  u = u + 1; first = 1;                     // emit it when the rest is done
  while (*u && *u != '}') {
    if (u[0] == 'r' && u[1] >= '0' && u[1] <= '9') {
      if (u[1] != '7') {                         // copy the WHOLE token: promote lists r10
        if (!first) ls(", ");                    // and r11, and a one-digit copy of r10 is
        ls("r"); lpool[lppos] = u[1]; lppos = lppos + 1;   // an r1 that as merges into the
        if (u[2] >= '0' && u[2] <= '9')          // save list - dropping r10/r11's words
          { lpool[lppos] = u[2]; lppos = lppos + 1; u = u + 1; }
        first = 0;
      }
      u = u + 2;
    }
    else u = u + 1;
  }
  if (!first) ls(", ");
  ls(last); ls("}"); lend();
  return 0;
}
// 1 if any line in [a, b) (L_DEL lines skipped) reads or writes r7 or sp beyond what
// fl_r7 excuses. frameless() calls it only for the tail past the exit pop: the buffer
// holds ONE function, and lines before the entry push are its own entry sequence (or
// already deleted), so there is nothing else to see outside fa-3..e except code after
// the return - which a linked .s can add, running in the CALLER's frame with r7/sp of
// its own accord.
int fl_outside(int a, int b) {
  int i;
  if (a < 0) a = 0; if (b > nlines) b = nlines;
  if (a >= b) return 0;
  i = a;
  while (i < b) {
    if (lkind[i] == L_INS) {
      opinfo(i);                            // read the globals before any call clobbers
      if (opmask & 8192 || oprd == 13) return 1;   // them: fl_r7 calls r7ref and friends
      opinfo(i);
      if (fl_r7(i)) return 1;
    }
    else if (lkind[i] == L_BR && lcond[i] < 0)     // an unconditional branch past the exit
      return 1;                               // reaches lines this linear scan skips
    i = nx(i);
  }
  return 0;
}
// 1 if line i forces the frame (or needs no rewrite). opinfo's globals do NOT survive
// calls (fl_pc_end, fl_r7 and friends run opinfo themselves), so every test reads them
// immediately after its own opinfo. The entry push and a pop of pc are exempted by the
// caller; the reservation trio at fa..fa+2 is trimframe's own work and always allowed.
int fl_excused(char *t) {                   // the entry/exit bookkeeping lines verbatim:
  return !strcmp(t, "mov\tr7, sp") || !strcmp(t, "mov\tsp, r7");   // the two sp moves cc
}                                           // itself emits (push/pop have their own rules)
int fl_frame(int i, int fa) {
  char *t;
  t = lpool + ltext[i];
  if (i >= fa - 3 && i <= fa + 2)           // the frame reservation trio at fa..fa+2
    return 0;                               // (trimframe's own work: always allowed)
  opinfo(i);
  if (opkind == 4) return 1;                // unknown form: it might read anything
  if (sprel_ok() && (sp_syscall_save(i) || in_window(i)))   // a builtin's argv window converts
    return 0;                               // with the frame: the walk proved its sp
                                            // moves cancel line for line, and every line
                                            // the prescan skipped is in the span table
  if (sprel_ok() && spholdcvt && phold >= 0         // a converted hold load reads [sp, #N]
    && i >= phold && i < phold + 21                 // by now: it needs no frame pointer
    && r7ref(i) == 1)                               // (the walk rewrote every such line;
    return 0;                                       // none of them reads r7 left)
  if (!strncmp(t, "push\t{", 6))             // a push with r7 (beyond the entry push the
    return inregs(t + 6, "r7");             // caller exempted) saves what the frame holds;
  if (!strncmp(t, "pop\t{", 5))              // a value spill is balanced by its own pop - the
    return !fl_pc_end(i) && !(sprel_ok()    // screen walk proved that - and once sprel
      || inregs(t + 5, "r7"));              // converted, any pop without r7 unwinds only
                                            // spilled values (frameless rewrites the exit).
                                            // A pop WITH r7 restores the frame: it stays
  if (fl_excused(t))                         // mov r7,sp and mov sp,r7 are our own book-
    return 0;                               // keeping: deleted by the rewrite below
  opinfo(i);                                // fl_r7's helpers clobbered the globals, so
  if (opmask & 8192 || oprd == 13)          // re-read them: a line that moves sp for
    return !(sprel_ok() && (split(i) >= 0   // another reason (a linked .s: add sp,
      || leasp(i)                           // r7, #4) keeps the frame. A constant add
      || !strncmp(t, "add\tsp, #", 9)       // or sub of sp is the call cleanup the depth
      || !strncmp(t, "sub\bsp, #", 9)));    // walk drained itself. A converted lea
                                            // (addw rD, sp, #o) only READS sp: opinfo
                                            // flags its source operand (oprd == 13)
                                            // but nothing moves.
  return fl_r7(i)                           // Any other r7 use
    || (!strncmp(t, "vpush", 5) && !strncmp(t, "vpop", 4));   // keeps it; vpush/vpop move
}                                           // sp by 8 and are caught here
int frameless(int fa, int sz) {
  int i; int e; char *t;
  if (phold < 0) return 0;                      // no prologue to strip
  if (sz && !sprel_ok()) return 0;              // a reservation unwinds at the exit through
                                                // mov sp, r7: unless sprel converted the body
                                                // that unwind must stay (an unconverted
                                                // function reads its slots through r7). A
                                                // converted body drains it instead: the loop
                                                // below turns every exit's mov sp, r7 into
                                                // add sp, #sz, which is what lets sz > 0
                                                // convert at all.
  if (sz > 4095) return 0;                      // add sp, #sz needs the add.w immediate form
  if (npro + nfpro && !sprel_ok()) return 0; // promote parked a value in r4-r11 or an s
                                            // register: its load at phold reads OUR r7, the
                                            // stripped save list no longer protects the
                                            // register, and vpush/vpop move sp. Such a
                                            // function keeps its frame pointer; calllive()
                                            // is what drives npro to 0 for the functions
                                            // whose values no callee-saved register survives
                                            // a call in. An sprel-converted function is the
                                            // exception: its hold loads read [sp, #N] now and
                                            // fl_list drops the parked registers with r7.
  t = lpool + ltext[fa - 3];                          // the entry sequence as emitted: push
  if (strncmp(t, "push\t{", 6) || !inregs(t + 6, "lr")) return 0;   // r7/lr (or the reglist
                                                     // promote grew it into), (vpush), mov r7,sp
  e = nlines - 1;                           // the exit: trimframe's own test - the last
  while (e >= 0 && lkind[e] == L_DEL) e = e - 1;   // live line, a pop of pc, or no rewrite
  if (e < 0 || !fl_pc_end(e)) return 0;
  i = nx(fa - 3);                           // an earlier pop of pc is trimframe's copied
  while (i < e) {                           // return: more than one exit keeps the frame,
    if (fl_pc_end(i) && !sprel_ok())        // unless sprel converted the function: then
      return 0;                             // every exit sits at reservation depth (A1: no
                                            // body sp moves) and the rewrite deleted the
                                            // mov sp, r7 that made an early pop correct -
                                            // each inline pop unwinds exactly what entry
                                            // pushed. (the copy happens after this pass)
    i = nx(i);
  }
  i = fa - 3;                               // the decisive range: from the entry push to
  while (i <= e) {                          // that exit. The entry push at fa-3 and the
    if (lkind[i] == L_INS) {                // exit pop of pc are exempt: they are rewritten
      t = lpool + ltext[i];                 // below once the scan is through
      if (i != fa - 3 && !fl_pc_end(i)) {   // pops of pc: fl_list strips them
        if (fl_frame(i, fa)) return 0;      // one test per line: opinfo's globals do not
                                            // survive the helpers, so nothing reads them
                                            // across a call here
      }
    }
    i = nx(i);
  }
  if (fl_outside(e + 1, nlines)) return 0;  // a linked .s can leave code after the exit
                                            // pop: it runs in the caller's frame and
                                            // reads the caller's r7/sp
  i = 0;                                    // fl_list rebuilds the two lists without r7:
  while (i < nlines) {                      // what remains of each list line is kept as
                                            // written (lr or pc plus any builtin's r4).
                                            // npro keeps its value on purpose: flush()'s
                                            // final peep() gates its promoted-register
                                            // merges on it, and a converted function still
                                            // has those registers. The next function resets
                                            // it (parse_global's tail), and trimframe's nor7
                                            // only decides deletion of the mov lines that
                                            // were just deleted here.
    if (lkind[i] == L_INS) {
      t = lpool + ltext[i];
      if (!strcmp(t, "mov\tr7, sp")) pdel(i);
      else if (!strcmp(t, "mov\tsp, r7")) {      // the exit's unwind: nothing to drain at
        if (!sz) pdel(i);                      // sz == 0; the reservation's drain otherwise
        else { pset(i); ls("add\tsp, #"); ls(dec(sz)); lend(); }   // (as picks add.w past the
      }                                        // 8-bit form; the gate capped sz at 4095)

      else if (i == fa - 3 && inregs(t + 6, "r7")) fl_list(i, t, "push", "lr");
      else if (fl_pc_end(i) && inregs(t + 5, "r7")) fl_list(i, t, "pop", "pc");
    }
    i = i + 1;
  }
  return 0;
}
int trimframe(int fa, int sz) {
  int i; int e; int nl; int k; int nor7; char *pop; int labs[8];
  if (pnum(fa, "movw\tr1, #") != (sz & 65535) || pnum(fa + 1, "movt\tr1, #") != ((sz >> 16) & 65535)
    || !isins(fa + 2, "sub.w\tsp, sp, r1")) return 0;
  if (sz > 0) {
    if (sz > 508) return 0;
    pset(fa); ls("sub\tsp, #"); ls(dec(sz)); lend(); pdel(fa + 1); pdel(fa + 2);
    return 0;
  }
  pdel(fa); pdel(fa + 1); pdel(fa + 2);
  nor7 = !npro && !nfpro;
  i = 0;
  while (i < nlines) {
    if (isins(i, "mov\tsp, r7")) pdel(i);
    else if (r7ref(i)) nor7 = 0;
    i = i + 1;
  }
  if (nor7) { i = 0; while (i < nlines) { if (isins(i, "mov\tr7, sp")) pdel(i); i = i + 1; } }
  e = nlines - 1;                            // the exit: labels, then the pop of pc (no vpop)
  while (e >= 0 && lkind[e] == L_DEL) e = e - 1;
  if (e < 0 || !preins(e, "pop\t{")) return 0;
  pop = lpool + ltext[e];
  k = strlen(pop);
  if (k < 3 || strcmp(pop + k - 3, "pc}")) return 0;   // only the return, never a pop of a value
  nl = 0; i = e - 1;
  while (i >= 0 && nl < 8 && (lkind[i] == L_DEL || lkind[i] == L_LAB)) {
    if (lkind[i] == L_LAB) { labs[nl] = llab[i]; nl = nl + 1; }
    i = i - 1;
  }
  i = 0;
  while (i < e) {
    if (lkind[i] == L_BR && lcond[i] < 0) {
      k = 0; while (k < nl && labs[k] != llab[i]) k = k + 1;
      if (k < nl) { pset(i); ls(pop); lend(); }
    }
    i = i + 1;
  }
  return 0;
}
// calllive(): does a value this frame slot holds ever live across a call?  A callee-saved
// register only pays for itself when it does: bl clobbers everything past r0-r3, so a slot
// whose every value dies before (or at) each call loses nothing by staying in its frame
// word.  promote() asks this while selecting, before it rewrites any line, so dropping a
// slot needs no undo.  The unit is the SLOT VALUE, not a register: a store writes one
// (born), the next store overwrites it (dies), and every load in between reads it.  A
// value born at s that some load at e reads keeps the slot when a call lies on any path
// from s to e - cl_reach answers reachability (fallthrough, forward branch targets, and a
// loop's back edge covering s: fib's parameter is read again after every recursive call),
// and the linear window in cl_between_call covers calls on all those paths at once.
// Unknown instruction forms keep the slot; an unknown r7 use makes promote() bail anyway.
// Call-free functions are exempt wholesale (has_calls): nothing dies at a call that never
// happens, and their shapes stay exactly as promote() made them today.
int has_calls;
int iscall(int i) {
  char *t;
  if (lkind[i] != L_INS) return 0;
  t = lpool + ltext[i];
  if (!strncmp(t, "bl\t", 3)) return 1;
  if (!strncmp(t, "blx\t", 4)) return strncmp(t + 4, "r8", 2);   // blx r8 is a syscall
  return 0;
}
int cl_reach(int s, int e) {                  // can control flow get from line s to line e?
  int i; int k;                               // (s <= e assumed). Forward: fallthrough and
  if (e <= s) return 1;                       // branch targets - a conditional branch also
  i = nx(s);                                  // falls through, only an unconditional one or
  while (i < nlines && i <= e) {              // a return ends the straight path. Backward:
    if (lkind[i] == L_BR) {                   // a value written in a loop body is read again
      k = labat(llab[i]);                     // next round when the loop's back edge covers
      if (k < 0) return 1;                    // s - licm's shape, read from inside outward.
      if (k <= e && k > i) return 1;          // A return on the way stops fallthrough; a
      if (k < i && k <= s) return 1;          // loop that falls out at its end never
      if (lcond[i] < 0) { i = nx(i); continue; }   // re-enters, so only heads at or above s
    }                                         // carry a value up. An unresolvable target
    else if (lkind[i] == L_INS) { opinfo(i); if (opkind == 2) return 0; }   // reaches: sure
    i = nx(i);
  }
  if (i > e) return 1;                        // fell through to e
  return 0;
}
int calllive(int slot) {
  int i; int j; char *t;
  if (!has_calls) return 1;
  i = 0;
  while (i < nlines) {                        // an unknown form might be a call: all or nothing
    if (lkind[i] == L_INS) { opinfo(i); if (opkind == 4) return 1; }
    i = i + 1;
  }
  j = 0;                                      // birth of the value currently in the slot:
  i = 0;                                      // entry until the first store writes one
  while (i < nlines) {
    if (r7ref(i) == 2) return 1;              // unknown r7 form: promote() bails anyway
    if (r7ref(i) != 1 || slotix(rfoff) != slot) { i = i + 1; continue; }
    t = lpool + ltext[i];
    if (t[0] == 's') j = i;                   // a store: this value is born here
    else if (cl_between_call(j, i)) return 1; // a load reads the value born at j, and a call
    i = i + 1;                                // lies between on some path
  }
  return 0;                                   // no value of this slot ever crosses a call
}
int cl_between_call(int s, int e) {           // any call from line s to line e on some path?
  int i;
  if (!cl_reach(s, e)) return 0;            // unreachable: nothing lives that way
  i = s + 1;                                  // reachable: the linear window covers the
  while (i < nlines && i <= e) {              // straight path and any swept loop body alike
    if (iscall(i)) return 1;
    i = i + 1;
  }
  return 0;
}
int promote() {
  int i; int k; int n; int w; int d; int best; int bw; int sh; char *t; char *rn;
  if (!slotw) { slotw = malloc(512 * 4); pdep = malloc(16400 * 4); }
  npro = 0; nfpro = 0;
  if (phold < 0) return 0;
  i = 0; while (i < 512) { if (slotk[i]) slotw[i] = -1; else slotw[i] = 0; i = i + 1; }
  i = 0; while (i <= nlines) { pdep[i] = 0; i = i + 1; }
  has_calls = 0; i = 0;
  while (i < nlines) { if (iscall(i)) has_calls = 1; i = i + 1; }
  i = 0;
  while (i < nlines) {                      // a branch back to label t makes lines t..i a loop
    if (lkind[i] == L_BR) { k = labat(llab[i]); if (k >= 0 && k < i) { pdep[k] = pdep[k] + 1; pdep[i + 1] = pdep[i + 1] - 1; } }
    i = i + 1;
  }
  i = 1; while (i < nlines) { pdep[i] = pdep[i] + pdep[i - 1]; i = i + 1; }
  i = 0;
  while (i < nlines) {
    k = r7ref(i);
    if (k == 2) return 0;
    if (k == 1) {
      n = slotix(rfoff);
      if (n >= 0 && slotw[n] >= 0) {
        if (!rfplain || (rfoff & 3) || rfoff == 0 || rfoff == 4) slotw[n] = -1;
        else {
          w = 1; d = pdep[i];
          while (d > 0 && w < 100000) { w = w * 8; d = d - 1; }
          slotw[n] = slotw[n] + w;
        }
      }
    }
    i = i + 1;
  }
  nfpro = 0;
  while (npro < 5 || nfpro < 16) {
    best = -1; bw = 0; i = 0;
    while (i < 512) {
      w = slotw[i];
      if (i >= 258 && w > 0) w = w - 1;     // a parameter costs a load in the prologue
      if (w > 0 && !calllive(i)) w = 0;      // dead at every call: no register for it
      if (slotf[i] && (nfpro >= 16 || (i - 256) * 4 > 900)) w = 0;   // vldr reaches 1020
      if (!slotf[i] && npro >= 5) w = 0;
      if (w > bw) { bw = w; best = i; }
      i = i + 1;
    }
    if (best < 0) break;
    if (slotf[best]) { slotw[best] = -20 - nfpro; nfpro = nfpro + 1; }
    else { slotw[best] = -2 - npro; npro = npro + 1; }
  }
  if (!npro && !nfpro) return 0;
  sh = 4 * (npro + nfpro);                  // the words reglist() grows the push list by:
                                            // promote shifts every un-promoted parameter's
                                            // [r7, #N] load with them (its own hold loads at
                                            // phold read the grown offsets), so this stays
                                            // promote's business; sprel() reads each line's
                                            // own literal instead and needs no shift here
  i = 0;
  while (i < nlines) {
    k = r7ref(i);
    if (k == 1) {
      n = slotix(rfoff);
      if (n >= 0 && slotw[n] <= -20) {
        t = lpool + ltext[i]; pset(i); ls("vmov\t");
        if (t[0] == 'l') { ls("r"); lpool[lppos] = t[5]; lppos = lppos + 1; ls(", s"); ls(dec(-4 - slotw[n])); }
        else { ls("s"); ls(dec(-4 - slotw[n])); ls(", r"); lpool[lppos] = t[5]; lppos = lppos + 1; }
        lend();
      }
      else if (n >= 0 && slotw[n] <= -2) {
        t = lpool + ltext[i]; rn = regnm(-2 - slotw[n]);
        pset(i); ls("mov\t");
        if (t[0] == 'l') { lpool[lppos] = 'r'; lpool[lppos + 1] = t[5]; lppos = lppos + 2; ls(", "); ls(rn); }
        else { ls(rn); ls(", r"); lpool[lppos] = t[5]; lppos = lppos + 1; }
        lend();
      }
      else if (rfoff > 4) r7shift(i, rfoff + sh);
    }
    else if (isins(i, "push\t{r7, lr}")) { pset(i); reglist("push", "lr"); }
    else if (isins(i, "pop\t{r7, pc}")) { pset(i); reglist("pop", "pc"); }
    i = i + 1;
  }
  if (nfpro) {
    pset(pvpush); ls("vpush\t{s16"); if (nfpro > 1) { ls("-s"); ls(dec(15 + nfpro)); } ls("}"); lend();
    pset(pvpop); ls("vpop\t{s16"); if (nfpro > 1) { ls("-s"); ls(dec(15 + nfpro)); } ls("}"); lend();
  }
  n = phold; i = 258;
  while (i < 512) {                         // promoted parameters: load them once
    if (slotw[i] <= -20) {
      pset(n); ls("vldr\ts"); ls(dec(-4 - slotw[i])); ls(", [r7, #"); ls(dec((i - 256) * 4 + sh)); ls("]"); lend();
      n = n + 1;
    }
    else if (slotw[i] <= -2) {
      pset(n); ls("ldr\t"); ls(regnm(-2 - slotw[i])); ls(", [r7, #"); ls(dec((i - 256) * 4 + sh)); ls("]"); lend();
      n = n + 1;
    }
    i = i + 1;
  }
  return 0;
}
