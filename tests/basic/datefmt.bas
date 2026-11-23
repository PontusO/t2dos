good = 1
IF LEN(DATE$) <> 10 THEN good = 0
IF MID$(DATE$, 5, 1) <> "-" THEN good = 0
IF MID$(DATE$, 8, 1) <> "-" THEN good = 0
IF LEN(TIME$) <> 8 THEN good = 0
IF MID$(TIME$, 3, 1) <> ":" THEN good = 0
IF MID$(TIME$, 6, 1) <> ":" THEN good = 0
IF good THEN PRINT "ok" ELSE PRINT "bad " + DATE$ + " " + TIME$
