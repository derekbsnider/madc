// The quoted header tests/testcomputedinclude.mad reaches through a
// macro-replaced #include (C11 6.10.2p4).
#ifndef TESTCOMPUTEDINCLUDE_HELPER_H
#define TESTCOMPUTEDINCLUDE_HELPER_H
static int helper_answer(void) { return 42; }
#endif
