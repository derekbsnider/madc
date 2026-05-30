/* __attribute__((cleanup)): fall-through scope exit runs cleanups in reverse
   declaration order. c2m has no __GNUC__, so glibc empties __attribute__;
   use c2mir's __mirc_attribute__ spelling there. */
#include <stdio.h>
#ifdef __GNUC__
#define CLEANUP(f) __attribute__ ((cleanup (f)))
#else
#define CLEANUP(f) __mirc_attribute__ ((cleanup (f)))
#endif

static void cl (int *p) { printf ("cleanup %d\n", *p); }

static void f (void) {
  int a CLEANUP (cl) = 1;
  int b CLEANUP (cl) = 2;
  printf ("body\n");
}

int main (void) {
  f ();
  return 0;
}
