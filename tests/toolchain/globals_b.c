// globals_b.c - with globals_a.c. The 70000-byte array puts counter and name past
// 65535 bytes into this file's data, so their references need movw and movt.
char big[70000];
int counter;
char name[8] = "bee";
char *greeting = "hi";
extern int shared;
extern int table[4];

int bump(int k) {
  shared = shared + k;
  counter = counter + 1;
  big[69999] = k;
  return table[k] + big[69999];
}
