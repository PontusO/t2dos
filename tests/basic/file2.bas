MKDIR "d2.tmp"
OPEN "d2.tmp\b.txt" FOR OUTPUT AS #1
PRINT #1, "bee"
CLOSE #1
OPEN "d2.tmp\a.txt" FOR OUTPUT AS #1: CLOSE #1
MKDIR "d2.tmp\sub"
FILES "d2.tmp"
NAME "d2.tmp\a.txt" AS "d2.tmp\c.txt"
CHDIR "d2.tmp"
FILES
OPEN "b.txt" FOR INPUT AS #1
LINE INPUT #1, a$
PRINT a$
CLOSE #1
KILL "b.txt"
KILL "c.txt"
CHDIR ".."
RMDIR "d2.tmp\sub"
PRINT "done"
