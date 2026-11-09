// lex.c - BASIC source text to tokens (struct Tok, basic.h), the whole file at load time.
// Keywords are case-insensitive; names keep their spelling. Comments (REM, ') and `_` line
// continuations disappear here; CR, a missing final newline and a DOS end-of-file mark
// (Ctrl-Z) are all fine. DATA is lexed here as raw text up to a ':' or an apostrophe outside
// quotes (the compiler splits it into items). Problems become TK_ERR tokens, reported when the compiler gets
// there, so the first error in the file is the one shown.
#include "basic.h"

struct Tok *toks;
int ntok;

// keyword table: kw_add() in lex_init()
char **kw_name;
int *kw_tk;
int nkw;
int kwcap;

void kw_add(char *name, int tk) {
  char **n; int *m;
  if (nkw >= kwcap) {                            // grow: the table is sized by what is added
    kwcap = kwcap * 2 + 64;
    n = malloc(kwcap * sizeof(char *)); m = malloc(kwcap * 4);
    if (nkw > 0) { memcpy(n, kw_name, nkw * sizeof(char *)); memcpy(m, kw_tk, nkw * 4); }
    if (kw_name) { free(kw_name); free(kw_tk); }
    kw_name = n; kw_tk = m;
  }
  kw_name[nkw] = name; kw_tk[nkw] = tk; nkw++;
}

// QBasic words outside this BASIC (spec non-goals and other QBasic statements and functions):
// a program using one gets "Advanced feature unavailable", never something else
char *unsup_words =
  "SCREEN PSET PRESET CIRCLE DRAW PAINT PALETTE VIEW WINDOW PCOPY PMAP POINT SOUND PLAY "
  "PEEK POKE DEF DATE$ TIME$ TYPE STATIC COMMON CHAIN ON ERROR RESUME ERR ERL ERDEV ERDEV$ "
  "USING GET PUT FIELD LSET RSET RANDOM BINARY ACCESS LOCK UNLOCK SEEK LOF LOC OUT INP WAIT "
  "KEY BLOAD BSAVE CVI CVL CVS CVD MKI$ MKL$ MKS$ MKD$ ENVIRON ENVIRON$ AUTO LIST RENUM "
  "OPTION LPRINT LPOS CSRLIN POS VARPTR VARPTR$ VARSEG SADD FRE CLEAR TRON TROFF IOCTL "
  "IOCTL$ STRIG STICK PEN UEVENT SETMEM ABSOLUTE INTERRUPT CALLS INPUT$ RESET FILEATTR";

