// madc_emit_c.cpp — C11 text emitter for madc's Gecko AST.
//
// Walks the Gecko gp_tree_node AST and emits C11 source text into
// a buffer.  The generated C is fed to c2mir → MIR → machine code.
//
// Follows the Ruby mirjit.c pattern: O(...) macro for printf-style
// output into a growing string buffer.

#include <cstdio>
#include <cstdarg>
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
#include "madc.h"

extern "C" {
#include "gecko.h"
}

// -----------------------------------------------------------------------
// Helpers for accessing Gecko AST nodes
// -----------------------------------------------------------------------

static bool is_anode(gp_tree_node *node, const char *name)
{
    return node && node->type == GP_ANODE &&
	   strcmp(node->val.anode.name, name) == 0;
}

static gp_tree_node *child(gp_tree_node *node, int n)
{
    if (!node || node->type != GP_ANODE ||
	n >= node->val.anode.children_num)
	return nullptr;
    return node->val.anode.children[n];
}

static int nchildren(gp_tree_node *node)
{
    if (!node || node->type != GP_ANODE) return 0;
    return node->val.anode.children_num;
}

static const char *anode_name(gp_tree_node *node)
{
    if (!node || node->type != GP_ANODE) return "";
    return node->val.anode.name;
}

static std::string term_text(gp_tree_node *node)
{
    if (!node || node->type != GP_TERM) return "";
    TokenBase *tb = (TokenBase *)node->val.term.attr;
    if (!tb) return "";
    TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
    return ti ? ti->str : "";
}

static int64_t term_ival(gp_tree_node *node)
{
    if (!node || node->type != GP_TERM) return 0;
    TokenBase *tb = (TokenBase *)node->val.term.attr;
    return tb ? tb->ival() : 0;
}

static double term_dval(gp_tree_node *node)
{
    if (!node || node->type != GP_TERM) return 0.0;
    TokenBase *tb = (TokenBase *)node->val.term.attr;
    return tb ? tb->dval() : 0.0;
}

static int term_code(gp_tree_node *node)
{
    if (!node || node->type != GP_TERM) return -1;
    return node->val.term.code;
}

// Gecko terminal codes (must match madc_grammar.cpp)
enum {
    GT_IDENT    = 256,
    GT_INTEGER  = 257,
    GT_REAL     = 258,
    GT_STRING   = 259,
    GT_CHAR_LIT = 260,
};

// -----------------------------------------------------------------------
// CEmitter — generates C11 text from a Gecko AST
// -----------------------------------------------------------------------

class CEmitter
{
    std::string header;      // extern declarations, typedefs
    std::string body;        // function definitions
    int indent_level;
    int tmp_counter;         // for generating unique temp var names

    // O() macro equivalent — append formatted text to body
    void O(const char *fmt, ...)
    {
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	body += buf;
    }

    // OH() — append to header section
    void OH(const char *fmt, ...)
    {
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	header += buf;
    }

    void emit_indent()
    {
	for (int i = 0; i < indent_level; i++)
	    body += "    ";
    }

    // Generate a unique temp variable name
    std::string tmp_var()
    {
	char buf[32];
	snprintf(buf, sizeof(buf), "__tmp_%d", tmp_counter++);
	return buf;
    }

    // ---------------------------------------------------------------
    // String escaping — re-escape a string for C source output
    // ---------------------------------------------------------------

    static std::string c_escape(const std::string &s)
    {
	std::string out;
	out.reserve(s.size() + 16);
	for (char c : s) {
	    switch (c) {
	    case '\n': out += "\\n"; break;
	    case '\r': out += "\\r"; break;
	    case '\t': out += "\\t"; break;
	    case '\\': out += "\\\\"; break;
	    case '"':  out += "\\\""; break;
	    case '\0': out += "\\0"; break;
	    default:
		if ((unsigned char)c < 32)
		    { char buf[8]; snprintf(buf, sizeof(buf), "\\x%02x", (unsigned char)c); out += buf; }
		else
		    out += c;
	    }
	}
	return out;
    }

    // ---------------------------------------------------------------
    // Builtin name mapping — translates madc builtin names to their
    // extern "C" wrapper names in libmadc.
    // ---------------------------------------------------------------

