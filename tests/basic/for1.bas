FOR i = 1 TO 3: PRINT i;: NEXT i
PRINT
FOR i = 10 TO 1 STEP -3: PRINT i;: NEXT
PRINT
FOR x = 1 TO 3 STEP .5: PRINT x;: NEXT
PRINT
n = 3
FOR i = 1 TO n
  n = 10
  c = c + 1
NEXT
PRINT c; i
FOR i = 1 TO 0: PRINT "never": NEXT
PRINT i
FOR i = 5 TO 1 STEP -2: NEXT
PRINT i
FOR i = 1 TO 2
  FOR j = 1 TO 2
    PRINT i * 10 + j;
  NEXT j, i
PRINT
FOR i% = 1 TO 3: s% = s% + i%: NEXT: PRINT s%
