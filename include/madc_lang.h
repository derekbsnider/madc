//////////////////////////////////////////////////////////////////////////
//									//
// madc::lang — the language services over a DOCUMENT (split arc 1C.11,	//
// design docs/plans/2026-10-10-language-services-design.md).		//
//									//
// ONE set of answers for every consumer — the LSP, the MCP tools, an	//
// editor's status line, Problems and colour: diagnostics, outline,	//
// the enclosing definition, definition, references, hover and the	//
// semantic spans. Each composes the parse handle's reads		//
// (madc::parse_* / graph_*) and adds no analysis of its own: it is the	//
// presentation arithmetic (the word under a point, a name's range, a	//
// diagnostic's range, the hover text) done once.			//
//									//
// The subject is a document of a hub world; its handle comes through	//
// ONE door, document_handle (the document's version decides a		//
// re-parse; the handle is the document's). One coordinate: a point is	//
// a 1-based line and a 0-based BYTE column in that line; a range is	//
// half-open. The LSP converts bytes to its negotiated encoding at its	//
// own boundary.							//
//									//
// Thread contract (C++ standard library convention): a document's	//
// handle keeps the runtime-eval confinement — used from the thread	//
// and Program that opened it; the parse slot is written through the	//
// world; every other function is a read.				//
//									//
//////////////////////////////////////////////////////////////////////////

#ifndef __MADC_LANG_H
#define __MADC_LANG_H 1

#include <cstdint>
#include <string>
#include <vector>

#include "highlight_class.h"
#include "madc/bits/diag_enums"

namespace madc {

class value;
namespace hub { class world; typedef uint32_t entity_id; }

namespace lang {

// A place in a document: the 1-based line, the 0-based byte column.
struct point
{
    int64_t line;
    int64_t column;
};

// [from, to) in one document.
struct range
{
    point from;
    point to;
};

// What a symbol is. The outline row builder decides which definitions it
// reports; this face names each one's kind.
enum class symbol_kind : unsigned char { function };
const char *symbol_kind_name(symbol_kind k);

// A definition: its name, kind, whole extent (its lines) and the range a
// client selects — the name itself.
struct symbol
{
    std::string name;
    symbol_kind kind;
    range extent;
    range selection;
};

// One diagnostic over the document's text.
struct diagnostic
{
    range at;
    diag_severity severity;
    diag_phase phase;
    std::string message;
};

// The document's parse handle, parsed from its CURRENT text under the
// standard its kind answers: opened on first use, refreshed when the text's
// version moved since the last parse, returned unchanged otherwise. 0 = no
// such document, or a kind the compiler does not parse. `checked` non-NULL
// is the validated refresh (the old tree stays when the new text has MORE
// errors) with the candidate's diagnostic rows in *checked.
int64_t document_handle(hub::world &w, hub::entity_id e,
			value *checked = 0);

// Each answer below parses through document_handle first; false = no
// handle (and an empty answer).
bool diagnostics(hub::world &w, hub::entity_id e,
		 std::vector<diagnostic> &out);
bool outline(hub::world &w, hub::entity_id e, std::vector<symbol> &out);
// The innermost definition containing `at`; false when none does.
bool enclosing(hub::world &w, hub::entity_id e, point at, symbol &out);
bool spans(hub::world &w, hub::entity_id e,
	   std::vector<highlight_span> &out);

// The word-addressed answers: the identifier at `at` (the buffer's word
// rule; a point just past a word names it, where a caret rests). A point in
// no word, or a word the TU does not define, answers nothing — an answer,
// not an error.
// Where the word is defined: the definition's NAME (as clangd answers). A
// definition the graph knows without a source span (a non-function type)
// has no place to answer with.
bool definition(hub::world &w, hub::entity_id e, point at,
		 std::vector<range> &out);
// Every use of the definition the word names: each range is the NAME at
// the use (a call's site anchors at its argument list, so the name is the
// last occurrence at or before the site's column — the inner one in
// `twice(twice(1))`); the definition's name last when asked.
bool references(hub::world &w, hub::entity_id e, point at,
		bool include_declaration, std::vector<range> &out);

// What a point is: `KIND NAME — defined on line N` over the word, else the
// enclosing definition `in NAME` (no range). False = nothing to say.
struct hover_info
{
    std::string text;
    bool has_range;
    range at;
};
bool hover(hub::world &w, hub::entity_id e, point at, hover_info &out);

} // namespace lang
} // namespace madc

#endif // __MADC_LANG_H
