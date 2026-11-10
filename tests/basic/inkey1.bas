DO
  k$ = INKEY$
  IF k$ = "" THEN EXIT DO
  IF LEN(k$) = 2 THEN
    PRINT "ext "; MID$(k$, 2, 1); ASC(LEFT$(k$, 1))
  ELSE
    PRINT "key "; k$
  END IF
LOOP
PRINT "end"
