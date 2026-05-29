/* cir_emit_c.cpp — render a cir_node tree (MC11-IR) to C source.
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

extern "C" {
#include "c2mir/c2mir_api.h"   // c2mir_node_op, c2mir_node_code_name
}

namespace {

inline node_t op(node_t n, int i) { return c2mir_node_op(n, i); }

void emit(FILE *f, node_t n, CirEmitLang lang);

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

// Emit a C declarator: id plus its suffix list (function params, etc.).
void emit_declarator(FILE *f, node_t decl, CirEmitLang lang)
{
	if (!decl) return;
	emit(f, op(decl, 0), lang);          // the identifier (or nothing for N_IGNORE)
	node_t suffixes = op(decl, 1);       // N_LIST of N_FUNC / pointer / N_ARR
	if (suffixes)
		for (int i = 0; ; i++) {
			node_t s = op(suffixes, i);
			if (!s) break;
			emit(f, s, lang);
		}
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
		emit_seq(f, op(n, 0), lang, 0, ", ");    // parameter list
		fputc(')', f);
		break;
	case N_TYPE:
		emit(f, op(n, 0), lang);                 // specifiers
		// declarator (abstract or named); op(1) may be N_DECL[N_IGNORE,...]
		if (op(n, 1) && op(op(n, 1), 0) && op(op(n, 1), 0)->code != N_IGNORE) {
			fputc(' ', f);
			emit_declarator(f, op(n, 1), lang);
		}
		break;
	case N_SPEC_DECL:
		emit(f, op(n, 0), lang);                 // specifiers
		fputc(' ', f);
		emit_declarator(f, op(n, 1), lang);      // declarator
		if (op(n, 2) && op(n, 2)->code != N_IGNORE) {
			fputs(" = ", f);
			emit(f, op(n, 2), lang);
		}
		fputc(';', f);
		break;
	case N_BLOCK:
		fputs("{\n", f);
		emit_seq(f, op(n, 1), lang, 0, "\n");     // [1] = statement list
		fputs("\n}", f);
		break;
	case N_RETURN:
		// [0] = label list, [1] = return expression (may be absent/N_IGNORE)
		fputs("return", f);
		if (op(n, 1) && op(n, 1)->code != N_IGNORE) {
			fputc(' ', f);
			emit(f, op(n, 1), lang);
		}
		fputc(';', f);
		break;
	case N_EXPR:
		// [0] = label list, [1] = expression
		emit(f, op(n, 1), lang);
		fputc(';', f);
		break;
	case N_ID:   fputs(n->u.s.s ? n->u.s.s : "", f); break;
	case N_I:
	case N_L:    fprintf(f, "%lld", (long long)n->u.l); break;
	case N_VOID: fputs("void", f); break;
	case N_CHAR: fputs("char", f); break;
	case N_INT:  fputs("int", f); break;
	case N_LONG: fputs("long", f); break;
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
