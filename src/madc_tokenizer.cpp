// madc_tokenizer.cpp — Adapter between madc's lexer and Gecko's parser.
//
// The lexer produces a deque of TokenBase*. This adapter walks that
// deque via a read_token() callback that Gecko calls repeatedly
// until it returns -1 (EOF).
//
// Each token's original TokenBase* pointer is stored as the Gecko
// attribute so the semantic walk (Phase 2) can recover source text,
// location, and type information from the original token.

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <map>
#include <list>
#include <vector>
#include <deque>
#include <queue>
#include <stack>
#include <stdint.h>
#include <asmjit/x86.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"

extern "C" {
#include "gecko.h"
}

// From madc_grammar.cpp
extern struct grammar *madc_create_gecko_grammar();
extern int madc_token_to_gecko(TokenBase *tb);

// -----------------------------------------------------------------------
// GeckoTokenizer — holds state for the read_token callback
// -----------------------------------------------------------------------

struct GeckoTokenizer {
    std::deque<TokenBase *> *tokens;
    size_t pos;

    GeckoTokenizer(std::deque<TokenBase *> *toks) : tokens(toks), pos(0) {}
};

// Gecko read_token callback.  Returns the terminal code for the next
// token, or -1 at EOF.  The attribute is the TokenBase* pointer cast
// to void* — the semantic walk recovers it to access source text,
// location, type, and value.
static int gecko_read_token(void **attr, GeckoTokenizer *state)
{
    while (state->pos < state->tokens->size()) {
	TokenBase *tb = (*state->tokens)[state->pos];
	state->pos++;

	int code = madc_token_to_gecko(tb);
	if (code < 0)
	    continue;  // skip whitespace, comments

	// Collapse std::string → STRING_T
	if (code == 256 /* GT_IDENT */) {
	    TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
	    if (ti && ti->str == "std") {
		// Peek for :: string (SCOPE STRING_T)
		size_t saved = state->pos;
		int c1 = -1;
		size_t i = saved;
		for (; i < state->tokens->size(); i++) {
		    c1 = madc_token_to_gecko((*state->tokens)[i]);
		    if (c1 >= 0) { i++; break; }
		}
		int c2 = -1;
		size_t j = i;
		for (; j < state->tokens->size(); j++) {
		    c2 = madc_token_to_gecko((*state->tokens)[j]);
		    if (c2 >= 0) { j++; break; }
		}
		if (c1 == 359 /* GT_SCOPE */ && c2 == 318 /* GT_STRING_T */) {
		    state->pos = j;  // skip past :: string
		    *attr = (void *)tb;
		    return 318;  // GT_STRING_T
		}
		// else fall through as normal IDENT
	    }
	}

	// Skip __attribute__((...)) — consume the entire construct
	if (code == 256 /* GT_IDENT */) {
	    TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
	    if (ti && (ti->str == "__attribute__" || ti->str == "__attribute" ||
		       ti->str == "__extension__")) {
		// Find opening '(' and count parens to find matching close
		int depth = 0;
		bool found_open = false;
		for (size_t i = state->pos; i < state->tokens->size(); i++) {
		    int c = madc_token_to_gecko((*state->tokens)[i]);
		    if (c < 0) continue;
		    if (c == '(') { depth++; found_open = true; }
		    else if (c == ')') { depth--; if (found_open && depth == 0) { state->pos = i + 1; break; } }
		    else if (!found_open) break;  // no parens after attribute — just skip the keyword
		}
		continue;  // skip the entire attribute
	    }
	}

	// Synthesize ELLIPSIS from three consecutive '.' tokens
	if (code == '.') {
	    size_t saved = state->pos;
	    int dots = 1;
	    for (size_t i = saved; i < state->tokens->size() && dots < 3; i++) {
		int c = madc_token_to_gecko((*state->tokens)[i]);
		if (c < 0) continue;  // skip whitespace
		if (c == '.') { dots++; state->pos = i + 1; }
		else break;
	    }
	    if (dots == 3) {
		*attr = (void *)tb;
		return 372;  // GT_ELLIPSIS
	    }
	    state->pos = saved;  // not three dots, revert
	}

	*attr = (void *)tb;
	return code;
    }
    // EOF
    *attr = nullptr;
    return -1;
}

// C-compatible wrapper matching Gecko's int (*)(void **) signature.
// The GeckoTokenizer* is passed through gp_parse's arg parameter,
// but Gecko's read_token doesn't receive arg — we use a thread-local.
static thread_local GeckoTokenizer *g_current_tokenizer = nullptr;

static int gecko_read_token_callback(void **attr)
{
    if (!g_current_tokenizer) return -1;
    return gecko_read_token(attr, g_current_tokenizer);
}

// Syntax error callback — reports file:line from the token attribute.
static void gecko_syntax_error(const char *err_nonterm, bool after_p,
    const char *err_tok_repr, void *err_tok_attr,
    const char *stop_tok_repr, void *stop_tok_attr)
{
    TokenBase *tb = (TokenBase *)err_tok_attr;
    const char *file = tb ? tb->file : "<unknown>";
    int line = tb ? tb->line : 0;
    int col = tb ? tb->column : 0;

    fprintf(stderr, "%s:%d:%d: syntax error %s %s",
	    file ? file : "<unknown>", line, col,
	    after_p ? "after" : "in", err_nonterm);
    if (err_tok_repr)
	fprintf(stderr, " on token '%s'", err_tok_repr);
    if (stop_tok_repr) {
	TokenBase *stop = (TokenBase *)stop_tok_attr;
	int sline = stop ? stop->line : 0;
	fprintf(stderr, ", recovery at '%s' (line %d)", stop_tok_repr, sline);
    }
    fprintf(stderr, "\n");
}

// -----------------------------------------------------------------------
// madc_gecko_parse — parse a token stream through Gecko.
//
// Takes the lexer's token deque, feeds it to Gecko via the adapter,
// and returns the AST root.  Returns nullptr on parse failure.
//
// The grammar is created once and cached (thread-local for safety).
// -----------------------------------------------------------------------

static thread_local struct grammar *g_grammar = nullptr;

struct gp_tree_node *madc_gecko_parse(std::deque<TokenBase *> *tokens,
				      int *out_ambiguity)
{
    // Create grammar on first use
    if (!g_grammar) {
	g_grammar = madc_create_gecko_grammar();
	if (!g_grammar) {
	    fprintf(stderr, "madc_gecko_parse: failed to create grammar\n");
	    return nullptr;
	}
    }

    // Set up tokenizer state
    GeckoTokenizer state(tokens);
    g_current_tokenizer = &state;

    // Configure error reporting
    gp_set_syntax_error(g_grammar, gecko_syntax_error);

    // Parse
    struct gp_tree_node *root = nullptr;
    int ambiguity = 0;
    int err = gp_parse(g_grammar, gecko_read_token_callback,
		       &root, &ambiguity, nullptr);

    g_current_tokenizer = nullptr;

    if (out_ambiguity)
	*out_ambiguity = ambiguity;

    if (err) {
	fprintf(stderr, "madc_gecko_parse: parse failed (%s)\n",
		gp_error_message(g_grammar));
	return nullptr;
    }

    return root;
}

// Free a Gecko parse tree.
void madc_gecko_free_tree(struct gp_tree_node *root)
{
    if (g_grammar && root)
	gp_free_tree(g_grammar, root);
}
