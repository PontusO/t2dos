// prolog_stub.c - linked with tests/opt/prolog.c so ext/cfext resolve. cc has no
// multi-file compilation beyond linking objects, and a body in prolog.c would let
// promote() see through the "external" call.
int ext(int a) { return a + 1; }
float cfext(float x) { return x + 0.25; }
// stublea converts under -O since 3c: its array descriptor leas and its float slot are
// r7 literals the sprel walk maps through sp. The w round-trip adds nothing to the sum
// (w holds b). It is the converted callee of the mixed-chain witness cmix in prolog.c;
// that chain's framed callee is csys (the syscall window keeps its frame) - the run
// links both and checks them against the native gcc reference.
int stublea(int a, int b) { int v[4]; int i; float w; w = b; v[0] = a; for (i = 1; i < 4; i++) v[i] = v[i - 1] + b; v[3] = v[3] + (int)w - b; return v[3]; }
