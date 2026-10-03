/* A local initializer zero-fills the member after a bit-field in its unit:
   `d` (byte 2) has no initializer, `x` (an int unit at offset 0) ends at
   byte 1.  The gap fill before `e` once started at the end of x's int unit
   (byte 4) and left d as stack garbage. */
#include <stdio.h>
struct G {
  char c;
  int x : 4;
  char d;
  char e[2];
};
__attribute__ ((noinline)) void dirty (void) {
  volatile unsigned char junk[256];
  for (int k = 0; k < 256; k++) junk[k] = 0xA5;
}
__attribute__ ((noinline)) void use (void) {
  struct G g = {.c = 1, .x = 2, .e = {3, 4}};
  printf ("g: %d %d %d %d %d\n", g.c, g.x, g.d, g.e[0], g.e[1]);
}
int main (void) {
  dirty ();
  use ();
  return 0;
}
