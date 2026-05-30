/* __attribute__((cleanup)): combined matrix — multiple vars, nested scopes,
   every exit path (fall-through, return, break, continue, goto). Compared
   byte-for-byte against gcc. */
#include <stdio.h>
#ifdef __GNUC__
#define CLEANUP(f) __attribute__ ((cleanup (f)))
#else
#define CLEANUP(f) __mirc_attribute__ ((cleanup (f)))
#endif
static void cl (int *p) { printf ("cl %d\n", *p); }
static int g (int n) {
  int a CLEANUP (cl) = 10;
  for (int i CLEANUP (cl) = 0; i < 3; i++) {
    int b CLEANUP (cl) = 20 + i;
    if (i == 1) continue;            /* clean b */
    if (i == 2) { int c CLEANUP (cl) = 30; goto done; } /* clean c,b then run done's */
    printf ("iter %d\n", i);
  }
done:;
  int d CLEANUP (cl) = 40;
  if (n) return 99;                  /* clean d,a; value preserved */
  printf ("tail\n");
  return 0;                          /* clean d,a */
}
int main (void) { printf ("=%d\n", g (1)); printf ("=%d\n", g (0)); return 0; }
