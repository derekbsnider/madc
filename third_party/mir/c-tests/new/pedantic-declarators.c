/* Strict ISO mode (-pedantic, see pedantic-declarators.c.flags): declarators
   with and without initializers -- several in one declaration, each keeping its
   own initializer -- a member declarator, and integer constants at the edge of
   their range must compile and run as gcc does: no GNU attribute parsing, no
   spurious "out of range" diagnostics. */
struct s {
  int a, b;
};

static unsigned long long max_ull = 18446744073709551615ULL;
static long long min_ll = -9223372036854775807LL - 1;

int main (void) {
  int x = 1, y;
  int p = 4, q = 5;
  struct s v = {2, 3};
  double d = 1e308;

  y = x + v.a + v.b;
  return !(y == 6 && p == 4 && q == 5 && max_ull + 1 == 0 && min_ll < 0 && d > 1e307);
}
