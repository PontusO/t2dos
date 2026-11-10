OPEN "fl2.tmp" FOR OUTPUT AS #1
a$ = STRING$(20000, "a")
PRINT #1, a$; LEFT$(a$, 12767)
PRINT #1, a$; LEFT$(a$, 12768)
CLOSE #1
OPEN "fl2.tmp" FOR INPUT AS #1
LINE INPUT #1, b$
PRINT LEN(b$)
LINE INPUT #1, b$
PRINT "not reached"
