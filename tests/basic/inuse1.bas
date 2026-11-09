DIM SHARED a(5)
bump a(2)
PRINT "never"
SUB bump (x)
  PRINT "in"
  ERASE a
  x = x + 1
END SUB
