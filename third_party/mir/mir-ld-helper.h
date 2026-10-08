/* This file was added to the MIR fork as part of the MadC project.
   Copyright (C) 2019-2026 Derek Snider <derekbsnider@gmail.com>.
   Same license as the MIR project (see LICENSE).
*/

/* libgcc's binary128 soft-float routines: what the aarch64-linux long double
   builtins call (mir-gen-aarch64.c, get_builtin), by the names gcc's own
   objects import.  The generator binds them BY ADDRESS, so a generator that is
   itself compiled by c2m -- MIR compiling its own sources, the c2mir-bootstrap
   tests -- imports these names, and an import resolver must hand back the
   host's addresses: libgcc links them into the host with hidden visibility,
   so dlsym never finds them.  MIR_ld_helper_resolver is that lookup, beside
   MIR_int128_helper_resolver in every driver.

   Declared ADDRESS-ONLY: no C call is ever made through these prototypes, so
   their C type is deliberately not the routine's (on a cross host, long
   double is not even binary128). */

#ifndef MIR_LD_HELPER_H
#define MIR_LD_HELPER_H

#include <string.h>

#define MIR_LD_HELPERS(H)                                                        \
  H (__floatditf)                                                                \
  H (__floatunditf)                                                              \
  H (__extendsftf2)                                                              \
  H (__extenddftf2)                                                              \
  H (__fixtfdi)                                                                  \
  H (__trunctfsf2)                                                               \
  H (__trunctfdf2)                                                               \
  H (__addtf3)                                                                   \
  H (__subtf3)                                                                   \
  H (__multf3)                                                                   \
  H (__divtf3)                                                                   \
  H (__negtf2)                                                                   \
  H (__eqtf2)                                                                    \
  H (__netf2)                                                                    \
  H (__lttf2)                                                                    \
  H (__getf2)                                                                    \
  H (__gttf2)                                                                    \
  H (__letf2)

#define MIR_LD_HELPER_DECL_(f) extern void f (void);
MIR_LD_HELPERS (MIR_LD_HELPER_DECL_)
#undef MIR_LD_HELPER_DECL_

/* Only an aarch64-linux host runs the generator that calls them.  inline: the
   generator includes this header for the declarations alone. */
static inline void *MIR_ld_helper_resolver (const char *name) {
#if defined(__aarch64__) && !defined(__APPLE__) && !defined(_WIN32)
#define MIR_LD_HELPER_CASE_(f) \
  if (strcmp (name, #f) == 0) return (void *) f;
  MIR_LD_HELPERS (MIR_LD_HELPER_CASE_)
#undef MIR_LD_HELPER_CASE_
#else
  (void) name;
#endif
  return NULL;
}

#endif /* MIR_LD_HELPER_H */
