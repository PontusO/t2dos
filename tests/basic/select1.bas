FOR n = 1 TO 8
  SELECT CASE n
    CASE 1
      PRINT "one";
    CASE 2, 3
      PRINT "two-three";
    CASE 4 TO 5
      PRINT "four-five";
    CASE IS > 7
      PRINT "big";
    CASE ELSE
      PRINT "other";
  END SELECT
  PRINT n
NEXT
a$ = "bob"
SELECT CASE a$
  CASE "alice"
    PRINT "A"
  CASE "bob", "carol"
    PRINT "B or C"
  CASE ELSE
    PRINT "?"
END SELECT
SELECT CASE a$
  CASE "a" TO "c"
    PRINT "in a..c"
  CASE IS >= "d"
    PRINT "d or later"
END SELECT
x = 2.5
SELECT CASE x
  CASE 2
    PRINT "2"
  CASE 2.5
    PRINT "2.5"
END SELECT
SELECT CASE 1
END SELECT
SELECT CASE 3
  CASE 1: PRINT "x"
  CASE 3: PRINT "three"
END SELECT
SELECT CASE n
  CASE IS < 0, 9
    PRINT "neg or nine"
END SELECT
