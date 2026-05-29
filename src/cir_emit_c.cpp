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
		case N_CASE:
			fputs("case ", f); emit(f, op(l, 0), lang); fputs(": ", f); break;
		case N_DEFAULT:
			fputs("default: ", f); break;
		default:
			break;
		}
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
		// [0]=specifiers (often N_SHARE-wrapped) [1]=declarator
		// [2],[3]=ignore (bit-field width etc.) [4]=initializer
		emit(f, op(n, 0), lang);
		fputc(' ', f);
		emit_declarator(f, op(n, 1), lang);
		if (op(n, 4) && op(n, 4)->code != N_IGNORE) {
			fputs(" = ", f);
			emit(f, op(n, 4), lang);
		}
		fputc(';', f);
		break;
	case N_SHARE:
		// single-operand wrapper around a type-specifier list
		emit(f, op(n, 0), lang);
		break;
	case N_ADD: case N_SUB: case N_MUL: case N_DIV: case N_MOD:
	case N_EQ:  case N_NE:  case N_LT:  case N_LE: case N_GT: case N_GE:
	case N_AND: case N_OR:  case N_XOR: case N_LSH: case N_RSH:
	case N_ANDAND: case N_OROR: case N_ASSIGN: {
		static const struct { int code; const char *o; } M[] = {
			{N_ADD,"+"},{N_SUB,"-"},{N_MUL,"*"},{N_DIV,"/"},{N_MOD,"%"},
			{N_EQ,"=="},{N_NE,"!="},{N_LT,"<"},{N_LE,"<="},{N_GT,">"},{N_GE,">="},
			{N_AND,"&"},{N_OR,"|"},{N_XOR,"^"},{N_LSH,"<<"},{N_RSH,">>"},
			{N_ANDAND,"&&"},{N_OROR,"||"},{N_ASSIGN,"="},{0,0}};
		const char *o = "?";
		for (int k = 0; M[k].o; k++) if (M[k].code == (int)n->code) o = M[k].o;
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
		if (op(n, 1) && op(n, 1)->code != N_IGNORE) emit(f, op(n, 1), lang);
		fputs("; ", f);
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
		// String literal: emit a C double-quoted literal, escaping as needed.
		// u.s.s holds the (interned) bytes; u.s.len includes the NUL terminator.
		const char *s = n->u.s.s;
		size_t len = n->u.s.len;
		if (len > 0 && s && s[len - 1] == '\0') len--;   // drop trailing NUL
		fputc('"', f);
		for (size_t i = 0; s && i < len; i++) {
			unsigned char c = (unsigned char)s[i];
			switch (c) {
			case '"':  fputs("\\\"", f); break;
			case '\\': fputs("\\\\", f); break;
			case '\n': fputs("\\n", f); break;
			case '\t': fputs("\\t", f); break;
			case '\r': fputs("\\r", f); break;
			default:
				if (c < 0x20 || c >= 0x7f) fprintf(f, "\\%03o", c);
				else fputc(c, f);
			}
		}
		fputc('"', f);
		break;
	}
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
	case N_ID:   fputs(n->u.s.s ? n->u.s.s : "", f); break;
	case N_STR16: case N_STR32: break; // wide strings: not supported (see c11-transpiler rule)
	case N_I:
	case N_L:    fprintf(f, "%lld", (long long)n->u.l); break;
	case N_VOID: fputs("void", f); break;
	case N_CHAR: fputs("char", f); break;
	case N_INT:  fputs("int", f); break;
	case N_LONG: fputs("long", f); break;
	case N_EXTERN:       fputs("extern", f); break;
	case N_STATIC:       fputs("static", f); break;
	case N_TYPEDEF:      fputs("typedef", f); break;
	case N_AUTO:         fputs("auto", f); break;
	case N_REGISTER:     fputs("register", f); break;
	case N_THREAD_LOCAL: fputs("_Thread_local", f); break;
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
