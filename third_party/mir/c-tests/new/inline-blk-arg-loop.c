/* A call inlined in a loop that passes a struct by value: the inliner copies
   the argument into an alloca at the call site, which must be released after
   each execution (BSTART/BEND), or the loop grows the stack until it
   overflows. */
typedef struct {
  double v[32];
} big;

static double first_and_last (big b) { return b.v[0] + b.v[31]; }

int main (void) {
  big b = {{0}};
  double s = 0;

  b.v[0] = 1.0;
  b.v[31] = 2.0;
  for (long i = 0; i < 1000000; i++) s += first_and_last (b);
  return s == 3000000.0 ? 0 : 1;
}
