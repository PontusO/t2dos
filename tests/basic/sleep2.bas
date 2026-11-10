SLEEP
k$ = INKEY$
PRINT ASC(LEFT$(k$, 1)); MID$(k$, 2); LEN(k$)
PRINT "[" + INKEY$ + "]"
