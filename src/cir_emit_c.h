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
#include "madc_file_kinds.h"	// madc::file_kind_of — the ONE target-name converter (C++ linkage)
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// Output language target. Shares meaning with --std=/--emit=:
// celC11 strips madc metadata; celMC11 adds `madc`-namespaced pragmas;
// celCxx reverse-renders the TU's retained source (mc11-ir.md — the
// attached tokens are the path back to the original source).
enum CirEmitLang { celC11 = 0, celMC11 = 1, celCxx = 2 };

// The emitter's DEPTH-OF-SUPPORT table (client-server design §2.1: depth of
// support is a table per layer owned by that layer, never a flag on the
// enumerator): which file kinds (<bits/file_kinds>) this emitter renders,
// and as what. The ONE kind -> CirEmitLang conversion — the CLI's --emit=
// parse and the madc::emit view query both ride it, so a new render target
// (madc, ...) lands in exactly one place. The C++ target is the FAMILY
// (fkCPP): the retained-source echo renders any standard. False = the
// emitter has no rendering for that kind.
inline bool cir_emit_lang_of_kind(int64_t kind, CirEmitLang &out)
{
    switch ( kind )
    {
	case madc::fkC11:  out = celC11;  return true;
	case madc::fkMC11: out = celMC11; return true;
	case madc::fkCPP:  out = celCxx;  return true;
    }
    return false;
}

// The name form: a target's spelling converts ONCE through the file-kind
// vocabulary's converter (src/file_kinds.cpp — "c11" is the --std= table's
// row, "mc11" / "c++" the vocabulary's own) and joins the table above; no
// render target is spelled here. False = unknown word or unrendered kind.
inline bool cir_emit_lang_of(const char *name, CirEmitLang &out)
{
    return cir_emit_lang_of_kind(madc::file_kind_of(name), out);
}

// The target list for "unknown target" messages — grows with the enum.
#define CIR_EMIT_TARGETS "c11|mc11|c++"

// What the C++ reverse-render reads, passed AS DATA so the renderer keeps
// no Program dependency: the TU's lex-order token stream (with trivia —
// requires Program::keep_trivia at tokenize time), the TU's file spelling
// as its tokens carry it, and the recorded include directives
// (writer file, directive-as-written).
class TokenBase;
class TokenStream;
struct CirEmitSource {
	const TokenStream *tokens;
	const char *tu_file;
	const std::vector<std::pair<std::string, std::string> > *includes;
	const std::string *trailing;	// whitespace/comments after the last token
	CirEmitSource() : tokens(NULL), tu_file(NULL), includes(NULL),
			  trailing(NULL) {}
};

// Render the TU's retained source (--emit=c++): the TU's own include
// directives, then every TU-file token echoed in stream order. The caller
// runs the tree validity gate first — never render an erroneous tree. See
// the implementation comment for the full contract.
void cir_emit_cxx(FILE *f, const CirEmitSource &src);

// One correlation row (V5 source↔MC11 map): the display byte offset where a
// statement/declaration's emitted text begins, paired with its SOURCE line.
// The buffer-owning layer turns the line into a stored byte offset and builds
// the monotone doc_map (madcdis/doc_lens.h) — a reordered/hoisted or synthetic
// row that breaks monotonicity "maps to nothing" (design §2.6). Statement/line
// granularity today; column/expression precision is the named later refinement.
struct CirEmitMapRow { size_t disp; int line; };

// Render a cir_node tree (which IS-A c2mir node_t) to C source on `f`. When
// `map` is non-null, append one CirEmitMapRow per emitted statement/declaration
// that carries a source origin — a pure side channel: the emitted BYTES are
// identical whether or not a map is collected.
void cir_emit_c(FILE *f, node_t tree, CirEmitLang lang,
		std::vector<CirEmitMapRow> *map = nullptr);

#endif // __CIR_EMIT_C_H
