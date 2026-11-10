OPEN "fl.tmp" FOR OUTPUT AS #1
PRINT #1, STRING$(1023, "a")
PRINT #1, STRING$(1024, "b")
CLOSE #1
OPEN "fl.tmp" FOR INPUT AS #1
INPUT #1, a$
PRINT LEN(a$)
INPUT #1, b$
PRINT "not reached"
