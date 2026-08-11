#include <stdio.h>

#ifdef __GNUC__
#define CLEANUP(f) __attribute__ ((cleanup (f)))
#else
#define CLEANUP(f) __mirc_attribute__ ((cleanup (f)))
#endif

static void cl (int *p) { printf ("cleanup %d\n", *p); }

/* break out of a while loop: body-local cleanup runs on break. */
static void test_break_while (void) {
  printf ("-- break_while\n");
  int i = 0;
  while (i < 5) {
    int a CLEANUP (cl) = 100 + i;
    if (i == 2) break;
    i++;
  }
  printf ("after break_while\n");
}

/* continue in a while loop: body-local cleanup runs each iteration. */
static void test_continue_while (void) {
  printf ("-- continue_while\n");
  int i = 0;
  while (i < 3) {
    int a CLEANUP (cl) = 200 + i;
    i++;
    if (i == 2) continue;
  }
  printf ("after continue_while\n");
}

/* nested inner block inside a loop: break runs both inner and outer
   body cleanups (reverse decl order), partial-scope exit. */
static void test_nested_block (void) {
  printf ("-- nested_block\n");
  int i = 0;
  while (i < 5) {
    int outer CLEANUP (cl) = 300 + i;
    {
      int inner CLEANUP (cl) = 400 + i;
      if (i == 1) break;
    }
    i++;
  }
  printf ("after nested_block\n");
}

/* for-init cleanup var: break must clean it, continue must NOT. */
static void test_for_init (void) {
  printf ("-- for_init\n");
  for (int fi CLEANUP (cl) = 500; fi < 503; fi++) {
    int body CLEANUP (cl) = 600 + (fi - 500);
    if (fi == 501) continue; /* cleans body only, not fi */
    if (fi == 502) break;    /* cleans body then fi */
  }
  printf ("after for_init\n");
}

int main (void) {
  test_break_while ();
  test_continue_while ();
  test_nested_block ();
  test_for_init ();
  return 0;
}
