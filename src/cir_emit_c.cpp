/* cir_emit_c.cpp — render a cir_node tree (MC11-IR) to C source.
 *
 * ============================ READ THIS FIRST ============================
 * THIS IS **NOT** THE c2mir / JIT PATH. It does NOT feed the backend.
 *
 * The live pipeline is:  madc parser -> cir_node tree (MC11-IR) -> c2mir
 * via c2mir_compile_tree()  (the *in-memory tree*, NOT text) -> MIR_gen.
 * See madc_cir.cpp (cir_compile / madc_cir_execute, the MIR_gen call).
 *
 * This file is a SEPARATE, OPTIONAL CONSUMER of that same tree: the
 * `--emit=c11` text RENDERER. Its outputs are (a) portable C for any C
 * toolchain and (b) the AOT / `--exe` path (emit C -> gcc/clang -> native).
 * It is also the cir-fidelity gate's reference. c2mir NEVER sees this text.
 *
 * Consequence for debugging: a bug reproduced ONLY through this renderer
 * (e.g. compiling the emitted C with gcc) tells you about the RENDER, not
 * about what c2mir/MIR actually receive. If the renderer and the live tree
 * disagree, THAT is the bug. To inspect the real backend input, dump the
 * tree (--dump-cir / --dump-cir-checked) or the MIR, never the emitted C.
 * =========================================================================
 *
 * Mirrors the structure of cir_dump_node() (cir_builder.cpp) but emits
 * compilable C instead of the debug format. Operand layouts are those
 * produced by CirBuilder (verified against cir_builder.cpp):
 *   N_MODULE   [0]=N_LIST of top-level decls
 *   N_FUNC_DEF [0]=ret specs(N_LIST) [1]=declarator(N_DECL) [2]=K&R(N_LIST) [3]=body(N_BLOCK)
 *   N_DECL     [0]=id(N_ID|N_IGNORE) [1]=suffix-list(N_LIST: N_FUNC / pointer / N_ARR)
 *   N_FUNC     [0]=param-list(N_LIST of N_TYPE|N_SPEC_DECL)
 *   N_TYPE     [0]=specs(N_LIST) [1]=declarator(N_DECL)
 *   N_BLOCK    [0]=scope-list [1]=items(N_LIST of statements)
 *   N_SPEC_DECL[0]=specs(N_LIST) [1]=declarator(N_DECL) [2]=initializer
 *
 * Unhandled node kinds emit a visible marker comment naming the node
 * code, so the fidelity gate localizes exactly which construct is missing.
 */

#include "cir_emit_c.h"
#include "cir_node.h"
#include "madc_posix_io.h"	// string-capture stream (the one FILE*-over-memory owner)
#include <set>
#include <string>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <climits>
#include <cmath>	// std::signbit (the prefix-sign adjacency rule)
#include <cstdarg>

// The C++ reverse-render (cir_emit_cxx, end of file) reads the retained
// tokens — the front-end types come in for that consumer only (the same
// order every front-end TU uses: datadef, tokens, datatokens, madc).
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

extern "C" {
#include "c2mir/c2mir_api.h"   // c2mir_node_op, c2mir_node_code_name
}

namespace {

inline node_t op(node_t n, int i) { return c2mir_node_op(n, i); }

// ---- the writer -----------------------------------------------------------
// ONE owner of the emitted text's LAYOUT (owner ruling 2026-09-09: every
// emitted code view is auto-indented). `depth` is the block depth; the first
// non-newline byte written on a line is preceded by `depth` tabs. The node
// cases below spell only tokens and line breaks — the indentation follows
// from the TREE's nesting, never from a re-indenting pass over the text — so
// `--emit=c11|mc11` on the CLI and the IDE's view lenses (madc::emit) show
// the same bytes.
struct CEmit
{
	FILE *f;
	CirEmitLang lang;
	int depth;
	bool bol;	// at the beginning of a line: the next byte indents
	size_t written;	// running display byte count (== the next byte's offset)
	std::vector<CirEmitMapRow> *cmap;	// correlation rows, or null (no-op)

	CEmit(FILE *f_, CirEmitLang l, bool at_bol)
		: f(f_), lang(l), depth(0), bol(at_bol), written(0), cmap(NULL) {}

