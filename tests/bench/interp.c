// interp.c - a small bytecode machine with its state in globals and a switch dispatch
#include <stdio.h>

int code[64]; int ncode;
int stack[64]; int sp; int pc; int acc;
int vars[8];

enum { PUSH, LOAD, STORE, ADD, SUB, MUL, JNZ, DEC, HALT, DUP, POP };

int emit(int op) { code[ncode] = op; ncode++; return 0; }

int run() {
  int op; int a; int b; int steps;
  pc = 0; sp = 0; steps = 0;
  while (1) {
    op = code[pc]; pc++; steps++;
    switch (op) {
      case PUSH: stack[sp] = code[pc]; pc++; sp++; break;
      case LOAD: stack[sp] = vars[code[pc]]; pc++; sp++; break;
      case STORE: sp--; vars[code[pc]] = stack[sp]; pc++; break;
      case ADD: sp--; b = stack[sp]; a = stack[sp - 1]; stack[sp - 1] = a + b; break;
      case SUB: sp--; b = stack[sp]; a = stack[sp - 1]; stack[sp - 1] = a - b; break;
      case MUL: sp--; b = stack[sp]; a = stack[sp - 1]; stack[sp - 1] = a * b; break;
      case DUP: stack[sp] = stack[sp - 1]; sp++; break;
      case POP: sp--; break;
      case DEC: vars[code[pc]] = vars[code[pc]] - 1; pc++; break;
      case JNZ: if (vars[code[pc]]) pc = code[pc + 1]; else pc = pc + 2; break;
      case HALT: return steps;
    }
  }
  return steps;
}

int main() {
  int t0; int steps;
  // v0 = 40000 ; v1 = 0 ; loop: v1 = v1 * 3 + v0 - 7 ; v0-- ; jnz v0 loop ; halt
  emit(PUSH); emit(40000); emit(STORE); emit(0);
  emit(PUSH); emit(0); emit(STORE); emit(1);
  emit(LOAD); emit(1); emit(PUSH); emit(3); emit(MUL); emit(LOAD); emit(0); emit(ADD);   // pc 8
  emit(PUSH); emit(7); emit(SUB); emit(DUP); emit(POP); emit(STORE); emit(1);
  emit(DEC); emit(0); emit(JNZ); emit(0); emit(8); emit(HALT);
  t0 = millis();
  steps = run() + run() + run();
  printf("check %d %d\n", steps, vars[1]);
  printf("time %d\n", millis() - t0);
  return 0;
}
