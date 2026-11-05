// ld - link T2DOS objects into a T2CC program (.bin)
//   ld [-o out.bin] [-e entry] file.o...      (default output a.bin, entry main)
// Errors print "ld: ..." and exit 1, and the output file is removed (no older one stays behind).
//
// Layout: the objects' text in command-line order, each object's text start aligned to 4
// with nop halfwords (an .align inside an object assumes a 4-aligned section start); then
// their data likewise, each object's data start aligned to 4 with zero bytes and nothing
// after the last object. A text symbol's value is its object's text base + value, a data
// symbol's its object's data base + value (data offsets are relative to r9), an absolute
// symbol's its value. A relocation's symbol, when it is defined in its own object (local or
// global, including the section symbols .text/.data), is that definition; when undefined,
// the one global definition among all objects. The symbol must suit the relocation: a text
// symbol for R_BL/R_BW/R_BCOND/R_FNADDR, a data or absolute one for R_DATA_ADDW/R_DATA_MOVW16,
// any defined one for R_DATA_MOVW/R_DATA_MOVT/R_WORD.
// With SYM_INIT symbols, an entry stub follows the text: push {r4, lr}; bl init (each in
// link order); pop.w {r4, lr}; b.w entry. The stub is then the entry.
// Archives (files starting with the archive magic) are searched after the objects: a member is
// added when its index names a global that is referenced and not yet defined; the search over all
// archives repeats until nothing changes. Members are laid out after the command-line objects in
// the order they were added, and are then like any object (named ARCHIVE(MEMBER) in messages).
// The data size is the end of the last object that has data (an object without data takes no
// alignment).
// Output: "T2CC", code size, data size, 0, entry offset (32-bit LE), the code, the data.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include "t2obj.c"

enum { NOP = 0xBF00, PUSH_R4_LR = 0xB510, POP_W_R4_LR = 0xE8BD, POP_W_R4_LR_2 = 0x4010,
       B_W = 0xF000, B_W_2 = 0x9000, BL_2 = 0xD000 };

struct t2o **objs; char **paths; int *tbase; int *dbase; int nobj;
// archives: the t2ar, its path, and for its members (flat, mbase[a] + member) the object and whether it is added
struct t2ar **ars; char **arpaths; int *mbase; int nar; struct t2o **mobj; int *madded;
char *code; int csize; char *data; int dsize;
// global definitions: open hash table of (object, symbol) pairs, -1 when empty
int *hobj; int *hsym; int hsize;
// referenced names (symbols that are undefined in a linked object): the same kind of table
int *uobj; int *usym;

int get16(char *b, int at) { return (b[at] & 255) | (b[at + 1] & 255) << 8; }
int put16(char *b, int at, int v) { b[at] = v & 255; b[at + 1] = (v >> 8) & 255; return 0; }
int put32(char *b, int at, int v) { put16(b, at, v & 65535); return put16(b, at + 2, (v >> 16) & 65535); }

char *symname(int ob, int sym) { return t2o_name(objs[ob], sym); }

int hash(char *s) {
  int h; h = 0;
  while (*s) { h = (h * 31 + (*s & 255)) & 0xFFFFFF; s++; }   // masked: no signed overflow
  return h & (hsize - 1);
}
// the slot of name s in the table (tob, tsym): its entry, or the empty slot where it goes
int tfind(int *tob, int *tsym, char *s) {
  int i; i = hash(s);
  while (tob[i] >= 0 && strcmp(symname(tob[i], tsym[i]), s) != 0) i = (i + 1) & (hsize - 1);
  return i;
}
int hfind(char *s) { return tfind(hobj, hsym, s); }

// value of symbol sym of object ob in the final segments (sym is defined, or its global is)
int symval(int ob, int sym) {
  struct t2sym *s; int h;
  s = objs[ob]->syms + sym;
  if (s->sect == SEC_UNDEF) { h = hfind(symname(ob, sym)); return symval(hobj[h], hsym[h]); }
  if (s->sect == SEC_TEXT) return tbase[ob] + s->value;
  if (s->sect == SEC_DATA) return dbase[ob] + s->value;
  return s->value;
}

