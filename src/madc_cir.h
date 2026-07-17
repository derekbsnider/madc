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
class CirFrozenForest;

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

    // Thaw a frozen forest container image and link its module — the
    // cross-process twin of build(): no Program, no parse; the tree, its
    // string closure, and its link environment come from the container.
    // The image must stay mapped for the session lifetime
    // (cir_forest_map_image). Requires a live string pool to be bound.
    bool build_frozen(const void *image, size_t image_len,
		      const char *module_name);
    bool built() const { return mod != 0; }

    // The generated code address for a module function by its EMITTED name
    // (plain madc functions emit under their source name; the eval entry is
    // "__madc_eval"). Generates on first use, memoized. NULL when absent.
    void *function_code(const char *emitted_name);

    // The linked runtime address of a module DATA/BSS item by its emitted
    // name (globals emit under their source identifier). Valid after
    // build() (module loaded + linked). NULL when absent.
    void *data_address(const char *emitted_name);

    // Generate and run main(argc, argv); returns main's return value.
    // `ok` (when non-null) reports whether main was found and invoked.
    // `out_secs` (when non-null) receives the execution wall time in seconds
    // (the main() call itself, incl. lazy MIR_gen of functions it calls).
    int run_main(int argc, char **argv, bool *ok = 0, double *out_secs = 0);

private:
    MIR_context_t ctx;
    c2m_ctx_t c2m;
    CirBuilder *builder;
    CirFrozenForest *forest;	// build_frozen(): owns the thawed node storage
    MIR_module_t mod;
    MIR_module_t cache_mod;	// build(): the container's MIR cache module,
				// loaded beside `mod` (rung 3); NULL = no cache
    std::map<std::string, void *> gen_cache;
    bool init_contexts(const char *source_name, bool dump_checked);
    bool load_and_link(const char *source_name, Program *prog);
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

// Freeze the parsed Program's module tree (PRE-check — c2mir's checker
// mutates trees it compiles) into a forest snapshot container at out_path:
// per-unit segments, string-pool/position/type-name closure, link libs,
// context-hash pin. `append` uses placement 2 (blob appended to an existing
// binary, found from its EOF footer). Backs --freeze / --freeze-append.
// `mir_cache` additionally compiles the assembled container's module and
// packs its MIR binary form as an optional cache segment
// (--freeze-mir-cache; blob failure never fails the freeze).
// Returns 0 on success, -1 on failure.
int madc_cir_freeze(Program *prog, const char *source_name,
		    const char *out_path, bool append, bool mir_cache = false);

// Thaw + compile + run a frozen forest: from the container file at
// `container_path`, or from the blob appended to the running executable
// when NULL (the /proc/self/exe placement). No parse happens — this is the
// cross-process consumer of madc_cir_freeze's output. Backs --run-frozen.
// Returns main()'s exit code, or -1 on failure.
int madc_cir_execute_frozen(const char *container_path,
			    int user_argc, char **user_argv);

// Print a container's directory + grove payload v2 surfaces (decl index,
// PP exports, edges, branch macros, canonical order) as stable
// line-oriented text — the B4a oracle data source. Backs --dump-forest.
// NULL = the blob appended to this executable. Returns 0, or -1 on failure.
int madc_cir_dump_forest(const char *container_path);

#endif // __MADC_CIR_H
