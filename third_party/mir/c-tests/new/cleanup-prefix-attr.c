/* __attribute__((cleanup)) in declaration-SPECS (prefix) position applies to
   every declarator of the declaration, same as gcc.  It was silently dropped:
   the check layer only scanned the declarator-suffix attrs slot (madc win
   lane, 2026-08-12).  Suffix position is covered by the other cleanup-*
   tests.  c2m has no __GNUC__, so glibc empties __attribute__; use c2mir's
   __mirc_attribute__ spelling there. */
#include <stdio.h>
#ifdef __GNUC__
#define CLEANUP(f) __attribute__ ((cleanup (f)))
#else
#define CLEANUP(f) __mirc_attribute__ ((cleanup (f)))
#endif

static void cl (int *p) { printf ("cleanup %d\n", *p); }

static void f (void) {
  CLEANUP (cl) int a = 1, b = 2;
  printf ("body\n");
}

int main (void) {
  f ();
  printf ("done\n");
  return 0;
}
