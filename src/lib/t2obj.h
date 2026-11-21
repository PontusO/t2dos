// t2obj.h - T2DOS object and archive file formats. All fields are 32-bit little endian ints.

// object file: header, text bytes, data bytes, symbols, relocations, string table
struct t2obj_hdr { int magic; int version; int text_size; int data_size; int nsyms; int nrels; int str_size; };
enum { T2OBJ_MAGIC = 0x424F3254, T2OBJ_VERSION = 1 };          // "T2OB"
struct t2sym { int name; int sect; int value; int flags; };      // name: offset in the string table
enum { SEC_UNDEF = 0, SEC_TEXT = 1, SEC_DATA = 2, SEC_ABS = 3 };
enum { SYM_GLOBAL = 1, SYM_INIT = 2 };                           // SYM_INIT: a run-before-main routine
struct t2rel { int sect; int off; int sym; int type; int addend; };   // sym: index into the symbol table
enum {
  R_BL = 1,          // bl (T1, 24-bit) at off: target = S + A
  R_BW = 2,          // b.w (T4, 24-bit)
  R_BCOND = 3,       // b<cond>.w (T3, 20-bit)
  R_DATA_ADDW = 4,   // addw rd, rn, #imm12: S + A, must be 0..4095
  R_DATA_MOVW = 5,   // movw rd, #imm16: low 16 bits of S + A
  R_DATA_MOVT = 6,   // movt rd, #imm16: high 16 bits of S + A
  R_DATA_MOVW16 = 7, // movw without a movt: S + A must be 0..65535
  R_FNADDR = 8,      // movw/movt/add rd,pc (10 bytes at off): S + 1 - (off + 12), the PC-relative Thumb address
  R_WORD = 9         // 32-bit word: S + A (an offset, since there are no absolute addresses; e.g. jump tables)
};
// archive: header, index (symbol name offset, member number), member table, string table, member objects
struct t2ar_hdr { int magic; int version; int nmembers; int nindex; int str_size; };
enum { T2AR_MAGIC = 0x52413254 };                                 // "T2AR"
struct t2ar_index { int name; int member; };
struct t2ar_member { int name; int offset; int size; };          // offset from the start of the archive file
