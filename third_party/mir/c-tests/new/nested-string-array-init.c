/* A STRING initializing an array sub-object consumes that sub-object WHOLE, so
   the initializer path must not descend into it.

   c2m left the path pointing INSIDE row 0, so the next initializer advanced to
   row0[1] -- offset 1 instead of 8 -- and "bob" was memcpy'd over the middle of
   row 0 while row 1 stayed empty. Silently: exit 0, no diagnostic, wrong data.

     char n[2][8] = {"ada", "bob"};   gcc/clang: [ada][bob]   c2m: [ada][]
     char n[2][2][4] = ...            gcc/clang: [ab][cd][ef][gh]
                                      c2m:       [ab][][ef][]

   Every char-array member of a struct hit it too, at any position. */

#include <string.h>

struct S {
  int tag;
  char n[2][8];
};

struct T {
  char n[2][8];
  int tag;
};

/* gen_initializer has SEPARATE paths for a local (memcpy into a frame slot) and
   for static/global data (MIR data items), so both are covered: the fix is in
   the shared collect_init_els path that feeds them, and a test of only one would
   not have shown that. */
static char g_a[2][8] = {"ada", "bob"};
static struct S g_s = {7, {"ada", "bob"}};
static char g_b[2][2][4] = {{"ab", "cd"}, {"ef", "gh"}};

int main (void) {
  static char sl[2][8] = {"zed", "yaw"};
  char a[2][8] = {"ada", "bob"};
  char b[2][2][4] = {{"ab", "cd"}, {"ef", "gh"}};
  struct S s = {7, {"ada", "bob"}};
  struct T t = {{"ada", "bob"}, 9};
  char one[8] = "abc";

  if (strcmp (a[0], "ada") != 0) return 1;
  if (strcmp (a[1], "bob") != 0) return 2;
  /* The tail of a row initialized by a SHORTER string is zero-filled, and the
     next row must not have been written over it. */
  if (a[0][4] != 0 || a[0][5] != 0 || a[0][6] != 0 || a[0][7] != 0) return 3;
  if (a[1][4] != 0 || a[1][7] != 0) return 4;

  if (strcmp (b[0][0], "ab") != 0) return 5;
  if (strcmp (b[0][1], "cd") != 0) return 6;
  if (strcmp (b[1][0], "ef") != 0) return 7;
  if (strcmp (b[1][1], "gh") != 0) return 8;

  if (s.tag != 7) return 9;
  if (strcmp (s.n[0], "ada") != 0) return 10;
  if (strcmp (s.n[1], "bob") != 0) return 11;

  if (t.tag != 9) return 12;
  if (strcmp (t.n[0], "ada") != 0) return 13;
  if (strcmp (t.n[1], "bob") != 0) return 14;

  /* The one-dimensional case always worked; keep it covered so a fix here
     cannot regress it. */
  if (strcmp (one, "abc") != 0) return 15;
  if (one[7] != 0) return 16;

  /* The static / global data path. */
  if (strcmp (g_a[0], "ada") != 0) return 17;
  if (strcmp (g_a[1], "bob") != 0) return 18;
  if (g_a[0][4] != 0 || g_a[0][7] != 0) return 19;
  if (g_s.tag != 7) return 20;
  if (strcmp (g_s.n[0], "ada") != 0) return 21;
  if (strcmp (g_s.n[1], "bob") != 0) return 22;
  if (strcmp (g_b[0][0], "ab") != 0) return 23;
  if (strcmp (g_b[0][1], "cd") != 0) return 24;
  if (strcmp (g_b[1][0], "ef") != 0) return 25;
  if (strcmp (g_b[1][1], "gh") != 0) return 26;
  if (strcmp (sl[0], "zed") != 0) return 27;
  if (strcmp (sl[1], "yaw") != 0) return 28;

  return 0;
}
