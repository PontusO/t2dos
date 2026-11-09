i = 0
WHILE i < 3
  PRINT i;
  i = i + 1
WEND
PRINT
i = 0
DO WHILE i < 3
  i = i + 1: PRINT i;
LOOP
PRINT
i = 10
DO UNTIL i <= 7
  i = i - 1: PRINT i;
LOOP
PRINT
i = 0
DO
  i = i + 1: PRINT i;
LOOP WHILE i < 3
PRINT
DO
  i = i - 1: PRINT i;
LOOP UNTIL i <= 0
PRINT
i = 100
DO WHILE i < 3
  PRINT "never"
LOOP
DO
  PRINT "once";
LOOP WHILE 0
PRINT
DO
  i = i + 1
  IF i > 103 THEN EXIT DO
LOOP
PRINT i
FOR a = 1 TO 3
  FOR b = 1 TO 3
    IF b = 2 THEN EXIT FOR
    PRINT a; b;
  NEXT b
NEXT a
PRINT
i = 0
WHILE -1
  i = i + 1
  IF i = 5 THEN GOTO leave
WEND
leave:
PRINT "i="; i
