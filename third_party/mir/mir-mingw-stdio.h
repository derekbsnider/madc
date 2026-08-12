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

   Every default-scope resolver on this host flavor consults this map FIRST
   (c2m's import_resolver; madc's madcdl_sym_default).  Do not hand-roll a
   second copy of the name list -- madc's scripts/check-one-mingw-stdio-map.sh
   gates on __mingw_ references outside this header. */

#ifndef MIR_MINGW_STDIO_H
#define MIR_MINGW_STDIO_H

#if defined(_WIN32) && defined(__MINGW32__) && __USE_MINGW_ANSI_STDIO

#include <stdio.h>
#include <string.h>

static const struct {
  const char *name;
  void *addr;
} mir_mingw_ansi_stdio_map[] = {
  {"printf", (void *) __mingw_printf},     {"vprintf", (void *) __mingw_vprintf},
  {"fprintf", (void *) __mingw_fprintf},   {"vfprintf", (void *) __mingw_vfprintf},
  {"sprintf", (void *) __mingw_sprintf},   {"vsprintf", (void *) __mingw_vsprintf},
  {"snprintf", (void *) __mingw_snprintf}, {"vsnprintf", (void *) __mingw_vsnprintf},
  {"scanf", (void *) __mingw_scanf},       {"vscanf", (void *) __mingw_vscanf},
  {"fscanf", (void *) __mingw_fscanf},     {"vfscanf", (void *) __mingw_vfscanf},
  {"sscanf", (void *) __mingw_sscanf},     {"vsscanf", (void *) __mingw_vsscanf},
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
