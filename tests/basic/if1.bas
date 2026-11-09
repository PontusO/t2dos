x = 5
IF x = 5 THEN PRINT "five"
IF x = 4 THEN PRINT "four" ELSE PRINT "not four"
IF x > 1 THEN
  PRINT "gt1"
  IF x > 10 THEN
    PRINT "gt10"
  ELSEIF x > 4 THEN
    PRINT "gt4"
  ELSE
    PRINT "le4"
  END IF
ELSE
  PRINT "le1"
END IF
FOR i = 1 TO 6
  IF i = 1 THEN
    PRINT "one";
  ELSEIF i = 2 THEN
    PRINT " two";
  ELSEIF i = 3 THEN
    PRINT " three";
  ELSE
    PRINT " other"; i;
  END IF
NEXT
PRINT
IF x = 5 THEN done
PRINT "skipped"
done:
IF x = 6 THEN nope ELSE yes
PRINT "skipped too"
nope:
PRINT "nope"
yes:
PRINT "yes"
IF x THEN PRINT "nonzero"
IF 0.5 THEN PRINT "half true"
IF x = 5 THEN IF x > 6 THEN PRINT "a" ELSE PRINT "b"
IF x = 5 GOTO fin
PRINT "no"
fin:
PRINT "end"
