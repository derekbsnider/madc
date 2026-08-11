/* __attribute__((cleanup)): cleanups run before return, across all enclosing
   scopes, inner-first, with the return value preserved. */
#include <stdio.h>
#ifdef __GNUC__
#define CLEANUP(f) __attribute__ ((cleanup (f)))
#else
#define CLEANUP(f) __mirc_attribute__ ((cleanup (f)))
#endif

static void cl (int *p) { printf ("cleanup %d\n", *p); }

static int f (int x) {
  int a CLEANUP (cl) = 1;
  if (x) {
    int b CLEANUP (cl) = 2;
    return 99; /* clean b then a, return 99 */
  }
  return 0; /* clean a, return 0 */
}

int main (void) {
  printf ("got %d\n", f (1));
  printf ("got %d\n", f (0));
  return 0;
}
