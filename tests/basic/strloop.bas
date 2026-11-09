FOR i = 1 TO 100000
  a$ = a$ + "x"
  IF LEN(a$) > 50 THEN a$ = ""
NEXT
PRINT "ok"
