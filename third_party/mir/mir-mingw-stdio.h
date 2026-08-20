/* This file was added to the MIR fork as part of the MadC project.
   Copyright (C) 2019-2026 Derek Snider <derekbsnider@gmail.com>.
   Same license as the MIR project (see LICENSE).

   mir-mingw-stdio.h -- the ONE name->address map for default-scope import
   resolvers on a mingw host built with __USE_MINGW_ANSI_STDIO.

   gcc parity: with __USE_MINGW_ANSI_STDIO, a mingw-gcc-compiled program's
   printf/scanf family is libmingwex's __mingw_* implementations (C99
   semantics: 80-bit %Lf long double, %a, %lld on any CRT), NOT the CRT's
   exports.  JIT'd/loaded code must bind to the SAME implementations the
   host was compiled against, and no module walk can find them at runtime:
   UCRT does not export the family by name (only the __stdio_common_* back
   ends), and PE ld auto-excludes the mingw runtime archives (libmingwex et
   al.) from --export-all-symbols, so even a fully-exported host exe never
   exports __mingw_printf.  Referencing __mingw_printf HERE makes the host's
   own static link bind the address into the table -- no export needed.

   Three entry classes, all the same root (statically-linked libmingwex code
   invisible to any runtime walk):
     1. PLAIN narrow printf/scanf names -> __mingw_* (the ANSI-stdio posture's
        own aliasing; a plain-name import must reach the same code).
     2. Direct __mingw_* spellings: the served mingw headers' OWN inline
        bodies call these (stdio.h's vfprintf body, swprintf.inl's C++
        overloads, the __USE_MINGW_STRTOX strtod bodies), so JIT-compiled
        header code imports them by exactly these names.  Wide PLAIN names
        deliberately have NO entries: plain swprintf/vswprintf are inline
        bodies the compiler compiles itself, and __mingw_swprintf's 3-arg
        variadic shape is NOT ISO 4-arg swprintf.
     3. Statically-bound PLAIN names the walk resolves WRONG or not at all:
        wrong-flavor libmingwex math (ucrtbase exports wcstold/strtold with
        MSVC 8-byte long double; mingw-gcc statically binds libmingwex's
        80-bit ones), ucrt "oldnames" aliases (strdup is an import-lib
        alias for _strdup -- link-time only, invisible to GetProcAddress),
        and libmingwex-only POSIX-compat surfaces (<dirent.h>).
   Signatures below are declared explicitly (extern-C) rather than trusting
   header guard states -- a drift from the real libmingwex prototypes fails
   the host build loudly instead of corrupting calls at run time.

   Every default-scope resolver on this host flavor consults this map FIRST
   (c2m's import_resolver; madc's madcdl_sym_default).  Do not hand-roll a
   second copy of the name list -- madc's scripts/check-one-mingw-stdio-map.sh
   gates on __mingw_ references outside this header. */

#ifndef MIR_MINGW_STDIO_H
#define MIR_MINGW_STDIO_H

#if defined(_WIN32) && defined(__MINGW32__) && __USE_MINGW_ANSI_STDIO

#include <stdio.h>
#include <string.h>
#include <wchar.h>
#include <math.h>
#include <dirent.h>

