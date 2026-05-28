/* madc_cir.h — CIR (C Internal Representation) translation layer.
 *
 * Translates madc's TokenBase AST into c2mir node_t trees that can be
 * fed directly to c2mir's type checker and MIR generator, bypassing
 * c2mir's own preprocessor and parser.
 *
 * This is the bridge between madc's parser (parser.cpp) and the MIR
 * backend. C++ and madc constructs are lowered to C11-shaped node_t
 * trees during translation.
 */

#ifndef __MADC_CIR_H
#define __MADC_CIR_H 1

#include "mir.h"

// Forward declarations (c2mir types)
struct c2m_ctx;
typedef struct c2m_ctx *c2m_ctx_t;
struct node;
typedef struct node *node_t;

// Forward declarations (madc types)
class TokenBase;
class TokenCpnd;
class TokenFunc;
class Program;

// Initialize a c2mir context for CIR translation.
// Call AFTER c2mir_init(mir_ctx).
c2m_ctx_t cir_init(MIR_context_t mir_ctx);

// Translate a parsed madc Program into a c2mir node_t tree (N_MODULE).
// The Program must have been tokenized and parsed (prog->parse(tp) called).
node_t cir_translate(c2m_ctx_t c2m, Program *prog);

// Type-check the translated tree and generate MIR.
// Returns 1 on success, 0 on error.
int cir_compile(MIR_context_t mir_ctx, c2m_ctx_t c2m, node_t tree,
		const char *module_name);

// Clean up the c2mir context.
void cir_finish(c2m_ctx_t c2m);

#endif // __MADC_CIR_H
