/* This file is a part of MIR project.
   Copyright (C) 2020-2024 Vladimir Makarov <vmakarov.gcc@gmail.com>.
*/

#ifndef MIR_INT128_HELPER_H
#define MIR_INT128_HELPER_H

#include <string.h>

#if defined(__SIZEOF_INT128__)
static __int128 MIR_int128_divti3 (__int128 a, __int128 b) { return a / b; }

static unsigned __int128 MIR_int128_udivti3 (unsigned __int128 a, unsigned __int128 b) {
  return a / b;
}

static __int128 MIR_int128_modti3 (__int128 a, __int128 b) { return a % b; }

static unsigned __int128 MIR_int128_umodti3 (unsigned __int128 a, unsigned __int128 b) {
  return a % b;
}

/* int128 <-> floating conversions (libgcc names).  The int128 argument /
   result ABI is two integer eightbytes, which is exactly how the c2mir call
   sites spell the prototypes (two u64 args / two u64 results). */
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

/* __builtin_{add,sub,mul}_overflow with an int128 result: the host builtin
   covers every edge (INT128_MIN, -1 multipliers, ...).  The result is stored
   through the pointer; the return value is the overflow flag. */
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
