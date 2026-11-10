DECLARE FUNCTION f (x)
OPEN "fo1.tmp" FOR OUTPUT AS #1
OPEN "fo2.tmp" FOR OUTPUT AS #2
PRINT #1, "a"; f(1); "b"
WRITE #2, f(2)
CLOSE
OPEN "fo1.tmp" FOR INPUT AS #1
LINE INPUT #1, a$
PRINT "o1:"; a$
OPEN "fo2.tmp" FOR INPUT AS #2
LINE INPUT #2, a$
PRINT "o2:"; a$
LINE INPUT #2, a$
PRINT "o2:"; a$
CLOSE
KILL "fo1.tmp"
KILL "fo2.tmp"
END

FUNCTION f (x)
  PRINT "console?"
  PRINT #2, "inner"
  f = x
END FUNCTION
