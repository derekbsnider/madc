/* This file was added to the MIR fork as part of the MadC project.
   Copyright (C) 2019-2026 Derek Snider <derekbsnider@gmail.com>.
   Same license as the MIR project (see LICENSE).
*/

#ifndef MIR_INT128_HELPER_H
#define MIR_INT128_HELPER_H

#include <string.h>
#include <stdint.h>

#if defined(__SIZEOF_INT128__)
#ifndef _WIN32
static __int128 MIR_int128_divti3 (__int128 a, __int128 b) { return a / b; }

static unsigned __int128 MIR_int128_udivti3 (unsigned __int128 a, unsigned __int128 b) {
  return a / b;
}

static __int128 MIR_int128_modti3 (__int128 a, __int128 b) { return a % b; }

static unsigned __int128 MIR_int128_umodti3 (unsigned __int128 a, unsigned __int128 b) {
  return a % b;
}

/* int128 <-> floating conversions (libgcc names).  On SysV the int128
   argument / result ABI is two integer eightbytes, which is exactly how the
   c2mir call sites spell the prototypes (two u64 args / two u64 results) --
   the native-__int128 bodies coincide with the MIR protos, and AOT ELF
   executables may equally bind the REAL libgcc entries by these names. */
static float MIR_int128_floattisf (__int128 a) { return (float) a; }
static double MIR_int128_floattidf (__int128 a) { return (double) a; }
static long double MIR_int128_floattixf (__int128 a) { return (long double) a; }
static float MIR_int128_floatuntisf (unsigned __int128 a) { return (float) a; }
static double MIR_int128_floatuntidf (unsigned __int128 a) { return (double) a; }
static long double MIR_int128_floatuntixf (unsigned __int128 a) { return (long double) a; }
static __int128 MIR_int128_fixsfti (float a) { return (__int128) a; }
static __int128 MIR_int128_fixdfti (double a) { return (__int128) a; }
static __int128 MIR_int128_fixxfti (long double a) { return (__int128) a; }
static unsigned __int128 MIR_int128_fixunssfti (float a) { return (unsigned __int128) a; }
static unsigned __int128 MIR_int128_fixunsdfti (double a) { return (unsigned __int128) a; }
static unsigned __int128 MIR_int128_fixunsxfti (long double a) { return (unsigned __int128) a; }

#else /* _WIN32 */

/* win64: the two-eightbyte TImode coincidence is SysV-ONLY.  Native win64
   __int128 passes by reference and returns in XMM0, so a native-__int128
   body can never match a MIR proto spelled in eightbytes, and MIR cannot
   spell a two-value return on win64 at all ("Windows x86-64 doesn't support
   multiple return values").  These twins make the C signature BE the MIR
   proto shape: int128 results are written through a result pointer (the
   res-addr-first convention the __mir_*oti overflow family already uses);
   int128 arguments arrive as explicit low/high eightbytes.  On win64 the
   import names therefore mean "the MIR proto contract", which does NOT
   match mingw libgcc's native-ABI __divti3 family -- a future win64 AOT
   lane (W3) must export THESE under the import names, never bind libgcc's. */
static __int128 mir_i128_compose_ (uint64_t low, uint64_t high) {
  return (__int128) (((unsigned __int128) high << 64) | low);
}
static void mir_i128_store_ (void *res, unsigned __int128 v) { memcpy (res, &v, 16); }

static void MIR_int128_divti3 (void *res, uint64_t a_low, uint64_t a_high, uint64_t b_low,
                               uint64_t b_high) {
  mir_i128_store_ (res, (unsigned __int128) (mir_i128_compose_ (a_low, a_high)
                                             / mir_i128_compose_ (b_low, b_high)));
}
static void MIR_int128_udivti3 (void *res, uint64_t a_low, uint64_t a_high, uint64_t b_low,
                                uint64_t b_high) {
  mir_i128_store_ (res, (unsigned __int128) mir_i128_compose_ (a_low, a_high)
                          / (unsigned __int128) mir_i128_compose_ (b_low, b_high));
}
static void MIR_int128_modti3 (void *res, uint64_t a_low, uint64_t a_high, uint64_t b_low,
                               uint64_t b_high) {
  mir_i128_store_ (res, (unsigned __int128) (mir_i128_compose_ (a_low, a_high)
                                             % mir_i128_compose_ (b_low, b_high)));
}
static void MIR_int128_umodti3 (void *res, uint64_t a_low, uint64_t a_high, uint64_t b_low,
                                uint64_t b_high) {
  mir_i128_store_ (res, (unsigned __int128) mir_i128_compose_ (a_low, a_high)
                          % (unsigned __int128) mir_i128_compose_ (b_low, b_high));
}