char *outpath;                         // removed on an error, once the options are read
int quit() { if (outpath) unlink(outpath); exit(1); return 0; }
int bad(char *path) { printf("ld: %s: not a T2DOS object\n", path); return quit(); }
int badarchive(char *path) { printf("ld: %s: not a T2DOS archive\n", path); return quit(); }

// 1 if not: relocations must patch whole fields inside their section; data holds only R_WORD;
// a run-before-main routine is a text symbol; a text or data symbol lies inside its section
int badrels(struct t2o *o) {
  struct t2rel *r; struct t2sym *s; int i; int n; int size;
  for (i = 0; i < o->h.nsyms; i++) {
    s = o->syms + i;
    if ((s->flags & SYM_INIT) && s->sect != SEC_TEXT) return 1;
    if (s->sect == SEC_TEXT && (s->value < 0 || s->value > o->h.text_size)) return 1;
    if (s->sect == SEC_DATA && (s->value < 0 || s->value > o->h.data_size)) return 1;
  }
  for (i = 0; i < o->h.nrels; i++) {
    r = o->rels + i;
    n = 4;
    if (r->type == R_FNADDR) n = 10;
    size = o->h.text_size;
    if (r->sect == SEC_DATA) { size = o->h.data_size; if (r->type != R_WORD) return 1; }
    if (r->type < R_BL || r->type > R_WORD || r->off < 0 || r->off > size - n) return 1;
  }
  return 0;
}

// b.w / bl (T4 / T1, 24-bit) or b<cond>.w (T3, 20-bit) at code + at to target
int patch_b(int at, int target, int cond, char *name, char *path) {
  int off; int s; int j1; int j2; int h1; int h2;
  off = target - (at + 4);
  h1 = get16(code, at); h2 = get16(code, at + 2);
  if ((off & 1) || (!cond && (off < -16777216 || off > 16777214)) || (cond && (off < -1048576 || off > 1048574))) {
    printf("ld: branch to %s out of range (in %s)\n", name, path); quit();
  }
  if (!cond) {
    s = (off >> 24) & 1;
    j1 = (((off >> 23) & 1) ^ 1) ^ s; j2 = (((off >> 22) & 1) ^ 1) ^ s;
    put16(code, at, (h1 & 0xF800) | s << 10 | ((off >> 12) & 1023));
  } else {
    s = (off >> 20) & 1; j2 = (off >> 19) & 1; j1 = (off >> 18) & 1;
    put16(code, at, (h1 & 0xFBC0) | s << 10 | ((off >> 12) & 63));   // keeps the cond bits
  }
  return put16(code, at + 2, (h2 & 0xD000) | j1 << 13 | j2 << 11 | ((off >> 1) & 2047));
}

// imm16 of the movw/movt at code + at (imm4:i:imm3:imm8)
int patch16(int at, int v) {
  put16(code, at, (get16(code, at) & 0xFBF0) | ((v >> 11) & 1) << 10 | ((v >> 12) & 15));
  return put16(code, at + 2, (get16(code, at + 2) & 0x8F00) | ((v >> 8) & 7) << 12 | (v & 255));
}

int toofar(char *name, char *path, int v, char *insn) {
  printf("ld: data offset of %s (%s) is %d, too far for %s\n", name, path, v, insn);
  quit();
  return 0;
}

// the section of symbol sym of object ob, or of its global definition
int symsect(int ob, int sym) {
  int h;
  if (objs[ob]->syms[sym].sect != SEC_UNDEF) return objs[ob]->syms[sym].sect;
  h = hfind(symname(ob, sym));
  return objs[hobj[h]]->syms[hsym[h]].sect;
}

