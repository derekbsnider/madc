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

#include <cstdio>
#include <map>
#include <string>
#include "mir.h"
#include "cir_emit_c.h"   // CirEmitLang

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
class CirBuilder;

// A compiled-and-linked CIR->c2mir->MIR module held alive for repeated
// in-process calls — the engine behind libmadc's program::exec / call /
// eval surface (madc_cir_execute is the same machinery one-shot).
// Lifecycle: build() once per compiled Program (translate + cir_compile +
// MIR_load_module + MIR_link), then function_code()/run_main() any number
// of times; the destructor tears down gen/c2mir/MIR and the node-arena-
// owning CirBuilder in the proven order. A RECOMPILE means a fresh session
// (the module references the old Program's tree).
class CirJitSession
{
public:
    CirJitSession();
    ~CirJitSession();

    // Translate `prog` and link the module. False on translate/compile
    // failure (diagnostics to stderr, mirroring madc_cir_execute). The dump
    // flags reproduce madc_cir_execute's CLI behavior; dump_checked stops
    // after the post-check dump (sets *dump_stop, returns false, no error).
    bool build(Program *prog, const char *source_name,
	       bool dump_tree = false, bool dump_nodes = false,
	       bool dump_checked = false, bool *dump_stop = 0);
    bool built() const { return mod != 0; }

    // The generated code address for a module function by its EMITTED name
    // (plain madc functions emit under their source name; the eval entry is
    // "__madc_eval"). Generates on first use, memoized. NULL when absent.
    void *function_code(const char *emitted_name);

    // Generate and run main(argc, argv); returns main's return value.
    // `ok` (when non-null) reports whether main was found and invoked.
    int run_main(int argc, char **argv, bool *ok = 0);

private:
    MIR_context_t ctx;
    c2m_ctx_t c2m;
    CirBuilder *builder;
    MIR_module_t mod;
    std::map<std::string, void *> gen_cache;
    void teardown();
    CirJitSession(const CirJitSession &);
    CirJitSession &operator=(const CirJitSession &);
};

// Initialize a c2mir context for CIR translation.
// Call AFTER c2mir_init(mir_ctx).
// debug_p (post-check dump): when true, c2mir prints the tree AFTER do_context
// to message_file — same stage as `c2m -d`, for stage-matched diffing.
c2m_ctx_t cir_init(MIR_context_t mir_ctx, bool debug_p = false);

// (The legacy cir_translate() entry was removed — CirBuilder::translate_module
// in cir_builder.cpp is the sole tree builder. See madc_cir_execute.)

// Type-check the translated tree and generate MIR.
// Returns 1 on success, 0 on error.
int cir_compile(MIR_context_t mir_ctx, c2m_ctx_t c2m, node_t tree,
		const char *module_name);

// Clean up the c2mir context.
void cir_finish(c2m_ctx_t c2m);

// Full CIR pipeline: tokenize+parse → CIR translate → c2mir compile → JIT execute.
// Returns the exit code from main(), or -1 on failure.
// If dump_tree is true, dumps the c2mir-format tree (c2mir_dump_tree, for
// c2m -d comparison). If dump_nodes is true, dumps our cir_node tree via
// the madc-owned walker (cir_dump_nodes, showing +madc fields).
// dump_checked: dump the POST-check tree (after c2mir's do_context) to stderr,
// matching `c2m -d`'s stage so the forward-decl folding lines up for diffing.
int madc_cir_execute(Program *prog, const char *source_name,
		     int user_argc, char **user_argv,
		     bool dump_tree = false, bool dump_nodes = false,
		     bool dump_checked = false);

// Build the cir_node tree and render it as C source (no compile/run).
// Backs --emit=c11|mc11. Returns 0 on success, -1 on build failure.
int madc_cir_emit(Program *prog, const char *source_name, FILE *out,
		  CirEmitLang lang);

#endif // __MADC_CIR_H