	void put(char c)
	{
		if (c == '\n') {
			fputc('\n', f);
			written++;
			bol = true;
			return;
		}
		if (bol) {
			for (int i = 0; i < depth; i++) { fputc('\t', f); written++; }
			bol = false;
		}
		fputc(c, f);
		written++;
	}
	void put(const char *s)
	{
		for (; s && *s; s++) put(*s);
	}
	void nl() { put('\n'); }
	void printf(const char *fmt, ...)
	{
		char buf[256];
		va_list ap;
		va_start(ap, fmt);
		int n = vsnprintf(buf, sizeof buf, fmt, ap);
		va_end(ap);
		if (n < 0) return;
		if ((size_t)n < sizeof buf) {
			put(buf);
			return;
		}
		std::string big((size_t)n + 1, '\0');
		va_start(ap, fmt);
		vsnprintf(&big[0], big.size(), fmt, ap);
		va_end(ap);
		big.resize((size_t)n);
		put(big.c_str());
	}
};

// ---- expression precedence (C11 6.5) -------------------------------------
// Higher binds tighter. An expression node is parenthesized only where its
// context binds tighter than it does (plus the -Wparentheses set below), so
// the render carries the parentheses a careful C programmer writes and no
// others — `if (a > b)`, `return a - b;`, `x = (a + b) * c;`. P_FORCE is a
// context no expression satisfies: always parenthesize.
enum {
	P_NONE = 0,	// statement position: nothing is parenthesized
	P_COMMA = 1, P_ASSIGN = 2, P_COND = 3, P_OROR = 4, P_ANDAND = 5,
	P_OR = 6, P_XOR = 7, P_AND = 8, P_EQ = 9, P_REL = 10, P_SHIFT = 11,
	P_ADD = 12, P_MUL = 13, P_UNARY = 15, P_POSTFIX = 16, P_PRIMARY = 17,
	P_FORCE = 99
};

// The node's own precedence; -1 = not an expression (statements, specifiers,
// declarators, types): never parenthesized.
int expr_prec(node_t n)
{
	switch (n->code) {
	case N_COMMA: return P_COMMA;
	case N_ASSIGN: case N_ADD_ASSIGN: case N_SUB_ASSIGN: case N_MUL_ASSIGN:
	case N_DIV_ASSIGN: case N_MOD_ASSIGN: case N_AND_ASSIGN: case N_OR_ASSIGN:
	case N_XOR_ASSIGN: case N_LSH_ASSIGN: case N_RSH_ASSIGN:
		return P_ASSIGN;
	case N_COND: return P_COND;
	case N_OROR: return P_OROR;
	case N_ANDAND: return P_ANDAND;
	case N_OR: return P_OR;
	case N_XOR: return P_XOR;
	case N_AND: return P_AND;
	case N_EQ: case N_NE: return P_EQ;
	case N_LT: case N_LE: case N_GT: case N_GE: return P_REL;
	case N_LSH: case N_RSH: return P_SHIFT;
	// c2mir's grammar spells unary minus/plus as a one-operand N_SUB/N_ADD.
	case N_ADD: case N_SUB: return op(n, 1) ? P_ADD : P_UNARY;
	case N_MUL: case N_DIV: case N_MOD: return P_MUL;
	case N_NOT: case N_BITWISE_NOT: case N_INC: case N_DEC: case N_ADDR:
	case N_DEREF: case N_CAST: case N_SIZEOF: case N_EXPR_SIZEOF:
	case N_ALIGNOF:
		return P_UNARY;
	case N_CALL: case N_IND: case N_FIELD: case N_DEREF_FIELD:
	case N_POST_INC: case N_POST_DEC: case N_COMPOUND_LITERAL:
		return P_POSTFIX;
	case N_ID: case N_STR: case N_STR16: case N_STR32: case N_STMTEXPR:
	case N_I: case N_L: case N_LL: case N_U: case N_UL: case N_ULL:
	case N_F: case N_D: case N_LD: case N_CF: case N_CD: case N_CLD:
	case N_CH: case N_CH16: case N_CH32:
		return P_PRIMARY;
	default: return -1;
	}
}

bool is_cmp(int code)
{
	return code == N_EQ || code == N_NE || code == N_LT || code == N_LE
	    || code == N_GT || code == N_GE;
}

bool is_binary_addsub(node_t n)
{
	return (n->code == N_ADD || n->code == N_SUB) && op(n, 1);
}

// gcc's -Wparentheses (c-family/c-warn.cc warn_about_parentheses) and clang's
// -Wparentheses group ask for these parentheses where precedence alone does
// not: the emitted C compiles warning-free under either canon compiler.
bool wants_parens(node_t parent, node_t child)
{
	int pc = parent->code, cc = child->code;
	switch (pc) {
	case N_LSH: case N_RSH:
		return is_binary_addsub(child);		// '+' inside '<<'
	case N_OROR:
		return cc == N_ANDAND;			// '&&' within '||'
	case N_OR:
		return cc == N_AND || cc == N_XOR || is_binary_addsub(child)
		    || is_cmp(cc);			// arithmetic/comparison in '|'
	case N_XOR:
		return cc == N_AND || is_binary_addsub(child) || is_cmp(cc);
	case N_AND:
		return is_binary_addsub(child) || is_cmp(cc);
	default:
		return is_cmp(pc) && is_cmp(cc);	// X<=Y<=Z has no mathematical meaning
	}
}

// The one lexical hazard of minimal parentheses: a prefix `-` (or `+`) whose
// operand's spelling begins with the same sign would re-lex as `--` / `++`.
// Such an operand keeps its parentheses: `-(-x)`, `-(--x)`, `-(-1)`.
bool leads_with_sign(node_t n, char sign)
{
	switch (n->code) {
	case N_SUB: return !op(n, 1) && sign == '-';
	case N_ADD: return !op(n, 1) && sign == '+';
	case N_DEC: return sign == '-';
	case N_INC: return sign == '+';
	case N_I: case N_L: return sign == '-' && n->u.l < 0;
	case N_LL: return sign == '-' && n->u.ll < 0;
	case N_F: case N_CF: return sign == '-' && std::signbit((double)n->u.f);
	case N_D: case N_CD: return sign == '-' && std::signbit(n->u.d);
	case N_LD: case N_CLD: return sign == '-' && std::signbit(n->u.ld);
	default: return false;
	}
}

bool layout_integer(node_t n, long long &value)
{
	if (!n) return false;
	switch (n->code) {
	case N_I: case N_L: value = (long long)n->u.l; return true;
	case N_LL: value = (long long)n->u.ll; return true;
	case N_U: case N_UL: value = (long long)n->u.ul; return true;
	case N_ULL:
		if (n->u.ull > (c2mir_ullong)LLONG_MAX) return false;
		value = (long long)n->u.ull;
		return true;
	default: return false;
	}
}

int aggregate_pack(node_t aggregate)
{
	node_t contract = op(aggregate, 2);
	long long version = 0, pack = 0;
	if (!contract || contract->code != N_LIST
	    || !layout_integer(op(contract, 0), version) || version != 1
	    || !layout_integer(op(contract, 3), pack) || pack <= 0
	    || pack > INT_MAX)
		return 0;
	return (int)pack;
}

int declaration_pack(node_t specs)
{
	if (!specs) return 0;
	if (specs->code == N_SHARE)
		return declaration_pack(op(specs, 0));
	if (specs->code == N_STRUCT || specs->code == N_UNION) {
		node_t members = op(specs, 1);
		return members && members->code != N_IGNORE
			? aggregate_pack(specs) : 0;
	}
	if (specs->code != N_LIST) return 0;
	for (int i = 0; ; i++) {
		node_t spec = op(specs, i);
		if (!spec) break;
		int pack = declaration_pack(spec);
		if (pack > 0) return pack;
	}
	return 0;
}

void emit_pack_push(CEmit &e, int pack)
{
	if (pack > 0) {
		e.printf("#pragma pack(push, %d)", pack);
		e.nl();
	}
}

void emit_pack_pop(CEmit &e, int pack)
{
	if (pack > 0) {
		e.nl();
		e.put("#pragma pack(pop)");
	}
}

void emit(CEmit &e, node_t n, int ctx);
void emit_initializer(CEmit &e, node_t n);

// V5 correlation: record a map row for a statement/declaration node about to
// be emitted — its display offset (e.written, the byte count so far, i.e. the
// start of this emitted line) paired with the node's SOURCE line. A synthetic
// node (origin_id 0 — a lowering artifact with no source home) is skipped, so
// it "maps to nothing" (design §2.6). No-op unless a map is being collected.
static void map_record(CEmit &e, node_t item)
{
	if (!e.cmap || !item) return;
	cir_node *cn = CIR_NODE(item);
	if (!cn->origin_id) return;
	int line = cn->src_line();
	if (line <= 0) return;
	CirEmitMapRow row;
	row.disp = e.written;
	row.line = line;
	e.cmap->push_back(row);
}

// Does this declarator render any text? A named id or a pointer/array/
// function suffix does; the empty declarator of a bare struct definition
// (`struct pt { ... };`) does not — so no separating space is owed to it.
bool declarator_present(node_t decl)
{
	if (!decl || decl->code != N_DECL) return false;
	node_t id = op(decl, 0);
	if (id && id->code != N_IGNORE) return true;
	node_t suffixes = op(decl, 1);
	return suffixes && op(suffixes, 0);
}

// The builder's LABEL CARRIER: a statement whose only purpose is to hold a
// label list — `<labels>: 0;` (cir_builder.cpp: switch arms, goto targets)
// — so c2mir sees a labelled statement. In rendered C the labels head the
// statement that FOLLOWS instead (identical meaning), so the carrier's `0;`
// never shows; a carrier that ends its block keeps a null statement (`;`) —
// C11 needs a statement after a label.
bool is_label_carrier(node_t s)
{
	if (!s || s->code != N_EXPR) return false;
	node_t labels = op(s, 0), x = op(s, 1);
	if (!labels || labels->code != N_LIST || !op(labels, 0)) return false;
	return x && (x->code == N_I || x->code == N_L) && x->u.l == 0;
}

// A C11 label may head a STATEMENT only — never a declaration (`case 1:
// int x;` is an error before C23), which is why the builder's carrier
// exists. The carrier's own statement is dropped only in front of one.
bool is_statement(node_t s)
{
	if (!s) return false;
	switch (s->code) {
	case N_EXPR: case N_IF: case N_WHILE: case N_DO: case N_FOR:
	case N_SWITCH: case N_RETURN: case N_BREAK: case N_CONTINUE:
	case N_GOTO: case N_BLOCK:
		return true;
	default:
		return false;
	}
}

// Emit an identifier with every non-C-identifier byte flattened to a
// deterministic mnemonic. Method symbols carry C++ operator spellings
// (`Cls__operator++_un`, `operator[]__o5`, `operator""s`): the JIT path
// feeds c2mir the tree directly, where an N_ID is an opaque string and any
// byte is legal — but RENDERED C is re-lexed, so raw spellings break every
// C toolchain on the emitted text. Per-byte mapping (Itanium-style
// mnemonics) keeps definition and use sites consistent by construction;
// names that are already valid C pass through byte-identical.
void emit_safe_ident(CEmit &e, const char *s)
{
	for (const char *p = s ? s : ""; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		    || (c >= '0' && c <= '9') || c == '_') {
			e.put((char)c);
			continue;
		}
		const char *m;
		switch (c) {
		case '+': m = "_pl"; break;
		case '-': m = "_mi"; break;
		case '*': m = "_ml"; break;
		case '/': m = "_dv"; break;
		case '%': m = "_rm"; break;
		case '[': m = "_lb"; break;
		case ']': m = "_rb"; break;
		case '<': m = "_lt"; break;
		case '>': m = "_gt"; break;
		case '=': m = "_eq"; break;
		case '!': m = "_nt"; break;
		case '&': m = "_an"; break;
		case '|': m = "_or"; break;
		case '^': m = "_eo"; break;
		case '~': m = "_co"; break;
		case '(': m = "_lp"; break;
		case ')': m = "_rp"; break;
		case '"': m = "_qu"; break;
		case ',': m = "_cm"; break;
		case ' ': m = "_sp"; break;
		default:  m = NULL; break;
		}
		if (m) e.put(m);
		else e.printf("_x%02x", c);
	}
}

