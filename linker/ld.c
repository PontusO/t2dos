// ld - link T2DOS objects into a T2CC program (.bin)
//   ld [-o out.bin] [-e entry] file.o...      (default output a.bin, entry main)
// Errors print "ld: ..." and exit 1 without writing the output.
//
// Layout: the objects' text in command-line order, each object's text start aligned to 4
// with nop halfwords (an .align inside an object assumes a 4-aligned section start); then
// their data likewise, each object's data start aligned to 4 with zero bytes and nothing
// after the last object. A text symbol's value is its object's text base + value, a data
// symbol's its object's data base + value (data offsets are relative to r9), an absolute
// symbol's its value. A relocation's symbol, when it is defined in its own object (local or
// global, including the section symbols .text/.data), is that definition; when undefined,
// the one global definition among all objects.
// With SYM_INIT symbols, an entry stub follows the text: push {r4, lr}; bl init (each in
// link order); pop.w {r4, lr}; b.w entry. The stub is then the entry.
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
char *code; int csize; char *data; int dsize;
// global definitions: open hash table of (object, symbol) pairs, -1 when empty
int *hobj; int *hsym; int hsize;

int get16(char *b, int at) { return (b[at] & 255) | (b[at + 1] & 255) << 8; }
int put16(char *b, int at, int v) { b[at] = v & 255; b[at + 1] = (v >> 8) & 255; return 0; }
int put32(char *b, int at, int v) { put16(b, at, v & 65535); return put16(b, at + 2, (v >> 16) & 65535); }

char *symname(int ob, int sym) { return t2o_name(objs[ob], sym); }

int hash(char *s) {
  int h; h = 0;
  while (*s) { h = h * 31 + (*s & 255); s++; }
  return h & (hsize - 1);
}
// the slot of global name s: its entry, or the empty slot where it goes
int hfind(char *s) {
  int i; i = hash(s);
  while (hobj[i] >= 0 && strcmp(symname(hobj[i], hsym[i]), s) != 0) i = (i + 1) & (hsize - 1);
  return i;
}

// value of symbol sym of object ob in the final segments (sym is defined, or its global is)
int symval(int ob, int sym) {
  struct t2sym *s; int h;
  s = objs[ob]->syms + sym;
  if (s->sect == SEC_UNDEF) { h = hfind(symname(ob, sym)); return symval(hobj[h], hsym[h]); }
  if (s->sect == SEC_TEXT) return tbase[ob] + s->value;
  if (s->sect == SEC_DATA) return dbase[ob] + s->value;
  return s->value;
}

int bad(char *path) { printf("ld: %s: not a T2DOS object\n", path); exit(1); return 0; }

// relocations must patch whole fields inside their section; data holds only R_WORD;
// a run-before-main routine is a text symbol
int checkrels(int ob) {
  struct t2o *o; struct t2rel *r; int i; int n; int size;
  o = objs[ob];
  for (i = 0; i < o->h.nsyms; i++) if ((o->syms[i].flags & SYM_INIT) && o->syms[i].sect != SEC_TEXT) bad(paths[ob]);
  for (i = 0; i < o->h.nrels; i++) {
    r = o->rels + i;
    n = 4;
    if (r->type == R_FNADDR) n = 10;
    size = o->h.text_size;
    if (r->sect == SEC_DATA) { size = o->h.data_size; if (r->type != R_WORD) bad(paths[ob]); }
    if (r->type < R_BL || r->type > R_WORD || r->off < 0 || r->off > size - n) bad(paths[ob]);
  }
  return 0;
}

