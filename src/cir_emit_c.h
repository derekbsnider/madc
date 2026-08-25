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
#include <cstring>
#include <string>
#include <utility>
#include <vector>

// Output language target. Shares meaning with --std=/--emit=:
// celC11 strips madc metadata; celMC11 adds `madc`-namespaced pragmas;
// celCxx reverse-renders the TU's retained source (mc11-ir.md — the
// attached tokens are the path back to the original source).
enum CirEmitLang { celC11 = 0, celMC11 = 1, celCxx = 2 };

// The ONE emit-target-name -> enum conversion — the CLI's --emit= parse
// and the madc::emit view query both ride it, so a new render target
// (madc, ...) lands in exactly one place. False = unknown target.
inline bool cir_emit_lang_of(const char *name, CirEmitLang &out)
{
    if ( name && strcmp(name, "c11") == 0 )
	out = celC11;
    else if ( name && strcmp(name, "mc11") == 0 )
	out = celMC11;
    else if ( name && strcmp(name, "c++") == 0 )
	out = celCxx;
    else
	return false;
    return true;
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

// Render a cir_node tree (which IS-A c2mir node_t) to C source on `f`.
void cir_emit_c(FILE *f, node_t tree, CirEmitLang lang);

#endif // __CIR_EMIT_C_H
