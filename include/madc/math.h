// madc embedded math.h — auto-loads libm and defines math constants
#load "libm.so.6" as libm;

#define M_PI       3.14159265358979323846
#define M_PI_2     1.57079632679489661923
#define M_PI_4     0.78539816339744830962
#define M_E        2.71828182845904523536
#define M_LOG2E    1.44269504088896340736
#define M_LOG10E   0.43429448190325182765
#define M_LN2      0.69314718055994530942
#define M_LN10     2.30258509299404568402
#define M_SQRT2    1.41421356237309504880
#define M_SQRT1_2  0.70710678118654752440
#define HUGE_VAL   __builtin_huge_val()
#define INFINITY   __builtin_inf()

// Declare the real return types for the libm family. Without these, a libm
// function resolves through the dlsym fallback with a generic 64-bit (long)
// return signature — so the floating-point result (returned in xmm0) is read
// out of the integer return register instead, producing garbage (e.g.
// pow(32.0, 1.0/3.0) read as 4.61e18 instead of 3.1748). This is the same
// return-type bug that the string.h comparison family fixed (strcmp -> int),
// here extended to the double / float / long double libm functions.
//
// Standard C99 trig / exponential / power / rounding functions: double,
// float ("f" suffix) and long double ("l" suffix) variants.

// Trigonometric
extern double sin(double x);
extern double cos(double x);
extern double tan(double x);
extern double asin(double x);
extern double acos(double x);
extern double atan(double x);
extern double atan2(double y, double x);
extern float sinf(float x);
extern float cosf(float x);
extern float tanf(float x);
extern float asinf(float x);
extern float acosf(float x);
extern float atanf(float x);
extern float atan2f(float y, float x);
extern long double sinl(long double x);
extern long double cosl(long double x);
extern long double tanl(long double x);
extern long double asinl(long double x);
extern long double acosl(long double x);
extern long double atanl(long double x);
extern long double atan2l(long double y, long double x);

// Hyperbolic
extern double sinh(double x);
extern double cosh(double x);
extern double tanh(double x);
extern double asinh(double x);
extern double acosh(double x);
extern double atanh(double x);
extern float sinhf(float x);
extern float coshf(float x);
extern float tanhf(float x);
extern float asinhf(float x);
extern float acoshf(float x);
extern float atanhf(float x);
extern long double sinhl(long double x);
extern long double coshl(long double x);
extern long double tanhl(long double x);
extern long double asinhl(long double x);
extern long double acoshl(long double x);
extern long double atanhl(long double x);

// Exponential / logarithmic
extern double exp(double x);
extern double exp2(double x);
extern double expm1(double x);
extern double log(double x);
extern double log2(double x);
extern double log10(double x);
extern double log1p(double x);
extern double logb(double x);
extern float expf(float x);
extern float exp2f(float x);
extern float expm1f(float x);
extern float logf(float x);
extern float log2f(float x);
extern float log10f(float x);
extern float log1pf(float x);
extern float logbf(float x);
extern long double expl(long double x);
extern long double exp2l(long double x);
extern long double expm1l(long double x);
extern long double logl(long double x);
extern long double log2l(long double x);
extern long double log10l(long double x);
extern long double log1pl(long double x);
extern long double logbl(long double x);

// Power / roots
extern double pow(double x, double y);
extern double sqrt(double x);
extern double cbrt(double x);
extern double hypot(double x, double y);
extern float powf(float x, float y);
extern float sqrtf(float x);
extern float cbrtf(float x);
extern float hypotf(float x, float y);
extern long double powl(long double x, long double y);
extern long double sqrtl(long double x);
extern long double cbrtl(long double x);
extern long double hypotl(long double x, long double y);

// Rounding / remainder / absolute value
extern double ceil(double x);
extern double floor(double x);
extern double trunc(double x);
extern double round(double x);
extern double nearbyint(double x);
extern double rint(double x);
extern double fabs(double x);
extern double fmod(double x, double y);
extern double remainder(double x, double y);
extern double copysign(double x, double y);
extern double nextafter(double x, double y);
extern double fdim(double x, double y);
extern double fmax(double x, double y);
extern double fmin(double x, double y);
extern double fma(double x, double y, double z);
extern float ceilf(float x);
extern float floorf(float x);
extern float truncf(float x);
extern float roundf(float x);
extern float nearbyintf(float x);
extern float rintf(float x);
extern float fabsf(float x);
extern float fmodf(float x, float y);
extern float remainderf(float x, float y);
extern float copysignf(float x, float y);
extern float nextafterf(float x, float y);
extern float fdimf(float x, float y);
extern float fmaxf(float x, float y);
extern float fminf(float x, float y);
extern float fmaf(float x, float y, float z);
extern long double ceill(long double x);
extern long double floorl(long double x);
extern long double truncl(long double x);
extern long double roundl(long double x);
extern long double nearbyintl(long double x);
extern long double rintl(long double x);
extern long double fabsl(long double x);
extern long double fmodl(long double x, long double y);
extern long double remainderl(long double x, long double y);
extern long double copysignl(long double x, long double y);
extern long double nextafterl(long double x, long double y);
extern long double fdiml(long double x, long double y);
extern long double fmaxl(long double x, long double y);
extern long double fminl(long double x, long double y);
extern long double fmal(long double x, long double y, long double z);

// Decomposition / scaling (take pointer / int args, still return floating)
extern double modf(double x, double *iptr);
extern double frexp(double x, int *exp);
extern double ldexp(double x, int exp);
extern double scalbn(double x, int n);
extern float modff(float x, float *iptr);
extern float frexpf(float x, int *exp);
extern float ldexpf(float x, int exp);
extern float scalbnf(float x, int n);
extern long double modfl(long double x, long double *iptr);
extern long double frexpl(long double x, int *exp);
extern long double ldexpl(long double x, int exp);
extern long double scalbnl(long double x, int n);

// Gamma / error functions
extern double tgamma(double x);
extern double lgamma(double x);
extern double erf(double x);
extern double erfc(double x);
extern float tgammaf(float x);
extern float lgammaf(float x);
extern float erff(float x);
extern float erfcf(float x);
extern long double tgammal(long double x);
extern long double lgammal(long double x);
extern long double erfl(long double x);
extern long double erfcl(long double x);

// Integer-returning math functions (NOT floating return)
extern int ilogb(double x);
extern int ilogbf(float x);
extern int ilogbl(long double x);
extern long lrint(double x);
extern long lround(double x);
extern long long llrint(double x);
extern long long llround(double x);
