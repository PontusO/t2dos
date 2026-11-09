DIM SHARED a(5)
a(2) = 1
bump a(2)
PRINT a(2)
ERASE a
REDIM a(3)
PRINT UBOUND(a)
SUB bump (x)
  x = x + 1
END SUB