char *relname(int t) {
  if (t == R_BL) return "R_BL";
  if (t == R_BW) return "R_BW";
  if (t == R_BCOND) return "R_BCOND";
  if (t == R_DATA_ADDW) return "R_DATA_ADDW";
  if (t == R_DATA_MOVW16) return "R_DATA_MOVW16";
  return "R_FNADDR";
}

int relocate(int ob) {
  struct t2o *o; struct t2rel *r; int i; int p; int v; char *name; int sc; int t;
  o = objs[ob];
  for (i = 0; i < o->h.nrels; i++) {
    r = o->rels + i;
    name = symname(ob, r->sym);
    sc = symsect(ob, r->sym); t = r->type;
    if (((t == R_BL || t == R_BW || t == R_BCOND || t == R_FNADDR) && sc != SEC_TEXT)
        || ((t == R_DATA_ADDW || t == R_DATA_MOVW16) && sc != SEC_DATA && sc != SEC_ABS)) {
      printf("ld: %s (%s): wrong kind of symbol for %s\n", name, paths[ob], relname(t));
      quit();
    }
    v = symval(ob, r->sym) + r->addend;
    if (r->sect == SEC_DATA) { put32(data, dbase[ob] + r->off, v); continue; }
    p = tbase[ob] + r->off;
    if (r->type == R_BL || r->type == R_BW) patch_b(p, v, 0, name, paths[ob]);
    else if (r->type == R_BCOND) patch_b(p, v, 1, name, paths[ob]);
    else if (r->type == R_DATA_ADDW) {
      if (v < 0 || v > 4095) toofar(name, paths[ob], v, "addw");
      put16(code, p, (get16(code, p) & 0xFBFF) | ((v >> 11) & 1) << 10);
      put16(code, p + 2, (get16(code, p + 2) & 0x8F00) | ((v >> 8) & 7) << 12 | (v & 255));
    }
    else if (r->type == R_DATA_MOVW) patch16(p, v & 65535);
    else if (r->type == R_DATA_MOVT) patch16(p, (v >> 16) & 65535);
    else if (r->type == R_DATA_MOVW16) {
      if (v < 0 || v > 65535) toofar(name, paths[ob], v, "movw");
      patch16(p, v);
    }
    else if (r->type == R_FNADDR) {
      v = v + 1 - (p + 12);
      patch16(p, v & 65535);
      patch16(p + 4, (v >> 16) & 65535);
    }
    else put32(code, p, v);
  }
  return 0;
}

int usage() { printf("usage: ld [-o out.bin] [-e entry] file.o...\n"); exit(1); return 0; }

// link object o (named path): its globals are defined (a second definition is an error),
// its undefined symbols become referenced names
int addobj(struct t2o *o, char *path) {
  int j; int h; struct t2sym *s;
  objs[nobj] = o; paths[nobj] = path;
  for (j = 0; j < o->h.nsyms; j++) {
    s = o->syms + j;
    if (s->sect == SEC_UNDEF) {
      h = tfind(uobj, usym, symname(nobj, j));
      if (uobj[h] < 0) { uobj[h] = nobj; usym[h] = j; }
      continue;
    }
    if (!(s->flags & SYM_GLOBAL)) continue;
    h = hfind(symname(nobj, j));
    if (hobj[h] >= 0) {
      printf("ld: duplicate symbol: %s (%s and %s)\n", symname(nobj, j), paths[hobj[h]], path);
      quit();
    }
    hobj[h] = nobj; hsym[h] = j;
  }
  nobj++;
  return 0;
}

// "ARCHIVE(MEMBER)"
char *memberpath(int a, int m) {
  char *name; char *p; int n; int k;
  name = ars[a]->str + ars[a]->members[m].name;
  n = strlen(arpaths[a]); k = strlen(name);
  p = malloc(n + k + 3);
  memcpy(p, arpaths[a], n); p[n] = '(';
  memcpy(p + n + 1, name, k); p[n + 1 + k] = ')'; p[n + 2 + k] = 0;
  return p;
}

