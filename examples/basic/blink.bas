' Blink the board LED (pin 25 on the CPico) five times.
CONST led = 25
PINMODE led, 1
FOR i = 1 TO 5
  PINWRITE led, 1
  PAUSE 200
  PINWRITE led, 0
  PAUSE 200
NEXT
PRINT "Done."
