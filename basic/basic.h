// basic.h - T2DOS BASIC: what the interpreter's files share.
//
// Files: main.c (command line, loading, error messages, console output), lex.c (source to
// tokens), comp.c (tokens to bytecode; every load-time check), vm.c (runs the bytecode),
// fmt.c (numbers to and from text the QBasic way), pcsys.h/pcsys.c (PC build only: the T2DOS
// library on top of libc). Written in the T2DOS C subset (no static, const, do/while, goto,
// struct assignment, own variadics, doubles, 64-bit ints): cc builds it for the board.
//
// How a program runs: lex() turns the whole file into tokens, compile() turns the tokens into
// bytecode (types, variables and constants resolved; any error stops the load before
// anything runs), vm_run() executes the bytecode.
//
// Adding a statement: a token (the enum below and a kw_add() line in lex_init()), a compile
// function registered in comp_init() (stmt_tab[K_X] = st_x), and its opcodes (the op enum
// below and a handler registered in vm_init()).
#include "sys.h"

// ---------------------------------------------------------------- types
// Static types: every expression, variable and constant has one, known at load time.
// INTEGER and LONG are both T_INT (32 bits); SINGLE and DOUBLE are both T_SNG (32-bit float).
enum { T_NONE = 0, T_INT = 1, T_SNG = 2, T_STR = 3 };

// ---------------------------------------------------------------- tokens
enum {
  TK_EOF = 0, TK_EOL,            // end of the program; end of a line (a ':' is TK_COLON)
  TK_INT, TK_SNG, TK_STR, TK_ID, // literals and names (see struct Tok)
  TK_ERR,                        // the lexer's error: val is the message, reported when reached
  TK_PLUS, TK_MINUS, TK_MUL, TK_DIV, TK_IDIV, TK_POW,
  TK_EQ, TK_NE, TK_LT, TK_GT, TK_LE, TK_GE,
  TK_LPAR, TK_RPAR, TK_COMMA, TK_SEMI, TK_COLON, TK_HASH,
  // operators written as words
  K_MOD, K_AND, K_OR, K_XOR, K_EQV, K_IMP, K_NOT,
  // statements and the words used inside them
  K_PRINT, K_LET, K_SWAP, K_CONST, K_DIM, K_REDIM, K_ERASE, K_SHARED, K_AS,
  K_INTEGER, K_LONG, K_SINGLE, K_DOUBLE, K_STRING,
  K_DEFINT, K_DEFLNG, K_DEFSNG, K_DEFDBL, K_DEFSTR,
  K_IF, K_THEN, K_ELSE, K_ELSEIF, K_END, K_FOR, K_TO, K_STEP, K_NEXT, K_WHILE, K_WEND,
  K_DO, K_LOOP, K_UNTIL, K_SELECT, K_CASE, K_IS, K_EXIT, K_GOTO, K_GOSUB, K_RETURN,
  K_STOP, K_SYSTEM, K_SUB, K_FUNCTION, K_CALL, K_DECLARE, K_DATA, K_READ, K_RESTORE,
  K_RANDOMIZE, K_INPUT, K_LINE, K_CLS, K_LOCATE, K_COLOR, K_BEEP, K_SLEEP, K_WIDTH,
  K_OPEN, K_CLOSE, K_OUTPUT, K_APPEND, K_WRITE, K_KILL, K_NAME, K_MKDIR, K_RMDIR, K_CHDIR,
  K_FILES, K_SHELL, K_PIPE, K_RUN, K_PINMODE, K_PINWRITE, K_PWM, K_PAUSE,
  // built-in functions, K_FN_FIRST to K_FN_LAST (a $ is part of the name: K_LEFTS is LEFT$)
  K_ABS, K_SGN, K_INT, K_FIX, K_SQR, K_SIN, K_COS, K_TAN, K_ATN, K_EXP, K_LOG, K_RND, K_TIMER,
  K_CINT, K_CLNG, K_CSNG, K_CDBL, K_LEN, K_LEFTS, K_RIGHTS, K_MIDS, K_INSTR, K_UCASES,
  K_LCASES, K_LTRIMS, K_RTRIMS, K_STRS, K_VAL, K_CHRS, K_ASC, K_STRINGS, K_SPACES, K_HEXS,
  K_OCTS, K_INKEYS, K_EOF, K_FREEFILE, K_LBOUND, K_UBOUND, K_COMMANDS, K_TAB, K_SPC,
  K_PINREAD, K_ANALOGREAD,
  // a QBasic word outside this BASIC (SCREEN, USING, ON, DEF, ...): val is the word as
  // written; reaching it is the load-time error "Advanced feature unavailable"
  K_UNSUP,
  K_REM,                         // lexer only: the rest of the line is a comment
  NTOK
};
#define K_FN_FIRST K_ABS
#define K_FN_LAST K_ANALOGREAD

