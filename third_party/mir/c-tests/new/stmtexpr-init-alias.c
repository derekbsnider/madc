/* Struct-valued statement expressions as declaration initializers: the
   N_STMTEXPR check arm reserves the result slots from the function scope's
   size, and process_func_decls_for_allocation must lay the scope's own
   decls AFTER those reservations. It used to start at offset 0 and
   recompute the size, so the first memory locals aliased the result slots:
   `y = ({...})` wrote its copy-out over x (x became (14,-6)), and z's over
   y. Reduced from madc's GNU integer-_Complex lowering (task #69). */
struct S { int a, b; };

int main (void) {
  struct S x = { 7, -3 };
  struct S y = ({ struct S t0; t0 = x; struct S t1; t1.a = t0.a * 2; t1.b = t0.b * 2; t1; });
  struct S z = ({ struct S t2; t2 = x; struct S t3; t3.a = t2.a + 1; t3.b = t2.b + 1; t3; });
  if (x.a != 7 || x.b != -3) return 1;
  if (y.a != 14 || y.b != -6) return 2;
  if (z.a != 8 || z.b != -2) return 3;
  return 0;
}
