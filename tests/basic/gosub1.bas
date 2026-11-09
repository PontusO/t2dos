PRINT "start"
GOSUB hello
GOSUB hello
FOR i = 1 TO 2
  GOSUB show
NEXT
GOSUB outer
PRINT "end"
END
hello:
  PRINT "hello"
  RETURN
show:
  PRINT "show"; i
  RETURN
outer:
  PRINT "outer"
  GOSUB hello
  FOR k = 1 TO 3
    IF k = 2 THEN RETURN
  NEXT
  PRINT "not reached"
  RETURN
