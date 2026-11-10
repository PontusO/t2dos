' Guess the number: the computer picks 1 to 100, you have to find it.
RANDOMIZE TIMER
secret = INT(RND * 100) + 1
tries = 0
PRINT "I am thinking of a number from 1 to 100."
DO
  INPUT "Your guess"; g
  tries = tries + 1
  IF g < secret THEN
    PRINT "Too low."
  ELSEIF g > secret THEN
    PRINT "Too high."
  END IF
LOOP UNTIL g = secret
PRINT "Right! You needed"; tries; "guesses."