// Emit each operand of `n` starting at `from`, separated by `sep`, each in
// expression context `ctx`.
void emit_seq(CEmit &e, node_t n, int from, const char *sep, int ctx)
{
	for (int i = from; ; i++) {
		node_t c = op(n, i);
		if (!c) break;
		if (i > from) e.put(sep);
		emit(e, c, ctx);
	}
}

// Emit a statement's leading label-list (op(0) of c2mir statements). Each
// entry is N_LABEL(id), N_CASE(expr), or N_DEFAULT — rendered as a C
// label/case/default line of its own, one level OUT from the statement it
// heads (a switch body's case labels sit at the switch's depth — K&R).
void emit_labels(CEmit &e, node_t labels)
{
	if (!labels) return;
	for (int i = 0; ; i++) {
		node_t l = op(labels, i);
		if (!l) break;
		int save = e.depth;
		if (e.depth > 0) e.depth--;
		switch (l->code) {
		case N_LABEL:
			emit(e, op(l, 0), P_NONE);
			e.put(':');
			break;
		case N_CASE: {
			// N_CASE(low) is a single value; N_CASE(low, high) is the GNU
			// range form `case LOW ... HIGH:`.
			e.put("case ");
			emit(e, op(l, 0), P_COND);
			node_t hi = op(l, 1);
			if (hi) {
				e.put(" ... ");
				emit(e, hi, P_COND);
			}
			e.put(':');
			break;
		}
		case N_DEFAULT:
			e.put("default:");
			break;
		default:
			e.depth = save;
			continue;
		}
		e.depth = save;
		e.nl();
	}
}

