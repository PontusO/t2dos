PIPE "bsrv"
PIPE "bsrv.out"
OPEN "/pipe/bsrv" FOR INPUT AS #1
DO
  LINE INPUT #1, a$
  IF a$ = "quit" THEN EXIT DO
  OPEN "/pipe/bsrv.out" FOR OUTPUT AS #2
  PRINT #2, "got: "; a$
  CLOSE #2
LOOP
CLOSE