    static std::string map_builtin(const std::string &name)
    {
	// madc builtins → libmadc extern "C" wrappers
	if (name == "puti")     return "madc_puti";
	if (name == "putu")     return "madc_putu";
	if (name == "putd")     return "madc_putd";
	if (name == "putf")     return "madc_putf";
	if (name == "printstr") return "madc_printstr";
	// These are already C-compatible names:
	// puts, printf, putchar, malloc, free, strlen, etc.
	return name;
    }

    // ---------------------------------------------------------------
    // Type emission
    // ---------------------------------------------------------------

    std::string emit_type(gp_tree_node *node)
    {
	if (!node) return "int";

	if (node->type == GP_TERM) {
	    int code = term_code(node);
	    switch (code) {
	    case 310: return "void";
	    case 311: return "int";       // bool → int in C
	    case 312: return "char";
	    case 313: return "short";
	    case 314: return "int";
	    case 315: return "long";
	    case 316: return "float";
	    case 317: return "double";
	    case 318: return "madc_string_t"; // string type (TODO)
	    case 319: return "int8_t";
	    case 320: return "int16_t";
	    case 321: return "int32_t";
	    case 322: return "int64_t";
	    case 323: return "uint8_t";
	    case 324: return "uint16_t";
	    case 325: return "uint32_t";
	    case 326: return "uint64_t";
	    case 285: return "signed";
	    case 286: return "unsigned";
	    case GT_IDENT: return term_text(node);
	    default: return "int";
	    }
	}

	if (node->type == GP_ANODE) {
	    const char *name = anode_name(node);
	    if (strcmp(name, "ptr_type") == 0)
		return emit_type(child(node, 0)) + " *";
	    if (strcmp(name, "ref_type") == 0)
		return emit_type(child(node, 0)) + " *"; // refs → pointers in C
	    if (strcmp(name, "const_type") == 0)
		return "const " + emit_type(child(node, 0));
	    if (strcmp(name, "type_const") == 0)
		return emit_type(child(node, 0)) + " const";
	    if (strcmp(name, "qual_type") == 0) {
		std::string qual = term_text(child(node, 0));
		return qual + " " + emit_type(child(node, 1));
	    }
	    if (strcmp(name, "struct_type") == 0)
		return "struct " + term_text(child(node, 0));
	    if (strcmp(name, "class_type") == 0)
		return term_text(child(node, 0)) + "_t"; // class → typedef'd struct
	    if (strcmp(name, "enum_type") == 0)
		return "enum " + term_text(child(node, 0));
	}

	return "int"; // fallback
    }

    // ---------------------------------------------------------------
    // Expression emission — returns the C expression as a string
    // ---------------------------------------------------------------

    std::string emit_expr(gp_tree_node *node)
    {
	if (!node) return "0";

	// Terminal: literal or identifier
	if (node->type == GP_TERM) {
	    int code = term_code(node);
	    if (code == GT_IDENT) return term_text(node);
	    if (code == GT_INTEGER) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%lld", (long long)term_ival(node));
		return buf;
	    }
	    if (code == GT_REAL) {
		char buf[64];
		snprintf(buf, sizeof(buf), "%.17g", term_dval(node));
		return buf;
	    }
	    if (code == GT_STRING) {
		// Lexer stores content without quotes and with escapes
		// resolved — re-escape for C output
		return "\"" + c_escape(term_text(node)) + "\"";
	    }
	    if (code == GT_CHAR_LIT) {
		// TokenChar stores the char value in _token, not in str
		int64_t ch = term_ival(node);
		char buf[8];
		if (ch == '\n') return "'\\n'";
		if (ch == '\t') return "'\\t'";
		if (ch == '\r') return "'\\r'";
		if (ch == '\\') return "'\\\\'";
		if (ch == '\'') return "'\\''";
		if (ch == 0)    return "'\\0'";
		snprintf(buf, sizeof(buf), "'%c'", (char)ch);
		return buf;
	    }
	    return term_text(node);
	}

	if (node->type == GP_NIL) return "0";

	const char *name = anode_name(node);

	// Parenthesized expression
	if (strcmp(name, "paren") == 0)
	    return "(" + emit_expr(child(node, 0)) + ")";

