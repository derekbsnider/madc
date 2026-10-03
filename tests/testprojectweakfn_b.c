/* The other TU of testprojectweakfn (see testprojectweakfn_a.c). */
int f(void) { return 2; }
__attribute__((weak)) int g(void) { return 4; }
int (*get_f(void))(void) { return f; }
int g_from_b(void) { return g(); }
int h(void) { return 6; }
int p(void) { return 8; }
