a$ = "Hello, World"
PRINT LEN(a$)
PRINT LEFT$(a$, 5); "|"; RIGHT$(a$, 5); "|"
PRINT MID$(a$, 8); "|"; MID$(a$, 8, 3); "|"; MID$(a$, 20); "|"
PRINT INSTR(a$, "o"); INSTR(6, a$, "o"); INSTR(a$, "xyz"); INSTR(a$, "")
PRINT UCASE$(a$); LCASE$(a$)
PRINT "["; LTRIM$("  ab  "); "]["; RTRIM$("  ab  "); "]"
PRINT "["; STR$(5); "]["; STR$(-5); "]["; STR$(1.5); "]"
PRINT VAL(" 12abc"); VAL("&HFF"); VAL("1.5E2"); VAL("abc"); VAL("-3")
PRINT CHR$(65); CHR$(97); ASC("A"); ASC("abc")
PRINT STRING$(3, "x"); STRING$(2, 65); "|"; SPACE$(3); "|"
PRINT HEX$(255); "|"; HEX$(-1); "|"; HEX$(0); "|"; OCT$(8); "|"; OCT$(-1)
PRINT LEFT$(a$, 0); "|"; LEFT$(a$, 100)
