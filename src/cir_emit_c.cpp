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

void emit_pack_push(FILE *f, int pack)
{
	if (pack > 0)
		fprintf(f, "#pragma pack(push, %d)\n", pack);
}

void emit_pack_pop(FILE *f, int pack)
{
	if (pack > 0)
		fputs("\n#pragma pack(pop)", f);
}

void emit(FILE *f, node_t n, CirEmitLang lang);
void emit_initializer(FILE *f, node_t n, CirEmitLang lang);

// Emit an identifier with every non-C-identifier byte flattened to a
// deterministic mnemonic. Method symbols carry C++ operator spellings
// (`Cls__operator++_un`, `operator[]__o5`, `operator""s`): the JIT path
// feeds c2mir the tree directly, where an N_ID is an opaque string and any
// byte is legal — but RENDERED C is re-lexed, so raw spellings break every
// C toolchain on the emitted text. Per-byte mapping (Itanium-style
// mnemonics) keeps definition and use sites consistent by construction;
// names that are already valid C pass through byte-identical.
void emit_safe_ident(FILE *f, const char *s)
{
	for (const char *p = s ? s : ""; *p; p++) {
		unsigned char c = (unsigned char)*p;
		if ((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z')
		    || (c >= '0' && c <= '9') || c == '_') {
			fputc(c, f);
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
		if (m) fputs(m, f);
		else fprintf(f, "_x%02x", c);
	}
}

// Emit each operand of `n` starting at `from`, separated by `sep`.
void emit_seq(FILE *f, node_t n, CirEmitLang lang, int from, const char *sep)
{
	for (int i = from; ; i++) {
		node_t c = op(n, i);
		if (!c) break;
		if (i > from) fputs(sep, f);
		emit(f, c, lang);
	}
}

// Emit a statement's leading label-list (op(0) of c2mir statements).
// Each entry is N_LABEL(id), N_CASE(expr), or N_DEFAULT — rendered as a
// C label/case/default prefix terminated by ':'.
void emit_labels(FILE *f, node_t labels, CirEmitLang lang)
{
	if (!labels) return;
	for (int i = 0; ; i++) {
		node_t l = op(labels, i);
		if (!l) break;
		switch (l->code) {
		case N_LABEL:
			emit(f, op(l, 0), lang); fputs(": ", f); break;
		case N_CASE: {
			// N_CASE(low) is a single value; N_CASE(low, high) is the GNU
			// range form `case LOW ... HIGH:`.
			fputs("case ", f); emit(f, op(l, 0), lang);
			node_t hi = op(l, 1);
			if (hi) { fputs(" ... ", f); emit(f, hi, lang); }
			fputs(": ", f);
			break;
		}
		case N_DEFAULT:
			fputs("default: ", f); break;
		default:
			break;
		}
	}
}

// Render `n` to a heap string through the normal emit() path. Used by
// emit_declarator to compose declarator text out-of-order (the spiral rule
// needs to wrap an already-rendered inner declarator in parentheses).
std::string emit_to_string(node_t n, CirEmitLang lang)
{
	madc::detail::StringCapture cap;
	if (!madc::detail::open_string_capture(cap))
		return std::string();
	emit(cap.f, n, lang);
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
void emit_declarator(FILE *f, node_t decl, CirEmitLang lang)
{
	if (!decl) return;
	// Only a true N_DECL carries id + suffix-list. A bare N_IGNORE (used as
	// an empty declarator, e.g. on a struct definition's SPEC_DECL) has no
	// operands; calling op(decl, 1) on it walks past the end of an empty
	// operand list, which c2mir_node_op does not guard (NL_NEXT(NULL)).
	if (decl->code != N_DECL) { emit(f, decl, lang); return; }
	node_t suffixes = op(decl, 1);       // N_LIST of N_POINTER / N_FUNC / N_ARR

	std::string d = emit_to_string(op(decl, 0), lang);  // identifier (empty for N_IGNORE)
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
				d += emit_to_string(s, lang);
				prefix_pointer = false;
			}
		}
	fputs(d.c_str(), f);
}

// Emit a declaration initializer. A scalar initializer is a bare
// expression; a brace initializer is an N_LIST of N_INIT entries, which is
// rendered as `{ e1, e2, ... }`.
void emit_initializer(FILE *f, node_t n, CirEmitLang lang)
{
	if (!n) return;
	if (n->code == N_LIST) {
		fputs("{ ", f);
		emit_seq(f, n, lang, 0, ", ");
		fputs(" }", f);
		return;
	}
	emit(f, n, lang);
}

void emit(FILE *f, node_t n, CirEmitLang lang)
{
	if (!n) return;
	switch (n->code) {
	case N_MODULE:
		// [0] = N_LIST of top-level declarations; one per line.
		emit_seq(f, op(n, 0), lang, 0, "\n");
		fputc('\n', f);
		break;
	case N_LIST:
		emit_seq(f, n, lang, 0, " ");
		break;
	case N_FUNC_DEF:
		emit(f, op(n, 0), lang);                 // return-type specifiers
		fputc(' ', f);
		emit_declarator(f, op(n, 1), lang);      // declarator: name(params)
		fputc(' ', f);
		emit(f, op(n, 3), lang);                 // body block
		break;
	case N_FUNC:
		fputc('(', f);
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
			fputc(')', f);
			break;
		}
		for (int i = 0; ; i++) {
			node_t p = op(op(n, 0), i);
			if (!p) break;
			if (i > 0) fputs(", ", f);
			if (p->code == N_SPEC_DECL) {
				emit(f, op(p, 0), lang);
				fputc(' ', f);
				emit_declarator(f, op(p, 1), lang);
			} else {
				emit(f, p, lang);
			}
		}
		fputc(')', f);
		break;
	case N_TYPE: {
		emit(f, op(n, 0), lang);                 // specifiers
		// Declarator (abstract or named). For a named declarator the id is
		// non-ignore; for an abstract one (cast / sizeof type-name) the id is
		// N_IGNORE but pointer/array suffixes (op(decl,1)) still matter, e.g.
		// the '*' in (int *) or sizeof(int *).
		node_t decl = op(n, 1);
		bool named = decl && decl->code == N_DECL && op(decl, 0)
			&& op(decl, 0)->code != N_IGNORE;
		bool has_suffix = decl && decl->code == N_DECL && op(decl, 1)
			&& op(op(decl, 1), 0);
		if (named || has_suffix) {
			fputc(' ', f);
			emit_declarator(f, decl, lang);
		}
		break;
	}
	case N_SPEC_DECL:
		// [0]=specifiers (often N_SHARE-wrapped) [1]=declarator
		// [2]=attribute list (N_LIST of N_ATTR) or N_IGNORE  [3]=asm  [4]=initializer
		{
		int pack = declaration_pack(op(n, 0));
		emit_pack_push(f, pack);
		emit(f, op(n, 0), lang);
		fputc(' ', f);
		emit_declarator(f, op(n, 1), lang);
		// Trailing attributes, e.g. __attribute__((vector_size(N))) on a SIMD
		// typedef; only present (an N_LIST) for attributed declarations.
		if (op(n, 2) && op(n, 2)->code == N_LIST && op(op(n, 2), 0)) {
			fputc(' ', f);
			emit_seq(f, op(n, 2), lang, 0, " ");
		}
		if (op(n, 4) && op(n, 4)->code != N_IGNORE) {
			fputs(" = ", f);
			emit_initializer(f, op(n, 4), lang);
		}
		fputc(';', f);
		emit_pack_pop(f, pack);
		}
		break;
	case N_SHARE:
		// single-operand wrapper around a type-specifier list
		emit(f, op(n, 0), lang);
		break;
	case N_STRUCT:
	case N_UNION: {
		// [0]=tag id (N_ID or N_IGNORE) [1]=member list (N_LIST of N_MEMBER),
		// or N_IGNORE for an incomplete/forward reference. [2], when present,
		// is MadC's settled-layout contract [version,size,align,pack]. The
		// enclosing declaration/member emits the matching #pragma pack pair so
		// recompiling --emit=c11 cannot silently restore natural alignment.
		node_t members = op(n, 1);
		fputs(n->code == N_UNION ? "union" : "struct", f);
		node_t tag = op(n, 0);
		if (tag && tag->code != N_IGNORE) { fputc(' ', f); emit(f, tag, lang); }
		if (members && members->code != N_IGNORE) {
			fputs(" {\n", f);
			for (int i = 0; ; i++) {
				node_t m = op(members, i);
				if (!m) break;
				emit(f, m, lang);
				fputc('\n', f);
			}
			fputc('}', f);
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
		emit_pack_push(f, pack);
		emit(f, op(n, 0), lang);
		fputc(' ', f);
		emit_declarator(f, op(n, 1), lang);
		node_t w = op(n, 3);
		if (w && w->code != N_IGNORE) {
			fputs(" : ", f);
			emit(f, w, lang);
		}
		fputc(';', f);
		emit_pack_pop(f, pack);
		break;
	}
	case N_FIELD:
		// [0]=object expression, [1]=member id (N_ID leaf — emit directly)
		emit(f, op(n, 0), lang);
		fputc('.', f);
		emit(f, op(n, 1), lang);
		break;
	case N_DEREF_FIELD:
		// [0]=pointer expression, [1]=member id
		emit(f, op(n, 0), lang);
		fputs("->", f);
		emit(f, op(n, 1), lang);
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
		if (n->code == N_SUB && !op(n, 1)) {
			fputs("(-", f);
			emit(f, op(n, 0), lang);
			fputc(')', f);
			break;
		}
		fputc('(', f);
		emit(f, op(n, 0), lang);
		fprintf(f, " %s ", o);
		emit(f, op(n, 1), lang);
		fputc(')', f);
		break;
	}
	case N_BLOCK:
		fputs("{\n", f);
		emit_seq(f, op(n, 1), lang, 0, "\n");     // [1] = statement list
		fputs("\n}", f);
		break;
	case N_STMTEXPR:
		// GNU statement expression: ({ stmts; value; }). [0] = the block.
		fputs("(", f);
		emit(f, op(n, 0), lang);
		fputs(")", f);
		break;
	case N_RETURN:
		// [0] = label list, [1] = return expression (may be absent/N_IGNORE)
		emit_labels(f, op(n, 0), lang);
		fputs("return", f);
		if (op(n, 1) && op(n, 1)->code != N_IGNORE) {
			fputc(' ', f);
			emit(f, op(n, 1), lang);
		}
		fputc(';', f);
		break;
	case N_EXPR:
		// [0] = label list, [1] = expression (may be N_IGNORE for empty stmt)
		emit_labels(f, op(n, 0), lang);
		emit(f, op(n, 1), lang);
		fputc(';', f);
		break;
	case N_IF:
		// [0]=labels [1]=cond [2]=then-stmt [3]=else-stmt (may be N_IGNORE)
		emit_labels(f, op(n, 0), lang);
		fputs("if (", f);
		emit(f, op(n, 1), lang);
		fputs(") ", f);
		emit(f, op(n, 2), lang);
		if (op(n, 3) && op(n, 3)->code != N_IGNORE) {
			fputs(" else ", f);
			emit(f, op(n, 3), lang);
		}
		break;
	case N_WHILE:
		// [0]=labels [1]=cond [2]=body
		emit_labels(f, op(n, 0), lang);
		fputs("while (", f);
		emit(f, op(n, 1), lang);
		fputs(") ", f);
		emit(f, op(n, 2), lang);
		break;
	case N_DO:
		// [0]=labels [1]=cond [2]=body  (builder order; rendered as do/while)
		emit_labels(f, op(n, 0), lang);
		fputs("do ", f);
		emit(f, op(n, 2), lang);
		fputs(" while (", f);
		emit(f, op(n, 1), lang);
		fputs(");", f);
		break;
	case N_FOR:
		// [0]=labels [1]=init [2]=cond [3]=incr [4]=body (each may be N_IGNORE)
		emit_labels(f, op(n, 0), lang);
		fputs("for (", f);
		{
			node_t finit = op(n, 1);
			bool init_present = finit && finit->code != N_IGNORE;
			if (init_present) emit(f, finit, lang);
			// A declaration init (N_SPEC_DECL) renders its own trailing ';';
			// don't emit a second separator. An expression / empty init needs
			// the explicit separator.
			if (!(init_present && finit->code == N_SPEC_DECL))
				fputc(';', f);
			fputc(' ', f);
		}
		if (op(n, 2) && op(n, 2)->code != N_IGNORE) emit(f, op(n, 2), lang);
		fputs("; ", f);
		if (op(n, 3) && op(n, 3)->code != N_IGNORE) emit(f, op(n, 3), lang);
		fputs(") ", f);
		emit(f, op(n, 4), lang);
		break;
	case N_SWITCH:
		// [0]=labels [1]=controlling-expr [2]=body block
		emit_labels(f, op(n, 0), lang);
		fputs("switch (", f);
		emit(f, op(n, 1), lang);
		fputs(") ", f);
		emit(f, op(n, 2), lang);
		break;
	case N_BREAK:
		// [0]=labels only
		emit_labels(f, op(n, 0), lang);
		fputs("break;", f);
		break;
	case N_CONTINUE:
		// [0]=labels only
		emit_labels(f, op(n, 0), lang);
		fputs("continue;", f);
		break;
	case N_GOTO:
		// [0]=labels [1]=target id
		emit_labels(f, op(n, 0), lang);
		fputs("goto ", f);
		emit(f, op(n, 1), lang);
		fputc(';', f);
		break;
	case N_CALL:
		// [0] = callee expression, [1] = N_LIST of argument expressions
		emit(f, op(n, 0), lang);
		fputc('(', f);
		emit_seq(f, op(n, 1), lang, 0, ", ");
		fputc(')', f);
		break;
	case N_STR: {
		// String literal: a C double-quoted literal through THE one
		// escape rule (madc_c_escape_string — dupaudit family
		// c_string_literal_escape). u.s.s holds the (interned)
		// bytes; u.s.len includes the NUL terminator.
		const char *s = n->u.s.s;
		size_t len = n->u.s.len;
		if (len > 0 && s && s[len - 1] == '\0') len--;   // drop trailing NUL
		fputc('"', f);
		std::string esc = madc_c_escape_string(s, len);
		fputs(esc.c_str(), f);
		fputc('"', f);
		break;
	}
	case N_ADDR:
		// [0] = operand expression
		fputs("(&", f); emit(f, op(n, 0), lang); fputc(')', f);
		break;
	case N_DEREF:
		// [0] = operand expression
		fputs("(*", f); emit(f, op(n, 0), lang); fputc(')', f);
		break;
	case N_IND:
		// [0] = base, [1] = subscript index
		emit(f, op(n, 0), lang);
		fputc('[', f); emit(f, op(n, 1), lang); fputc(']', f);
		break;
	case N_ARR:
		// declarator suffix: [0]=ignore [1]=qualifier-list [2]=size expr
		fputc('[', f);
		if (op(n, 2) && op(n, 2)->code != N_IGNORE) emit(f, op(n, 2), lang);
		fputc(']', f);
		break;
	case N_INIT: {
		// [0] = designator list (often empty), [1] = value (scalar or nested
		// brace list). Emit designators (=) then the value initializer.
		node_t desig = op(n, 0);
		if (desig && op(desig, 0)) {
			emit_seq(f, desig, lang, 0, " ");
			fputs(" = ", f);
		}
		emit_initializer(f, op(n, 1), lang);
		break;
	}
	case N_CAST:
		// [0] = N_TYPE (target type), [1] = operand expression
		fputs("((", f);
		emit(f, op(n, 0), lang);
		fputs(")", f);
		emit(f, op(n, 1), lang);
		fputc(')', f);
		break;
	case N_COMPOUND_LITERAL:
		// [0]=N_TYPE(target type) [1]=N_LIST(N_INIT...)  ->  (TYPE){ ... }
		fputc('(', f);
		emit(f, op(n, 0), lang);
		fputc(')', f);
		emit_initializer(f, op(n, 1), lang);
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
			fputs("__attribute__((weak))", f);
			break;
		}
		fputs("__attribute__((", f);
		emit(f, op(n, 0), lang);
		node_t aargs = op(n, 1);
		if (aargs && op(aargs, 0)) {
			fputc('(', f);
			emit_seq(f, aargs, lang, 0, ", ");
			fputc(')', f);
		}
		fputs("))", f);
		break;
	}
	case N_SIZEOF:
		// [0] = N_TYPE (type-name operand)
		fputs("sizeof(", f);
		emit(f, op(n, 0), lang);
		fputc(')', f);
		break;
	case N_EXPR_SIZEOF:
		// [0] = expression operand
		fputs("sizeof(", f);
		emit(f, op(n, 0), lang);
		fputc(')', f);
		break;
	case N_ALIGNOF:
		// [0] = N_TYPE operand
		fputs("_Alignof(", f);
		emit(f, op(n, 0), lang);
		fputc(')', f);
		break;
	case N_NOT:
		fputc('(', f); fputc('!', f); emit(f, op(n, 0), lang); fputc(')', f);
		break;
	case N_BITWISE_NOT:
		fputc('(', f); fputc('~', f); emit(f, op(n, 0), lang); fputc(')', f);
		break;
	case N_INC:
		fputs("(++", f); emit(f, op(n, 0), lang); fputc(')', f);
		break;
	case N_DEC:
		fputs("(--", f); emit(f, op(n, 0), lang); fputc(')', f);
		break;
	case N_POST_INC:
		fputc('(', f); emit(f, op(n, 0), lang); fputs("++)", f);
		break;
	case N_POST_DEC:
		fputc('(', f); emit(f, op(n, 0), lang); fputs("--)", f);
		break;
	case N_COND:
		// [0]=cond [1]=true-expr [2]=false-expr (no label list — this is an expr)
		fputc('(', f);
		emit(f, op(n, 0), lang);
		fputs(" ? ", f);
		emit(f, op(n, 1), lang);
		fputs(" : ", f);
		emit(f, op(n, 2), lang);
		fputc(')', f);
		break;
	case N_ID:   emit_safe_ident(f, n->u.s.s); break;
	case N_STR16: case N_STR32: break; // wide strings: not supported (see c11-transpiler rule)
	case N_I:
	case N_L:    fprintf(f, "%lld", (long long)n->u.l); break;
	case N_LL:   fprintf(f, "%lldLL", (long long)n->u.ll); break;
	case N_U:    fprintf(f, "%lluU", (unsigned long long)n->u.ul); break;
	case N_UL:   fprintf(f, "%lluUL", (unsigned long long)n->u.ul); break;
	case N_ULL:  fprintf(f, "%lluULL", (unsigned long long)n->u.ull); break;
	// Floating literals: hex float (%a) round-trips the exact bit pattern and
	// is unambiguously typed (avoids "3.0" reparsing as int / precision loss).
	case N_F:    fprintf(f, "%af", (double)n->u.f); break;
	case N_D:    fprintf(f, "%a", n->u.d); break;
	case N_LD:   fprintf(f, "%LaL", n->u.ld); break;
	// Imaginary constants (the i/I-suffixed literal c2mir lexes to N_CF/N_CD/N_CLD).
	// Re-emit with the matching imaginary suffix so the portable-C output parses back
	// to the same _Complex value.
	case N_CF:   fprintf(f, "%afi", (double)n->u.f); break;
	case N_CD:   fprintf(f, "%ai", n->u.d); break;
	case N_CLD:  fprintf(f, "%aLi", (double)n->u.ld); break;
	case N_CH: case N_CH16: case N_CH32: {
		int c = (int)n->u.ch;
		switch (c) {
		case '\n': fputs("'\\n'", f); break;
		case '\t': fputs("'\\t'", f); break;
		case '\r': fputs("'\\r'", f); break;
		case '\0': fputs("'\\0'", f); break;
		case '\\': fputs("'\\\\'", f); break;
		case '\'': fputs("'\\''", f); break;
		default:
			if (c >= 32 && c < 127) fprintf(f, "'%c'", c);
			else fprintf(f, "'\\x%02x'", (unsigned char)c);
		}
		break;
	}
	case N_VOID:     fputs("void", f); break;
	case N_CHAR:     fputs("char", f); break;
	case N_INT:      fputs("int", f); break;
	case N_LONG:     fputs("long", f); break;
	case N_SHORT:    fputs("short", f); break;
	case N_UNSIGNED: fputs("unsigned", f); break;
	case N_SIGNED:   fputs("signed", f); break;
	case N_DOUBLE:   fputs("double", f); break;
	case N_FLOAT:    fputs("float", f); break;
	case N_INT128:   fputs("__int128", f); break;
	case N_COMPLEX:  fputs("_Complex", f); break;
	case N_BOOL:     fputs("_Bool", f); break;
	case N_CONST:    fputs("const", f); break;
	case N_VOLATILE: fputs("volatile", f); break;
	case N_RESTRICT: fputs("restrict", f); break;
	case N_EXTERN:       fputs("extern", f); break;
	case N_STATIC:       fputs("static", f); break;
	case N_TYPEDEF:      fputs("typedef", f); break;
	case N_AUTO:         fputs("auto", f); break;
	case N_REGISTER:     fputs("register", f); break;
	case N_THREAD_LOCAL: fputs("_Thread_local", f); break;
	case N_ALIGNAS:
		// _Alignas(N) in a specifier list (e.g. the madc `array` member's
		// alignof(madc::value) buffer).
		fputs("_Alignas(", f); emit(f, op(n, 0), lang); fputc(')', f);
		break;
	case N_DOTS:         fputs("...", f); break;
	case N_IGNORE: break;
	default:
		// Localize the missing construct for the fidelity gate.
		fprintf(f, "/*<unhandled %s>*/",
			c2mir_node_code_name((c2mir_node_code_t)n->code));
		break;
	}
}

} // namespace

void cir_emit_c(FILE *f, node_t tree, CirEmitLang lang)
{
	emit(f, tree, lang);
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
