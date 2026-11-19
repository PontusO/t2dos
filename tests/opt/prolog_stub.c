// prolog_stub.c - linked with tests/opt/prolog.c so ext/cfext resolve. cc has no
// multi-file compilation beyond linking objects, and a body in prolog.c would let
// promote() see through the "external" call.
int ext(int a) { return a + 1; }
float cfext(float x) { return x + 0.25; }
