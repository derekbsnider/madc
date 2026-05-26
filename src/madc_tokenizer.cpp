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

	// Contextual keyword remapping: CLASS/MAP/SET/VECTOR/LIST are
	// keywords only in specific syntactic positions.  Peek at the next
	// token to decide; otherwise remap to GT_IDENT so Gecko treats
	// them as plain identifiers (variable names, struct members, etc.).
	// This is the Cfront approach: solve it at the token level, not
	// in the grammar.
	if (code == 274 /* GT_CLASS */) {
	    // CLASS is a keyword only when followed by IDENT (class Foo)
	    // or '{' (anonymous class).  Otherwise it's an identifier.
	    int next = -1;
	    for (size_t i = state->pos; i < state->tokens->size(); i++) {
		next = madc_token_to_gecko((*state->tokens)[i]);
		if (next >= 0) break;
	    }
	    if (next != 256 /* GT_IDENT */ && next != '{')
		code = 256;  // remap to GT_IDENT
	}
	if (code == 331 /* GT_MAP */ || code == 332 /* GT_SET */ ||
	    code == 330 /* GT_VECTOR */ || code == 333 /* GT_LIST */) {
	    // Container keywords only when followed by '<'.
	    int next = -1;
	    for (size_t i = state->pos; i < state->tokens->size(); i++) {
		next = madc_token_to_gecko((*state->tokens)[i]);
		if (next >= 0) break;
	    }
	    if (next != '<')
		code = 256;  // remap to GT_IDENT
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
// Lambda preprocessing — extract lambdas from the token deque into
// hoisted free functions before Gecko sees them.  Gecko's ANSI C
// grammar can't parse C++ lambda syntax, so we rewrite:
//
//   [](string name) { body }   →   void __lambda_0(void *name) { body }
//   [&](int n) { body }        →   void __lambda_1(int n) { body }
//   [int](int a, int b) { ... }→   int  __lambda_2(int a, int b) { ... }
//
// The lambda tokens are replaced with a single identifier token for
// the generated function name.  The function definition tokens are
// prepended to the deque so Gecko parses them as top-level decls.
// -----------------------------------------------------------------------

static bool is_whitespace_token(TokenBase *tb) {
    TokenType tt = tb->type();
    return tt == TokenType::ttSpace || tt == TokenType::ttTab ||
           tt == TokenType::ttEOL  || tt == TokenType::ttComment;
}

// Find the next non-whitespace token index starting from pos.
// Returns tokens->size() if none found.
static size_t skip_ws(std::deque<TokenBase *> *tokens, size_t pos) {
    while (pos < tokens->size() && is_whitespace_token((*tokens)[pos]))
	pos++;
    return pos;
}

// Find the matching closing bracket/brace, respecting nesting.
// open_char/close_char are the token characters (e.g. '('/')' or '{'/'}').
// Returns the index of the closing token, or tokens->size() if not found.
static size_t find_matching(std::deque<TokenBase *> *tokens, size_t start,
                            int open_id, int close_id) {
    int depth = 1;
    for (size_t i = start; i < tokens->size(); i++) {
	if (is_whitespace_token((*tokens)[i])) continue;
	TokenID tid = (*tokens)[i]->id();
	if (tid == (TokenID)open_id) depth++;
	else if (tid == (TokenID)close_id) {
	    depth--;
	    if (depth == 0) return i;
	}
    }
    return tokens->size();
}

static void preprocess_lambdas(std::deque<TokenBase *> *tokens)
{
    static int lambda_counter = 0;

    // Collect hoisted function definitions (token sequences to prepend)
    std::vector<std::vector<TokenBase *>> hoisted;

    for (size_t i = 0; i < tokens->size(); i++) {
	if (is_whitespace_token((*tokens)[i])) continue;
	if ((*tokens)[i]->id() != TokenID::tkOpSqr) continue;

	// Check if this '[' is a lambda start (not array subscript).
	// Array subscript: preceded by identifier, ')', or ']'
	// Lambda: preceded by ',', '(', '=', ';', '{', or start of tokens
	bool is_lambda = true;
	size_t prev = i;
	if (prev > 0) {
	    prev--;
	    while (prev > 0 && is_whitespace_token((*tokens)[prev]))
		prev--;
	    if (!is_whitespace_token((*tokens)[prev])) {
		TokenID pid = (*tokens)[prev]->id();
		TokenType ptt = (*tokens)[prev]->type();
		// If preceded by ident, ), ], integer, or real — it's subscript
		if (ptt == TokenType::ttIdentifier || ptt == TokenType::ttInteger ||
		    ptt == TokenType::ttReal ||
		    pid == TokenID::tkClBrk || pid == TokenID::tkClSqr)
		    is_lambda = false;
	    }
	}
	if (!is_lambda) continue;

	size_t lambda_start = i;  // position of '['
	size_t pos = skip_ws(tokens, i + 1);
	if (pos >= tokens->size()) continue;

	// Parse capture/return-type inside brackets
	std::string return_type = "void";

	if ((*tokens)[pos]->id() == TokenID::tkBand) {
	    // [&] — capture by reference
	    pos = skip_ws(tokens, pos + 1);
	} else if ((*tokens)[pos]->id() != TokenID::tkClSqr) {
	    // [type] — return type
	    TokenBase *rt = (*tokens)[pos];
	    if (rt->type() == TokenType::ttDataType ||
		rt->type() == TokenType::ttIdentifier) {
		TokenIdent *ti = dynamic_cast<TokenIdent *>(rt);
		if (ti) return_type = ti->str;
		pos = skip_ws(tokens, pos + 1);
	    }
	}

	if (pos >= tokens->size() || (*tokens)[pos]->id() != TokenID::tkClSqr)
	    continue;
	size_t close_sq = pos;

	// Expect '(' after ']'
	pos = skip_ws(tokens, close_sq + 1);
	if (pos >= tokens->size() || (*tokens)[pos]->id() != TokenID::tkOpBrk)
	    continue;
	size_t open_paren = pos;

	// Find matching ')'
	size_t close_paren = find_matching(tokens, open_paren + 1,
					   (int)TokenID::tkOpBrk,
					   (int)TokenID::tkClBrk);
	if (close_paren >= tokens->size()) continue;

	// Expect '{' after ')'
	pos = skip_ws(tokens, close_paren + 1);
	if (pos >= tokens->size() || (*tokens)[pos]->id() != TokenID::tkOpBrc)
	    continue;
	size_t open_brace = pos;

	// Find matching '}'
	size_t close_brace = find_matching(tokens, open_brace + 1,
					   (int)TokenID::tkOpBrc,
					   (int)TokenID::tkClBrc);
	if (close_brace >= tokens->size()) continue;

	// We have a valid lambda: [lambda_start .. close_brace]
	std::string lambda_name = "__lambda_" + std::to_string(lambda_counter++);
	TokenBase *ref_tok = (*tokens)[lambda_start];

	DBG(std::cout << "preprocess_lambdas: found lambda at line "
	    << ref_tok->line << ", extracting as " << lambda_name << std::endl);

	// Build the hoisted function definition tokens.
	// return_type __lambda_N ( params ) { body }
	std::vector<TokenBase *> func_toks;

	// Return type
	TokenIdent *ret_tok = new TokenIdent(return_type);
	ret_tok->line = ref_tok->line;
	ret_tok->file = ref_tok->file;
	func_toks.push_back(ret_tok);
	func_toks.push_back(new TokenSpace());

	// Function name
	TokenIdent *name_tok = new TokenIdent(lambda_name);
	name_tok->line = ref_tok->line;
	name_tok->file = ref_tok->file;
	func_toks.push_back(name_tok);

	// ( params ) — copy from open_paren to close_paren inclusive
	for (size_t j = open_paren; j <= close_paren; j++)
	    func_toks.push_back((*tokens)[j]->clone());

	// newline before body
	func_toks.push_back(new TokenEOL());

	// { body } — copy from open_brace to close_brace inclusive
	for (size_t j = open_brace; j <= close_brace; j++)
	    func_toks.push_back((*tokens)[j]->clone());

	// newline after body
	func_toks.push_back(new TokenEOL());

	hoisted.push_back(func_toks);

	// Replace the lambda span [lambda_start .. close_brace] with
	// a single identifier token
	TokenIdent *replacement = new TokenIdent(lambda_name);
	replacement->line = ref_tok->line;
	replacement->file = ref_tok->file;

	// Erase the lambda tokens and insert the replacement
	size_t span = close_brace - lambda_start + 1;
	tokens->erase(tokens->begin() + lambda_start,
		      tokens->begin() + lambda_start + span);
	tokens->insert(tokens->begin() + lambda_start, replacement);

	// Continue scanning from current position (the replacement token)
	// i stays at lambda_start, loop will increment
    }

    // Prepend hoisted function definitions at the beginning of the deque
    // (before main/other functions). Find a good insertion point — after
    // any #include / using / extern lines but before the first function.
    // Simplest: insert at the front.
    if (!hoisted.empty()) {
	size_t insert_pos = 0;
	for (auto it = hoisted.rbegin(); it != hoisted.rend(); ++it) {
	    tokens->insert(tokens->begin() + insert_pos,
			   it->begin(), it->end());
	}
    }
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

    // Extract lambdas before Gecko sees them
    preprocess_lambdas(tokens);

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