// One token. line: the physical source line (from 1) where its logical line starts, so all
// tokens of a `_` continuation carry the first line. val by kind:
//   TK_INT the value; TK_SNG the float's bit pattern; TK_STR a str (see strings below, owned
//   by the token list); TK_ID the name as written (char *); suf: TK_ID's suffix character
//   ('%' '&' '!' '#' '$') or 0; K_UNSUP the word; TK_ERR the message.
struct Tok { int tk; int val; int line; int suf; };
extern struct Tok *toks;         // the program's tokens, ending in TK_EOL, TK_EOF
extern int ntok;
int lex_init();
int lex(char *src, int len);     // fills toks from the source text; returns ntok
void lex_free();

// ---------------------------------------------------------------- strings
// A str is a char * to len bytes of data (any byte, CHR$(0) too) followed by a 0, with the
// length in the int just before the data. The null pointer is the empty string "", so
// zeroed memory holds empty strings. Length 0 to 32767.
// Ownership rule: every str on the value stack or in a slot owns its heap block. Storing
// into a slot frees the slot's old block; popping a temporary (PRINT, a comparison, a
// function argument) frees it; an op that builds a new str frees the operands it consumed;
// string literals belong to the program (token list, constants) and OP_PUSHS pushes a copy,
// so nothing on the stack is ever shared and a str is freed exactly once.
#define STR_MAX 32767
char *str_new(int len);          // len bytes (zeroed), or 0 for len 0
void str_free(char *s);
int str_len(char *s);
char *str_dup(char *s);
char *str_from(char *p, int len); // a new str holding p[0..len)
int str_cmp(char *a, char *b);   // <0, 0, >0 by character codes, then length

// ---------------------------------------------------------------- bytecode
// code[] is a flat array of ints: an opcode followed by its arguments (noted below). The
// VM is a stack machine on vs[] (ints, float bit patterns and str pointers, one cell each;
// the static types say which). An op pops its operands and pushes its result.
// Bytecode position to source line: the compiler calls line_mark(line) at the start of
// every statement, recording (code position, line); line_of(pos) finds the last mark at or
// before pos. Run-time errors report err_at(position of the failing op).
enum {
  OP_END,                        // stop the program (END, end of the file)
  OP_HALT,                       // stop vm_run(), leaving the stack as it is (CONST at load)
  OP_PUSHI, OP_PUSHF, OP_PUSHS,  // arg: int / float bits / str literal (pushes a copy)
  OP_LDI, OP_LDF, OP_LDS,        // arg: global slot; push its value (a copy for strings)
  OP_STI, OP_STF, OP_STS,        // arg: global slot; pop into it (STS frees the old str)
  OP_REFI, OP_REFF, OP_REFS,     // arg: global slot; push the cell's address
  OP_SWAP,                       // pop two cell addresses, swap the cells (any one type)
  OP_I2F, OP_I2F2, OP_F2I,       // int to float (top, second from top); float to int
                                 // rounded half to even ("Overflow" outside 32 bits)
  OP_ADDI, OP_SUBI, OP_MULI, OP_IDIV, OP_MODI, OP_NEGI,
  OP_ADDF, OP_SUBF, OP_MULF, OP_DIVF, OP_NEGF,
  OP_POWF, OP_POWI,              // float ^ float (pow), float ^ int (exact products)
  OP_CAT,                        // str + str
  OP_EQI, OP_NEI, OP_LTI, OP_GTI, OP_LEI, OP_GEI,   // compare: push -1 (true) or 0
  OP_EQF, OP_NEF, OP_LTF, OP_GTF, OP_LEF, OP_GEF,
  OP_CMPS,                       // arg: TK_EQ .. TK_GE; compares two strs
  OP_AND, OP_OR, OP_XOR, OP_EQV, OP_IMP, OP_NOT,    // bitwise on ints
  OP_PRINTI, OP_PRINTF, OP_PRINTS, // PRINT one value (numbers with their spaces)
  OP_PRCOMMA, OP_PRNL,           // PRINT's ',' (next 14-column zone); end of line
  OP_JMP, OP_JZ, OP_JNZ,         // arg: target; jump; pop and jump if 0; pop and jump if not 0
  OP_FORI, OP_FORF,              // arg: position after the loop; pops step, limit, the variable's
                                 // address; skips the loop or pushes a FOR frame
  OP_NEXTI, OP_NEXTF,            // pops the variable's address; steps it, loops or pops the frame
  OP_EXITFOR,                    // pops the variable's address; drops its FOR frame (EXIT FOR)
  OP_GOSUB, OP_RETURN,           // arg: target; push a GOSUB frame; pop it and go back
  OP_READI, OP_READF, OP_READS,  // push the next DATA item as an int / float / str
  OP_RESTORE,                    // arg: index of the next DATA item
  OP_FN,                         // args: function token (K_ABS ..), count or type (funcs.c)
  NOPS
};