int lex_init() {
  char *p; char *w; int n;
  kw_name = 0; kw_tk = 0; nkw = 0; kwcap = 0;
  kw_add("MOD", K_MOD); kw_add("AND", K_AND); kw_add("OR", K_OR); kw_add("XOR", K_XOR);
  kw_add("EQV", K_EQV); kw_add("IMP", K_IMP); kw_add("NOT", K_NOT);
  kw_add("PRINT", K_PRINT); kw_add("LET", K_LET); kw_add("SWAP", K_SWAP); kw_add("CONST", K_CONST);
  kw_add("DIM", K_DIM); kw_add("REDIM", K_REDIM); kw_add("ERASE", K_ERASE); kw_add("SHARED", K_SHARED);
  kw_add("AS", K_AS); kw_add("INTEGER", K_INTEGER); kw_add("LONG", K_LONG); kw_add("SINGLE", K_SINGLE);
  kw_add("DOUBLE", K_DOUBLE); kw_add("STRING", K_STRING);
  kw_add("DEFINT", K_DEFINT); kw_add("DEFLNG", K_DEFLNG); kw_add("DEFSNG", K_DEFSNG);
  kw_add("DEFDBL", K_DEFDBL); kw_add("DEFSTR", K_DEFSTR);
  kw_add("IF", K_IF); kw_add("THEN", K_THEN); kw_add("ELSE", K_ELSE); kw_add("ELSEIF", K_ELSEIF);
  kw_add("END", K_END); kw_add("FOR", K_FOR); kw_add("TO", K_TO); kw_add("STEP", K_STEP);
  kw_add("NEXT", K_NEXT); kw_add("WHILE", K_WHILE); kw_add("WEND", K_WEND); kw_add("DO", K_DO);
  kw_add("LOOP", K_LOOP); kw_add("UNTIL", K_UNTIL); kw_add("SELECT", K_SELECT); kw_add("CASE", K_CASE);
  kw_add("IS", K_IS); kw_add("EXIT", K_EXIT); kw_add("GOTO", K_GOTO); kw_add("GOSUB", K_GOSUB);
  kw_add("RETURN", K_RETURN); kw_add("STOP", K_STOP); kw_add("SYSTEM", K_SYSTEM); kw_add("SUB", K_SUB);
  kw_add("FUNCTION", K_FUNCTION); kw_add("CALL", K_CALL); kw_add("DECLARE", K_DECLARE);
  kw_add("DATA", K_DATA); kw_add("READ", K_READ); kw_add("RESTORE", K_RESTORE);
  kw_add("RANDOMIZE", K_RANDOMIZE); kw_add("INPUT", K_INPUT); kw_add("LINE", K_LINE); kw_add("CLS", K_CLS);
  kw_add("LOCATE", K_LOCATE); kw_add("COLOR", K_COLOR); kw_add("BEEP", K_BEEP); kw_add("SLEEP", K_SLEEP);
  kw_add("WIDTH", K_WIDTH); kw_add("OPEN", K_OPEN); kw_add("CLOSE", K_CLOSE); kw_add("OUTPUT", K_OUTPUT);
  kw_add("APPEND", K_APPEND); kw_add("WRITE", K_WRITE); kw_add("KILL", K_KILL); kw_add("NAME", K_NAME);
  kw_add("MKDIR", K_MKDIR); kw_add("RMDIR", K_RMDIR); kw_add("CHDIR", K_CHDIR); kw_add("FILES", K_FILES);
  kw_add("SHELL", K_SHELL); kw_add("PIPE", K_PIPE); kw_add("RUN", K_RUN); kw_add("PINMODE", K_PINMODE);
  kw_add("PINWRITE", K_PINWRITE); kw_add("PWM", K_PWM); kw_add("PAUSE", K_PAUSE);
  kw_add("ABS", K_ABS); kw_add("SGN", K_SGN); kw_add("INT", K_INT); kw_add("FIX", K_FIX);
  kw_add("SQR", K_SQR); kw_add("SIN", K_SIN); kw_add("COS", K_COS); kw_add("TAN", K_TAN);
  kw_add("ATN", K_ATN); kw_add("EXP", K_EXP); kw_add("LOG", K_LOG); kw_add("RND", K_RND);
  kw_add("TIMER", K_TIMER); kw_add("CINT", K_CINT); kw_add("CLNG", K_CLNG); kw_add("CSNG", K_CSNG);
  kw_add("CDBL", K_CDBL); kw_add("LEN", K_LEN); kw_add("LEFT$", K_LEFTS); kw_add("RIGHT$", K_RIGHTS);
  kw_add("MID$", K_MIDS); kw_add("INSTR", K_INSTR); kw_add("UCASE$", K_UCASES); kw_add("LCASE$", K_LCASES);
  kw_add("LTRIM$", K_LTRIMS); kw_add("RTRIM$", K_RTRIMS); kw_add("STR$", K_STRS); kw_add("VAL", K_VAL);
  kw_add("CHR$", K_CHRS); kw_add("ASC", K_ASC); kw_add("STRING$", K_STRINGS); kw_add("SPACE$", K_SPACES);
  kw_add("HEX$", K_HEXS); kw_add("OCT$", K_OCTS); kw_add("INKEY$", K_INKEYS); kw_add("EOF", K_EOF);
  kw_add("FREEFILE", K_FREEFILE); kw_add("LBOUND", K_LBOUND); kw_add("UBOUND", K_UBOUND);
  kw_add("COMMAND$", K_COMMANDS); kw_add("TAB", K_TAB); kw_add("SPC", K_SPC);
  kw_add("PINREAD", K_PINREAD); kw_add("ANALOGREAD", K_ANALOGREAD);
  kw_add("REM", K_REM);
  p = unsup_words;
  while (*p) {
    n = 0;
    while (p[n] && p[n] != ' ') n++;
    w = malloc(n + 1); memcpy(w, p, n);
    kw_add(w, K_UNSUP);
    p = p + n;
    if (*p) p++;
  }
  return 0;
}

int kw_find(char *uname) {
  int i;
  for (i = 0; i < nkw; i++)
    if (kw_name[i][0] == uname[0] && !strcmp(kw_name[i], uname)) return kw_tk[i];
  return 0;
}

