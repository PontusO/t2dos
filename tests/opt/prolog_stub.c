// prolog_stub.c - linked with tests/opt/prolog.c so ext/cfext resolve. cc has no
// multi-file compilation beyond linking objects, and a body in prolog.c would let
// promote() see through the "external" call.
int ext(int a) { return a + 1; }
float cfext(float x) { return x + 0.25; }
// stublea keeps its frame under -O since the reserved frames convert (3b-iii): its float
// local lives in a frame slot that vstr/vldr read through r7, a form sprel cannot move.
// It is the framed half of the mixed-chain witness: cmix in prolog.c is sprel-converted
// and calls it, like a converted caller into a hand-written .s callee that reads
// [r7, #off]. The w round-trip adds nothing to the sum (w holds b).
int stublea(int a, int b) { int v[4]; int i; float w; w = b; v[0] = a; for (i = 1; i < 4; i++) v[i] = v[i - 1] + b; v[3] = v[3] + (int)w - b; return v[3]; }
