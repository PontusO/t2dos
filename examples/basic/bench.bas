' Speed: 100000 passes of two statements each (the assignment and NEXT).
t = TIMER
FOR i = 1 TO 100000
  s = s + i * 2
NEXT
e = TIMER - t
PRINT "seconds:"; e
PRINT "statements per second:"; INT(200000 / e)