// Render `n` to a heap string through the normal emit() path. Used by
// emit_declarator to compose declarator text out-of-order (the spiral rule
// needs to wrap an already-rendered inner declarator in parentheses). The
// capture continues the caller's line (no indentation of its own).
std::string emit_to_string(CEmit &e, node_t n)
{
	madc::detail::StringCapture cap;
	if (!madc::detail::open_string_capture(cap))
		return std::string();
	CEmit sub(cap.f, e.lang, false);
	sub.depth = e.depth;
	emit(sub, n, P_NONE);
	return madc::detail::finish_string_capture(cap);
}

// Emit a C declarator following the C "spiral rule". The builder's suffix
// list (op(1)) is in c2m order — innermost binding first — and mixes
// N_POINTER (a prefix `*`) with N_FUNC / N_ARR (postfix). A pointer is a
// lower-precedence prefix than the postfix `()`/`[]`, so when a function or
// array suffix binds *outside* a pointer (pointer-to-function,
// pointer-to-array) the inner declarator must be parenthesized:
// `int (*fp)(int)`, `int (*ap)[4]`. Plain cases (`*p`, `a[3]`, `f(int)`,
// `*f(int)`) never have a pointer preceding a postfix in the list, so they
// render exactly as the previous flat emitter did.
void emit_declarator(CEmit &e, node_t decl)
{
	if (!decl) return;
	// Only a true N_DECL carries id + suffix-list. A bare N_IGNORE (used as
	// an empty declarator, e.g. on a struct definition's SPEC_DECL) has no
	// operands; calling op(decl, 1) on it walks past the end of an empty
	// operand list, which c2mir_node_op does not guard (NL_NEXT(NULL)).
	if (decl->code != N_DECL) { emit(e, decl, P_NONE); return; }
	node_t suffixes = op(decl, 1);       // N_LIST of N_POINTER / N_FUNC / N_ARR

	std::string d = emit_to_string(e, op(decl, 0));  // identifier (empty for N_IGNORE)
	bool prefix_pointer = false;         // inner declarator's outermost form is `*...`
	if (suffixes)
		for (int i = 0; ; i++) {
			node_t s = op(suffixes, i);
			if (!s) break;
			if (s->code == N_POINTER) {
				d = "*" + d;
				prefix_pointer = true;
			} else {                     // N_FUNC -> "(params)", N_ARR -> "[size]"
				if (prefix_pointer) d = "(" + d + ")";
				d += emit_to_string(e, s);
				prefix_pointer = false;
			}
		}
	e.put(d.c_str());
}

// Emit a declaration initializer. A scalar initializer is a bare
// assignment-expression; a brace initializer is an N_LIST of N_INIT
// entries, rendered as `{ e1, e2, ... }`.
void emit_initializer(CEmit &e, node_t n)
{
	if (!n) return;
	if (n->code == N_LIST) {
		e.put("{ ");
		emit_seq(e, n, 0, ", ", P_ASSIGN);
		e.put(" }");
		return;
	}
	emit(e, n, P_ASSIGN);
}

// A statement in a control-flow head's body: a block stays on the head's
// line (`if (c) {`); any other statement goes on the next line, one level in.
void emit_body(CEmit &e, node_t s)
{
	if (s && s->code == N_BLOCK) {
		e.put(' ');
		emit(e, s, P_NONE);
		return;
	}
	e.nl();
	e.depth++;
	emit(e, s, P_NONE);
	e.depth--;
}

// A prefix operator and its operand (`-x`, `!x`, `*p`, `++x`, `(T)x`). The
// operand is a cast-expression: anything binding looser than unary takes
// parentheses, and so does an operand whose spelling would fuse with the
// operator's sign (leads_with_sign).
void emit_prefix(CEmit &e, const char *o, node_t operand)
{
	e.put(o);
	int ctx = P_UNARY;
	if (operand && (o[0] == '-' || o[0] == '+') && o[1] == '\0'
	    && leads_with_sign(operand, o[0]))
		ctx = P_FORCE;
	emit(e, operand, ctx);
}

