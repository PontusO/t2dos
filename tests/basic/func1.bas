DECLARE FUNCTION fact& (n AS INTEGER)
FUNCTION fact& (n AS INTEGER)
  IF n <= 1 THEN fact& = 1 ELSE fact& = n * fact&(n - 1)
END FUNCTION

FUNCTION half! (x!)
  half = x / 2
END FUNCTION

FUNCTION rev$ (s$)
  IF LEN(s$) <= 1 THEN
    rev$ = s$
  ELSE
    rev$ = rev$(MID$(s$, 2)) + LEFT$(s$, 1)
  END IF
END FUNCTION

FUNCTION kind$ (n)
  SELECT CASE n
    CASE 0
      kind$ = "zero"
    CASE IS > 0
      kind$ = "pos(" + kind$(n - 1) + ")"
    CASE ELSE
      kind$ = "neg"
  END SELECT
END FUNCTION

' the recursion runs inside a CASE test: the SELECT value must be the frame's own
FUNCTION sel (n)
  IF n = 0 THEN sel = 0: EXIT FUNCTION
  SELECT CASE n
    CASE sel(n - 1) - 5
      sel = -1
    CASE 1 TO 100
      sel = n * 10
  END SELECT
END FUNCTION

FUNCTION sumto% (n%)
  s% = 0
  FOR i% = 1 TO n%
    s% = s% + i%
    IF i% = 5 THEN EXIT FUNCTION
  NEXT
  sumto% = s%
END FUNCTION

FUNCTION seven
  seven = 7
END FUNCTION

PRINT fact&(10)
PRINT fact(12)
PRINT half(5)
PRINT rev$("hello")
PRINT kind$(2)
PRINT sel(3)
PRINT sumto%(3); sumto%(10)
PRINT seven + seven
f = fact&(5): PRINT f
