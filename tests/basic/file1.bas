OPEN "f1.tmp" FOR OUTPUT AS #1
PRINT #1, "alpha"
PRINT #1, 12; -3; "x"
WRITE #1, "q,uote", 5, 2.5
CLOSE #1
OPEN "f1.tmp" FOR APPEND AS #1
PRINT #1, "tail"
PRINT #1, 1, 2
CLOSE 1
OPEN "f1.tmp" FOR INPUT AS #1
LINE INPUT #1, a$
PRINT "["; a$; "]"
LINE INPUT #1, a$
PRINT "["; a$; "]"
INPUT #1, s$, n%, f!
PRINT s$; "|"; n%; "|"; f!
n = 0
DO UNTIL EOF(1)
  LINE INPUT #1, a$
  n = n + 1
  PRINT n; ":"; a$
LOOP
PRINT EOF(1)
CLOSE
PRINT FREEFILE
KILL "f1.tmp"