#ifdef __cplusplus
extern "C" {
#endif
int __mingw_printf (const char *, ...);
int __mingw_vprintf (const char *, __builtin_va_list);
int __mingw_fprintf (FILE *, const char *, ...);
int __mingw_vfprintf (FILE *, const char *, __builtin_va_list);
int __mingw_sprintf (char *, const char *, ...);
int __mingw_vsprintf (char *, const char *, __builtin_va_list);
int __mingw_snprintf (char *, size_t, const char *, ...);
int __mingw_vsnprintf (char *, size_t, const char *, __builtin_va_list);
int __mingw_asprintf (char **, const char *, ...);
int __mingw_vasprintf (char **, const char *, __builtin_va_list);
int __mingw_scanf (const char *, ...);
int __mingw_vscanf (const char *, __builtin_va_list);
int __mingw_fscanf (FILE *, const char *, ...);
int __mingw_vfscanf (FILE *, const char *, __builtin_va_list);
int __mingw_sscanf (const char *, const char *, ...);
int __mingw_vsscanf (const char *, const char *, __builtin_va_list);
int __mingw_wprintf (const wchar_t *, ...);
int __mingw_vwprintf (const wchar_t *, __builtin_va_list);
int __mingw_fwprintf (FILE *, const wchar_t *, ...);
int __mingw_vfwprintf (FILE *, const wchar_t *, __builtin_va_list);
int __mingw_swprintf (wchar_t *, const wchar_t *, ...);
int __mingw_vswprintf (wchar_t *, const wchar_t *, __builtin_va_list);
int __mingw_snwprintf (wchar_t *, size_t, const wchar_t *, ...);
int __mingw_vsnwprintf (wchar_t *, size_t, const wchar_t *, __builtin_va_list);
int __mingw_wscanf (const wchar_t *, ...);
int __mingw_vwscanf (const wchar_t *, __builtin_va_list);
int __mingw_fwscanf (FILE *, const wchar_t *, ...);
int __mingw_vfwscanf (FILE *, const wchar_t *, __builtin_va_list);
int __mingw_swscanf (const wchar_t *, const wchar_t *, ...);
int __mingw_vswscanf (const wchar_t *, const wchar_t *, __builtin_va_list);
double __mingw_strtod (const char *, char **);
float __mingw_strtof (const char *, char **);
long double __mingw_strtold (const char *, char **);
double __mingw_wcstod (const wchar_t *, wchar_t **);
float __mingw_wcstof (const wchar_t *, wchar_t **);
long double __mingw_wcstold (const wchar_t *, wchar_t **);
long double strtold (const char *, char **);
float wcstof (const wchar_t *, wchar_t **);
long double wcstold (const wchar_t *, wchar_t **);
#ifdef __cplusplus
}
#endif

