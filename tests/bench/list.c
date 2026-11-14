// list.c - linked structs (field access through pointers)
#include <stdio.h>
#include <stdlib.h>

struct Node { int key; int val; struct Node *next; };

struct Node pool[2000];

int main() {
  struct Node *head; struct Node *p; struct Node **pp; int i; int r; unsigned sum; int t0;
  t0 = millis(); sum = 0;
  for (r = 0; r < 30; r++) {
    head = 0;
    for (i = 0; i < 2000; i++) {          // insert keeping the list sorted by key (descending inserts are cheap)
      p = pool + i; p->key = (i * 37) % 2000; p->val = i;
      pp = &head;
      while (*pp && (*pp)->key > p->key && i % 8) pp = &(*pp)->next;
      p->next = *pp; *pp = p;
      if (i % 8 == 0) { p = head; while (p) { sum = sum + p->val; p = p->next; if (p && p->key < 1000) break; } }
    }
    p = head; while (p) { sum = sum + p->key * 3 + p->val; p = p->next; }
  }
  printf("check %u\n", sum);
  printf("time %d\n", millis() - t0);
  return 0;
}