static float MIR_int128_floattisf (uint64_t low, uint64_t high) {
  return (float) mir_i128_compose_ (low, high);
}
static double MIR_int128_floattidf (uint64_t low, uint64_t high) {
  return (double) mir_i128_compose_ (low, high);
}
static long double MIR_int128_floattixf (uint64_t low, uint64_t high) {
  return (long double) mir_i128_compose_ (low, high);
}
static float MIR_int128_floatuntisf (uint64_t low, uint64_t high) {
  return (float) (unsigned __int128) mir_i128_compose_ (low, high);
}
static double MIR_int128_floatuntidf (uint64_t low, uint64_t high) {
  return (double) (unsigned __int128) mir_i128_compose_ (low, high);
}
static long double MIR_int128_floatuntixf (uint64_t low, uint64_t high) {
  return (long double) (unsigned __int128) mir_i128_compose_ (low, high);
}
static void MIR_int128_fixsfti (void *res, float a) {
  mir_i128_store_ (res, (unsigned __int128) (__int128) a);
}
static void MIR_int128_fixdfti (void *res, double a) {
  mir_i128_store_ (res, (unsigned __int128) (__int128) a);
}
static void MIR_int128_fixxfti (void *res, long double a) {
  mir_i128_store_ (res, (unsigned __int128) (__int128) a);
}
static void MIR_int128_fixunssfti (void *res, float a) {
  mir_i128_store_ (res, (unsigned __int128) a);
}
static void MIR_int128_fixunsdfti (void *res, double a) {
  mir_i128_store_ (res, (unsigned __int128) a);
}
static void MIR_int128_fixunsxfti (void *res, long double a) {
  mir_i128_store_ (res, (unsigned __int128) a);
}
#endif /* _WIN32 */

/* __builtin_{add,sub,mul}_overflow with an int128 result: the host builtin
   covers every edge (INT128_MIN, -1 multipliers, ...).  The result is stored
   through the pointer; the return value is the overflow flag.
   win64: same res-addr proto, but the int128 OPERANDS arrive as explicit
   eightbytes (see the twin rationale above) — the native-__int128 parameter
   spelling would go by reference there and shear against the MIR proto. */
