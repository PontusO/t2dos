DIM a(5)
FOR i = 0 TO 5: a(i) = i * i: NEXT
FOR i = 0 TO 5: PRINT a(i); : NEXT: PRINT
a = 42: PRINT a; a(1)
DIM b(1 TO 3, 0 TO 2) AS INTEGER
FOR i = 1 TO 3: FOR j = 0 TO 2: b(i, j) = i * 10 + j: NEXT j, i
PRINT b(1, 0); b(2, 1); b(3, 2)
PRINT LBOUND(b); UBOUND(b); LBOUND(b, 2); UBOUND(b, 2)
c(10) = 7
PRINT c(10); c(0); LBOUND(c); UBOUND(c)
DIM s$(2)
s$(0) = "x": s$(1) = "yy": s$(2) = s$(0) + s$(1)
PRINT s$(2)
REDIM a(2 TO 4)
PRINT a(2); a(3); LBOUND(a); UBOUND(a)
a(3) = 9: PRINT a(3)
REDIM s$(1)
PRINT "["; s$(0); s$(1); "]"
s$(1) = "keep"
ERASE s$, a
REDIM a(1)
PRINT a(0); a(1)
DIM n%(-2 TO 2)
n%(-2) = -5: PRINT n%(-2); LBOUND(n%); UBOUND(n%)
DIM m(1, 1, 1)
m(1, 1, 1) = 2.5: PRINT m(1, 1, 1) + m(0, 0, 0)
SWAP b(1, 0), b(3, 2)
PRINT b(1, 0); b(3, 2)
DIM w AS STRING, z AS INTEGER
w = "typed": z = 3.7: PRINT w; z
REDIM s$(1)
FOR i = 0 TO 2: READ s$(i MOD 2): NEXT
PRINT s$(0); s$(1)
DATA p, q, r
