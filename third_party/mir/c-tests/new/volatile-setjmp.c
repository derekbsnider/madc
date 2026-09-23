/* A volatile local keeps its last stored value across longjmp (C11 7.13.2.1p3): every
   access to a volatile object is a real load or store (5.1.2.3p6), so it must never be
   allocated to a register -- longjmp restores registers to their setjmp-time contents. */
#include <stdio.h>
#include <setjmp.h>
static jmp_buf jb;
static void jump (int v) { longjmp (jb, v); }
static int work (int n) {
  volatile int acc = 0;
  volatile int i;
  int *volatile last = 0;
  static int cell;
  if (setjmp (jb) == 0) {
    for (i = 0; i < n; i++) { acc += i; last = &cell; }
    jump (1);
  }
  return acc * 100 + i + (last == &cell);
}
static int nested (void) {
  volatile long total = 5;
  volatile int round;
  for (round = 0; round < 3; round++) {
    if (setjmp (jb) == 0) { total *= 2; jump (round + 1); }
    total += 1;
  }
  return (int) total;
}
int main (void) {
  printf ("jmp: %d\n", work (10));
  printf ("nested: %d\n", nested ());
  return 0;
}
