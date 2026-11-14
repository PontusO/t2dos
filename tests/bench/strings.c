// strings.c - character loops through pointers (char loads and stores, pointer increments)
#include <stdio.h>

char text[4096]; char copy[4096];

int my_strlen(char *s) { char *p; p = s; while (*p) p++; return p - s; }
int my_strcpy(char *d, char *s) { while (*s) { *d = *s; d++; s++; } *d = 0; return 0; }
int reverse(char *s) {
  char *e; char c;
  e = s + my_strlen(s) - 1;
  while (s < e) { c = *s; *s = *e; *e = c; s++; e--; }
  return 0;
}
int count(char *s, int c) { int n; n = 0; while (*s) { if (*s == c) n++; s++; } return n; }
int upper(char *s) { while (*s) { if (*s >= 'a' && *s <= 'z') *s = *s - 32; s++; } return 0; }

int main() {
  int i; int r; unsigned sum; int t0;
  for (i = 0; i < 4095; i++) text[i] = 'a' + (i * 7 + i / 13) % 26;
  text[4095] = 0;
  t0 = millis(); sum = 0;
  for (r = 0; r < 80; r++) {
    my_strcpy(copy, text);
    reverse(copy);
    upper(copy);
    sum = sum + my_strlen(copy) + count(copy, 'E') * 3 + count(text, 'q') + copy[r * 50 % 4000];
  }
  printf("check %u\n", sum);
  printf("time %d\n", millis() - t0);
  return 0;
}
