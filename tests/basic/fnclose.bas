DECLARE FUNCTION g (x)
OPEN "fc1.tmp" FOR OUTPUT AS #1
PRINT "start"
PRINT #1, "a"; g(1); "b"
PRINT "not reached"
END

FUNCTION g (x)
  CLOSE #1
  g = x
END FUNCTION