	// Binary operators
	struct { const char *node_name; const char *c_op; } binops[] = {
	    {"add", "+"}, {"sub", "-"}, {"mul", "*"}, {"div", "/"},
	    {"mod", "%"}, {"bsl", "<<"}, {"bsr", ">>"},
	    {"bitand", "&"}, {"bitor", "|"}, {"bitxor", "^"},
	    {"lor", "||"}, {"land", "&&"},
	    {"eq", "=="}, {"ne", "!="}, {"lt", "<"}, {"gt", ">"},
	    {"le", "<="}, {"ge", ">="}, {"three_way", "-"}, // spaceship approx
	    {nullptr, nullptr}
	};
	for (int i = 0; binops[i].node_name; i++) {
	    if (strcmp(name, binops[i].node_name) == 0) {
		return "(" + emit_expr(child(node, 0)) + " " +
		       binops[i].c_op + " " +
		       emit_expr(child(node, 1)) + ")";
	    }
	}

	// Assignment operators
	struct { const char *node_name; const char *c_op; } assigns[] = {
	    {"assign", "="}, {"add_assign", "+="}, {"sub_assign", "-="},
	    {"mul_assign", "*="}, {"div_assign", "/="}, {"mod_assign", "%="},
	    {"bsl_assign", "<<="}, {"bsr_assign", ">>="}, {"band_assign", "&="},
	    {"bor_assign", "|="}, {"xor_assign", "^="},
	    {nullptr, nullptr}
	};
	for (int i = 0; assigns[i].node_name; i++) {
	    if (strcmp(name, assigns[i].node_name) == 0) {
		return emit_expr(child(node, 0)) + " " +
		       assigns[i].c_op + " " +
		       emit_expr(child(node, 1));
	    }
	}

