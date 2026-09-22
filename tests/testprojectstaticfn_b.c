/* Same-named file-local functions as testprojectstaticfn_a.c, different
 * bodies — internal linkage keeps them TU-local. */
static int helper(void) { return 100; }
static inline int twice(int v) { return 3 * v; }
int from_b(void) { return twice(helper()); }

/* The enum-typed twins, with DIFFERENT values: if an enum-typed static leaks
 * to external linkage the two TUs collide at MIR link, or this TU binds a's
 * copy and prints a's answer. */
enum kind { K_NONE, K_A, K_B };
static enum kind which(void) { return K_B; }
static enum kind which_var = K_B;
int enum_from_b(void) { return (int) which() + (int) which_var - 2; }
