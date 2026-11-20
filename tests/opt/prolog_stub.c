// prolog_stub.c - linked with tests/opt/prolog.c so ext/cfext resolve. cc has no
// multi-file compilation beyond linking objects, and a body in prolog.c would let
// promote() see through the "external" call.
int ext(int a) { return a + 1; }
float cfext(float x) { return x + 0.25; }
// stublea has locals: -O keeps its frame (a reservation only unwinds through mov sp, r7,
// which frameless deletes), like a hand-written .s callee that reads [r7, #off]. It is the
// framed half of the mixed-chain witness: cmix in prolog.c is sprel-converted and calls it.
int stublea(int a, int b) { int v[4]; int i; v[0] = a; for (i = 1; i < 4; i++) v[i] = v[i - 1] + b; return v[3]; }
