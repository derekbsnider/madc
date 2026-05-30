/* cir_emit_c.h — render a cir_node tree (MC11-IR) to C source.
 *
 * The cir_node tree IS-A c2mir node_t, so we walk it with the same
 * operand API as cir_dump_node() and emit C syntax. See
 * docs/rules/mc11-ir.md and docs/superpowers/specs/2026-05-29-cir-fidelity-test-suite-design.md.
 */

#ifndef __CIR_EMIT_C_H
#define __CIR_EMIT_C_H 1

extern "C" {
#include "c2mir/c2mir_node.h"
}
#include <cstdio>

// Output language target. Shares meaning with --std=/--emit=:
// celC11 strips madc metadata; celMC11 adds `madc`-namespaced pragmas.
enum CirEmitLang { celC11 = 0, celMC11 = 1 };

// Render a cir_node tree (which IS-A c2mir node_t) to C source on `f`.
void cir_emit_c(FILE *f, node_t tree, CirEmitLang lang);

#endif // __CIR_EMIT_C_H