int add_tok(int tk, int val, int line) {
  toks[ntok].tk = tk; toks[ntok].val = val; toks[ntok].line = line; toks[ntok].suf = 0;
  ntok++;
  return ntok - 1;
}

int is_alpha(int c) { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z'); }
int is_alnum(int c) { return is_alpha(c) || (c >= '0' && c <= '9'); }
int is_suffix(int c) { return c == '%' || c == '&' || c == '!' || c == '#' || c == '$'; }

// at src[i]: only blanks (and a CR) up to the end of the line?
int rest_blank(char *src, int len, int i) {
  while (i < len && src[i] != '\n') {
    if (src[i] != ' ' && src[i] != '\t' && src[i] != '\r') return 0;
    i++;
  }
  return 1;
}

// DATA is raw text up to a ':' or an apostrophe outside quotes, or the end of
// the line; a `_` ending a line joins
// the next one (as everywhere). data_end(): where the statement ends, *lines = the physical
// lines joined on the way; data_text(): its length, or (with out) the text without the joins.
int data_end(char *src, int len, int i, int *lines) {
  int q; int c;
  q = 0; *lines = 0;
  while (i < len && src[i] != '\n') {
    c = src[i];
    if (c == '_' && rest_blank(src, len, i + 1)) {
      while (i < len && src[i] != '\n') i++;
      if (i < len) { i++; *lines = *lines + 1; }
      continue;
    }
    if (c == '\r') {
      if (rest_blank(src, len, i)) break;
    }
    else if (c == '"') q = !q;
    else if (!q && (c == ':' || c == '\'')) break;
    i++;
  }
  return i;
}

int data_text(char *src, int len, int i, int e, char *out) {
  int n; int c;
  n = 0;
  while (i < e) {
    c = src[i];
    if (c == '_' && rest_blank(src, len, i + 1)) {
      while (i < e && src[i] != '\n') i++;
      if (i < e) i++;
      continue;
    }
    if (c != '\r') { if (out) out[n] = c; n++; }
    i++;
  }
  return n;
}

int lex(char *src, int len) {
  int i; int c; int line; int lline; int j; int k; int t; int v; int typ; char *s; char *u; int f; float g; int q; int e;
  toks = malloc((len + 4) * sizeof(struct Tok));   // each token takes at least one character
  ntok = 0;
  i = 0; line = 1; lline = 1;                      // physical line; where the logical line began
  while (i < len) {
    c = src[i] & 255;
    if (c == ' ' || c == '\t' || c == '\r' || c == 26) { i++; continue; }
    if (c == '\n') {
      add_tok(TK_EOL, 0, lline);
      i++; line++; lline = line;
      continue;
    }
    if (c == '_' && rest_blank(src, len, i + 1)) {  // continuation: join the next line
      while (i < len && src[i] != '\n') i++;
      if (i < len) { i++; line++; }
      continue;
    }
    if (c == '\'') { while (i < len && src[i] != '\n') i++; continue; }
    if (c == '"') {
      j = i + 1;
      while (j < len && src[j] != '"' && src[j] != '\n') j++;
      k = j;                                       // QBasic: a string may end at the end of the line
      while (k > i + 1 && src[k - 1] == '\r' && (k == len || src[k] == '\n')) k--;
      if (k - (i + 1) > STR_MAX) add_tok(TK_ERR, (int)"String too long", lline);
      else add_tok(TK_STR, (int)str_from(src + i + 1, k - (i + 1)), lline);
      i = j;
      if (i < len && src[i] == '"') i++;
      continue;
    }
    if ((c >= '0' && c <= '9') || (c == '.' && i + 1 < len && src[i + 1] >= '0' && src[i + 1] <= '9') || c == '&') {
      j = num_scan(src + i, len - i, &typ, &v);
      if (j > 0) {
        i = i + j;
        if (i < len && is_suffix(src[i]) && src[i] != '$') {
          c = src[i]; i++;
          if ((c == '%' || c == '&') && typ == T_SNG) {   // 3.0% is fine, 1.5% is not
            g = bitsf(v); typ = -1;
            if (g > -2147483648.0 && g < 2147483648.0) { f = (int)g; if ((float)f == g) { typ = T_INT; v = f; } }
          }
          else if ((c == '!' || c == '#') && typ == T_INT) { typ = T_SNG; v = fbits((float)v); }
        }
        if (typ == T_INT) add_tok(TK_INT, v, lline);
        else if (typ == T_SNG) add_tok(TK_SNG, v, lline);
        else if (typ == T_NONE) add_tok(TK_ERR, (int)"Overflow", lline);
        else add_tok(TK_ERR, (int)"Syntax error", lline);
        continue;
      }
    }
    if (is_alpha(c)) {
      j = i + 1;
      while (j < len && (is_alnum(src[j]) || src[j] == '.' || (src[j] == '_' && j + 1 < len && is_alnum(src[j + 1])))) j++;
      if (j - i > 40) { add_tok(TK_ERR, (int)"Identifier too long", lline); i = j; continue; }
      u = malloc(j - i + 2);
      for (k = 0; k < j - i; k++) { c = src[i + k]; if (c >= 'a' && c <= 'z') c = c - 32; u[k] = c; }
      t = 0;
      if (j < len && src[j] == '$') { u[k] = '$'; t = kw_find(u); u[k] = 0; if (t) j++; }
      if (!t) t = kw_find(u);
      free(u);
      if (t == K_REM) { while (j < len && src[j] != '\n') j++; i = j; continue; }
      if (t == K_DATA) {                           // the rest of the statement is raw text
        k = add_tok(K_DATA, 0, lline);
        i = j;
        e = data_end(src, len, i, &q);             // q: physical lines joined by `_`
        if (data_text(src, len, i, e, 0) > STR_MAX) { toks[k].tk = TK_ERR; toks[k].val = (int)"String too long"; }
        else {
          v = data_text(src, len, i, e, 0);
          s = str_new(v);
          data_text(src, len, i, e, s);
          toks[k].val = (int)s;
        }
        i = e; line = line + q;
        continue;
      }
      if (t) {
        k = add_tok(t, 0, lline);
        if (t == K_UNSUP) toks[k].val = (int)str_from(src + i, j - i);
        i = j;
        continue;
      }
      s = malloc(j - i + 1); memcpy(s, src + i, j - i);
      k = add_tok(TK_ID, (int)s, lline);
      i = j;
      if (i < len && is_suffix(src[i])) { toks[k].suf = src[i]; i++; }
      continue;
    }
    t = 0; k = 1;
    if (c == '+') t = TK_PLUS;
    else if (c == '-') t = TK_MINUS;
    else if (c == '*') t = TK_MUL;
    else if (c == '/') t = TK_DIV;
    else if (c == '\\') t = TK_IDIV;
    else if (c == '^') t = TK_POW;
    else if (c == '=') t = TK_EQ;
    else if (c == '(') t = TK_LPAR;
    else if (c == ')') t = TK_RPAR;
    else if (c == ',') t = TK_COMMA;
    else if (c == ';') t = TK_SEMI;
    else if (c == ':') t = TK_COLON;
    else if (c == '#') t = TK_HASH;
    else if (c == '?') t = K_PRINT;
    else if (c == '<') {
      t = TK_LT;
      if (i + 1 < len && src[i + 1] == '=') { t = TK_LE; k = 2; }
      else if (i + 1 < len && src[i + 1] == '>') { t = TK_NE; k = 2; }
    }
    else if (c == '>') { t = TK_GT; if (i + 1 < len && src[i + 1] == '=') { t = TK_GE; k = 2; } }
    if (t) add_tok(t, 0, lline);
    else add_tok(TK_ERR, (int)"Syntax error", lline);
    i = i + k;
  }
  if (ntok == 0 || toks[ntok - 1].tk != TK_EOL) add_tok(TK_EOL, 0, lline);
  add_tok(TK_EOF, 0, lline);
  return ntok;
}

void lex_free() {
  int i;
  for (i = 0; i < ntok; i++) {
    if (toks[i].tk == TK_STR || toks[i].tk == K_UNSUP || toks[i].tk == K_DATA) str_free((char *)toks[i].val);
    else if (toks[i].tk == TK_ID) free((char *)toks[i].val);
  }
  for (i = 0; i < nkw; i++) if (kw_tk[i] == K_UNSUP) free(kw_name[i]);
  if (kw_name) { free(kw_name); free(kw_tk); }
  kw_name = 0; kw_tk = 0; nkw = 0; kwcap = 0;
  free(toks); toks = 0; ntok = 0;
}
