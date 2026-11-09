DIM SHARED a$(5)
a$(1) = "hello"
a$(1) = f$
PRINT "never"
FUNCTION f$
  REDIM a$(2)
  f$ = "x"
END FUNCTION