// b.w / bl (T4 / T1, 24-bit) or b<cond>.w (T3, 20-bit) at code + at to target
int patch_b(int at, int target, int cond, char *name, char *path) {
  int off; int s; int j1; int j2; int h1; int h2;
  off = target - (at + 4);
  h1 = get16(code, at); h2 = get16(code, at + 2);
  if ((off & 1) || (!cond && (off < -16777216 || off > 16777214)) || (cond && (off < -1048576 || off > 1048574))) {
    printf("ld: branch to %s out of range (in %s)\n", name, path); exit(1);
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
  exit(1);
  return 0;
}

int relocate(int ob) {
  struct t2o *o; struct t2rel *r; int i; int p; int v; char *name;
  o = objs[ob];
  for (i = 0; i < o->h.nrels; i++) {
    r = o->rels + i;
    name = symname(ob, r->sym);
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

int main(int argc, char **argv) {
  char *out; char *entname; char *buf; int size; int i; int j; int k; int h; int nundef;
  int ninit; int pos; int entry; int stub; int fd; int hdr; struct t2o *o; struct t2sym *s;
  out = "a.bin"; entname = "main";
  objs = malloc((argc + 1) * sizeof(struct t2o *));
  paths = malloc((argc + 1) * sizeof(char *));
  tbase = malloc((argc + 1) * sizeof(int));
  dbase = malloc((argc + 1) * sizeof(int));
  nobj = 0;
  for (i = 1; i < argc; i++) {
    if (strcmp(argv[i], "-o") == 0 && i + 1 < argc) { i++; out = argv[i]; }
    else if (strcmp(argv[i], "-e") == 0 && i + 1 < argc) { i++; entname = argv[i]; }
    else if (argv[i][0] == '-') usage();
    else {
      buf = t2_readfile(argv[i], &size);
      if (!buf) { printf("ld: cannot read %s\n", argv[i]); return 1; }
      o = malloc(sizeof(struct t2o));
      if (t2o_read(buf, size, o) < 0) bad(argv[i]);
      objs[nobj] = o; paths[nobj] = argv[i];
      checkrels(nobj);
      nobj++;
    }
  }
  if (nobj == 0) usage();

  // layout
  csize = 0; dsize = 0; ninit = 0; k = 0;
  for (i = 0; i < nobj; i++) {
    csize = (csize + 3) & -4; dsize = (dsize + 3) & -4;
    tbase[i] = csize; dbase[i] = dsize;
    csize = csize + objs[i]->h.text_size; dsize = dsize + objs[i]->h.data_size;
    for (j = 0; j < objs[i]->h.nsyms; j++) if (objs[i]->syms[j].flags & SYM_INIT) ninit++;
    k = k + objs[i]->h.nsyms;
  }
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

  // global definitions
  hsize = 16; while (hsize < 2 * k) hsize = hsize * 2;
  hobj = malloc(hsize * sizeof(int)); hsym = malloc(hsize * sizeof(int));
  for (i = 0; i < hsize; i++) hobj[i] = -1;
  for (i = 0; i < nobj; i++) {
    for (j = 0; j < objs[i]->h.nsyms; j++) {
      s = objs[i]->syms + j;
      if (s->sect == SEC_UNDEF || !(s->flags & SYM_GLOBAL)) continue;
      h = hfind(symname(i, j));
      if (hobj[h] >= 0) {
        printf("ld: duplicate symbol: %s (%s and %s)\n", symname(i, j), paths[hobj[h]], paths[i]);
        return 1;
      }
      hobj[h] = i; hsym[h] = j;
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
  if (nundef) return 1;
  entry = symval(hobj[h], hsym[h]);

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
  if (fd < 0) { printf("ld: cannot write %s\n", out); return 1; }
  hdr = 0;
  if (write(fd, "T2CC", 4) != 4 || write(fd, &csize, 4) != 4 || write(fd, &dsize, 4) != 4
      || write(fd, &hdr, 4) != 4 || write(fd, &entry, 4) != 4
      || write(fd, code, csize) != csize || (dsize && write(fd, data, dsize) != dsize)) {
    printf("ld: cannot write %s\n", out); close(fd); return 1;
  }
  close(fd);
  return 0;
}
