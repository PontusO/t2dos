FUNCTION depth (n)
  IF n = 0 THEN depth = 0 ELSE depth = 1 + depth(n - 1)
END FUNCTION

FUNCTION fib (n)
  IF n < 2 THEN fib = n ELSE fib = fib(n - 1) + fib(n - 2)
END FUNCTION

SUB down (n)
  DIM t(2)
  t$ = STR$(n)
  t(1) = n
  IF n > 0 THEN down n - 1
  IF n = 1000 THEN PRINT "down"; t(1); t$
END SUB

PRINT depth(1000)
PRINT fib(15)
down 1000