// ---------------------------------------------------------------- variables
// The slot store: global variables live in one array per type, indexed by the slot number
// the compiler gives each variable (nslot[T] slots of type T). Cells are 4 bytes, so an
// address (OP_REF*) works the same for every type. Zeroed at the start: 0, 0.0 and "".
// Reserved for Task 5: SUB/FUNCTION locals get their own cells in a call frame.
extern int *ivar;
extern float *fvar;
extern char **svar;
extern int nslot[4];             // indexed by T_INT, T_SNG, T_STR

// DATA items (compiler fills, READ reads): the text of each item as a str (the null pointer
// for an empty one) and whether it was quoted (a quoted item is a string only).
extern char **d_item;
extern int *d_quoted;
extern int ndata;

// Symbols (compiler only). kind S_VAR: a variable, slot in the slot store of its type;
// S_CONST: val is the constant's int, float bits or str. scope: 0 is the main program
// (Task 5 gives each SUB/FUNCTION its own). uname is the name in capitals; suf the type
// suffix ('%' '&' '!' '#' '$', after DEFtype for names written without one).
enum { S_VAR = 1, S_CONST };
struct Sym { char *uname; int suf; int kind; int type; int slot; int val; int scope; int link; };
extern struct Sym *syms;
extern int nsym;

// ---------------------------------------------------------------- compiler
extern int *code;                // the bytecode
extern int ncode;
extern int tp;                   // position in toks
extern void (*stmt_tab[NTOK])(); // statement compilers by first token
extern int (*fn_tab[NTOK])();    // built-in function compilers by token (Task 4): called with
                                 // the function's token current; return the result type
int comp_init();
int compile();                   // the whole token list, then OP_END; returns nerr == 0
void comp_free();
int emit(int op);                // append; returns the position written
int emit_arg(int v);
int expr();                      // compile an expression; returns its type
int expr_to(int type);           // expr() converted to type (Type mismatch if impossible)
int next();                      // advance; returns the new token code
int accept(int tk);              // 1 and advance if the token is tk
int need(int tk);                // advance over tk, or a load-time error
int at_end();                    // end of a statement: ':', end of line, ELSE, end of file
int cerr(char *msg);             // load-time error at the current token
int bad_token();                 // the error for an unexpected token (Syntax error, ...)
void line_mark(int line);
int line_of(int pos);
int suf_type(int suf);

// ---------------------------------------------------------------- VM
// Value stack: vs[sp] is the top (vs[0] is never used); fs is the same memory seen as floats.
// Control stack cs[], csp (index of the top word): frames that outlive a statement, the kind
// code on top. FOR: variable address, limit, step, body position, CK_FOR (5 words). GOSUB:
// return position, CK_GOSUB (2 words). Task 5 adds CALL frames (return position, caller's
// frame and stack base, locals). GOTO out of a loop leaves its frame behind: NEXT, EXIT FOR
// and RETURN drop the frames above the one they find, and a FOR on a variable that already
// has a frame replaces it, so such programs do not grow the stack.
enum { CK_FOR = 1, CK_GOSUB = 2 };
#define VSTACK 4096
#define CSTACK 4096
extern int pc;
extern int *vs;
extern float *fs;
extern int sp;
extern int *cs;
extern int csp;
extern int running;              // the VM loop runs while this is 1
extern void (*optab[NOPS])();
int vm_init();
int vm_alloc();                  // (re)allocate the slot store for nslot[]
int vm_run(int start);           // run from code[start] to OP_END / OP_HALT / an error
void vm_free();
int vm_err(char *msg);           // run-time error at the current op
int f2i(float f);                // QBasic float to integer: round half to even
int fok(float f);                // 1 if f is a number; else the run-time error Overflow, 0

// ---------------------------------------------------------------- numbers (fmt.c)
int fmt_int(char *buf, int v);   // "-12", "7"; returns the length
int fmt_sng(char *buf, float f); // QBasic SINGLE form without the spaces: ".3333333", "1E+07"
int num_scan(char *s, int n, int *type, int *val);  // a number at s (no sign): chars used
                                 // (0: none), *type T_INT / T_SNG (T_NONE: Overflow), *val
int fbits(float f);              // a float's bit pattern and back
float bitsf(int b);

// ---------------------------------------------------------------- funcs.c
// Built-in functions (and RANDOMIZE): funcs_init() fills fn_tab[] / stmt_tab[] with their
// compilers and optab[OP_FN] with the handler, which runs the function by its token. A
// function is a row in the table at the top of funcs.c (argument types, count, result).
void funcs_init();

// ---------------------------------------------------------------- main.c
extern char *srcname;            // the program's file name as given, for messages
extern int nerr;                 // errors so far (only the first is reported)
extern int prog_argc;            // the arguments after the file name (COMMAND$, Task 7)
extern char **prog_argv;
int err_line(int line, char *msg);  // print FILE:LINE: MSG once, stop compiling and running
int err_at(int pos, char *msg);     // the same for a bytecode position
extern int out_col;              // console column of the next character (0 = line start)
void out_char(int c);
void out_str(char *s, int n);
void out_cstr(char *s);
void out_flush();