	// Unary operators
	if (strcmp(name, "neg") == 0)
	    return "(-" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "pos") == 0)
	    return "(+" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "lnot") == 0)
	    return "(!" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "bnot") == 0)
	    return "(~" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "deref") == 0)
	    return "(*" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "addrof") == 0)
	    return "(&" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "pre_inc") == 0)
	    return "(++" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "pre_dec") == 0)
	    return "(--" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "post_inc") == 0)
	    return "(" + emit_expr(child(node, 0)) + "++)";
	if (strcmp(name, "post_dec") == 0)
	    return "(" + emit_expr(child(node, 0)) + "--)";

	// Ternary
	if (strcmp(name, "ternary") == 0)
	    return "(" + emit_expr(child(node, 0)) + " ? " +
		   emit_expr(child(node, 1)) + " : " +
		   emit_expr(child(node, 2)) + ")";

	// Function call
	if (strcmp(name, "call") == 0) {
	    std::string func = emit_expr(child(node, 0));
	    std::string args = emit_arg_list(child(node, 1));
	    // Map madc builtins to their extern "C" wrapper names
	    func = map_builtin(func);
	    return func + "(" + args + ")";
	}

	// Member access
	if (strcmp(name, "member") == 0)
	    return emit_expr(child(node, 0)) + "." + term_text(child(node, 1));
	if (strcmp(name, "arrow_member") == 0)
	    return emit_expr(child(node, 0)) + "->" + term_text(child(node, 1));

	// Method call
	if (strcmp(name, "method_call") == 0) {
	    std::string obj = emit_expr(child(node, 0));
	    std::string method = term_text(child(node, 1));
	    std::string args = emit_arg_list(child(node, 2));
	    // TODO: emit as ClassName__method(&obj, args) after sema
	    return obj + "." + method + "(" + args + ")";
	}
	if (strcmp(name, "arrow_call") == 0) {
	    std::string obj = emit_expr(child(node, 0));
	    std::string method = term_text(child(node, 1));
	    std::string args = emit_arg_list(child(node, 2));
	    return obj + "->" + method + "(" + args + ")";
	}

	// Subscript
	if (strcmp(name, "subscript") == 0)
	    return emit_expr(child(node, 0)) + "[" + emit_expr(child(node, 1)) + "]";

	// Namespace call: ns::func(args)
	if (strcmp(name, "ns_call") == 0) {
	    std::string ns = term_text(child(node, 0));
	    std::string func = term_text(child(node, 1));
	    std::string args = emit_arg_list(child(node, 2));
	    return "__" + ns + "_" + func + "(" + args + ")";
	}
	if (strcmp(name, "ns_name") == 0) {
	    std::string ns = term_text(child(node, 0));
	    std::string func = term_text(child(node, 1));
	    return "__" + ns + "_" + func;
	}

	// Cast
	if (strcmp(name, "cast") == 0)
	    return "((" + emit_type(child(node, 0)) + ")" +
		   emit_expr(child(node, 1)) + ")";

	// sizeof
	if (strcmp(name, "sizeof_type") == 0)
	    return "sizeof(" + emit_type(child(node, 0)) + ")";
	if (strcmp(name, "sizeof_expr") == 0)
	    return "sizeof(" + emit_expr(child(node, 0)) + ")";

	// new/delete
	if (strcmp(name, "new_plain") == 0)
	    return "malloc(sizeof(" + term_text(child(node, 0)) + "_t))";
	if (strcmp(name, "new_ctor") == 0) {
	    std::string type = term_text(child(node, 0));
	    return "malloc(sizeof(" + type + "_t))"; // TODO: call ctor
	}

	// Comma operator
	if (strcmp(name, "comma") == 0)
	    return "(" + emit_expr(child(node, 0)) + ", " +
		   emit_expr(child(node, 1)) + ")";

	// Compound literal
	if (strcmp(name, "compound_lit") == 0)
	    return "((" + emit_type(child(node, 0)) + "){" +
		   emit_init_list(child(node, 1)) + "})";

	// Init declaration (in expression context)
	if (strcmp(name, "init_decl") == 0)
	    return emit_expr(child(node, 1));

	// Fallback: if it has children, try first child
	if (nchildren(node) > 0)
	    return emit_expr(child(node, 0));

	return "/* unknown expr: " + std::string(name) + " */0";
    }

    // Emit argument list (flattened from nested arg_list nodes)
    std::string emit_arg_list(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";
	if (node->type == GP_TERM) return emit_expr(node);
	if (!is_anode(node, "arg_list"))
	    return emit_expr(node);
	return emit_arg_list(child(node, 0)) + ", " +
	       emit_expr(child(node, 1));
    }

    // Emit initializer list
    std::string emit_init_list(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";
	if (!is_anode(node, "init_seq"))
	    return emit_expr(node);
	return emit_init_list(child(node, 0)) + ", " +
	       emit_expr(child(node, 1));
    }

    // ---------------------------------------------------------------
    // Statement emission
    // ---------------------------------------------------------------

    void emit_stmt(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;

	// Terminal — could be an expression statement
	if (node->type == GP_TERM) {
	    emit_indent();
	    O("%s;\n", emit_expr(node).c_str());
	    return;
	}

	const char *name = anode_name(node);

	// Statement list
	if (strcmp(name, "stmt_list") == 0) {
	    emit_stmt(child(node, 0));
	    emit_stmt(child(node, 1));
	    return;
	}

	// Expression statement
	if (strcmp(name, "expr_stmt") == 0) {
	    emit_indent();
	    O("%s;\n", emit_expr(child(node, 0)).c_str());
	    return;
	}

	// Block
	if (strcmp(name, "block") == 0) {
	    emit_indent();
	    O("{\n");
	    indent_level++;
	    emit_stmt(child(node, 0));
	    indent_level--;
	    emit_indent();
	    O("}\n");
	    return;
	}

	// Declaration
	if (strcmp(name, "decl") == 0) {
	    emit_decl(node);
	    return;
	}

	// If/else
	if (strcmp(name, "if") == 0) {
	    emit_indent();
	    O("if (%s)\n", emit_expr(child(node, 0)).c_str());
	    indent_level++;
	    emit_stmt(child(node, 1));
	    indent_level--;
	    return;
	}
	if (strcmp(name, "if_else") == 0) {
	    emit_indent();
	    O("if (%s)\n", emit_expr(child(node, 0)).c_str());
	    indent_level++;
	    emit_stmt(child(node, 1));
	    indent_level--;
	    emit_indent();
	    O("else\n");
	    indent_level++;
	    emit_stmt(child(node, 2));
	    indent_level--;
	    return;
	}

	// While
	if (strcmp(name, "while") == 0) {
	    emit_indent();
	    O("while (%s)\n", emit_expr(child(node, 0)).c_str());
	    indent_level++;
	    emit_stmt(child(node, 1));
	    indent_level--;
	    return;
	}

	// Do-while
	if (strcmp(name, "do_while") == 0) {
	    emit_indent();
	    O("do\n");
	    indent_level++;
	    emit_stmt(child(node, 0));
	    indent_level--;
	    emit_indent();
	    O("while (%s);\n", emit_expr(child(node, 1)).c_str());
	    return;
	}

	// For loop
	if (strcmp(name, "for") == 0) {
	    emit_indent();
	    std::string init, cond, iter;
	    gp_tree_node *init_node = child(node, 0);
	    gp_tree_node *cond_node = child(node, 1);
	    gp_tree_node *iter_node = child(node, 2);

	    if (init_node && init_node->type != GP_NIL) {
		if (is_anode(init_node, "decl"))
		    init = emit_decl_inline(init_node);
		else
		    init = emit_expr(init_node);
	    }
	    if (cond_node && cond_node->type != GP_NIL)
		cond = emit_expr(cond_node);
	    if (iter_node && iter_node->type != GP_NIL)
		iter = emit_expr(iter_node);

	    O("for (%s; %s; %s)\n", init.c_str(), cond.c_str(), iter.c_str());
	    indent_level++;
	    emit_stmt(child(node, 3));
	    indent_level--;
	    return;
	}

	// Return
	if (strcmp(name, "return") == 0) {
	    emit_indent();
	    O("return;\n");
	    return;
	}
	if (strcmp(name, "return_val") == 0) {
	    emit_indent();
	    O("return %s;\n", emit_expr(child(node, 0)).c_str());
	    return;
	}

	// Break / Continue
	if (strcmp(name, "break") == 0) {
	    emit_indent();
	    O("break;\n");
	    return;
	}
	if (strcmp(name, "continue") == 0) {
	    emit_indent();
	    O("continue;\n");
	    return;
	}

	// Goto / Label
	if (strcmp(name, "goto") == 0) {
	    emit_indent();
	    O("goto %s;\n", term_text(child(node, 0)).c_str());
	    return;
	}
	if (strcmp(name, "label") == 0) {
	    O("%s:;\n", term_text(child(node, 0)).c_str());
	    return;
	}

	// Switch
	if (strcmp(name, "switch") == 0) {
	    emit_indent();
	    O("switch (%s) {\n", emit_expr(child(node, 0)).c_str());
	    emit_case_list(child(node, 1));
	    emit_indent();
	    O("}\n");
	    return;
	}

	// Try/catch — emit as setjmp/longjmp (simplified for now)
	if (strcmp(name, "try") == 0) {
	    emit_indent();
	    O("/* try */ {\n");
	    indent_level++;
	    emit_stmt(child(node, 0));
	    indent_level--;
	    emit_indent();
	    O("}\n");
	    // TODO: catch clauses via setjmp/longjmp
	    return;
	}

	// Throw
	if (strcmp(name, "throw_expr") == 0) {
	    emit_indent();
	    O("/* throw */ ;\n"); // TODO: longjmp
	    return;
	}

	// Delete
	if (strcmp(name, "delete") == 0) {
	    emit_indent();
	    O("free(%s);\n", emit_expr(child(node, 0)).c_str());
	    return;
	}

	// Defer
	if (strcmp(name, "defer") == 0) {
	    emit_indent();
	    O("/* defer */ ;\n"); // TODO: goto-cleanup
	    return;
	}

	// Fallback: try to emit as expression
	emit_indent();
	O("/* TODO stmt: %s */\n", name);
    }

    // ---------------------------------------------------------------
    // Declaration emission
    // ---------------------------------------------------------------

    void emit_decl(gp_tree_node *node)
    {
	std::string type = emit_type(child(node, 0));
	emit_declarator_list(type, child(node, 1));
    }

    // Emit a declarator list as C statements
    void emit_declarator_list(const std::string &type, gp_tree_node *node)
    {
	if (!node) return;

	if (node->type == GP_TERM) {
	    // Simple: type name;
	    emit_indent();
	    O("%s %s;\n", type.c_str(), term_text(node).c_str());
	    return;
	}

	const char *name = anode_name(node);

	if (strcmp(name, "init_decl") == 0) {
	    emit_indent();
	    O("%s %s = %s;\n", type.c_str(),
	      term_text(child(node, 0)).c_str(),
	      emit_expr(child(node, 1)).c_str());
	    return;
	}

	if (strcmp(name, "array_decl") == 0) {
	    emit_indent();
	    O("%s %s[%s];\n", type.c_str(),
	      term_text(child(node, 0)).c_str(),
	      emit_expr(child(node, 1)).c_str());
	    return;
	}

	if (strcmp(name, "unsized_array") == 0) {
	    emit_indent();
	    O("%s %s[];\n", type.c_str(),
	      term_text(child(node, 0)).c_str());
	    return;
	}

	if (strcmp(name, "array_init_decl") == 0) {
	    emit_indent();
	    O("%s %s[%s] = %s;\n", type.c_str(),
	      term_text(child(node, 0)).c_str(),
	      emit_expr(child(node, 1)).c_str(),
	      emit_expr(child(node, 2)).c_str());
	    return;
	}

	if (strcmp(name, "unsized_array_init") == 0) {
	    emit_indent();
	    O("%s %s[] = %s;\n", type.c_str(),
	      term_text(child(node, 0)).c_str(),
	      emit_expr(child(node, 1)).c_str());
	    return;
	}

	if (strcmp(name, "decl_list") == 0) {
	    emit_declarator_list(type, child(node, 0));
	    emit_declarator_list(type, child(node, 1));
	    return;
	}

	// Fallback
	emit_indent();
	O("%s /* unknown decl */;\n", type.c_str());
    }

    // Emit a declaration as an inline string (for for-loop init)
    std::string emit_decl_inline(gp_tree_node *node)
    {
	std::string type = emit_type(child(node, 0));
	gp_tree_node *decl = child(node, 1);
	if (!decl) return type;

	if (decl->type == GP_TERM)
	    return type + " " + term_text(decl);

	const char *name = anode_name(decl);
	if (strcmp(name, "init_decl") == 0)
	    return type + " " + term_text(child(decl, 0)) + " = " +
		   emit_expr(child(decl, 1));

	return type;
    }

    // ---------------------------------------------------------------
    // Switch case emission
    // ---------------------------------------------------------------

    void emit_case_list(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;
	if (is_anode(node, "case_list")) {
	    emit_case_list(child(node, 0));
	    emit_case_clause(child(node, 1));
	    return;
	}
	emit_case_clause(node);
    }

    void emit_case_clause(gp_tree_node *node)
    {
	if (!node) return;
	const char *name = anode_name(node);
	if (strcmp(name, "case") == 0) {
	    emit_indent();
	    O("case %s:\n", emit_expr(child(node, 0)).c_str());
	    indent_level++;
	    emit_case_stmts(child(node, 1));
	    indent_level--;
	} else if (strcmp(name, "default") == 0) {
	    emit_indent();
	    O("default:\n");
	    indent_level++;
	    emit_case_stmts(child(node, 0));
	    indent_level--;
	}
    }

    void emit_case_stmts(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;
	if (is_anode(node, "case_stmts")) {
	    emit_case_stmts(child(node, 0));
	    emit_stmt(child(node, 1));
	    return;
	}
	emit_stmt(node);
    }

    // ---------------------------------------------------------------
    // Function emission
    // ---------------------------------------------------------------

    void emit_func_def(gp_tree_node *node)
    {
	std::string ret_type = emit_type(child(node, 0));
	std::string func_name = term_text(child(node, 1));
	std::string params = emit_param_list(child(node, 2));
	gp_tree_node *body_node = child(node, 3);

	// Forward declaration in header
	OH("%s %s(%s);\n", ret_type.c_str(), func_name.c_str(),
	   params.empty() ? "void" : params.c_str());

	// Function body
	O("%s %s(%s)\n", ret_type.c_str(), func_name.c_str(),
	  params.empty() ? "void" : params.c_str());

	// Emit body (block node)
	indent_level = 0;
	emit_stmt(body_node);
	O("\n");
    }

    std::string emit_param_list(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";

	if (node->type == GP_TERM)
	    return emit_type(node); // unnamed parameter

	const char *name = anode_name(node);

	if (strcmp(name, "param") == 0)
	    return emit_type(child(node, 0)) + " " +
		   term_text(child(node, 1));

	if (strcmp(name, "param_list") == 0) {
	    std::string left = emit_param_list(child(node, 0));
	    std::string right = emit_param_list(child(node, 1));
	    return left + ", " + right;
	}

	if (strcmp(name, "ellipsis_param") == 0)
	    return "...";

	return emit_type(node);
    }

    // ---------------------------------------------------------------
    // Struct emission
    // ---------------------------------------------------------------

    void emit_struct_def(gp_tree_node *node)
    {
	std::string struct_name = term_text(child(node, 0));
	OH("struct %s {\n", struct_name.c_str());
	emit_struct_body_to_header(child(node, 1));
	OH("};\n\n");
    }

    void emit_struct_body_to_header(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;
	if (is_anode(node, "struct_body")) {
	    emit_struct_body_to_header(child(node, 0));
	    emit_struct_field(child(node, 1));
	}
    }

    void emit_struct_field(gp_tree_node *node)
    {
	if (!node) return;
	if (is_anode(node, "struct_field")) {
	    std::string type = emit_type(child(node, 0));
	    // TODO: handle field_list with multiple fields
	    gp_tree_node *fld = child(node, 1);
	    if (fld && fld->type == GP_TERM) {
		OH("    %s %s;\n", type.c_str(), term_text(fld).c_str());
	    } else if (is_anode(fld, "field_array")) {
		OH("    %s %s[%s];\n", type.c_str(),
		   term_text(child(fld, 0)).c_str(),
		   emit_expr(child(fld, 1)).c_str());
	    } else if (is_anode(fld, "field_ptr")) {
		OH("    %s *%s;\n", type.c_str(),
		   term_text(child(fld, 0)).c_str());
	    }
	}
    }

    // ---------------------------------------------------------------
    // Enum emission
    // ---------------------------------------------------------------

    void emit_enum_def(gp_tree_node *node)
    {
	std::string enum_name = term_text(child(node, 0));
	OH("enum %s {\n", enum_name.c_str());
	emit_enum_body(child(node, 1));
	OH("};\n\n");
    }

    void emit_enum_body(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;
	if (is_anode(node, "enum_list")) {
	    emit_enum_body(child(node, 0));
	    OH(",\n");
	    emit_enum_val(child(node, 1));
	    return;
	}
	emit_enum_val(node);
    }

    void emit_enum_val(gp_tree_node *node)
    {
	if (!node) return;
	if (node->type == GP_TERM) {
	    OH("    %s", term_text(node).c_str());
	} else if (is_anode(node, "enum_assign")) {
	    OH("    %s = %s", term_text(child(node, 0)).c_str(),
	       emit_expr(child(node, 1)).c_str());
	}
    }

    // ---------------------------------------------------------------
    // Top-level dispatch
    // ---------------------------------------------------------------

    void emit_top_level(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;

	// Translation unit: iterate children
	if (is_anode(node, "tu")) {
	    emit_top_level(child(node, 0));
	    emit_top_level(child(node, 1));
	    return;
	}

	// Function definition
	if (is_anode(node, "func_def")) {
	    emit_func_def(node);
	    return;
	}

	// Function prototype
	if (is_anode(node, "func_proto")) {
	    std::string ret = emit_type(child(node, 0));
	    std::string name = term_text(child(node, 1));
	    std::string params = emit_param_list(child(node, 2));
	    // Don't add extern if type already includes it
	    const char *prefix = (ret.find("extern") == std::string::npos) ? "extern " : "";
	    OH("%s%s %s(%s);\n", prefix, ret.c_str(), name.c_str(),
	       params.empty() ? "void" : params.c_str());
	    return;
	}

	// Global declaration
	if (is_anode(node, "decl")) {
	    // Emit as global variable in header
	    std::string type = emit_type(child(node, 0));
	    gp_tree_node *d = child(node, 1);
	    if (d && d->type == GP_TERM) {
		OH("%s %s;\n", type.c_str(), term_text(d).c_str());
	    } else if (is_anode(d, "init_decl")) {
		OH("%s %s = %s;\n", type.c_str(),
		   term_text(child(d, 0)).c_str(),
		   emit_expr(child(d, 1)).c_str());
	    }
	    return;
	}

	// Struct definition
	if (is_anode(node, "struct_def")) {
	    emit_struct_def(node);
	    return;
	}

	// Enum definition
	if (is_anode(node, "enum_def") || is_anode(node, "anon_enum")) {
	    emit_enum_def(node);
	    return;
	}

	// Typedef variants
	if (is_anode(node, "typedef")) {
	    OH("typedef %s %s;\n",
	       emit_type(child(node, 0)).c_str(),
	       term_text(child(node, 1)).c_str());
	    return;
	}
	if (is_anode(node, "typedef_struct")) {
	    OH("typedef struct %s %s;\n",
	       term_text(child(node, 0)).c_str(),
	       term_text(child(node, 1)).c_str());
	    return;
	}
	if (is_anode(node, "typedef_struct_def")) {
	    // typedef struct Name { ... } Alias;
	    std::string sname = term_text(child(node, 0));
	    std::string alias = term_text(child(node, 2));
	    OH("typedef struct %s {\n", sname.c_str());
	    emit_struct_body_to_header(child(node, 1));
	    OH("} %s;\n\n", alias.c_str());
	    return;
	}
	if (is_anode(node, "typedef_anon_struct")) {
	    // typedef struct { ... } Alias;
	    std::string alias = term_text(child(node, 1));
	    OH("typedef struct {\n");
	    emit_struct_body_to_header(child(node, 0));
	    OH("} %s;\n\n", alias.c_str());
	    return;
	}
	if (is_anode(node, "typedef_union_def")) {
	    std::string uname = term_text(child(node, 0));
	    std::string alias = term_text(child(node, 2));
	    OH("typedef union %s {\n", uname.c_str());
	    emit_struct_body_to_header(child(node, 1));
	    OH("} %s;\n\n", alias.c_str());
	    return;
	}
	if (is_anode(node, "typedef_anon_union")) {
	    std::string alias = term_text(child(node, 1));
	    OH("typedef union {\n");
	    emit_struct_body_to_header(child(node, 0));
	    OH("} %s;\n\n", alias.c_str());
	    return;
	}
	if (is_anode(node, "typedef_enum_def")) {
	    std::string ename = term_text(child(node, 0));
	    std::string alias = term_text(child(node, 2));
	    OH("typedef enum %s {\n", ename.c_str());
	    emit_enum_body(child(node, 1));
	    OH("\n} %s;\n\n", alias.c_str());
	    return;
	}
	if (is_anode(node, "typedef_anon_enum")) {
	    std::string alias = term_text(child(node, 1));
	    OH("typedef enum {\n");
	    emit_enum_body(child(node, 0));
	    OH("\n} %s;\n\n", alias.c_str());
	    return;
	}

	// Class — TODO in Phase 5
	if (is_anode(node, "class_def") || is_anode(node, "class_inherit")) {
	    OH("/* TODO: class %s */\n", term_text(child(node, 0)).c_str());
	    return;
	}

	// Pass-through: single child
	if (node->type == GP_ANODE && nchildren(node) == 1) {
	    emit_top_level(child(node, 0));
	    return;
	}

	// Terminal at top level — skip
	if (node->type == GP_TERM) return;

	O("/* unhandled top-level: %s */\n", anode_name(node));
    }

