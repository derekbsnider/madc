/* __attribute__((cleanup(fn))) — Phase 5: goto.
   Cleanups must run for every scope EXITED between the goto and the label
   (scopes on the path from the goto's scope up to, but not including, the
   nearest common ancestor of the goto's scope and the label's scope).
   Cleanups run in reverse declaration order; inner scopes before outer. */
#include <stdio.h>

#ifdef __GNUC__
#define CLEANUP(f) __attribute__ ((cleanup (f)))
#else
#define CLEANUP(f) __mirc_attribute__ ((cleanup (f)))
#endif

static void clr (int *p) { printf ("cleanup %d\n", *p); }

/* goto OUT of an inner block to a label in the enclosing scope:
   the inner var must be cleaned. */
static void goto_out_one_level (void) {
  printf ("-- one level --\n");
  {
    int a CLEANUP (clr) = 1;
    if (a) goto done;
    printf ("unreached\n");
  }
done:
  printf ("at done\n");
}

/* goto crossing two nested levels: both inner vars cleaned, innermost first. */
static void goto_out_two_levels (void) {
  printf ("-- two levels --\n");
  {
    int outer CLEANUP (clr) = 2;
    {
      int inner CLEANUP (clr) = 3;
      if (inner) goto target;
      printf ("unreached\n");
    }
  }
target:
  printf ("at target\n");
}

/* goto to a label at the SAME scope level exits nothing -> no cleanup
   between goto and label. (The var at the goto's own scope is cleaned only
   on its own scope exit, which here happens via fall-through after the
   label, in reverse decl order.) */
static void goto_same_scope (void) {
  printf ("-- same scope --\n");
  int x CLEANUP (clr) = 4;
  if (x) goto here;
  printf ("unreached\n");
here:
  printf ("at here\n");
}

/* Sibling block: declare-clean in first inner block, then goto from a second
   inner block. The first block already cleaned up on its own exit; the second
   block's var is cleaned when the goto leaves it. */
static void goto_sibling_blocks (void) {
  printf ("-- sibling blocks --\n");
  {
    int first CLEANUP (clr) = 5;
    printf ("in first\n");
  }
  {
    int second CLEANUP (clr) = 6;
    if (second) goto out;
    printf ("unreached\n");
  }
out:
  printf ("at out\n");
}

int main (void) {
  goto_out_one_level ();
  goto_out_two_levels ();
  goto_same_scope ();
  goto_sibling_blocks ();
  return 0;
}