void emit(CEmit &e, node_t n, int ctx)
{
	if (!n) return;
	int prec = expr_prec(n);
	bool paren = prec >= 0 && prec < ctx;
	if (paren) e.put('(');
	switch (n->code) {
	case N_MODULE: {
		// [0] = N_LIST of top-level declarations; one per line. Each
		// carries a correlation row (V5) at its emitted line-start — the
		// same iteration emit_seq(…,"\n",…) does, with the hook inlined.
		node_t items = op(n, 0);
		if (items)
			for (int i = 0; ; i++) {
				node_t d = op(items, i);
				if (!d) break;
				if (i > 0) e.put('\n');
				map_record(e, d);
				emit(e, d, P_NONE);
			}
		e.nl();
		break;
	}
	case N_LIST:
		emit_seq(e, n, 0, " ", P_NONE);
		break;
	case N_FUNC_DEF:
		emit(e, op(n, 0), P_NONE);               // return-type specifiers
		e.put(' ');
		emit_declarator(e, op(n, 1));            // declarator: name(params)
		e.put(' ');
		emit(e, op(n, 3), P_NONE);               // body block
		break;
	case N_FUNC:
		e.put('(');
		// Parameter list: each entry is an N_TYPE (abstract) or N_SPEC_DECL
		// (named). A parameter declarator carries no trailing ';', so emit
		// the SPEC_DECL specs + declarator directly rather than via the
		// statement-context N_SPEC_DECL case.
		// A DOTS-ONLY list (the unknown-signature extern shape the JIT
		// tree carries for dlsym-resolved calls) renders as `()` — C
		// requires a named parameter before `...`, and an empty list
		// declares the same "unspecified arguments" contract.
		if (op(op(n, 0), 0) && op(op(n, 0), 0)->code == N_DOTS
		    && !op(op(n, 0), 1)) {
			e.put(')');
			break;
		}
		for (int i = 0; ; i++) {
			node_t p = op(op(n, 0), i);
			if (!p) break;
			if (i > 0) e.put(", ");
			if (p->code == N_SPEC_DECL) {
				emit(e, op(p, 0), P_NONE);
				e.put(' ');
				emit_declarator(e, op(p, 1));
			} else {
				emit(e, p, P_NONE);
			}
		}
		e.put(')');
		break;
	case N_TYPE: {
		emit(e, op(n, 0), P_NONE);               // specifiers
		// Declarator (abstract or named). For a named declarator the id is
		// non-ignore; for an abstract one (cast / sizeof type-name) the id is
		// N_IGNORE but pointer/array suffixes (op(decl,1)) still matter, e.g.
		// the '*' in (int *) or sizeof(int *).
		node_t decl = op(n, 1);
		if (declarator_present(decl)) {
			e.put(' ');
			emit_declarator(e, decl);
		}
		break;
	}
	case N_SPEC_DECL:
		// [0]=specifiers (often N_SHARE-wrapped) [1]=declarator
		// [2]=attribute list (N_LIST of N_ATTR) or N_IGNORE  [3]=asm  [4]=initializer
		{
		int pack = declaration_pack(op(n, 0));
		emit_pack_push(e, pack);
		emit(e, op(n, 0), P_NONE);
		if (declarator_present(op(n, 1))) {
			e.put(' ');
			emit_declarator(e, op(n, 1));
		}
		// Trailing attributes, e.g. __attribute__((vector_size(N))) on a SIMD
		// typedef; only present (an N_LIST) for attributed declarations.
		if (op(n, 2) && op(n, 2)->code == N_LIST && op(op(n, 2), 0)) {
			e.put(' ');
			emit_seq(e, op(n, 2), 0, " ", P_NONE);
		}
		if (op(n, 4) && op(n, 4)->code != N_IGNORE) {
			e.put(" = ");
			emit_initializer(e, op(n, 4));
		}
		e.put(';');
		emit_pack_pop(e, pack);
		}
		break;
	case N_SHARE:
		// single-operand wrapper around a type-specifier list
		emit(e, op(n, 0), P_NONE);
		break;
	case N_STRUCT:
	case N_UNION: {
		// [0]=tag id (N_ID or N_IGNORE) [1]=member list (N_LIST of N_MEMBER),
		// or N_IGNORE for an incomplete/forward reference. [2], when present,
		// is MadC's settled-layout contract [version,size,align,pack]. The
		// enclosing declaration/member emits the matching #pragma pack pair so
		// recompiling --emit=c11 cannot silently restore natural alignment.
		node_t members = op(n, 1);
		e.put(n->code == N_UNION ? "union" : "struct");
		node_t tag = op(n, 0);
		if (tag && tag->code != N_IGNORE) {
			e.put(' ');
			emit(e, tag, P_NONE);
		}
		if (members && members->code != N_IGNORE) {
			e.put(" {");
			e.nl();
			e.depth++;
			for (int i = 0; ; i++) {
				node_t m = op(members, i);
				if (!m) break;
				emit(e, m, P_NONE);
				e.nl();
			}
			e.depth--;
			e.put('}');
		}
		break;
	}
	case N_MEMBER: {
		// [0]=N_SHARE(specs) [1]=declarator [2]=attrs [3]=bit-field width
		// (member_node appends the width const-expr, or N_IGNORE). Dropping
		// the width rendered every bit-field member FULL-WIDTH: the text's
		// layout silently diverged from the tree c2mir lays out (libc++'s
		// basic_string rep measured 32 bytes under gcc against the real 24),
		// which sent a whole debugging arc chasing a layout bug that only
		// existed in the rendering.
		int pack = declaration_pack(op(n, 0));
		emit_pack_push(e, pack);
		emit(e, op(n, 0), P_NONE);
		if (declarator_present(op(n, 1))) {
			e.put(' ');
			emit_declarator(e, op(n, 1));
		}
		node_t w = op(n, 3);
		if (w && w->code != N_IGNORE) {
			e.put(" : ");
			emit(e, w, P_COND);
		}
		e.put(';');
		emit_pack_pop(e, pack);
		break;
	}
	case N_FIELD:
		// [0]=object expression, [1]=member id (N_ID leaf — emit directly)
		emit(e, op(n, 0), P_POSTFIX);
		e.put('.');
		emit(e, op(n, 1), P_NONE);
		break;
	case N_DEREF_FIELD:
		// [0]=pointer expression, [1]=member id
		emit(e, op(n, 0), P_POSTFIX);
		e.put("->");
		emit(e, op(n, 1), P_NONE);
		break;
	case N_ADD: case N_SUB: case N_MUL: case N_DIV: case N_MOD:
	case N_EQ:  case N_NE:  case N_LT:  case N_LE: case N_GT: case N_GE:
	case N_AND: case N_OR:  case N_XOR: case N_LSH: case N_RSH:
	case N_ANDAND: case N_OROR: case N_ASSIGN:
	case N_ADD_ASSIGN: case N_SUB_ASSIGN: case N_MUL_ASSIGN:
	case N_DIV_ASSIGN: case N_MOD_ASSIGN: case N_AND_ASSIGN:
	case N_OR_ASSIGN:  case N_XOR_ASSIGN: case N_LSH_ASSIGN:
	case N_RSH_ASSIGN: case N_COMMA: {
		static const struct { int code; const char *o; } M[] = {
			{N_ADD,"+"},{N_SUB,"-"},{N_MUL,"*"},{N_DIV,"/"},{N_MOD,"%"},
			{N_EQ,"=="},{N_NE,"!="},{N_LT,"<"},{N_LE,"<="},{N_GT,">"},{N_GE,">="},
			{N_AND,"&"},{N_OR,"|"},{N_XOR,"^"},{N_LSH,"<<"},{N_RSH,">>"},
			{N_ANDAND,"&&"},{N_OROR,"||"},{N_ASSIGN,"="},
			{N_ADD_ASSIGN,"+="},{N_SUB_ASSIGN,"-="},{N_MUL_ASSIGN,"*="},
			{N_DIV_ASSIGN,"/="},{N_MOD_ASSIGN,"%="},{N_AND_ASSIGN,"&="},
			{N_OR_ASSIGN,"|="},{N_XOR_ASSIGN,"^="},{N_LSH_ASSIGN,"<<="},
			{N_RSH_ASSIGN,">>="},{N_COMMA,","},{0,0}};
		const char *o = "?";
		for (int k = 0; M[k].o; k++) if (M[k].code == (int)n->code) o = M[k].o;
		if ((n->code == N_SUB || n->code == N_ADD) && !op(n, 1)) {
			emit_prefix(e, o, op(n, 0));	// unary minus / plus
			break;
		}
		// Operand contexts: a left-associative operator's left operand may
		// share its precedence (`a - b - c`), its right operand may not
		// (`a - (b - c)`); assignment is right-associative with a
		// unary-expression on the left; the comma's right operand is an
		// assignment-expression. wants_parens adds the -Wparentheses set.
		node_t l = op(n, 0), r = op(n, 1);
		int lctx = prec, rctx = prec + 1;
		if (prec == P_ASSIGN) {
			lctx = P_UNARY;
			rctx = P_ASSIGN;
		} else if (prec == P_COMMA) {
			rctx = P_ASSIGN;
		}
		if (wants_parens(n, l)) lctx = P_FORCE;
		if (wants_parens(n, r)) rctx = P_FORCE;
		emit(e, l, lctx);
		e.put(' ');
		e.put(o);
		e.put(' ');
		emit(e, r, rctx);
		break;
	}
	case N_BLOCK: {
		// [1] = statement list: one statement per line, one level in.
		e.put('{');
		e.nl();
		e.depth++;
		node_t items = op(n, 1);
		if (items)
			for (int i = 0; ; i++) {
				node_t s = op(items, i);
				if (!s) break;
				map_record(e, s);	// V5 correlation row at the stmt line-start
				if (is_label_carrier(s)) {
					emit_labels(e, op(s, 0));
					if (is_statement(op(items, i + 1)))
						continue;	// the labels head the next statement
					e.put(';');		// a declaration or nothing follows:
							// a null statement keeps the C11 rule
				} else
					emit(e, s, P_NONE);
				e.nl();
			}
		e.depth--;
		e.put('}');
		break;
	}
	case N_STMTEXPR:
		// GNU statement expression: ({ stmts; value; }). [0] = the block.
		e.put('(');
		emit(e, op(n, 0), P_NONE);
		e.put(')');
		break;
	case N_RETURN:
		// [0] = label list, [1] = return expression (may be absent/N_IGNORE)
		emit_labels(e, op(n, 0));
		e.put("return");
		if (op(n, 1) && op(n, 1)->code != N_IGNORE) {
			e.put(' ');
			emit(e, op(n, 1), P_NONE);
		}
		e.put(';');
		break;
	case N_EXPR:
		// [0] = label list, [1] = expression (may be N_IGNORE for empty stmt)
		emit_labels(e, op(n, 0));
		emit(e, op(n, 1), P_NONE);
		e.put(';');
		break;
	case N_IF: {
		// [0]=labels [1]=cond [2]=then-stmt [3]=else-stmt (may be N_IGNORE)
		// A condition is an expression, yet an assignment or a comma there
		// keeps its parentheses (P_COND): gcc/clang read `if (a = b)` as a
		// probable typo and warn — `if ((a = b))` is the idiom that says
		// "meant it".
		emit_labels(e, op(n, 0));
		e.put("if (");
		emit(e, op(n, 1), P_COND);
		e.put(')');
		node_t then_s = op(n, 2), else_s = op(n, 3);
		emit_body(e, then_s);
		if (else_s && else_s->code != N_IGNORE) {
			if (then_s && then_s->code == N_BLOCK)
				e.put(" else");
			else {
				e.nl();
				e.put("else");
			}
			if (else_s->code == N_IF) {	// the `else if` chain
				e.put(' ');
				emit(e, else_s, P_NONE);
			} else
				emit_body(e, else_s);
		}
		break;
	}
	case N_WHILE:
		// [0]=labels [1]=cond [2]=body
		emit_labels(e, op(n, 0));
		e.put("while (");
		emit(e, op(n, 1), P_COND);
		e.put(')');
		emit_body(e, op(n, 2));
		break;
	case N_DO: {
		// [0]=labels [1]=cond [2]=body  (builder order; rendered as do/while)
		emit_labels(e, op(n, 0));
		e.put("do");
		node_t body = op(n, 2);
		emit_body(e, body);
		if (body && body->code == N_BLOCK)
			e.put(" while (");
		else {
			e.nl();
			e.put("while (");
		}
		emit(e, op(n, 1), P_COND);
		e.put(");");
		break;
	}
	case N_FOR:
		// [0]=labels [1]=init [2]=cond [3]=incr [4]=body (each may be N_IGNORE)
		emit_labels(e, op(n, 0));
		e.put("for (");
		{
			node_t finit = op(n, 1);
			bool init_present = finit && finit->code != N_IGNORE;
			if (init_present) emit(e, finit, P_NONE);
			// A declaration init (N_SPEC_DECL) renders its own trailing ';';
			// don't emit a second separator. An expression / empty init needs
			// the explicit separator.
			if (!(init_present && finit->code == N_SPEC_DECL))
				e.put(';');
			e.put(' ');
		}
		if (op(n, 2) && op(n, 2)->code != N_IGNORE) emit(e, op(n, 2), P_COND);
		e.put("; ");
		if (op(n, 3) && op(n, 3)->code != N_IGNORE) emit(e, op(n, 3), P_NONE);
		e.put(')');
		emit_body(e, op(n, 4));
		break;
	case N_SWITCH:
		// [0]=labels [1]=controlling-expr [2]=body block
		emit_labels(e, op(n, 0));
		e.put("switch (");
		emit(e, op(n, 1), P_COND);
		e.put(')');
		emit_body(e, op(n, 2));
		break;
	case N_BREAK:
		// [0]=labels only
		emit_labels(e, op(n, 0));
		e.put("break;");
		break;
	case N_CONTINUE:
		// [0]=labels only
		emit_labels(e, op(n, 0));
		e.put("continue;");
		break;
	case N_GOTO:
		// [0]=labels [1]=target id
		emit_labels(e, op(n, 0));
		e.put("goto ");
		emit(e, op(n, 1), P_NONE);
		e.put(';');
		break;
	case N_CALL:
		// [0] = callee expression, [1] = N_LIST of argument expressions
		// (each an assignment-expression: a comma expression takes parens)
		emit(e, op(n, 0), P_POSTFIX);
		e.put('(');
		emit_seq(e, op(n, 1), 0, ", ", P_ASSIGN);
		e.put(')');
		break;
	case N_STR: {
		// String literal: a C double-quoted literal through THE one
		// escape rule (madc_c_escape_string — dupaudit family
		// c_string_literal_escape). u.s.s holds the (interned)
		// bytes; u.s.len includes the NUL terminator.
		const char *s = n->u.s.s;
		size_t len = n->u.s.len;
		if (len > 0 && s && s[len - 1] == '\0') len--;   // drop trailing NUL
		e.put('"');
		std::string esc = madc_c_escape_string(s, len);
		e.put(esc.c_str());
		e.put('"');
		break;
	}
	case N_ADDR:
		// [0] = operand expression
		emit_prefix(e, "&", op(n, 0));
		break;
	case N_DEREF:
		// [0] = operand expression
		emit_prefix(e, "*", op(n, 0));
		break;
	case N_IND:
		// [0] = base, [1] = subscript index
		emit(e, op(n, 0), P_POSTFIX);
		e.put('[');
		emit(e, op(n, 1), P_NONE);
		e.put(']');
		break;
	case N_ARR:
		// declarator suffix: [0]=ignore [1]=qualifier-list [2]=size expr
		e.put('[');
		if (op(n, 2) && op(n, 2)->code != N_IGNORE) emit(e, op(n, 2), P_NONE);
		e.put(']');
		break;
	case N_INIT: {
		// [0] = designator list (often empty), [1] = value (scalar or nested
		// brace list). Emit designators (=) then the value initializer.
		node_t desig = op(n, 0);
		if (desig && op(desig, 0)) {
			emit_seq(e, desig, 0, " ", P_NONE);
			e.put(" = ");
		}
		emit_initializer(e, op(n, 1));
		break;
	}
	case N_CAST:
		// [0] = N_TYPE (target type), [1] = operand expression (a
		// cast-expression: `(T)x`, `(T)(a + b)`, `((T)p)->f` by context)
		e.put('(');
		emit(e, op(n, 0), P_NONE);
		e.put(')');
		emit(e, op(n, 1), P_UNARY);
		break;
	case N_COMPOUND_LITERAL:
		// [0]=N_TYPE(target type) [1]=N_LIST(N_INIT...)  ->  (TYPE){ ... }
		e.put('(');
		emit(e, op(n, 0), P_NONE);
		e.put(')');
		emit_initializer(e, op(n, 1));
		break;
	case N_ATTR: {
		// [0]=N_ID(name) [1]=N_LIST(args)  ->  __attribute__((name(args)))
		// Emitted for vector_size on SIMD types; rendered in a spec list (cast /
		// type-name) or via the N_SPEC_DECL attrs operand (typedef).
		// `linkonce` is madc's internal vague-linkage marker (S4) — no such
		// gcc attribute exists; the portable C spelling with the same
		// link-time dedupe (STB_WEAK, first def wins) is `weak`.
		// `ret_addr` marks the hidden result-address PARAMETER of a by-value
		// non-trivial class return so MIR places it in the target's
		// indirect-result register. There is no portable C spelling — C has
		// no way to say "always return this indirectly", which is why the
		// marker exists — and gcc/clang would warn "unknown attribute" and
		// then treat the parameter as the plain pointer it already is. So
		// DROP it: the emitted C keeps exactly the behaviour it had before
		// the marker existed, with no diagnostic. (That behaviour is
		// correct only where the indirect-result pointer is the first
		// argument register — see docs/plans/2026-08-07-macos-release-lane-plan.md
		// on why portable C cannot call such a function at all.)
		node_t aname = op(n, 0);
		if (aname && aname->code == N_ID && strcmp(aname->u.s.s, "ret_addr") == 0)
			break;
		if (aname && aname->code == N_ID && strcmp(aname->u.s.s, "linkonce") == 0) {
			e.put("__attribute__((weak))");
			break;
		}
		e.put("__attribute__((");
		emit(e, op(n, 0), P_NONE);
		node_t aargs = op(n, 1);
		if (aargs && op(aargs, 0)) {
			e.put('(');
			emit_seq(e, aargs, 0, ", ", P_ASSIGN);
			e.put(')');
		}
		e.put("))");
		break;
	}
	case N_SIZEOF:
		// [0] = N_TYPE (type-name operand)
		e.put("sizeof(");
		emit(e, op(n, 0), P_NONE);
		e.put(')');
		break;
	case N_EXPR_SIZEOF:
		// [0] = expression operand
		e.put("sizeof(");
		emit(e, op(n, 0), P_NONE);
		e.put(')');
		break;
	case N_ALIGNOF:
		// [0] = N_TYPE operand
		e.put("_Alignof(");
		emit(e, op(n, 0), P_NONE);
		e.put(')');
		break;
	case N_NOT:
		emit_prefix(e, "!", op(n, 0));
		break;
	case N_BITWISE_NOT:
		emit_prefix(e, "~", op(n, 0));
		break;
	case N_INC:
		emit_prefix(e, "++", op(n, 0));
		break;
	case N_DEC:
		emit_prefix(e, "--", op(n, 0));
		break;
	case N_POST_INC:
		emit(e, op(n, 0), P_POSTFIX);
		e.put("++");
		break;
	case N_POST_DEC:
		emit(e, op(n, 0), P_POSTFIX);
		e.put("--");
		break;
	case N_COND:
		// [0]=cond [1]=true-expr [2]=false-expr (no label list — this is an
		// expr). Right-associative: the condition is a logical-OR-expression,
		// the middle operand an expression (a comma still takes parens for
		// the reader), the last a conditional-expression.
		emit(e, op(n, 0), P_OROR);
		e.put(" ? ");
		emit(e, op(n, 1), P_ASSIGN);
		e.put(" : ");
		emit(e, op(n, 2), P_COND);
		break;
	case N_ID:   emit_safe_ident(e, n->u.s.s); break;
	case N_STR16: case N_STR32: break; // wide strings: not supported (see c11-transpiler rule)
	case N_I:
	case N_L:    e.printf("%lld", (long long)n->u.l); break;
	case N_LL:   e.printf("%lldLL", (long long)n->u.ll); break;
	case N_U:    e.printf("%lluU", (unsigned long long)n->u.ul); break;
	case N_UL:   e.printf("%lluUL", (unsigned long long)n->u.ul); break;
	case N_ULL:  e.printf("%lluULL", (unsigned long long)n->u.ull); break;
	// Floating literals: hex float (%a) round-trips the exact bit pattern and
	// is unambiguously typed (avoids "3.0" reparsing as int / precision loss).
	case N_F:    e.printf("%af", (double)n->u.f); break;
	case N_D:    e.printf("%a", n->u.d); break;
	case N_LD:   e.printf("%LaL", n->u.ld); break;
	// Imaginary constants (the i/I-suffixed literal c2mir lexes to N_CF/N_CD/N_CLD).
	// Re-emit with the matching imaginary suffix so the portable-C output parses back
	// to the same _Complex value.
	case N_CF:   e.printf("%afi", (double)n->u.f); break;
	case N_CD:   e.printf("%ai", n->u.d); break;
	case N_CLD:  e.printf("%aLi", (double)n->u.ld); break;
	case N_CH: case N_CH16: case N_CH32: {
		int c = (int)n->u.ch;
		switch (c) {
		case '\n': e.put("'\\n'"); break;
		case '\t': e.put("'\\t'"); break;
		case '\r': e.put("'\\r'"); break;
		case '\0': e.put("'\\0'"); break;
		case '\\': e.put("'\\\\'"); break;
		case '\'': e.put("'\\''"); break;
		default:
			if (c >= 32 && c < 127) e.printf("'%c'", c);
			else e.printf("'\\x%02x'", (unsigned char)c);
		}
		break;
	}
	case N_VOID:     e.put("void"); break;
	case N_CHAR:     e.put("char"); break;
	case N_INT:      e.put("int"); break;
	case N_LONG:     e.put("long"); break;
	case N_SHORT:    e.put("short"); break;
	case N_UNSIGNED: e.put("unsigned"); break;
	case N_SIGNED:   e.put("signed"); break;
	case N_DOUBLE:   e.put("double"); break;
	case N_FLOAT:    e.put("float"); break;
	case N_INT128:   e.put("__int128"); break;
	case N_COMPLEX:  e.put("_Complex"); break;
	case N_BOOL:     e.put("_Bool"); break;
	case N_CONST:    e.put("const"); break;
	case N_VOLATILE: e.put("volatile"); break;
	case N_RESTRICT: e.put("restrict"); break;
	case N_EXTERN:       e.put("extern"); break;
	case N_STATIC:       e.put("static"); break;
	case N_TYPEDEF:      e.put("typedef"); break;
	case N_AUTO:         e.put("auto"); break;
	case N_REGISTER:     e.put("register"); break;
	case N_THREAD_LOCAL: e.put("_Thread_local"); break;
	case N_ALIGNAS:
		// _Alignas(N) in a specifier list (e.g. the madc `array` member's
		// alignof(madc::value) buffer).
		e.put("_Alignas(");
		emit(e, op(n, 0), P_NONE);
		e.put(')');
		break;
	case N_DOTS:         e.put("..."); break;
	case N_IGNORE: break;
	default:
		// Localize the missing construct for the fidelity gate.
		e.printf("/*<unhandled %s>*/",
			 c2mir_node_code_name((c2mir_node_code_t)n->code));
		break;
	}
	if (paren) e.put(')');
}

} // namespace

