' names are case sensitive, keywords are not
DIM Arr(2), arr(2)
Arr(1) = 1: arr(1) = 2
print Arr(1); arr(1)
total = 1: Total = 2: TOTAL = 3
PRINT total; Total; TOTAL
Greet "a"
greet "b"
PRINT Twice(3); twice(3)
GOSUB Here
GOSUB here
DEFSTR s
sName = "lower": SName = "upper"
PRINT sName; " "; SName
END
Here: PRINT "Here": RETURN
here: PRINT "here": RETURN
SUB Greet (x$)
  PRINT "Greet "; x$
END SUB
SUB greet (x$)
  PRINT "greet "; x$
END SUB
FUNCTION Twice (v)
  Twice = v * 2
END FUNCTION
FUNCTION twice (v)
  twice = v * 20
END FUNCTION
