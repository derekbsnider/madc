/* An alignment-specifier on a struct or union member (C11 6.7.2.1p1: a
   specifier-qualifier-list may hold one).  It raises the member's alignment and
   so its offset, the aggregate's alignment and its size.  gcc and clang return 0. */
#include <stddef.h>
struct a {
  char c;
  _Alignas (16) int x;
};
struct b {
  _Alignas (16) long v;
  long w;
};
struct c {
  char c;
  _Alignas (double) char d;
  _Alignas (8) _Alignas (4) short s; /* the strictest one wins */
};
union u { /* 16: the largest alignment c2mir's targets admit (invalid_alignment) */
  char c;
  _Alignas (16) char x;
};
int main (void) {
  struct b b = {7, 3};
  if (offsetof (struct a, x) != 16 || sizeof (struct a) != 32 || _Alignof (struct a) != 16) return 1;
  if (sizeof (struct b) != 16 || _Alignof (struct b) != 16 || b.v * 10 + b.w != 73) return 2;
  if (offsetof (struct c, d) != _Alignof (double) || offsetof (struct c, s) != 2 * _Alignof (double))
    return 3;
  if (sizeof (union u) != 16 || _Alignof (union u) != 16) return 4;
  return 0;
}
