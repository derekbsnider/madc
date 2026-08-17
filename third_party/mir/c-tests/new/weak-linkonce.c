/* madc fork: weak/linkonce symbol bindings (ELF-completion S4).
   __attribute__((weak)) = interposable weak: STB_WEAK in the object
   capture, calls never inlined (the definition may be replaced at link).
   __attribute__((linkonce)) = C++ vague linkage: STB_WEAK too, but copies
   are ODR-identical so calls still inline.
   Uses __mirc_attribute__ so glibc's sys/cdefs.h (which defines
   __attribute__(x) away for non-GNU compilers) cannot erase the markers.
   Runs in every lane: JIT interp/gen directly; object lanes emit a .o whose
   weak symbols an external cc link must accept. */
#include <stdio.h>

__mirc_attribute__ ((weak)) int wget (void) { return 11; }
__mirc_attribute__ ((linkonce)) int pick (void) { return 7; }
__mirc_attribute__ ((weak)) int wtab[2] = {5, 6};

int use (void) { return wget () + pick () + wtab[0]; }

int main (void) {
  printf ("%d %d %d %d\n", wget (), pick (), wtab[1], use ());
  return 0;
}
