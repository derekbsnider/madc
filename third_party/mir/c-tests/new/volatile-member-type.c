/* C11 6.5.2.3p3-4: a member of a qualified structure has the so-qualified
   type -- an array member's elements (6.7.3p9) -- through `.` and `->`. */
#include <stdio.h>
struct in { int x; };
struct S { int m; int a[2]; struct in sub; };
#define KIND(e) _Generic ((e), int *: 1, volatile int *: 2, const int *: 3, \
                          const volatile int *: 4, default: 0)
int main (void) {
  volatile struct S vs = { 1, { 2, 3 }, { 4 } };
  const struct S cs = { 5, { 6, 7 }, { 8 } };
  struct S s = { 9, { 10, 11 }, { 12 } };
  volatile struct S *pvs = &s;
  const volatile struct S *pcvs = &s;
  printf ("dot: %d %d %d %d\n", KIND (&vs.m), KIND (&vs.a[1]), KIND (&vs.sub.x), KIND (&s.m));
  printf ("arrow: %d %d %d\n", KIND (&pvs->m), KIND (&pvs->a[0]), KIND (&pcvs->m));
  printf ("const: %d %d\n", KIND (&cs.m), KIND (&cs.a[1]));
  printf ("vals: %d %d %d %d\n", vs.a[1], cs.sub.x, pvs->m, pcvs->a[1]);
  return 0;
}
