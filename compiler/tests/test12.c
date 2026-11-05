// test12.c - addresses of struct members (&p->h, &s.h, arrays at offset 0, nested, nonzero offset).
// Standard C: gcc builds it natively for reference output. Never prints raw addresses.
#include <stdio.h>
#include <string.h>

struct H { int a; int b; int c; int d; int e; int f; int g; };
struct O { struct H h; int later; struct H h2; char c; };
struct A { int arr[4]; int tail; };
struct N { struct A a; struct H h; };

int sum7(struct H *h) { return h->a + h->b + h->c + h->d + h->e + h->f + h->g; }
int first(int *q) { return *q; }
int setfirst(int *q, int v) { *q = v; return 0; }

int main() {
  struct O o; struct O *p; struct A a; struct A *pa; struct N n; struct N *pn; struct H h; char buf[64]; int i;
  struct H *hp; int *ip;

  memset(&o, 0, sizeof(struct O)); memset(&a, 0, sizeof(struct A)); memset(&n, 0, sizeof(struct N));
  p = &o; pa = &a; pn = &n;

  // &p->h : struct member at offset 0 through a pointer
  printf("diff h %d\n", (int)((char *)&p->h - (char *)p));
  for (i = 0; i < 64; i++) buf[i] = i + 1;
  memcpy(&p->h, buf, 28);
  printf("h %d %d %d %d\n", o.h.a & 255, o.h.b & 255, o.h.c & 255, o.h.g & 255);
  memset(&p->h, 0, sizeof(struct H));
  printf("h zero %d %d\n", o.h.a, o.h.g);
  p->h.a = 1; p->h.g = 6;
  printf("sum7 %d\n", sum7(&p->h));
  hp = &p->h; hp->b = 5; printf("via hp %d %d\n", o.h.b, p->h.b);

  // &s.h : struct variable
  printf("diff s.h %d\n", (int)((char *)&o.h - (char *)&o));
  memcpy(&o.h, buf, 28);
  printf("s.h %d %d\n", o.h.a & 255, o.h.g & 255);
  h.a = 9; h.b = 8; h.c = 7; h.d = 6; h.e = 5; h.f = 4; h.g = 3;
  memcpy(&o.h, &h, sizeof(struct H));
  printf("copy %d %d\n", o.h.a, o.h.g);
  printf("sum7 s %d\n", sum7(&o.h));

  // array member at offset 0
  pa->arr[0] = 11; pa->arr[1] = 22; pa->arr[2] = 33; pa->arr[3] = 44; pa->tail = 55;
  printf("diff arr %d %d\n", (int)((char *)pa->arr - (char *)pa), (int)((char *)&pa->arr[0] - (char *)pa));
  printf("first %d %d %d\n", first(pa->arr), first(&pa->arr[0]), first(&pa->arr[2]));
  setfirst(pa->arr, 100); setfirst(&pa->arr[3], 400);
  printf("set %d %d %d\n", a.arr[0], a.arr[3], a.tail);
  ip = pa->arr; ip[1] = 200; printf("ip %d\n", a.arr[1]);
  memset(pa->arr, 0, 8);
  printf("memset arr %d %d %d\n", a.arr[0], a.arr[1], a.arr[3]);
  memset(a.arr, 0, 16); memcpy(a.arr, buf, 4);
  printf("var arr %d %d\n", a.arr[0] & 255, a.arr[1]);

  // nested: &p->h.x, &p->a.arr, &p->a.arr[1]
  pn->a.arr[1] = 7; pn->h.a = 70; pn->h.c = 90; pn->a.tail = 5;
  printf("diff nested %d %d %d\n", (int)((char *)&pn->a - (char *)pn), (int)((char *)&pn->h.a - (char *)pn), (int)((char *)&pn->h.c - (char *)pn));
  printf("nested %d %d %d\n", first(&pn->h.a), first(&pn->h.c), first(&pn->a.arr[1]));
  setfirst(&pn->h.a, 71); setfirst(pn->a.arr, 3); setfirst(&n.h.c, 91);
  printf("nested set %d %d %d %d\n", n.h.a, n.a.arr[0], n.h.c, n.a.tail);
  printf("diff nested var %d\n", (int)((char *)&n.h.a - (char *)&n));

  // nonzero offsets
  o.later = 123;
  printf("diff later %d\n", (int)((char *)&p->later - (char *)p));
  printf("later %d\n", first(&p->later));
  setfirst(&p->later, 321); printf("later set %d\n", o.later);
  printf("diff h2 %d\n", (int)((char *)&p->h2 - (char *)p));
  memcpy(&p->h2, buf, 28);
  printf("h2 %d %d\n", o.h2.a & 255, o.h2.g & 255);
  memset(&p->h2, 0, sizeof(struct H)); printf("h2 zero %d\n", o.h2.b);
  printf("diff c %d\n", (int)((char *)&p->c - (char *)p));
  setfirst((int *)&p->h2.c, 77); printf("h2.c %d\n", o.h2.c);
  printf("p intact %d\n", p == &o);
  return 0;
}
