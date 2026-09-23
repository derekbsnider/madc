/* _Generic's controlling expression undergoes lvalue conversion and
   array/function-to-pointer conversion ONLY (C11 6.5.1.1p2, C17 DR 481): no
   integer promotions -- a char selects char, not int -- and only the TOP
   level's qualifiers drop; a pointee's stay, so char * and const char * are
   two distinct associations. */
#include <stdio.h>

#define K(x)                                                                                  \
  _Generic ((x), char: "char", signed char: "schar", unsigned char: "uchar", short: "short", \
            unsigned short: "ushort", int: "int", _Bool: "bool", default: "other")
#define P(x) _Generic ((x), char *: "char*", const char *: "cchar*", int *: "int*", default: "other")

int main (void) {
  char c = 1;
  signed char sc = 2;
  unsigned char uc = 3;
  short s = 4;
  unsigned short us = 5;
  _Bool b = 1;
  const int ci = 6;
  volatile short vs = 7;
  char arr[4] = "abc";
  const char *cp = arr;
  printf ("int: %s %s %s %s %s %s\n", K (c), K (sc), K (uc), K (s), K (us), K (b));
  printf ("conv: %s %s %s %s\n", K ((char) 65), K (ci), K (vs), K (c + 0));
  printf ("ptr: %s %s %s %s\n", P (arr), P (cp), P ((const char *) arr), P (&c));
  return 0;
}
