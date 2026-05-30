/* fnptr.c — function-pointer variables (parenthesized declarators).
 *
 * Drives the cir_builder fn-pointer declarator construction and the
 * cir_emit_c parenthesized-declarator (spiral-rule) emission. The shapes
 * mirror SMAUG's command/spell tables: a scalar fn-pointer parameter, and
 * a local fn-pointer assigned a function address and called through. A flat
 * declarator emitter renders these as `*fp(int)` (function-returning-pointer)
 * — the wrong type — so this reducer only reaches FIDELITY-OK once (*fp) is
 * parenthesized.
 */

int add1(int x) { return x + 1; }
int add2(int x) { return x + 2; }

int apply(int (*fp)(int), int v) { return fp(v); }

int main(void)
{
	int (*fp)(int);
	int total;
	fp = add1;
	total = fp(5);
	total = total + apply(add2, 7);
	return total;
}
