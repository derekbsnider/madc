/* c2mir_node.h — struct node definition for external use.
 *
 * This file was added to the MIR fork as part of the MadC project.
 * Copyright (C) 2019-2026 Derek Snider <derekbsnider@gmail.com>.
 * Same license as the MIR project (see LICENSE).
 *
 * Extracted from c2mir.c so that external code (like madc's cir_node)
 * can extend struct node via composition/inheritance while keeping
 * binary compatibility with c2mir's internal node representation.
 */

#ifndef C2MIR_NODE_H
#define C2MIR_NODE_H

#include <stdint.h>
#include <stddef.h>
#include "mir-dlist.h"
#include "c2mir_node_code.h"

#ifdef __cplusplus
extern "C" {
#endif

/* String type matching c2mir.c's internal str_t */
typedef struct {
  const char *s;
  size_t len;
} c2mir_str_t;

/* Forward declare node for DLIST */
typedef struct node *node_t;

/* DLIST infrastructure for node_t.
   These must match the DEF_DLIST_LINK/DEF_DLIST_TYPE in c2mir.c */
DEF_DLIST_LINK (node_t);
DEF_DLIST_TYPE (node_t);

/* Platform types matching c2mir.c's internal types (x86_64).
   Must match the typedefs in c2mir/x86_64/cx86_64.h exactly — INCLUDING
   the LLP64 arm: on _WIN32 mir_long is int32_t, so an unconditional
   64-bit c2mir_long here would make madc write 8-byte u.l values that
   c2mir reads back as 4-byte mir_long (the win64 literal-truncation
   class). 64-bit constants therefore ride N_LL/u.ll, never N_L/u.l. */
#ifndef C2MIR_NODE_TYPES_DEFINED
#define C2MIR_NODE_TYPES_DEFINED
typedef signed char   c2mir_schar;
typedef c2mir_schar   c2mir_char;
#ifdef _WIN32
typedef int32_t       c2mir_long;
typedef uint32_t      c2mir_ulong;
#else
typedef int64_t       c2mir_long;
typedef uint64_t      c2mir_ulong;
#endif
typedef int64_t       c2mir_llong;
typedef uint64_t      c2mir_ullong;
typedef float         c2mir_float;
typedef double        c2mir_double;
typedef long double   c2mir_ldouble;
#endif

/* The node structure — must match c2mir.c's struct node exactly.
   Field order and types are ABI-critical. */
struct node {
  node_code_t code;
  unsigned uid;
  void *attr;
  DLIST_LINK (node_t) op_link;
  union {
    c2mir_str_t s;
    c2mir_char ch;
    c2mir_long l;
    c2mir_llong ll;
    c2mir_ulong ul;
    c2mir_ullong ull;
    c2mir_float f;
    c2mir_double d;
    c2mir_ldouble ld;
    DLIST (node_t) ops;
  } u;
};

#ifdef __cplusplus
}
#endif

#endif /* C2MIR_NODE_H */
