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
#endif

static void *MIR_int128_helper_resolver (const char *name) {
#if defined(__SIZEOF_INT128__)
  if (strcmp (name, "__divti3") == 0) return (void *) MIR_int128_divti3;
  if (strcmp (name, "__udivti3") == 0) return (void *) MIR_int128_udivti3;
  if (strcmp (name, "__modti3") == 0) return (void *) MIR_int128_modti3;
  if (strcmp (name, "__umodti3") == 0) return (void *) MIR_int128_umodti3;
#else
  (void) name;
#endif
  return NULL;
}

#endif
