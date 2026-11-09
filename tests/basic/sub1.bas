DECLARE SUB bump (x)
DECLARE SUB fill (arr(), n AS INTEGER)
DIM SHARED total
DIM v(3)
a = 1
bump a
PRINT a
bump (a)
PRINT a
CALL bump(a)
PRINT a
CALL bump((a))
PRINT a
bump a + 1
PRINT a
v(2) = 10
bump v(2)
PRINT v(2)
fill v(), 3
PRINT v(0); v(1); v(2); v(3)
PRINT total
k = 5
leak
PRINT k; "["; q$; "]"
s$ = "abc"
addx s$
PRINT s$
addx s$ + "z"
PRINT s$
n% = 7
twice n%
PRINT n%
IF n% > 0 THEN leak
END

SUB bump (x)
  x = x + 1
  total = total + 1
END SUB

SUB fill (arr(), n AS INTEGER)
  FOR i = 0 TO n
    arr(i) = arr(i) + i * 100
  NEXT
END SUB

SUB leak
  k = 99
  q$ = "local"
  PRINT k; q$; total
END SUB

SUB addx (t$)
  t$ = t$ + "x"
END SUB

SUB twice (m AS INTEGER)
  m = m * 2
  IF m < 50 THEN twice m
END SUB
