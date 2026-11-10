' Towers of Hanoi with a recursive SUB.
DECLARE SUB Move (n, src$, dst$, via$)
n = 3
PRINT "Moving"; n; "disks from A to C:"
Move n, "A", "C", "B"
PRINT "Moves needed:"; 2 ^ n - 1

SUB Move (n, src$, dst$, via$)
  IF n = 0 THEN EXIT SUB
  Move n - 1, src$, via$, dst$
  PRINT "Disk"; n; "from "; src$; " to "; dst$
  Move n - 1, via$, dst$, src$
END SUB
