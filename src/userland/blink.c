// blink [pin] [count] : board I/O demo
#include "sys.h"
int main(int argc, char **argv) {
  int pin; int i; int count;
  pin = argc > 1 ? atoi(argv[1]) : 25; count = argc > 2 ? atoi(argv[2]) : 10;
  gpio_mode(pin, PIN_OUTPUT);
  for (i = 0; i < count; i++) { gpio_write(pin, 1); sleep(100); gpio_write(pin, 0); sleep(100); }
  return 0;
}
