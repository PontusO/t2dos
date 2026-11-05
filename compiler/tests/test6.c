// test6.c - multi-dimensional arrays. Standard C: gcc builds it natively for reference output.
#include <stdio.h>
#include <string.h>

#define ROWS 3
#define COLS 4
int grid[ROWS][COLS];
int init2[2][3] = { { 1, 2, 3 }, { 4, 5, 6 } };
int part2[3][3] = { { 1 }, { 0, 2 } };            // rest zero
int inf2[][2] = { { 10, 11 }, { 20, 21 }, { 30, 31 } };
char names[][8] = { "alpha", "beta", "gamma" };
char board[3][3];
int cube[2][3][4];
struct Mat { int id; int m[2][3]; char tag[2][4]; };
struct Mat gm = { 7, { { 1, 2, 3 }, { 4, 5, 6 } }, { "ab", "cd" } };

int sum(int m[][COLS], int rows) { int i; int j; int s; s = 0; for (i = 0; i < rows; i++) for (j = 0; j < COLS; j++) s += m[i][j]; return s; }
int trace(int (*m)[COLS], int n) { int i; int s; s = 0; for (i = 0; i < n; i++) s += m[i][i]; return s; }
int rowsum(int *row, int n) { int i; int s; s = 0; for (i = 0; i < n; i++) s += row[i]; return s; }
int fill(struct Mat *pm) { int i; int j; for (i = 0; i < 2; i++) for (j = 0; j < 3; j++) pm->m[i][j] = (i + 1) * 10 + j; return 0; }

int main() {
  int i; int j; int k; int (*p)[COLS]; int *q;
  int local[2][2] = { { 5, 6 }, { 7, 8 } };
  char lnames[2][6] = { "one", "two" };
  int lz[2][3];
  struct Mat lm;

  for (i = 0; i < ROWS; i++) for (j = 0; j < COLS; j++) grid[i][j] = i * 10 + j;
  printf("grid %d %d %d %d sum %d trace %d\n", grid[0][0], grid[1][2], grid[2][3], grid[2][0], sum(grid, ROWS), trace(grid, 3));
  printf("rows %d %d %d\n", rowsum(grid[0], COLS), rowsum(grid[1], COLS), rowsum(grid[2], COLS));
  p = grid; q = grid[1];
  printf("ptr %d %d %d %d %d\n", p[2][1], (*(p + 1))[3], q[2], *(*(grid + 2) + 1), (int)(&grid[2][0] - &grid[0][0]));
  p++; printf("p++ %d\n", p[0][0]);
  printf("init2 %d %d %d %d %d %d\n", init2[0][0], init2[0][2], init2[1][0], init2[1][2], init2[1][1], init2[0][1]);
  printf("part2 %d %d %d %d %d %d\n", part2[0][0], part2[0][1], part2[1][1], part2[1][2], part2[2][0], part2[2][2]);
  printf("inf2 %d %d %d\n", inf2[0][1], inf2[1][0], inf2[2][1]);
  printf("names %s %s %s %d %c\n", names[0], names[1], names[2], strlen(names[2]), names[1][2]);
  strcpy(names[1], "BETA"); printf("names[1] now %s, names[2] still %s\n", names[1], names[2]);
  for (i = 0; i < 3; i++) for (j = 0; j < 3; j++) board[i][j] = (i + j) % 2 ? 'x' : 'o';
  for (i = 0; i < 3; i++) { for (j = 0; j < 3; j++) printf("%c", board[i][j]); printf(" "); }
  printf("\n");
  for (i = 0; i < 2; i++) for (j = 0; j < 3; j++) for (k = 0; k < 4; k++) cube[i][j][k] = i * 100 + j * 10 + k;
  printf("cube %d %d %d %d\n", cube[0][0][0], cube[1][2][3], cube[1][0][2], cube[0][2][1]);
  printf("gm %d %d %d %s %s\n", gm.id, gm.m[0][2], gm.m[1][1], gm.tag[0], gm.tag[1]);
  fill(&lm); printf("lm %d %d %d\n", lm.m[0][0], lm.m[1][2], lm.m[1][0]);
  printf("local %d %d %d %d lnames %s %s\n", local[0][0], local[0][1], local[1][0], local[1][1], lnames[0], lnames[1]);
  for (i = 0; i < 2; i++) for (j = 0; j < 3; j++) lz[i][j] = i - j;
  printf("lz %d %d %d\n", lz[0][0], lz[1][2], lz[0][2]);
  printf("sizeof %d %d %d\n", sizeof(int[3][4]), sizeof(char[2][5]), sizeof(struct Mat));
  return 0;
}
