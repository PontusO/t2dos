// test10.c - conditional compilation: #if #ifdef #ifndef #elif #else #endif, defined.
// Standard C: gcc builds it natively for reference output. Build both with
// FROM_CMDLINE=7 defined: gcc -DFROM_CMDLINE=7, cc -D FROM_CMDLINE=7.
#include <stdio.h>

#define VERSION_MAJOR 1
#define VERSION_MINOR 2
#define FEATURE_A
#define LEVEL 3
#define MAX(a, b) ((a) > (b) ? (a) : (b))

#ifndef GUARD_ONCE                 /* include guard pattern, used twice */
#define GUARD_ONCE
int guarded = 1;
#endif
#ifndef GUARD_ONCE
int guarded = 2;                   // would be a duplicate global
#endif

#ifdef FEATURE_A
char *feature_a = "on";
#else
char *feature_a = "off";
#endif

#ifdef FEATURE_B
char *feature_b = "on";
#else
char *feature_b = "off";
#endif

#if VERSION_MAJOR * 100 + VERSION_MINOR >= 102   // a comment after the expression
char *version = "new enough";
#else
char *version = "too old";
#endif

#if LEVEL == 1
char *level = "one";
#elif LEVEL == 2
char *level = "two";
#elif LEVEL == 3
char *level = "three";
#elif LEVEL == 3
char *level = "three again";
#else
char *level = "other";
#endif

#if 0
this is not C at all { and must be skipped
#if 1
#error not reached either
#else
#endif
#elif 1
char *zero = "elif after 0";
#else
char *zero = "else";
#endif

#if defined FEATURE_A && !defined(FEATURE_B) && defined(VERSION_MAJOR)
int defs = 1;
#else
int defs = 0;
#endif

#if UNDEFINED_NAME                 // unknown names are 0
int undef_is_zero = 0;
#else
int undef_is_zero = 1;
#endif

#if MAX(2, 3) == 3 && \
    LEVEL > 2
int fn_macro = 1;
#else
int fn_macro = 0;
#endif

#if defined(__T2CC__) || defined(__GNUC__)
char *compiler = "known compiler";
#else
char *compiler = "unknown compiler";
#endif

#ifdef FROM_CMDLINE
int cmdline = FROM_CMDLINE;
#else
int cmdline = -1;
#endif

#define TEMP 5
#undef TEMP
#ifdef TEMP
int temp = 1;
#else
int temp = 0;
#endif

#if LEVEL > 1
#  if LEVEL > 2
#    define DEPTH "deep"
#  else
#    define DEPTH "shallow"
#  endif
#else
#  define DEPTH "flat"
#endif

int main() {
  int x = 0;
#ifdef FEATURE_A
  x += 1;
#endif
#if LEVEL >= 3
  x += 10;
#else
  x += 100;
#endif
#ifndef FEATURE_A
  x += 1000;
#endif
  printf("guarded %d feature_a %s feature_b %s\n", guarded, feature_a, feature_b);
  printf("version %s level %s zero %s\n", version, level, zero);
  printf("defs %d undef %d fn %d %s cmdline %d temp %d depth %s x %d\n", defs, undef_is_zero, fn_macro, compiler, cmdline, temp, DEPTH, x);
  return 0;
}
