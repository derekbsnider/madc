/* Same-named file-local functions as testprojectstaticfn_a.c, different
 * bodies — internal linkage keeps them TU-local. */
static int helper(void) { return 100; }
static inline int twice(int v) { return 3 * v; }
int from_b(void) { return twice(helper()); }