#ifdef _WIN32
static long long MIR_int128_addoti (void *res, uint64_t a_low, uint64_t a_high, uint64_t b_low,
                                    uint64_t b_high) {
  __int128 r;
  long long ov = __builtin_add_overflow (mir_i128_compose_ (a_low, a_high),
                                         mir_i128_compose_ (b_low, b_high), &r);
  memcpy (res, &r, 16);
  return ov;
}
static long long MIR_int128_uaddoti (void *res, uint64_t a_low, uint64_t a_high, uint64_t b_low,
                                     uint64_t b_high) {
  unsigned __int128 r;
  long long ov
    = __builtin_add_overflow ((unsigned __int128) mir_i128_compose_ (a_low, a_high),
                              (unsigned __int128) mir_i128_compose_ (b_low, b_high), &r);
  memcpy (res, &r, 16);
  return ov;
}
static long long MIR_int128_suboti (void *res, uint64_t a_low, uint64_t a_high, uint64_t b_low,
                                    uint64_t b_high) {
  __int128 r;
  long long ov = __builtin_sub_overflow (mir_i128_compose_ (a_low, a_high),
                                         mir_i128_compose_ (b_low, b_high), &r);
  memcpy (res, &r, 16);
  return ov;
}
static long long MIR_int128_usuboti (void *res, uint64_t a_low, uint64_t a_high, uint64_t b_low,
                                     uint64_t b_high) {
  unsigned __int128 r;
  long long ov
    = __builtin_sub_overflow ((unsigned __int128) mir_i128_compose_ (a_low, a_high),
                              (unsigned __int128) mir_i128_compose_ (b_low, b_high), &r);
  memcpy (res, &r, 16);
  return ov;
}
static long long MIR_int128_muloti (void *res, uint64_t a_low, uint64_t a_high, uint64_t b_low,
                                    uint64_t b_high) {
  __int128 r;
  long long ov = __builtin_mul_overflow (mir_i128_compose_ (a_low, a_high),
                                         mir_i128_compose_ (b_low, b_high), &r);
  memcpy (res, &r, 16);
  return ov;
}
static long long MIR_int128_umuloti (void *res, uint64_t a_low, uint64_t a_high, uint64_t b_low,
                                     uint64_t b_high) {
  unsigned __int128 r;
  long long ov
    = __builtin_mul_overflow ((unsigned __int128) mir_i128_compose_ (a_low, a_high),
                              (unsigned __int128) mir_i128_compose_ (b_low, b_high), &r);
  memcpy (res, &r, 16);
  return ov;
}
#else /* !_WIN32 */
static long MIR_int128_addoti (void *res, __int128 a, __int128 b) {
  __int128 r;
  long ov = __builtin_add_overflow (a, b, &r);
  memcpy (res, &r, 16);
  return ov;
}
static long MIR_int128_uaddoti (void *res, unsigned __int128 a, unsigned __int128 b) {
  unsigned __int128 r;
  long ov = __builtin_add_overflow (a, b, &r);
  memcpy (res, &r, 16);
  return ov;
}
static long MIR_int128_suboti (void *res, __int128 a, __int128 b) {
  __int128 r;
  long ov = __builtin_sub_overflow (a, b, &r);
  memcpy (res, &r, 16);
  return ov;
}
static long MIR_int128_usuboti (void *res, unsigned __int128 a, unsigned __int128 b) {
  unsigned __int128 r;
  long ov = __builtin_sub_overflow (a, b, &r);
  memcpy (res, &r, 16);
  return ov;
}
#ifdef __APPLE__
/* Checked 128-bit multiply WITHOUT __builtin_mul_overflow: clang lowers the
   TI case to a __muloti4 libcall, and darwin's libSystem(libcompiler_rt)
   exports it for x86_64 only -- an arm64-macos link has no home for it (the
   cross toolchain ships no darwin compiler-rt archive either).  Plain
   128-bit multiply inlines on both darwin arches, and the wrap check
   divides with __udivti3, which libSystem exports on both. */
static long MIR_int128_umul_ov (unsigned __int128 a, unsigned __int128 b, unsigned __int128 *r) {
  *r = a * b;
  return a != 0 && *r / a != b;
}
static long MIR_int128_muloti (void *res, __int128 a, __int128 b) {
  unsigned __int128 ua = a < 0 ? -(unsigned __int128) a : (unsigned __int128) a;
  unsigned __int128 ub = b < 0 ? -(unsigned __int128) b : (unsigned __int128) b;
  unsigned __int128 ur;
  int neg = (a < 0) != (b < 0);
  long ov = MIR_int128_umul_ov (ua, ub, &ur);
  unsigned __int128 lim = ((unsigned __int128) 1 << 127) - (neg ? 0 : 1);
  if (ur > lim) ov = 1;
  __int128 r = neg ? -(__int128) ur : (__int128) ur; /* wraps like the builtin */
  memcpy (res, &r, 16);
  return ov;
}
static long MIR_int128_umuloti (void *res, unsigned __int128 a, unsigned __int128 b) {
  unsigned __int128 r;
  long ov = MIR_int128_umul_ov (a, b, &r);
  memcpy (res, &r, 16);
  return ov;
}
#else
static long MIR_int128_muloti (void *res, __int128 a, __int128 b) {
  __int128 r;
  long ov = __builtin_mul_overflow (a, b, &r);
  memcpy (res, &r, 16);
  return ov;
}
static long MIR_int128_umuloti (void *res, unsigned __int128 a, unsigned __int128 b) {
  unsigned __int128 r;
  long ov = __builtin_mul_overflow (a, b, &r);
  memcpy (res, &r, 16);
  return ov;
}
#endif /* __APPLE__ */
#endif /* !_WIN32 */

