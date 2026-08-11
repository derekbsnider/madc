/* Complex equality with mixed operand types: both operands must convert to
   the common complex type BEFORE their components are loaded. The N_EQ/N_NE
   arm used to load a _Complex float's memory at double width (garbage — the
   compare was always unequal, gcc.c-torture complex-6 test_float) and never
   promoted a scalar operand at all. */
int main (void) {
  _Complex float f = 1.0f - 2.0if;
  _Complex double d = 1.0 - 2.0i;

  if (!(f == (1.0f - 2.0if))) return 1; /* same width */
  if (!(f == (1.0 - 2.0i))) return 2;   /* float vs double complex */
  if (!(d == (1.0 - 2.0i))) return 3;
  if (f != d) return 4;                 /* complex vs complex, mixed width */
  if (!(d + 2.0i == 1.0)) return 5;     /* complex vs scalar */
  if (1.0 != d + 2.0i) return 6;        /* scalar vs complex */
  return 0;
}
