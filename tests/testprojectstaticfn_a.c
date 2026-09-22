#include <stdio.h>
/* C internal linkage for FUNCTIONS: both TUs define static helper() and
 * static inline twice() with DIFFERENT bodies — each TU must call its own
 * copy (gcc/clang oracle: a=2 b=300). Pre-fix, madc emitted statics with
 * external linkage: the two-module MIR link died with "func helper is
 * prohibited for redefinition" (same root as the AOT-ledger rt_dump.h
 * static-inline pair). */
static int helper(void) { return 1; }
static inline int twice(int v) { return 2 * v; }
int from_a(void) { return twice(helper()); }
extern int from_b(void);

/* The ENUM-TYPED twins of the same rule. `static` reached parseDeclaration
 * through a Program flag that TokenSTATIC::parse restored on the way out, and
 * TokenENUM::parse DEFERS its declarator by pushing the resolved type token
 * back — so the deferred declaration never saw the flag and every enum-typed
 * static (function OR variable) silently got EXTERNAL linkage. A plain `int`
 * or `struct` static was unaffected, which is why the helper()/twice() pair
 * above could not catch it. Three of c2mir.c's file-local helpers leaked as
 * global symbols this way. gcc/clang oracle: ea=1 eb=2. */
enum kind { K_NONE, K_A, K_B };
static enum kind which(void) { return K_A; }
static enum kind which_var = K_A;
int enum_from_a(void) { return (int) which() + (int) which_var - 1; }
extern int enum_from_b(void);

int main(void)
{
	printf("a=%d b=%d\n", from_a(), from_b());
	printf("ea=%d eb=%d\n", enum_from_a(), enum_from_b());
	return 0;
}
