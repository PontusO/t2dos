DIM SHARED c
FOR k = 1 TO 100000
  work
NEXT
PRINT "c="; c
END

SUB work
  FOR i = 1 TO 10
    IF i = 3 THEN GOTO skip
  NEXT
skip:
  c = c + 1
  FOR j = 1 TO 5
    IF j = 2 THEN EXIT SUB
  NEXT
END SUB
