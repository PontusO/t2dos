FOR k = 1 TO 100000
  FOR i = 1 TO 10
    IF i = 3 THEN GOTO skip
  NEXT i
  skip:
NEXT k
PRINT "k="; k
c = 0
again:
c = c + 1
FOR j = 1 TO 5
  IF j = 2 THEN GOTO next1
NEXT
next1:
IF c < 100000 THEN again
PRINT "c="; c
PRINT "last line"
