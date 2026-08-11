int main (void) {
  float neg_zero = -0.0f;
  double dnan;
  float fnan;

  if (__builtin_copysignf (1.0, neg_zero) != -1.0f) return 1;
  if (__builtin_copysignf (-2.0f, 0.0f) != 2.0f) return 2;

  dnan = __builtin_nan ("");
  if (!(dnan != dnan)) return 3;

  fnan = __builtin_nan ("");
  if (!(fnan != fnan)) return 4;

  return 0;
}