public:
    CEmitter() : indent_level(0), tmp_counter(0) {}

    // Generate C11 text from a Gecko AST.
    // Returns the complete C source as a string.
    std::string emit(gp_tree_node *root)
    {
	header.clear();
	body.clear();

	// Standard C header for generated code
	header += "/* Generated by madc transpiler */\n";
	header += "#include <stdint.h>\n";
	header += "#include <stddef.h>\n";
	header += "\n";

	// madc runtime declarations — auto-included for every program.
	// These are exported from libmadc with extern "C" linkage.
	header += "/* madc runtime builtins */\n";
	header += "extern void madc_puti(int64_t);\n";
	header += "extern void madc_putu(uint64_t);\n";
	header += "extern void madc_putd(double);\n";
	header += "extern void madc_putf(float);\n";
	header += "extern void madc_puts(const char *);\n";
	header += "extern void madc_printstr(const char *);\n";
	header += "extern int putchar(int);\n";
	header += "extern int printf(const char *, ...);\n";
	header += "extern int puts(const char *);\n";
	header += "extern void *malloc(unsigned long);\n";
	header += "extern void *calloc(unsigned long, unsigned long);\n";
	header += "extern void free(void *);\n";
	header += "extern void *memcpy(void *, const void *, unsigned long);\n";
	header += "extern void *memset(void *, int, unsigned long);\n";
	header += "extern unsigned long strlen(const char *);\n";
	header += "extern int strcmp(const char *, const char *);\n";
	header += "extern int atoi(const char *);\n";
	header += "extern double atof(const char *);\n";
	header += "\n";

	emit_top_level(root);

	return header + "\n" + body;
    }
};

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

std::string madc_emit_c(gp_tree_node *root)
{
    CEmitter emitter;
    return emitter.emit(root);
}
