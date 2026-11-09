READ a, b, c
PRINT a; b; c
READ s$, t$
PRINT s$; "|"; t$
READ u$, v$
PRINT u$; "|"; v$
READ x!, n%
PRINT x!; n%
RESTORE second
READ a, b
PRINT a; b
RESTORE
READ a
PRINT a
RESTORE second
READ a, b, c
PRINT a; b; c
READ d
PRINT "unreached"
END
DATA 1, 2, -3
DATA hello, "world, with comma"
DATA "  padded  ",  trimmed  
DATA 2.5, 7
second: DATA 40, 50
DATA 1E2
