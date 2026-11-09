FOR i = 1 TO 2
GOSUB s
PRINT "back"; i
NEXT i
PRINT "done"
END
s: FOR i = 1 TO 1: NEXT i
RETURN
