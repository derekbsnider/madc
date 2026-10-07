/* A program that DEFINES variadic functions: every va_arg is a call to libmir's
   mir.va_arg / mir.va_block_arg in a madc aarch64 object, which an executable
   could not resolve (undefined at link on Linux, dyld abort on macOS).  Reads
   ints, doubles, an HFA, a non-HFA struct and a struct passed by reference
   (over 16 bytes), from registers and, past eight, from the stack. */
#include <stdarg.h>

int printf (const char *, ...);

typedef struct { double a, b; } d2;        /* HFA: FP registers */
typedef struct { long x; double y; } mixed; /* not an HFA: GP registers */
typedef struct { long v[5]; } big;          /* 40 bytes: by reference */

static long sum_ints (int n, ...) {
  va_list ap;
  long s = 0;
  va_start (ap, n);
  for (int i = 0; i < n; i++) s = s * 10 + va_arg (ap, int);
  va_end (ap);
  return s;
}

static double sum_doubles (int n, ...) {
  va_list ap;
  double s = 0;
  va_start (ap, n);
  for (int i = 0; i < n; i++) s = s * 10 + va_arg (ap, double);
  va_end (ap);
  return s;
}

static double sum_structs (int n, ...) {
  va_list ap;
  double s = 0;
  va_start (ap, n);
  for (int i = 0; i < n; i++) {
    d2 h = va_arg (ap, d2);
    mixed m = va_arg (ap, mixed);
    big b = va_arg (ap, big);
    s = s * 1000 + h.a + 10 * h.b + 100 * (m.x + m.y) + b.v[0] + b.v[4];
  }
  va_end (ap);
  return s;
}

int main (void) {
  d2 h = {1, 2};
  mixed m = {3, 0.5};
  big b = {{1, 2, 3, 4, 5}};

  printf ("ints %ld\n", sum_ints (3, 1, 2, 3));
  printf ("ints-stack %ld\n", sum_ints (10, 1, 2, 3, 4, 5, 6, 7, 8, 9, 1));
  printf ("doubles %.17g\n", sum_doubles (3, 1.0, 2.0, 3.0));
  printf ("doubles-stack %.17g\n", sum_doubles (10, 1.0, 2.0, 3.0, 4.0, 5.0, 6.0, 7.0, 8.0, 9.0, 1.0));
  printf ("structs %.17g\n", sum_structs (1, h, m, b));
  printf ("structs-stack %.17g\n", sum_structs (3, h, m, b, h, m, b, h, m, b));
  return 0;
}