void cir_emit_c(FILE *f, node_t tree, CirEmitLang lang,
		std::vector<CirEmitMapRow> *map)
{
	CEmit e(f, lang, true);
	e.cmap = map;
	emit(e, tree, P_NONE);
}

// ---- the C++ reverse-render (--emit=c++; madcide AST-4 slice 1) -----------
// Renders the TU's RETAINED SOURCE (mc11-ir.md: the attached tokens + trivia
// are the path back to the original source): the TU's own recorded #include
// directives, then every TU-file token echoed in stream order (leading
// trivia + the one spelling owner, madc_token_spelling), then the trailing
// trivia. The suppressions the Phase-5 design asked for are INHERENT to
// this shape: lowered machinery (a string decl's storage/ctor/dtor synth
// group, mangled call forms, __madc_global_init scaffolding) exists only as
// TREE nodes and never enters the TU token stream, and #if'd-out regions
// never lexed — so the echo is the high-level statement stream by
// construction. The tree's role here is the validity gate the caller
// already ran (never render an erroneous tree); tree-scoped rendering (a
// single function's view) is the named later lever. Known slice-1
// normalizations: macro uses echo EXPANDED (the name token is consumed at
// lex; the definition line echoes nothing — semantics preserved), and
// numeric literals canonicalize where the original text was not retained.
// madc-dialect constructs pass through UNRESPELLED — cross-language
// respelling is the named next seat
// (docs/plans/2026-08-25-madcide-ast-arc-design.md §3.2).

void cir_emit_cxx(FILE *f, const CirEmitSource &si)
{
	// 1. The TU's own include directives, as written, in order.
	if (si.includes && si.tu_file)
		for (size_t i = 0; i < si.includes->size(); i++)
			if ((*si.includes)[i].first == si.tu_file)
				fprintf(f, "%s\n", (*si.includes)[i].second.c_str());

	// 2. Echo the TU's tokens, stream order (TokenStream iteration walks
	// the WHOLE lexed buffer, cursor-independent).
	if (si.tokens && si.tu_file)
		for (TokenBase *tb : *si.tokens) {
			if (!tb || !tb->file || strcmp(tb->file, si.tu_file))
				continue;
			fputs(tb->leading_trivia.c_str(), f);
			fputs(madc_token_spelling(tb).c_str(), f);
		}

	// 3. Whitespace/comments after the last token — faithful to the byte.
	if (si.trailing)
		fputs(si.trailing->c_str(), f);
}
