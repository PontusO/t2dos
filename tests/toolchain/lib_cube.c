// lib_cube.c - member of libtc.a (lib_use) that lib_use does not need
int cubes;
int cube(int x) { cubes = cubes + 1; return x * x * x; }
