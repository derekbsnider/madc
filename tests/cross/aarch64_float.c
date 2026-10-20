/* aarch64_float.c — <float.h> on aarch64-linux, whose long double is IEEE
   binary128. Both headers under test spell the target's predefined
   __FLT_*__ / __DBL_*__ / __LDBL_*__ macros: madc's (the aot leg) carried
   x86-64's x87 values, and c2mir's (the jit leg, MIR's c2m driver) said long
   double was double, FLT_DIG 9 and DBL_DIG 17, and typed FLT_MAX double. Only
   values that survive the x86-hosted cross compiler's 64-bit constant folding
   are printed (binary128's LDBL_MAX and LDBL_TRUE_MIN do not: aarch64_ldouble_const.c). */
#include <float.h>
int printf (const char *, ...);
int main(void) {
    printf("flt: %d %d %d %d %d %d %d\n", FLT_RADIX, FLT_MANT_DIG, FLT_DIG, FLT_MIN_EXP,
           FLT_MIN_10_EXP, FLT_MAX_10_EXP, FLT_DECIMAL_DIG);
    printf("dbl: %d %d %d %d %d %d\n", DBL_MANT_DIG, DBL_DIG, DBL_MIN_10_EXP, DBL_MAX_EXP,
           DBL_MAX_10_EXP, DBL_DECIMAL_DIG);
    printf("ldbl: %d %d %d %d %d %d %d\n", LDBL_MANT_DIG, LDBL_DIG, LDBL_MIN_EXP, LDBL_MIN_10_EXP,
           LDBL_MAX_EXP, LDBL_MAX_10_EXP, LDBL_DECIMAL_DIG);
    printf("decimal: %d eval: %d rounds: %d subnorm: %d %d %d\n", DECIMAL_DIG, FLT_EVAL_METHOD,
           FLT_ROUNDS, FLT_HAS_SUBNORM, DBL_HAS_SUBNORM, LDBL_HAS_SUBNORM);
    printf("eps: %La %a %a\n", LDBL_EPSILON, DBL_EPSILON, (double)FLT_EPSILON);
    printf("min: %La %a %a\n", LDBL_MIN, DBL_MIN, DBL_TRUE_MIN);
    printf("types: %d %d %d\n", (int)sizeof(FLT_MAX), (int)sizeof(DBL_MAX), (int)sizeof(LDBL_EPSILON));
    return 0;
}