static const struct {
  const char *name;
  void *addr;
} mir_mingw_ansi_stdio_map[] = {
  /* 1. plain narrow names -> the ANSI-stdio implementations */
  {"printf", (void *) __mingw_printf},     {"vprintf", (void *) __mingw_vprintf},
  {"fprintf", (void *) __mingw_fprintf},   {"vfprintf", (void *) __mingw_vfprintf},
  {"sprintf", (void *) __mingw_sprintf},   {"vsprintf", (void *) __mingw_vsprintf},
  {"snprintf", (void *) __mingw_snprintf}, {"vsnprintf", (void *) __mingw_vsnprintf},
  {"scanf", (void *) __mingw_scanf},       {"vscanf", (void *) __mingw_vscanf},
  {"fscanf", (void *) __mingw_fscanf},     {"vfscanf", (void *) __mingw_vfscanf},
  {"sscanf", (void *) __mingw_sscanf},     {"vsscanf", (void *) __mingw_vsscanf},
  /* 2. direct __mingw_* spellings the served headers' inline bodies call */
  {"__mingw_printf", (void *) __mingw_printf},
  {"__mingw_vprintf", (void *) __mingw_vprintf},
  {"__mingw_fprintf", (void *) __mingw_fprintf},
  {"__mingw_vfprintf", (void *) __mingw_vfprintf},
  {"__mingw_sprintf", (void *) __mingw_sprintf},
  {"__mingw_vsprintf", (void *) __mingw_vsprintf},
  {"__mingw_snprintf", (void *) __mingw_snprintf},
  {"__mingw_vsnprintf", (void *) __mingw_vsnprintf},
  {"__mingw_asprintf", (void *) __mingw_asprintf},
  {"__mingw_vasprintf", (void *) __mingw_vasprintf},
  {"__mingw_scanf", (void *) __mingw_scanf},
  {"__mingw_vscanf", (void *) __mingw_vscanf},
  {"__mingw_fscanf", (void *) __mingw_fscanf},
  {"__mingw_vfscanf", (void *) __mingw_vfscanf},
  {"__mingw_sscanf", (void *) __mingw_sscanf},
  {"__mingw_vsscanf", (void *) __mingw_vsscanf},
  {"__mingw_wprintf", (void *) __mingw_wprintf},
  {"__mingw_vwprintf", (void *) __mingw_vwprintf},
  {"__mingw_fwprintf", (void *) __mingw_fwprintf},
  {"__mingw_vfwprintf", (void *) __mingw_vfwprintf},
  {"__mingw_swprintf", (void *) __mingw_swprintf},
  {"__mingw_vswprintf", (void *) __mingw_vswprintf},
  {"__mingw_snwprintf", (void *) __mingw_snwprintf},
  {"__mingw_vsnwprintf", (void *) __mingw_vsnwprintf},
  {"__mingw_wscanf", (void *) __mingw_wscanf},
  {"__mingw_vwscanf", (void *) __mingw_vwscanf},
  {"__mingw_fwscanf", (void *) __mingw_fwscanf},
  {"__mingw_vfwscanf", (void *) __mingw_vfwscanf},
  {"__mingw_swscanf", (void *) __mingw_swscanf},
  {"__mingw_vswscanf", (void *) __mingw_vswscanf},
  {"__mingw_strtod", (void *) __mingw_strtod},
  {"__mingw_strtof", (void *) __mingw_strtof},
  {"__mingw_strtold", (void *) __mingw_strtold},
  {"__mingw_wcstod", (void *) __mingw_wcstod},
  {"__mingw_wcstof", (void *) __mingw_wcstof},
  {"__mingw_wcstold", (void *) __mingw_wcstold},
  /* 3. libmingwex plain names whose ucrtbase export is the wrong LD flavor
        or absent from ucrtbase entirely (fabsf/fabsl have no UCRT import;
        fabsl is x87 80-bit in libmingwex vs MSVC 8-byte) */
  {"strtold", (void *) strtold},
  {"wcstof", (void *) wcstof},
  {"wcstold", (void *) wcstold},
  {"fabs", (void *) (double (*) (double)) fabs}, /* C++ sees overloads */
  {"fabsf", (void *) fabsf},
  {"fabsl", (void *) fabsl},
  /* 3 (cont.): the COMPLETE C99 long-double math family.  mingw long
     double is x87 80-bit; ucrtbase exports NONE of most *l names (loud
     undefined import) and the few it does export (lgammal, cbrtl) are
     the MSVC 8-byte flavor (silent garbage).  libmingwex implements the
     whole family; the included <math.h> is the prototype source at host
     compile time, so a hidden or drifted declaration fails THIS build
     loudly.  Closed as the complete standard surface rather than
     extended one error at a time (the defect-#3 note's threshold).
     nexttoward/nexttowardf ride along: their second parameter is long
     double, so the unsuffixed names are flavor-sensitive too. */
  {"acosl", (void *) acosl},           {"acoshl", (void *) acoshl},
  {"asinl", (void *) asinl},           {"asinhl", (void *) asinhl},
  {"atanl", (void *) atanl},           {"atanhl", (void *) atanhl},
  {"atan2l", (void *) atan2l},         {"cbrtl", (void *) cbrtl},
  {"ceill", (void *) ceill},           {"copysignl", (void *) copysignl},
  {"cosl", (void *) cosl},             {"coshl", (void *) coshl},
  {"erfl", (void *) erfl},             {"erfcl", (void *) erfcl},
  {"expl", (void *) expl},             {"exp2l", (void *) exp2l},
  {"expm1l", (void *) expm1l},         {"fdiml", (void *) fdiml},
  {"floorl", (void *) floorl},         {"fmal", (void *) fmal},
  {"fmaxl", (void *) fmaxl},           {"fminl", (void *) fminl},
  {"fmodl", (void *) fmodl},           {"frexpl", (void *) frexpl},
  {"hypotl", (void *) hypotl},         {"ilogbl", (void *) ilogbl},
  {"ldexpl", (void *) ldexpl},         {"lgammal", (void *) lgammal},
  {"llrintl", (void *) llrintl},       {"llroundl", (void *) llroundl},
  {"logl", (void *) logl},             {"log10l", (void *) log10l},
  {"log1pl", (void *) log1pl},         {"log2l", (void *) log2l},
  {"logbl", (void *) logbl},           {"lrintl", (void *) lrintl},
  {"lroundl", (void *) lroundl},       {"modfl", (void *) modfl},
  {"nearbyintl", (void *) nearbyintl}, {"nextafterl", (void *) nextafterl},
  {"nexttowardl", (void *) nexttowardl},
  {"nexttoward", (void *) (double (*) (double, long double)) nexttoward},
  {"nexttowardf", (void *) (float (*) (float, long double)) nexttowardf},
  {"powl", (void *) powl},             {"remainderl", (void *) remainderl},
  {"remquol", (void *) remquol},       {"rintl", (void *) rintl},
  {"roundl", (void *) roundl},         {"scalblnl", (void *) scalblnl},
  {"scalbnl", (void *) scalbnl},       {"sinl", (void *) sinl},
  {"sinhl", (void *) sinhl},           {"sqrtl", (void *) sqrtl},
  {"tanl", (void *) tanl},             {"tanhl", (void *) tanhl},
  {"tgammal", (void *) tgammal},       {"truncl", (void *) truncl},
  /* 3 (cont.): the FLOAT math names ucrtbase does not export.  The
     complete C99 <math.h> float-suffix surface was audited against the
     real export list (__imp_ stubs in the mingw import lib, cross-checked
     with GetProcAddress under wine): exactly these four are hidden --
     ucrt serves them as header inlines / the _hypotf oldname, so
     mingw-gcc statically binds libmingwex's.  Closed as the complete
     hidden set, same closure rule as the long-double family above (the
     unsuffixed surface is fully exported; nothing else to pin). */
  {"fabsf", (void *) fabsf},           {"frexpf", (void *) frexpf},
  {"hypotf", (void *) hypotf},         {"ldexpf", (void *) ldexpf},
  /* 3 (cont.): ucrt "oldnames" aliases.  ucrtbase exports only the
     underscore spellings (_strdup, _wcsdup); the plain POSIX names exist
     purely as import-library aliases resolved at LINK time, so no module
     walk can ever see them.  Only the names the suite has hit so far --
     if this class keeps growing, generate the full oldnames list from nm
     (the HOSTTAB approach) instead of extending it one name at a time. */
  {"strdup", (void *) strdup},
  {"wcsdup", (void *) wcsdup},
  /* 3 (cont.): the <dirent.h> family -- implemented entirely in
     libmingwex (no CRT export at any spelling).  The complete surface of
     the served header, narrow and wide, same closure rule as the math
     family above. */
  {"opendir", (void *) opendir},       {"readdir", (void *) readdir},
  {"closedir", (void *) closedir},     {"rewinddir", (void *) rewinddir},
  {"telldir", (void *) telldir},       {"seekdir", (void *) seekdir},
  {"_wopendir", (void *) _wopendir},   {"_wreaddir", (void *) _wreaddir},
  {"_wclosedir", (void *) _wclosedir}, {"_wrewinddir", (void *) _wrewinddir},
  {"_wtelldir", (void *) _wtelldir},   {"_wseekdir", (void *) _wseekdir},
};

static inline void *mir_mingw_ansi_stdio_lookup (const char *name) {
  for (size_t i = 0; i < sizeof (mir_mingw_ansi_stdio_map) / sizeof (mir_mingw_ansi_stdio_map[0]);
       i++)
    if (strcmp (name, mir_mingw_ansi_stdio_map[i].name) == 0)
      return mir_mingw_ansi_stdio_map[i].addr;
  return NULL;
}

#endif /* _WIN32 && __MINGW32__ && __USE_MINGW_ANSI_STDIO */

#endif /* MIR_MINGW_STDIO_H */