int main(int argc, char **argv) {
  char *out; char *entname; char *buf; int size; int i; int j; int k; int h; int nundef;
  int ninit; int pos; int entry; int stub; int fd; int hdr; struct t2o *o; struct t2ar *ar;
  char **fpath; char **fbuf; int *fsize; int *fobj; int nf; int nmembers; int changed; int a; int m;
  out = "a.bin"; entname = "main";
  fpath = malloc((argc + 1) * sizeof(char *)); fbuf = malloc((argc + 1) * sizeof(char *));
  fsize = malloc((argc + 1) * sizeof(int)); fobj = malloc((argc + 1) * sizeof(int));
  ars = malloc((argc + 1) * sizeof(struct t2ar *)); arpaths = malloc((argc + 1) * sizeof(char *));
  mbase = malloc((argc + 1) * sizeof(int));
  nf = 0; nar = 0; nmembers = 0; k = 0; outpath = 0;
  // the options first: from here on an error removes the output
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { i++; out = argv[i]; }
    else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) i++;
    else if (argv[i][0] == '-') usage();
  }
  outpath = out;
  // read the inputs: objects (fobj 1, validated here) and archives (fobj 0)
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) i++;
    else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) { i++; entname = argv[i]; }
    else {
      buf = t2_readfile(argv[i], &size);
      if (!buf) { printf("ld: cannot read %s\n", argv[i]); quit(); }
      fpath[nf] = argv[i]; fbuf[nf] = buf; fsize[nf] = size;
      if (size >= 4 && get16(buf, 0) == (T2AR_MAGIC & 65535) && get16(buf, 2) == ((T2AR_MAGIC >> 16) & 65535)) {
        ar = malloc(sizeof(struct t2ar));
        if (t2ar_read(buf, size, ar) < 0) badarchive(argv[i]);
        ars[nar] = ar; arpaths[nar] = argv[i]; mbase[nar] = nmembers;
        nmembers = nmembers + ar->h.nmembers;
        nar++;
        fobj[nf] = 0;
      } else {
        o = malloc(sizeof(struct t2o));
        if (t2o_read(buf, size, o) < 0 || badrels(o)) bad(argv[i]);
        k = k + o->h.nsyms;
        fobj[nf] = 1; fbuf[nf] = (char *)o;
      }
      nf++;
    }
  }
  if (nf == 0) { outpath = 0; usage(); }
  mobj = malloc((nmembers + 1) * sizeof(struct t2o *)); madded = malloc((nmembers + 1) * sizeof(int));
  for (a = 0; a < nar; a++) {
    for (m = 0; m < ars[a]->h.nmembers; m++) {
      o = malloc(sizeof(struct t2o));
      if (t2ar_member(ars[a], m, o) < 0 || badrels(o)) badarchive(arpaths[a]);
      mobj[mbase[a] + m] = o; madded[mbase[a] + m] = 0;
      k = k + o->h.nsyms;
    }
  }
  objs = malloc((nf + nmembers + 1) * sizeof(struct t2o *));
  paths = malloc((nf + nmembers + 1) * sizeof(char *));
  tbase = malloc((nf + nmembers + 1) * sizeof(int));
  dbase = malloc((nf + nmembers + 1) * sizeof(int));
  hsize = 16; while (hsize < 2 * k) hsize = hsize * 2;
  hobj = malloc(hsize * sizeof(int)); hsym = malloc(hsize * sizeof(int));
  uobj = malloc(hsize * sizeof(int)); usym = malloc(hsize * sizeof(int));
  for (i = 0; i < hsize; i++) { hobj[i] = -1; uobj[i] = -1; }

  // the objects, then the archive members that define something still undefined
  nobj = 0;
  for (i = 0; i < nf; i++) if (fobj[i]) addobj((struct t2o *)fbuf[i], fpath[i]);
  changed = 1;
  while (changed) {
    changed = 0;
    for (a = 0; a < nar; a++) {
      for (i = 0; i < ars[a]->h.nindex; i++) {
        m = ars[a]->index[i].member;
        if (madded[mbase[a] + m]) continue;
        buf = ars[a]->str + ars[a]->index[i].name;
        if (hobj[hfind(buf)] >= 0 || uobj[tfind(uobj, usym, buf)] < 0) continue;
        madded[mbase[a] + m] = 1;
        addobj(mobj[mbase[a] + m], memberpath(a, m));
        changed = 1;
      }
    }
  }

  // every undefined symbol, then the entry
  nundef = 0;
  for (i = 0; i < nobj; i++) {
    for (j = 0; j < objs[i]->h.nsyms; j++) {
      if (objs[i]->syms[j].sect != SEC_UNDEF) continue;
      if (hobj[hfind(symname(i, j))] < 0) {
        printf("ld: undefined symbol: %s (in %s)\n", symname(i, j), paths[i]);
        nundef++;
      }
    }
  }
  h = hfind(entname);
  if (hobj[h] < 0 || objs[hobj[h]]->syms[hsym[h]].sect != SEC_TEXT) {
    printf("ld: undefined symbol: %s\n", entname);
    nundef++;
  }
  if (nundef) quit();

  // layout
  csize = 0; dsize = 0; ninit = 0;
  for (i = 0; i < nobj; i++) {
    csize = (csize + 3) & -4;
    if (objs[i]->h.data_size > 0) dsize = (dsize + 3) & -4;
    tbase[i] = csize; dbase[i] = dsize;
    csize = csize + objs[i]->h.text_size; dsize = dsize + objs[i]->h.data_size;
    for (j = 0; j < objs[i]->h.nsyms; j++) if (objs[i]->syms[j].flags & SYM_INIT) ninit++;
  }
  entry = symval(hobj[h], hsym[h]);
  stub = (csize + 1) & -2;
  code = malloc(stub + 10 + 4 * ninit); memset(code, 0, stub + 10 + 4 * ninit);
  data = malloc(dsize + 1); memset(data, 0, dsize + 1);
  pos = 0;
  for (i = 0; i < nobj; i++) {
    while (pos < tbase[i]) {
      if (pos & 1) pos++;                                  // after an odd-sized text: a zero byte
      else { put16(code, pos, NOP); pos = pos + 2; }
    }
    memcpy(code + tbase[i], objs[i]->text, objs[i]->h.text_size);
    memcpy(data + dbase[i], objs[i]->data, objs[i]->h.data_size);
    pos = tbase[i] + objs[i]->h.text_size;
  }

  for (i = 0; i < nobj; i++) relocate(i);

  // entry stub for the run-before-main routines
  if (ninit) {
    pos = stub;
    put16(code, pos, PUSH_R4_LR); pos = pos + 2;
    for (i = 0; i < nobj; i++) {
      for (j = 0; j < objs[i]->h.nsyms; j++) {
        if (!(objs[i]->syms[j].flags & SYM_INIT)) continue;
        put16(code, pos, B_W); put16(code, pos + 2, BL_2);
        patch_b(pos, symval(i, j), 0, symname(i, j), paths[i]);
        pos = pos + 4;
      }
    }
    put16(code, pos, POP_W_R4_LR); put16(code, pos + 2, POP_W_R4_LR_2); pos = pos + 4;
    put16(code, pos, B_W); put16(code, pos + 2, B_W_2);
    patch_b(pos, entry, 0, entname, out);
    csize = pos + 4;
    entry = stub;
  }

  fd = open(out, 577, 420);    // O_WRONLY|O_CREAT|O_TRUNC, 0644
  if (fd < 0) { printf("ld: cannot write %s\n", out); quit(); }
  hdr = 0;
  if (write(fd, "T2CC", 4) != 4 || write(fd, &csize, 4) != 4 || write(fd, &dsize, 4) != 4
      || write(fd, &hdr, 4) != 4 || write(fd, &entry, 4) != 4
      || write(fd, code, csize) != csize || (dsize && write(fd, data, dsize) != dsize)) {
    printf("ld: cannot write %s\n", out); close(fd); quit();
  }
  close(fd);
  return 0;
}
