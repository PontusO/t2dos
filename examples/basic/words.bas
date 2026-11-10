' Count the lines, words and characters of a text file.
' Usage: basic words.bas file
f$ = COMMAND$
IF f$ = "" THEN
  PRINT "usage: basic words.bas file"
  END
END IF
OPEN f$ FOR INPUT AS #1
DO WHILE NOT EOF(1)
  LINE INPUT #1, l$
  lines = lines + 1
  chars = chars + LEN(l$) + 1
  l$ = LTRIM$(RTRIM$(l$))
  DO WHILE LEN(l$) > 0
    words = words + 1
    p = INSTR(l$, " ")
    IF p = 0 THEN
      l$ = ""
    ELSE
      l$ = LTRIM$(MID$(l$, p))
    END IF
  LOOP
LOOP
CLOSE #1
PRINT lines; "lines,"; words; "words,"; chars; "characters"