/* AOT: the __mir_*oti overflow helpers have no libgcc equivalent (the
   __divti3 family resolves from libgcc_s in a linked process), so a linked
   .o/executable needs them under their import names as dynamic symbols.
   Exported from exactly ONE including TU (c2mir.c defines the macro) via
   asm-name aliases, mirroring the mir.* builtin exports.
   Apple hosts are excluded: Mach-O has no symbol aliases, and the lane the
   exports serve (ELF executables dyld-binding __mir_* against the host /
   libmadc.so) does not exist on Apple targets -- there is no libmadc dylib
   and runtime-needing emits are refused; the JIT resolves these through
   MIR_int128_helper_resolver in-process.  Revisit with the Mach-O runtime
   dylib story (note the writer's `_` symbol-prefix convention then). */
#if defined(MIR_INT128_EXPORT_ALIASES) && defined(__GNUC__) && !defined(_WIN32) \
  && !defined(__APPLE__)
extern __typeof (MIR_int128_addoti) MIR_int128_addoti_export asm ("__mir_addoti")
  __attribute__ ((alias ("MIR_int128_addoti"), used));
extern __typeof (MIR_int128_uaddoti) MIR_int128_uaddoti_export asm ("__mir_uaddoti")
  __attribute__ ((alias ("MIR_int128_uaddoti"), used));
extern __typeof (MIR_int128_suboti) MIR_int128_suboti_export asm ("__mir_suboti")
  __attribute__ ((alias ("MIR_int128_suboti"), used));
extern __typeof (MIR_int128_usuboti) MIR_int128_usuboti_export asm ("__mir_usuboti")
  __attribute__ ((alias ("MIR_int128_usuboti"), used));
extern __typeof (MIR_int128_muloti) MIR_int128_muloti_export asm ("__mir_muloti")
  __attribute__ ((alias ("MIR_int128_muloti"), used));
extern __typeof (MIR_int128_umuloti) MIR_int128_umuloti_export asm ("__mir_umuloti")
  __attribute__ ((alias ("MIR_int128_umuloti"), used));
#endif
#endif

static void *MIR_int128_helper_resolver (const char *name) {
#if defined(__SIZEOF_INT128__)
  if (strcmp (name, "__divti3") == 0) return (void *) MIR_int128_divti3;
  if (strcmp (name, "__udivti3") == 0) return (void *) MIR_int128_udivti3;
  if (strcmp (name, "__modti3") == 0) return (void *) MIR_int128_modti3;
  if (strcmp (name, "__umodti3") == 0) return (void *) MIR_int128_umodti3;
  if (strcmp (name, "__floattisf") == 0) return (void *) MIR_int128_floattisf;
  if (strcmp (name, "__floattidf") == 0) return (void *) MIR_int128_floattidf;
  if (strcmp (name, "__floattixf") == 0) return (void *) MIR_int128_floattixf;
  if (strcmp (name, "__floatuntisf") == 0) return (void *) MIR_int128_floatuntisf;
  if (strcmp (name, "__floatuntidf") == 0) return (void *) MIR_int128_floatuntidf;
  if (strcmp (name, "__floatuntixf") == 0) return (void *) MIR_int128_floatuntixf;
  if (strcmp (name, "__fixsfti") == 0) return (void *) MIR_int128_fixsfti;
  if (strcmp (name, "__fixdfti") == 0) return (void *) MIR_int128_fixdfti;
  if (strcmp (name, "__fixxfti") == 0) return (void *) MIR_int128_fixxfti;
  if (strcmp (name, "__fixunssfti") == 0) return (void *) MIR_int128_fixunssfti;
  if (strcmp (name, "__fixunsdfti") == 0) return (void *) MIR_int128_fixunsdfti;
  if (strcmp (name, "__fixunsxfti") == 0) return (void *) MIR_int128_fixunsxfti;
  if (strcmp (name, "__mir_addoti") == 0) return (void *) MIR_int128_addoti;
  if (strcmp (name, "__mir_uaddoti") == 0) return (void *) MIR_int128_uaddoti;
  if (strcmp (name, "__mir_suboti") == 0) return (void *) MIR_int128_suboti;
  if (strcmp (name, "__mir_usuboti") == 0) return (void *) MIR_int128_usuboti;
  if (strcmp (name, "__mir_muloti") == 0) return (void *) MIR_int128_muloti;
  if (strcmp (name, "__mir_umuloti") == 0) return (void *) MIR_int128_umuloti;
#else
  (void) name;
#endif
  return NULL;
}

#endif
