// madc_emit_c.cpp — C11 text emitter for madc's Gecko AST.
//
// Walks the Gecko gp_tree_node AST and emits C11 source text into
// a buffer.  The generated C is fed to c2mir → MIR → machine code.
//
// Updated for the ANSI C-based grammar (pointer on declarator, not type).

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
#include <set>
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

#include "madc_sema.h"

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

static bool is_nil(gp_tree_node *node)
{
    return !node || node->type == GP_NIL;
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
    int tmp_counter;

    // Semantic pre-pass data (populated before emission)
    SemaInfo *sema;

    // Namespace function names collected during emission (for extern decls)
    std::set<std::string> ns_funcs_used;

    // Per-function local variable type overlay (cleared each function)
    // Augments sema->var_types with function-local declarations
    std::map<std::string, char> local_var_types;

    // Per-function local class var map overlay
    std::map<std::string, std::string> local_var_class_map;

    // Class vars in current scope needing destructor injection (LIFO)
    std::vector<std::string> scope_class_vars;

    // Look up variable type: local overlay first, then sema
    char lookup_var_type(const std::string &name) {
	auto it = local_var_types.find(name);
	if (it != local_var_types.end()) return it->second;
	if (sema) return sema->get_var_type(name);
	return 'i';
    }

    // Look up function return type: sema first
    char lookup_func_ret_type(const std::string &name) {
	if (sema) return sema->get_func_ret_type(name);
	return 'i';
    }

    // Check if a name is a known class
    bool is_known_class(const std::string &name) {
	if (sema) return sema->is_class(name);
	return false;
    }

    // Check if class has constructor
    bool class_has_ctor(const std::string &name) {
	if (sema) return sema->classes_with_ctor.count(name) > 0;
	return false;
    }

    // Check if class has destructor
    bool class_has_dtor(const std::string &name) {
	if (sema) return sema->classes_with_dtor.count(name) > 0;
	return false;
    }

    // Get class info for a variable
    std::string lookup_var_class(const std::string &name) {
	auto it = local_var_class_map.find(name);
	if (it != local_var_class_map.end()) return it->second;
	if (sema) return sema->get_var_class(name);
	return "";
    }

    void O(const char *fmt, ...)
    {
	char buf[4096];
	va_list ap;
	va_start(ap, fmt);
	vsnprintf(buf, sizeof(buf), fmt, ap);
	va_end(ap);
	body += buf;
    }

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

    std::string tmp_var()
    {
	char buf[32];
	snprintf(buf, sizeof(buf), "__tmp_%d", tmp_counter++);
	return buf;
    }

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

    // Map namespace names to their registered prefix abbreviation
    static std::string ns_prefix(const std::string &ns)
    {
	if (ns == "python" || ns == "py") return "py";
	if (ns == "ruby"   || ns == "rb") return "rb";
	return ns;  // php, perl, js, rust use their full name
    }

    static std::string map_builtin(const std::string &name)
    {
	if (name == "puti")     return "madc_puti";
	if (name == "putu")     return "madc_putu";
	if (name == "putd")     return "madc_putd";
	if (name == "putf")     return "madc_putf";
	if (name == "printstr") return "madc_printstr";
	return name;
    }

    // ---------------------------------------------------------------
    // Type emission — handles declaration_specifiers (qual chains),
    // terminals (INT, CHAR, etc.), and type_name nodes.
    // ---------------------------------------------------------------

    std::string emit_type(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";

	// Terminal: keyword type
	if (node->type == GP_TERM) {
	    int code = term_code(node);
	    switch (code) {
	    case 277: return "typedef";
	    case 279: return "static";
	    case 280: return "extern";
	    case 281: return "const";
	    case 282: return "volatile";
	    case 283: return "register";
	    case 284: return "inline";
	    case 285: return "signed";
	    case 286: return "unsigned";
	    case 287: return "auto";
	    case 293: return "/* virtual */";
	    case 302: return "restrict";
	    case 310: return "void";
	    case 311: return "int";       // bool → int in C
	    case 312: return "char";
	    case 313: return "short";
	    case 314: return "int";
	    case 315: return "long";
	    case 316: return "float";
	    case 317: return "double";
	    case 318: return "const char *";
	    case 319: return "int8_t";
	    case 320: return "int16_t";
	    case 321: return "int32_t";
	    case 322: return "int64_t";
	    case 323: return "uint8_t";
	    case 324: return "uint16_t";
	    case 325: return "uint32_t";
	    case 326: return "uint64_t";
	    // Stream types → opaque pointers (typedef'd in preamble)
	    case 340: return "ifstream";
	    case 341: return "ofstream";
	    case 342: return "fstream";
	    case 343: return "stringstream";
	    case 344: return "ostream";
	    // Container types → opaque pointers
	    case 330: return "void *";  // vector
	    case 331: return "void *";  // map
	    case 332: return "void *";  // set
	    case 333: return "void *";  // list
	    case GT_IDENT: {
		std::string t = term_text(node);
		if (is_known_class(t))
		    return "struct " + t;
		return t;
	    }
	    default: {
		std::string t = term_text(node);
		return t.empty() ? "int" : t;
	    }
	    }
	}

	if (node->type != GP_ANODE) return "int";

	const char *name = anode_name(node);

	// qual(specifier, rest) — recursive declaration_specifiers chain
	if (strcmp(name, "qual") == 0) {
	    std::string spec = emit_type(child(node, 0));
	    gp_tree_node *rest = child(node, 1);
	    if (is_nil(rest)) return spec;
	    std::string r = emit_type(rest);
	    if (spec.empty()) return r;
	    if (r.empty()) return spec;
	    return spec + " " + r;
	}

	// struct/union reference: struct foo
	if (strcmp(name, "struct_ref") == 0) {
	    std::string su = term_text(child(node, 0));  // "struct" or "union"
	    std::string nm = term_text(child(node, 1));
	    return su + " " + nm;
	}

	// struct/union definition (inline): struct foo { ... }
	if (strcmp(name, "struct_def") == 0) {
	    std::string su = term_text(child(node, 0));
	    gp_tree_node *nm = child(node, 1);
	    std::string sname = is_nil(nm) ? "" : (" " + term_text(nm));
	    std::string result = su + sname + " { ";
	    result += emit_struct_body_inline(child(node, 2));
	    result += " }";
	    return result;
	}

	// enum reference: enum foo
	if (strcmp(name, "enum_ref") == 0)
	    return "enum " + term_text(child(node, 0));

	// enum definition (inline) — emit the full body
	if (strcmp(name, "enum_def") == 0) {
	    gp_tree_node *nm = child(node, 0);
	    std::string ename = is_nil(nm) ? "" : (" " + term_text(nm));
	    std::string result = "enum" + ename + " { ";
	    result += emit_enum_body_inline(child(node, 1));
	    result += " }";
	    return result;
	}

	// type_name(specifier_qualifier_list, abstract_declarator_opt)
	if (strcmp(name, "type_name") == 0) {
	    std::string base = emit_type(child(node, 0));
	    gp_tree_node *abs = child(node, 1);
	    if (is_nil(abs)) return base;
	    return base + " " + emit_abstract_declarator(abs);
	}

	// container types
	if (strcmp(name, "vector_type") == 0 ||
	    strcmp(name, "set_type") == 0 ||
	    strcmp(name, "list_type") == 0)
	    return "void *";  // TODO: proper container support
	if (strcmp(name, "map_type") == 0)
	    return "void *";

	// Legacy compatibility
	if (strcmp(name, "qual_list") == 0) {
	    return emit_type(child(node, 0)) + " " + emit_type(child(node, 1));
	}

	return "int";
    }

    // Inline struct/union body (for typedef struct { ... } name;)
    std::string emit_struct_body_inline(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";
	if (is_anode(node, "struct_body"))
	    return emit_struct_body_inline(child(node, 0)) + " " +
		   emit_struct_field_inline(child(node, 1));
	return emit_struct_field_inline(node);
    }

    std::string emit_struct_field_inline(gp_tree_node *node)
    {
	if (!node) return "";
	if (is_anode(node, "struct_field")) {
	    std::string type = emit_type(child(node, 0));
	    std::string decls = emit_declarator_str(child(node, 1));
	    return type + " " + decls + ";";
	}
	return "";
    }

    // Inline enum body (for typedef enum { ... } name;)
    std::string emit_enum_body_inline(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";
	if (is_anode(node, "enum_list"))
	    return emit_enum_body_inline(child(node, 0)) + ", " +
		   emit_enum_val_inline(child(node, 1));
	return emit_enum_val_inline(node);
    }

    std::string emit_enum_val_inline(gp_tree_node *node)
    {
	if (!node) return "";
	if (node->type == GP_TERM)
	    return term_text(node);
	if (is_anode(node, "enum_assign"))
	    return term_text(child(node, 0)) + " = " + emit_expr(child(node, 1));
	return "";
    }

    // ---------------------------------------------------------------
    // Pointer emission — convert pointer chain to string of *'s
    // ---------------------------------------------------------------

    std::string emit_pointer(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";

	if (is_anode(node, "star")) {
	    gp_tree_node *quals = child(node, 0);
	    if (!is_nil(quals))
		return "* " + emit_type(quals) + " ";
	    return "*";
	}
	if (is_anode(node, "stars")) {
	    gp_tree_node *quals = child(node, 0);
	    std::string s = "*";
	    if (!is_nil(quals))
		s += " " + emit_type(quals) + " ";
	    return s + emit_pointer(child(node, 1));
	}
	return "*";
    }

    // ---------------------------------------------------------------
    // Abstract declarator emission (for casts, sizeof)
    // ---------------------------------------------------------------

    std::string emit_abstract_declarator(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";

	if (node->type == GP_ANODE) {
	    const char *name = anode_name(node);
	    if (strcmp(name, "star") == 0 || strcmp(name, "stars") == 0)
		return emit_pointer(node);
	    if (strcmp(name, "abs_ref") == 0)
		return "*";  // refs → pointers in C
	    if (strcmp(name, "abs_ptr") == 0)
		return emit_pointer(child(node, 0));
	    if (strcmp(name, "abs_func") == 0)
		return "(*)(void)";  // function pointer abstract
	    if (strcmp(name, "abs_array") == 0)
		return "[]";
	}
	return "";
    }

    // ---------------------------------------------------------------
    // Declarator helpers — extract name and pointer from declarator
    // ---------------------------------------------------------------

    // Extract the deepest identifier name from a declarator tree
    std::string extract_name(gp_tree_node *node)
    {
	if (!node) return "";
	if (node->type == GP_TERM) return term_text(node);
	const char *name = anode_name(node);
	if (strcmp(name, "ptr_decl") == 0)
	    return extract_name(child(node, 1));
	if (strcmp(name, "ref_decl") == 0)
	    return extract_name(child(node, 0));
	if (strcmp(name, "func_decl") == 0)
	    return extract_name(child(node, 0));
	if (strcmp(name, "array_decl") == 0)
	    return extract_name(child(node, 0));
	// Fallback: try first child
	if (nchildren(node) > 0)
	    return extract_name(child(node, 0));
	return "";
    }

    // Emit a complete declarator (pointer + name + array/func suffixes)
    // Returns: "**name" or "name[10]" or "(*name)(int, int)" etc.
    std::string emit_declarator_str(gp_tree_node *node)
    {
	if (!node) return "";
	if (node->type == GP_TERM) return term_text(node);

	const char *name = anode_name(node);

	if (strcmp(name, "ptr_decl") == 0)
	    return emit_pointer(child(node, 0)) + emit_declarator_str(child(node, 1));

	if (strcmp(name, "ref_decl") == 0)
	    return "*" + emit_declarator_str(child(node, 0));  // refs → pointers in C

	if (strcmp(name, "func_decl") == 0) {
	    gp_tree_node *dd = child(node, 0);
	    std::string nm = emit_declarator_str(dd);
	    std::string params = emit_param_list(child(node, 1));
	    // Wrap in parens if the direct declarator has a pointer
	    // (needed for function pointer declarators: int (*fp)(int))
	    if (is_anode(dd, "ptr_decl") || is_anode(dd, "ref_decl"))
		return "(" + nm + ")(" + params + ")";
	    return nm + "(" + params + ")";
	}

	if (strcmp(name, "array_decl") == 0) {
	    std::string nm = emit_declarator_str(child(node, 0));
	    gp_tree_node *sz = child(node, 1);
	    if (is_nil(sz)) return nm + "[]";
	    return nm + "[" + emit_expr(sz) + "]";
	}

	if (strcmp(name, "vla_decl") == 0)
	    return emit_declarator_str(child(node, 0)) + "[*]";

	// init_decl — declarator = initializer
	if (strcmp(name, "init_decl") == 0)
	    return emit_declarator_str(child(node, 0)) + " = " + emit_expr(child(node, 1));

	// decl_list / field_list — multiple declarators
	if (strcmp(name, "decl_list") == 0 || strcmp(name, "field_list") == 0)
	    return emit_declarator_str(child(node, 0)) + ", " + emit_declarator_str(child(node, 1));

	// bitfield
	if (strcmp(name, "bitfield") == 0) {
	    gp_tree_node *d = child(node, 0);
	    std::string nm = is_nil(d) ? "" : emit_declarator_str(d);
	    return nm + " : " + emit_expr(child(node, 1));
	}

	return term_text(node);
    }

    // ---------------------------------------------------------------
    // Expression type inference for cout format selection.
    // Returns: 'c' char, 's' string, 'd' double, 'i' int (default)
    // ---------------------------------------------------------------

    char infer_expr_cout_type(gp_tree_node *node)
    {
	if (!node) return 'i';

	// Terminal
	if (node->type == GP_TERM) {
	    int code = term_code(node);
	    if (code == GT_STRING) return 's';
	    if (code == GT_REAL) return 'd';
	    if (code == GT_CHAR_LIT) return 'c';
	    if (code == GT_IDENT) return lookup_var_type(term_text(node));
	    return 'i';
	}

	if (node->type != GP_ANODE) return 'i';
	const char *nm = anode_name(node);

	// Deref: *p where p is char* → char
	if (strcmp(nm, "deref") == 0) {
	    char inner = infer_expr_cout_type(child(node, 0));
	    if (inner == 's') return 'c';  // deref string/char* → char
	    return 'i';
	}

	// Subscript: a[i] where a is char* → char
	if (strcmp(nm, "subscript") == 0) {
	    char inner = infer_expr_cout_type(child(node, 0));
	    if (inner == 's') return 'c';
	    return 'i';
	}

	// Cast — check target type
	if (strcmp(nm, "cast") == 0) {
	    std::string ct = emit_type(child(node, 0));
	    return classify_type(ct);
	}

	// Function call — check return type
	if (strcmp(nm, "call") == 0) {
	    std::string fn = extract_name(child(node, 0));
	    return lookup_func_ret_type(fn);
	}

	// Parenthesized
	if (strcmp(nm, "paren") == 0)
	    return infer_expr_cout_type(child(node, 0));

	// Arithmetic — pointer arithmetic preserves pointer type
	if (strcmp(nm, "add") == 0 || strcmp(nm, "sub") == 0) {
	    char l = infer_expr_cout_type(child(node, 0));
	    char r = infer_expr_cout_type(child(node, 1));
	    if (l == 's' || r == 's') return 's';  // ptr + int = ptr
	    if (l == 'd' || r == 'd') return 'd';
	    return 'i';
	}
	if (strcmp(nm, "mul") == 0 || strcmp(nm, "div") == 0) {
	    char l = infer_expr_cout_type(child(node, 0));
	    char r = infer_expr_cout_type(child(node, 1));
	    if (l == 'd' || r == 'd') return 'd';
	    return 'i';
	}

	// Assignment — type of LHS
	if (strcmp(nm, "assign") == 0)
	    return infer_expr_cout_type(child(node, 0));

	// Address-of — produces a pointer
	if (strcmp(nm, "addrof") == 0) {
	    char inner = infer_expr_cout_type(child(node, 0));
	    if (inner == 'c') return 's';  // &char → char*
	    return 'i';  // &int → int* (print as int)
	}

	// Member access — default to int for now
	if (strcmp(nm, "member") == 0 || strcmp(nm, "arrow_member") == 0)
	    return 'i';

	// Ternary — type of true branch
	if (strcmp(nm, "ternary") == 0)
	    return infer_expr_cout_type(child(node, 1));

	// Pre/post inc/dec — same as operand
	if (strcmp(nm, "pre_inc") == 0 || strcmp(nm, "pre_dec") == 0 ||
	    strcmp(nm, "post_inc") == 0 || strcmp(nm, "post_dec") == 0)
	    return infer_expr_cout_type(child(node, 0));

	// Neg — same as operand
	if (strcmp(nm, "neg") == 0)
	    return infer_expr_cout_type(child(node, 0));

	return 'i';
    }

    // ---------------------------------------------------------------
    // cout << expr << endl  →  C output calls
    //
    // Detects bsl (<<) chains rooted at "cout" and converts them to
    // printf/madc_puti/etc.  Each value in the chain becomes a
    // separate call.  "endl" maps to putchar('\n').
    // ---------------------------------------------------------------

    // Check if a node is the identifier "cout"
    static bool is_cout(gp_tree_node *node)
    {
	if (!node) return false;
	if (node->type == GP_TERM && term_code(node) == GT_IDENT)
	    return term_text(node) == "cout";
	// Paren-wrapped: (cout << ...)
	if (is_anode(node, "paren"))
	    return is_cout_chain(child(node, 0));
	return false;
    }

    // Check if a bsl chain is rooted at "cout"
    static bool is_cout_chain(gp_tree_node *node)
    {
	if (!node) return false;
	if (node->type == GP_TERM) return is_cout(node);
	if (is_anode(node, "paren"))
	    return is_cout_chain(child(node, 0));
	if (!is_anode(node, "bsl")) return false;
	gp_tree_node *lhs = child(node, 0);
	return is_cout(lhs) || is_cout_chain(lhs);
    }

    // Collect all values from a cout << chain (left to right)
    void collect_cout_values(gp_tree_node *node,
			     std::vector<gp_tree_node *> &vals)
    {
	if (!node) return;
	if (is_anode(node, "paren")) {
	    collect_cout_values(child(node, 0), vals);
	    return;
	}
	if (!is_anode(node, "bsl")) return;

	gp_tree_node *lhs = child(node, 0);
	gp_tree_node *rhs = child(node, 1);

	// Recurse into left side (cout or more bsl's)
	if (is_anode(lhs, "bsl") && is_cout_chain(lhs))
	    collect_cout_values(lhs, vals);
	else if (is_anode(lhs, "paren") && is_cout_chain(lhs))
	    collect_cout_values(child(lhs, 0), vals);
	// else: lhs is "cout" — skip it

	// Add right side as a value
	vals.push_back(rhs);
    }

    // Emit a cout chain as a sequence of C output calls
    std::string emit_cout_chain(gp_tree_node *node)
    {
	std::vector<gp_tree_node *> vals;
	collect_cout_values(node, vals);

	std::string result;
	for (size_t i = 0; i < vals.size(); i++) {
	    gp_tree_node *v = vals[i];
	    if (i > 0) result += ", ";

	    // Check for "endl"
	    if (v->type == GP_TERM && term_code(v) == GT_IDENT &&
		term_text(v) == "endl") {
		result += "putchar('\\n')";
		continue;
	    }

	    // Determine the type of value and emit appropriate call
	    if (v->type == GP_TERM) {
		int code = term_code(v);
		if (code == GT_STRING) {
		    result += "printf(\"%s\", \"" + c_escape(term_text(v)) + "\")";
		} else if (code == GT_INTEGER) {
		    char buf[32];
		    snprintf(buf, sizeof(buf), "%lld", (long long)term_ival(v));
		    result += std::string("printf(\"%lld\", (long long)") + buf + ")";
		} else if (code == GT_REAL) {
		    result += "printf(\"%g\", " + emit_expr(v) + ")";
		} else if (code == GT_CHAR_LIT) {
		    result += "putchar(" + emit_expr(v) + ")";
		} else {
		    // Identifier — use sema + local type info
		    std::string id = term_text(v);
		    char tc = lookup_var_type(id);
		    if (tc == 's')
			result += "printf(\"%s\", " + id + ")";
		    else if (tc == 'c')
			result += "printf(\"%c\", " + id + ")";
		    else if (tc == 'd')
			result += "printf(\"%g\", (double)" + id + ")";
		    else
			result += "printf(\"%lld\", (long long)" + id + ")";
		}
	    } else {
		// Complex expression — infer type for format
		std::string e = emit_expr(v);
		char etc = infer_expr_cout_type(v);
		if (!e.empty() && e[0] == '"')
		    result += "printf(\"%s\", " + e + ")";
		else if (etc == 's')
		    result += "printf(\"%s\", " + e + ")";
		else if (etc == 'c')
		    result += "printf(\"%c\", " + e + ")";
		else if (etc == 'd')
		    result += "printf(\"%g\", (double)(" + e + "))";
		else
		    result += "printf(\"%lld\", (long long)(" + e + "))";
	    }
	}

	return "(" + result + ")";
    }

    // ---------------------------------------------------------------
    // Expression emission
    // ---------------------------------------------------------------

    std::string emit_expr(gp_tree_node *node)
    {
	if (!node) return "0";
	if (node->type == GP_NIL) return "0";

	// Terminal: literal or identifier
	if (node->type == GP_TERM) {
	    int code = term_code(node);
	    if (code == GT_IDENT) {
		std::string id = term_text(node);
		// Inside class methods: unqualified members → __this->member
		if (!current_class.empty() &&
		    current_class_fields.count(id) &&
		    !local_var_types.count(id))  // don't transform local vars
		    return "__this->" + id;
		return id;
	    }
	    if (code == GT_INTEGER) {
		char buf[32];
		snprintf(buf, sizeof(buf), "%lld", (long long)term_ival(node));
		return buf;
	    }
	    if (code == GT_REAL) {
		char buf[64];
		double d = term_dval(node);
		snprintf(buf, sizeof(buf), "%.17g", d);
		std::string s = buf;
		if (s.find('.') == std::string::npos &&
		    s.find('e') == std::string::npos &&
		    s.find('E') == std::string::npos)
		    s += ".0";
		return s;
	    }
	    if (code == GT_STRING) {
		return "\"" + c_escape(term_text(node)) + "\"";
	    }
	    if (code == GT_CHAR_LIT) {
		int64_t ch = term_ival(node);
		if (ch == '\n') return "'\\n'";
		if (ch == '\t') return "'\\t'";
		if (ch == '\r') return "'\\r'";
		if (ch == '\\') return "'\\\\'";
		if (ch == '\'') return "'\\''";
		if (ch == 0)    return "'\\0'";
		char buf[8];
		snprintf(buf, sizeof(buf), "'%c'", (char)ch);
		return buf;
	    }
	    return term_text(node);
	}

	const char *name = anode_name(node);

	// Parenthesized expression
	if (strcmp(name, "paren") == 0)
	    return "(" + emit_expr(child(node, 0)) + ")";

	// cout << expr << endl → C output calls
	if (strcmp(name, "bsl") == 0 && is_cout_chain(node))
	    return emit_cout_chain(node);

	// Binary operators
	struct { const char *node_name; const char *c_op; } binops[] = {
	    {"add", "+"}, {"sub", "-"}, {"mul", "*"}, {"div", "/"},
	    {"mod", "%"}, {"bsl", "<<"}, {"bsr", ">>"},
	    {"bitand", "&"}, {"bitor", "|"}, {"bitxor", "^"},
	    {"lor", "||"}, {"land", "&&"},
	    {"eq", "=="}, {"ne", "!="}, {"lt", "<"}, {"gt", ">"},
	    {"le", "<="}, {"ge", ">="}, {"three_way", "-"},
	    {"eq3", "==="},
	    {nullptr, nullptr}
	};
	for (int i = 0; binops[i].node_name; i++) {
	    if (strcmp(name, binops[i].node_name) == 0)
		return "(" + emit_expr(child(node, 0)) + " " +
		       binops[i].c_op + " " +
		       emit_expr(child(node, 1)) + ")";
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
	    if (strcmp(name, assigns[i].node_name) == 0)
		return emit_expr(child(node, 0)) + " " +
		       assigns[i].c_op + " " +
		       emit_expr(child(node, 1));
	}

	// Unary operators
	if (strcmp(name, "neg") == 0)     return "(-" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "pos") == 0)     return "(+" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "lnot") == 0)    return "(!" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "bnot") == 0)    return "(~" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "deref") == 0)   return "(*" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "addrof") == 0)  return "(&" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "pre_inc") == 0) return "(++" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "pre_dec") == 0) return "(--" + emit_expr(child(node, 0)) + ")";
	if (strcmp(name, "post_inc") == 0) return "(" + emit_expr(child(node, 0)) + "++)";
	if (strcmp(name, "post_dec") == 0) return "(" + emit_expr(child(node, 0)) + "--)";

	// Ternary
	if (strcmp(name, "ternary") == 0)
	    return "(" + emit_expr(child(node, 0)) + " ? " +
		   emit_expr(child(node, 1)) + " : " +
		   emit_expr(child(node, 2)) + ")";

	// Function call
	if (strcmp(name, "call") == 0) {
	    gp_tree_node *callee = child(node, 0);
	    std::string args = emit_arg_list(child(node, 1));

	    // Handle method calls: call(member(obj, method), args)
	    if (is_anode(callee, "member") || is_anode(callee, "arrow_member")) {
		std::string obj = emit_expr(child(callee, 0));
		std::string method = term_text(child(callee, 1));
		bool is_arrow = is_anode(callee, "arrow_member");

		// String methods: c_str() → identity (string is already char*)
		if (method == "c_str") return obj;
		// String methods: length()/size() → strlen()
		if (method == "length" || method == "size")
		    return "strlen(" + obj + ")";

		// Check if obj is a known class instance → mangled method call
		std::string obj_name = term_text(child(callee, 0));
		std::string cls = lookup_var_class(obj_name);
		if (!cls.empty()) {
		    // Resolve method through inheritance chain
		    std::string owner = cls;
		    if (sema) {
			std::string check = cls;
			while (!check.empty()) {
			    const SemaClassInfo *ci = sema->get_class(check);
			    if (ci && ci->methods.count(method)) {
				owner = check;
				break;
			    }
			    // Walk up inheritance chain
			    auto bit = sema->class_bases.find(check);
			    if (bit != sema->class_bases.end())
				check = bit->second;
			    else
				break;
			}
		    }
		    std::string mangled = owner + "__" + method;
		    std::string addr = is_arrow ? obj : ("&" + obj);
		    if (args.empty())
			return mangled + "(" + addr + ")";
		    return mangled + "(" + addr + ", " + args + ")";
		}

		// Fallback: pass through as C member access
		std::string op = is_arrow ? "->" : ".";
		return obj + op + method + "(" + args + ")";
	    }

	    std::string func = emit_expr(callee);
	    func = map_builtin(func);
	    return func + "(" + args + ")";
	}

	// Member access
	if (strcmp(name, "member") == 0)
	    return emit_expr(child(node, 0)) + "." + term_text(child(node, 1));
	if (strcmp(name, "arrow_member") == 0)
	    return emit_expr(child(node, 0)) + "->" + term_text(child(node, 1));

	// Subscript
	if (strcmp(name, "subscript") == 0)
	    return emit_expr(child(node, 0)) + "[" + emit_expr(child(node, 1)) + "]";

	// Namespace call: ns::func(args)
	if (strcmp(name, "ns_call") == 0) {
	    std::string ns = term_text(child(node, 0));
	    std::string func = term_text(child(node, 1));
	    std::string args = emit_arg_list(child(node, 2));
	    std::string mangled = "__" + ns_prefix(ns) + "_" + func;
	    ns_funcs_used.insert(mangled);
	    return mangled + "(" + args + ")";
	}
	if (strcmp(name, "ns_name") == 0) {
	    std::string ns = term_text(child(node, 0));
	    std::string func = term_text(child(node, 1));
	    std::string mangled = "__" + ns_prefix(ns) + "_" + func;
	    ns_funcs_used.insert(mangled);
	    return mangled;
	}

	// Cast: (type)expr
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
	if (strcmp(name, "new_ctor") == 0)
	    return "malloc(sizeof(" + term_text(child(node, 0)) + "_t))";

	// Comma operator
	if (strcmp(name, "comma") == 0)
	    return "(" + emit_expr(child(node, 0)) + ", " +
		   emit_expr(child(node, 1)) + ")";

	// Compound literal
	if (strcmp(name, "compound_lit") == 0)
	    return "((" + emit_type(child(node, 0)) + "){" +
		   emit_init_list(child(node, 1)) + "})";

	// Initializer list in expression context
	if (strcmp(name, "init_list") == 0)
	    return "{" + emit_init_list(child(node, 0)) + "}";
	if (strcmp(name, "empty_init") == 0)
	    return "{0}";

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

	// desig_init(designation_opt, initializer)
	if (is_anode(node, "desig_init")) {
	    gp_tree_node *desig = child(node, 0);
	    std::string val = emit_init_item(child(node, 1));
	    if (!is_nil(desig))
		return emit_designator(desig) + " = " + val;
	    return val;
	}

	// init_seq(prev_list, designation_opt, initializer)
	if (is_anode(node, "init_seq")) {
	    std::string prev = emit_init_list(child(node, 0));
	    gp_tree_node *desig = child(node, 1);
	    std::string val = emit_init_item(child(node, 2));
	    std::string item;
	    if (!is_nil(desig))
		item = emit_designator(desig) + " = " + val;
	    else
		item = val;
	    return prev + ", " + item;
	}

	return emit_expr(node);
    }

    std::string emit_init_item(gp_tree_node *node)
    {
	if (!node) return "0";
	if (is_anode(node, "init_list"))
	    return "{" + emit_init_list(child(node, 0)) + "}";
	if (is_anode(node, "empty_init"))
	    return "{0}";
	return emit_expr(node);
    }

    std::string emit_designator(gp_tree_node *node)
    {
	if (!node) return "";
	if (is_anode(node, "member_desig"))
	    return "." + term_text(child(node, 0));
	if (is_anode(node, "index_desig"))
	    return "[" + emit_expr(child(node, 0)) + "]";
	if (is_anode(node, "desig_chain"))
	    return emit_designator(child(node, 0)) + emit_designator(child(node, 1));
	return "";
    }

    // ---------------------------------------------------------------
    // Statement emission
    // ---------------------------------------------------------------

    void emit_stmt(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;

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
	    gp_tree_node *e = child(node, 0);
	    if (!is_nil(e)) {
		emit_indent();
		O("%s;\n", emit_expr(e).c_str());
	    } else {
		emit_indent();
		O(";\n");
	    }
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

	// Declaration (inside function body)
	if (strcmp(name, "decl") == 0) {
	    emit_decl_stmt(node);
	    return;
	}

	// Constructor-call declaration: ClassName var(args);
	if (strcmp(name, "ctor_decl") == 0) {
	    std::string type = emit_type(child(node, 0));
	    std::string vname = term_text(child(node, 1));
	    std::string args = emit_arg_list(child(node, 2));
	    std::string clean_type = type;
	    if (clean_type.substr(0, 7) == "struct ")
		clean_type = clean_type.substr(7);

	    // Emit: type var; ClassName__ClassName(&var, args);
	    emit_indent();
	    O("%s %s;\n", type.c_str(), vname.c_str());
	    emit_indent();
	    O("%s__%s(&%s, %s);\n", clean_type.c_str(), clean_type.c_str(),
	      vname.c_str(), args.c_str());

	    // Track type
	    if (is_known_class(clean_type))
		local_var_class_map[vname] = clean_type;
	    if (class_has_dtor(clean_type))
		scope_class_vars.push_back(vname + "|" + clean_type);
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
	    // for(expr; expr; expr) stmt
	    emit_indent();
	    std::string init = is_nil(child(node, 0)) ? "" : emit_expr(child(node, 0));
	    std::string cond = is_nil(child(node, 1)) ? "" : emit_expr(child(node, 1));
	    std::string iter = is_nil(child(node, 2)) ? "" : emit_expr(child(node, 2));
	    O("for (%s; %s; %s)\n", init.c_str(), cond.c_str(), iter.c_str());
	    indent_level++;
	    emit_stmt(child(node, 3));
	    indent_level--;
	    return;
	}
	if (strcmp(name, "for_decl") == 0) {
	    // for(decl; expr; expr) stmt
	    emit_indent();
	    std::string init = emit_decl_inline(child(node, 0));
	    std::string cond = is_nil(child(node, 1)) ? "" : emit_expr(child(node, 1));
	    std::string iter = is_nil(child(node, 2)) ? "" : emit_expr(child(node, 2));
	    O("for (%s; %s; %s)\n", init.c_str(), cond.c_str(), iter.c_str());
	    indent_level++;
	    emit_stmt(child(node, 3));
	    indent_level--;
	    return;
	}

	// Return — inject destructor calls before returning
	if (strcmp(name, "return_val") == 0) {
	    // Call destructors in LIFO order
	    for (int i = (int)scope_class_vars.size() - 1; i >= 0; i--) {
		size_t sep = scope_class_vars[i].find('|');
		if (sep != std::string::npos) {
		    std::string vn = scope_class_vars[i].substr(0, sep);
		    std::string cn = scope_class_vars[i].substr(sep + 1);
		    emit_indent();
		    O("%s__dtor(&%s);\n", cn.c_str(), vn.c_str());
		}
	    }
	    emit_indent();
	    gp_tree_node *val = child(node, 0);
	    if (is_nil(val))
		O("return;\n");
	    else
		O("return %s;\n", emit_expr(val).c_str());
	    return;
	}
	if (strcmp(name, "return_multi") == 0) {
	    emit_indent();
	    O("return %s;\n", emit_expr(child(node, 0)).c_str());
	    return;
	}

	// Break / Continue
	if (strcmp(name, "break") == 0)    { emit_indent(); O("break;\n"); return; }
	if (strcmp(name, "continue") == 0) { emit_indent(); O("continue;\n"); return; }

	// Goto / Label
	if (strcmp(name, "goto") == 0) {
	    emit_indent();
	    O("goto %s;\n", term_text(child(node, 0)).c_str());
	    return;
	}
	if (strcmp(name, "goto_indirect") == 0) {
	    emit_indent();
	    O("goto *%s;\n", emit_expr(child(node, 0)).c_str());
	    return;
	}
	if (strcmp(name, "label") == 0) {
	    // label has children: [ident, statement]
	    O("%s:;\n", term_text(child(node, 0)).c_str());
	    emit_stmt(child(node, 1));
	    return;
	}

	// Switch
	if (strcmp(name, "switch") == 0) {
	    emit_indent();
	    O("switch (%s)\n", emit_expr(child(node, 0)).c_str());
	    emit_stmt(child(node, 1));
	    return;
	}

	// Case / Default (labeled statements)
	if (strcmp(name, "case") == 0) {
	    emit_indent();
	    O("case %s:\n", emit_expr(child(node, 0)).c_str());
	    indent_level++;
	    emit_stmt(child(node, 1));
	    indent_level--;
	    return;
	}
	if (strcmp(name, "case_range") == 0) {
	    emit_indent();
	    O("case %s ... %s:\n", emit_expr(child(node, 0)).c_str(),
	      emit_expr(child(node, 1)).c_str());
	    indent_level++;
	    emit_stmt(child(node, 2));
	    indent_level--;
	    return;
	}
	if (strcmp(name, "default") == 0) {
	    emit_indent();
	    O("default:\n");
	    indent_level++;
	    emit_stmt(child(node, 0));
	    indent_level--;
	    return;
	}

	// Try/catch
	if (strcmp(name, "try") == 0) {
	    emit_indent();
	    O("/* try */ {\n");
	    indent_level++;
	    emit_stmt(child(node, 0));
	    indent_level--;
	    emit_indent();
	    O("}\n");
	    return;
	}

	// Throw
	if (strcmp(name, "throw_expr") == 0) {
	    emit_indent();
	    O("/* throw */ ;\n");
	    return;
	}
	if (strcmp(name, "throw") == 0) {
	    emit_indent();
	    O("/* throw */ ;\n");
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
	    O("/* defer */ ;\n");
	    return;
	}

	// Fallback
	emit_indent();
	O("/* TODO stmt: %s */\n", name);
    }

    // ---------------------------------------------------------------
    // Declaration emission (inside function bodies)
    // ---------------------------------------------------------------

    // Classify a type string for cout format inference
    static char classify_type(const std::string &type)
    {
	if (type.find("char") != std::string::npos) {
	    if (type.find("*") != std::string::npos) return 's';
	    return 'c';
	}
	if (type.find("const char") != std::string::npos) return 's';
	if (type.find("float") != std::string::npos) return 'd';
	if (type.find("double") != std::string::npos) return 'd';
	return 'i';
    }

    // Track variable types from a declaration for cout inference
    void track_decl_types(const std::string &type, gp_tree_node *decls)
    {
	if (is_nil(decls)) return;
	std::string vname = extract_name(decls);
	if (!vname.empty()) {
	    // Check if this is a class type
	    std::string clean_type = type;
	    if (clean_type.substr(0, 7) == "struct ")
		clean_type = clean_type.substr(7);
	    if (is_known_class(clean_type)) {
		local_var_class_map[vname] = clean_type;
		local_var_types[vname] = 'i';  // class objects print as int
	    } else {
		bool has_ptr = is_anode(decls, "ptr_decl");
		char tc = classify_type(type);
		if (has_ptr && tc == 'c') tc = 's';  // char * → string
		local_var_types[vname] = tc;
	    }
	}
	// Handle decl_list / init_decl
	if (is_anode(decls, "decl_list") || is_anode(decls, "init_decl")) {
	    track_decl_types(type, child(decls, 0));
	    if (nchildren(decls) > 1)
		track_decl_types(type, child(decls, 1));
	}
    }

    // Track parameter types for cout format inference
    void track_param_types(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;
	const char *name = anode_name(node);
	if (strcmp(name, "param") == 0) {
	    std::string type = emit_type(child(node, 0));
	    gp_tree_node *decl = child(node, 1);
	    if (!is_nil(decl)) track_decl_types(type, decl);
	} else if (strcmp(name, "param_list") == 0) {
	    track_param_types(child(node, 0));
	    track_param_types(child(node, 1));
	} else if (strcmp(name, "param_va") == 0) {
	    track_param_types(child(node, 0));
	}
    }

    void emit_decl_stmt(gp_tree_node *node)
    {
	// decl(declaration_specifiers, init_declarator_list_opt)
	std::string type = emit_type(child(node, 0));
	gp_tree_node *decls = child(node, 1);

	if (is_nil(decls)) {
	    emit_indent();
	    O("%s;\n", type.c_str());
	    return;
	}

	// Track types for cout format inference
	track_decl_types(type, decls);

	// Check for class constructor with arguments:
	// `Vec2 v(10, 20)` parses as decl with func_decl(v, args)
	std::string clean_type = type;
	if (clean_type.substr(0, 7) == "struct ")
	    clean_type = clean_type.substr(7);

	if (is_anode(decls, "func_decl") && is_known_class(clean_type)) {
	    // This is a constructor call, not a function declaration
	    std::string vname = extract_name(child(decls, 0));
	    std::string args = emit_arg_list(child(decls, 1));
	    emit_indent();
	    O("%s %s;\n", type.c_str(), vname.c_str());
	    emit_indent();
	    if (args.empty())
		O("%s__%s(&%s);\n", clean_type.c_str(), clean_type.c_str(),
		  vname.c_str());
	    else
		O("%s__%s(&%s, %s);\n", clean_type.c_str(), clean_type.c_str(),
		  vname.c_str(), args.c_str());
	    local_var_class_map[vname] = clean_type;
	    if (class_has_dtor(clean_type))
		scope_class_vars.push_back(vname + "|" + clean_type);
	    return;
	}

	emit_indent();
	O("%s %s;\n", type.c_str(), emit_declarator_str(decls).c_str());

	// Inject constructor call for class variables (no-args ctor)
	std::string vname = extract_name(decls);
	if (!vname.empty() && class_has_ctor(clean_type)) {
	    emit_indent();
	    O("%s__%s(&%s);\n", clean_type.c_str(), clean_type.c_str(),
	      vname.c_str());
	    if (class_has_dtor(clean_type))
		scope_class_vars.push_back(vname + "|" + clean_type);
	}
    }

    // Emit declaration as inline string (for for-loop init)
    std::string emit_decl_inline(gp_tree_node *node)
    {
	if (!node) return "";
	if (!is_anode(node, "decl")) return emit_expr(node);
	std::string type = emit_type(child(node, 0));
	gp_tree_node *decls = child(node, 1);
	if (is_nil(decls)) return type;
	return type + " " + emit_declarator_str(decls);
    }

    // ---------------------------------------------------------------
    // Parameter list emission
    // ---------------------------------------------------------------

    std::string emit_param_list(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";

	if (node->type == GP_TERM)
	    return emit_type(node);

	const char *name = anode_name(node);

	// param(declaration_specifiers, declarator_or_abstract)
	if (strcmp(name, "param") == 0) {
	    std::string type = emit_type(child(node, 0));
	    gp_tree_node *decl = child(node, 1);
	    if (is_nil(decl)) return type;
	    // The declarator may have pointer, name, etc.
	    std::string d = emit_declarator_str(decl);
	    if (d.empty()) return type;
	    return type + " " + d;
	}

	// param_list(prev, next_param)
	if (strcmp(name, "param_list") == 0)
	    return emit_param_list(child(node, 0)) + ", " +
		   emit_param_list(child(node, 1));

	// param_va(param_list) — variadic
	if (strcmp(name, "param_va") == 0)
	    return emit_param_list(child(node, 0)) + ", ...";

	// type_name used as param (abstract)
	if (strcmp(name, "type_name") == 0)
	    return emit_type(node);

	// qual (declaration_specifiers used as param type)
	if (strcmp(name, "qual") == 0)
	    return emit_type(node);

	return emit_type(node);
    }

    // ---------------------------------------------------------------
    // Function definition emission
    // ---------------------------------------------------------------

    void emit_func_def(gp_tree_node *node)
    {
	// func_def(declaration_specifiers, declarator, compound_statement)
	local_var_types.clear();  // fresh scope for each function
	local_var_class_map.clear();
	scope_class_vars.clear();
	std::string ret_type = emit_type(child(node, 0));
	gp_tree_node *decl = child(node, 1);
	gp_tree_node *body_node = child(node, 2);

	// Navigate declarator to extract pointer stars, name, and params
	std::string ptr_str;
	gp_tree_node *inner = decl;

	// Peel off pointer
	if (is_anode(inner, "ptr_decl")) {
	    ptr_str = emit_pointer(child(inner, 0));
	    inner = child(inner, 1);
	}
	if (is_anode(inner, "ref_decl")) {
	    ptr_str = "&";
	    inner = child(inner, 0);
	}

	// inner should be func_decl(name, params) or just identifier
	std::string func_name;
	std::string params;

	if (is_anode(inner, "func_decl")) {
	    func_name = extract_name(child(inner, 0));
	    track_param_types(child(inner, 1));
	    params = emit_param_list(child(inner, 1));
	} else {
	    func_name = term_text(inner);
	}

	std::string full_ret = ret_type;
	if (!ptr_str.empty())
	    full_ret += " " + ptr_str;

	// Forward declaration in header
	OH("%s %s(%s);\n", full_ret.c_str(), func_name.c_str(),
	   params.empty() ? "void" : params.c_str());

	// Function body
	O("%s %s(%s)\n", full_ret.c_str(), func_name.c_str(),
	  params.empty() ? "void" : params.c_str());

	indent_level = 0;
	emit_stmt(body_node);
	O("\n");
    }

    // ---------------------------------------------------------------
    // Struct/Union/Enum emission (top-level)
    // ---------------------------------------------------------------

    void emit_struct_def_toplevel(gp_tree_node *node)
    {
	// struct_def(struct_or_union, identifier_opt, struct_declaration_list)
	std::string su = term_text(child(node, 0));
	gp_tree_node *nm = child(node, 1);
	std::string sname = is_nil(nm) ? "" : term_text(nm);

	OH("%s %s {\n", su.c_str(), sname.c_str());
	emit_struct_body(child(node, 2));
	OH("};\n\n");
    }

    void emit_struct_body(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;
	if (is_anode(node, "struct_body")) {
	    emit_struct_body(child(node, 0));
	    emit_struct_field(child(node, 1));
	    return;
	}
	emit_struct_field(node);
    }

    void emit_struct_field(gp_tree_node *node)
    {
	if (!node) return;
	if (is_anode(node, "struct_field")) {
	    std::string type = emit_type(child(node, 0));
	    gp_tree_node *decls = child(node, 1);
	    OH("    %s %s;\n", type.c_str(), emit_declarator_str(decls).c_str());
	    return;
	}
	// Nested struct/union/enum definitions
	if (is_anode(node, "struct_def")) {
	    emit_struct_def_toplevel(node);
	    return;
	}
    }

    void emit_enum_def_toplevel(gp_tree_node *node)
    {
	// enum_def(identifier_opt, enumerator_list)
	gp_tree_node *nm = child(node, 0);
	std::string ename = is_nil(nm) ? "" : term_text(nm);

	OH("enum %s {\n", ename.c_str());
	emit_enum_body(child(node, 1));
	OH("\n};\n\n");
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
	if (node->type == GP_TERM)
	    OH("    %s", term_text(node).c_str());
	else if (is_anode(node, "enum_assign"))
	    OH("    %s = %s", term_text(child(node, 0)).c_str(),
	       emit_expr(child(node, 1)).c_str());
    }

    // ---------------------------------------------------------------
    // Class definition emission → C struct + free functions
    //
    // class Counter { int count; void inc() { count++; } int get() { return count; } };
    // →
    // struct Counter { int count; };
    // void Counter__inc(struct Counter *__this) { __this->count++; }
    // int Counter__get(struct Counter *__this) { return __this->count; }
    // ---------------------------------------------------------------

    // Current class being emitted (for member access → __this->member)
    std::string current_class;
    std::set<std::string> current_class_fields;

    // Map of emitted class fields for inheritance
    std::map<std::string, std::string> emitted_class_fields;

    void emit_class_def(gp_tree_node *node)
    {
	std::string class_name;
	std::string base_class;
	gp_tree_node *body_node = nullptr;

	if (is_anode(node, "class_def")) {
	    // class_def(name, class_body)
	    class_name = term_text(child(node, 0));
	    body_node = child(node, 1);
	} else if (is_anode(node, "class_inherit")) {
	    // class_inherit(name, access_spec, base_name, class_body)
	    class_name = term_text(child(node, 0));
	    base_class = term_text(child(node, 2));
	    body_node = child(node, 3);
	}

	if (class_name.empty()) return;

	// Collect field names for member→__this->member transformation
	// Use sema info if available, otherwise collect from AST
	current_class_fields.clear();
	if (sema) {
	    const SemaClassInfo *ci = sema->get_class(class_name);
	    if (ci)
		current_class_fields = ci->fields;
	}
	if (current_class_fields.empty())
	    collect_class_fields(body_node);

	// First pass: emit the struct with fields only
	OH("struct %s {\n", class_name.c_str());
	// Copy base class fields if inheriting
	if (!base_class.empty()) {
	    auto bit = emitted_class_fields.find(base_class);
	    if (bit != emitted_class_fields.end())
		OH("%s", bit->second.c_str());
	}
	// Save header position to capture this class's fields
	std::string saved_header = header;
	header.clear();
	emit_class_fields(body_node);
	std::string field_text = header;
	header = saved_header + field_text;
	emitted_class_fields[class_name] = field_text;
	// Also include base fields in the stored text for further inheritance
	if (!base_class.empty()) {
	    auto bit = emitted_class_fields.find(base_class);
	    if (bit != emitted_class_fields.end())
		emitted_class_fields[class_name] = bit->second + field_text;
	}
	OH("};\n\n");

	// Second pass: emit methods as free functions
	emit_class_methods(class_name, body_node);
	current_class_fields.clear();
    }

    void collect_class_fields(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;
	if (is_anode(node, "class_body")) {
	    collect_class_fields(child(node, 0));
	    gp_tree_node *item = child(node, 1);
	    if (is_anode(item, "decl")) {
		// Extract field names from declarator
		gp_tree_node *decls = child(item, 1);
		std::string fname = extract_name(decls);
		if (!fname.empty())
		    current_class_fields.insert(fname);
	    }
	}
    }

    void emit_class_fields(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;
	if (is_anode(node, "class_body")) {
	    emit_class_fields(child(node, 0));
	    emit_class_field_item(child(node, 1));
	    return;
	}
	emit_class_field_item(node);
    }

    void emit_class_field_item(gp_tree_node *node)
    {
	if (!node) return;
	const char *name = anode_name(node);
	// Only emit field declarations, skip methods/ctors/access specs
	if (strcmp(name, "decl") == 0) {
	    std::string type = emit_type(child(node, 0));
	    gp_tree_node *decls = child(node, 1);
	    if (!is_nil(decls))
		OH("    %s %s;\n", type.c_str(), emit_declarator_str(decls).c_str());
	}
	// Skip: method, ctor, dtor, oper_method, access
    }

    void emit_class_methods(const std::string &class_name, gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;
	if (is_anode(node, "class_body")) {
	    emit_class_methods(class_name, child(node, 0));
	    emit_class_method_item(class_name, child(node, 1));
	    return;
	}
	emit_class_method_item(class_name, node);
    }

    void emit_class_method_item(const std::string &class_name,
				gp_tree_node *node)
    {
	if (!node) return;
	const char *name = anode_name(node);

	// method(declaration_specifiers, declarator, compound_statement)
	if (strcmp(name, "method") == 0) {
	    std::string prev_class = current_class;
	    current_class = class_name;

	    std::string ret_type = emit_type(child(node, 0));
	    gp_tree_node *decl = child(node, 1);
	    gp_tree_node *body_node = child(node, 2);

	    // Extract method name and params from declarator
	    std::string method_name;
	    std::string params;
	    gp_tree_node *inner = decl;
	    if (is_anode(inner, "func_decl")) {
		method_name = extract_name(child(inner, 0));
		params = emit_param_list(child(inner, 1));
	    } else {
		method_name = term_text(inner);
	    }

	    std::string mangled = class_name + "__" + method_name;
	    std::string this_param = "struct " + class_name + " *__this";
	    std::string full_params = params.empty() ? this_param
					: this_param + ", " + params;

	    // Forward declaration
	    OH("%s %s(%s);\n", ret_type.c_str(), mangled.c_str(),
	       full_params.c_str());

	    // Function body
	    local_var_types.clear();
	    local_var_class_map.clear();
	    O("%s %s(%s)\n", ret_type.c_str(), mangled.c_str(),
	      full_params.c_str());
	    indent_level = 0;
	    emit_stmt(body_node);
	    O("\n");

	    current_class = prev_class;
	    return;
	}

	// ctor(name, params, body)
	if (strcmp(name, "ctor") == 0) {
	    std::string prev_class = current_class;
	    current_class = class_name;

	    std::string params = emit_param_list(child(node, 1));
	    gp_tree_node *body_node = child(node, 2);

	    std::string mangled = class_name + "__" + class_name;
	    std::string this_param = "struct " + class_name + " *__this";
	    std::string full_params = params.empty() ? this_param
					: this_param + ", " + params;

	    OH("void %s(%s);\n", mangled.c_str(), full_params.c_str());

	    local_var_types.clear();
	    local_var_class_map.clear();
	    O("void %s(%s)\n", mangled.c_str(), full_params.c_str());
	    indent_level = 0;
	    emit_stmt(body_node);
	    O("\n");

	    current_class = prev_class;
	    return;
	}

	// dtor(name, body)
	if (strcmp(name, "dtor") == 0) {
	    std::string prev_class = current_class;
	    current_class = class_name;

	    gp_tree_node *body_node = child(node, 1);

	    std::string mangled = class_name + "__dtor";
	    std::string this_param = "struct " + class_name + " *__this";

	    OH("void %s(%s);\n", mangled.c_str(), this_param.c_str());

	    local_var_types.clear();
	    local_var_class_map.clear();
	    O("void %s(%s)\n", mangled.c_str(), this_param.c_str());
	    indent_level = 0;
	    emit_stmt(body_node);
	    O("\n");

	    current_class = prev_class;
	    return;
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

	// Top-level declaration (globals, prototypes, typedefs)
	if (is_anode(node, "decl")) {
	    std::string type = emit_type(child(node, 0));
	    gp_tree_node *decls = child(node, 1);

	    // Check if this is a typedef
	    bool is_typedef = (type.find("typedef") != std::string::npos);

	    if (is_nil(decls)) {
		// Type-only decl (struct/enum definition with no variable)
		// Check if type contains a struct/enum definition
		gp_tree_node *specs = child(node, 0);
		if (specs && specs->type == GP_ANODE) {
		    // Walk qual chain to find struct_def or enum_def
		    gp_tree_node *s = specs;
		    while (is_anode(s, "qual")) {
			gp_tree_node *c0 = child(s, 0);
			if (is_anode(c0, "struct_def")) {
			    emit_struct_def_toplevel(c0);
			    return;
			}
			if (is_anode(c0, "enum_def")) {
			    emit_enum_def_toplevel(c0);
			    return;
			}
			s = child(s, 1);
		    }
		    // Single specifier
		    if (is_anode(specs, "struct_def")) {
			emit_struct_def_toplevel(specs);
			return;
		    }
		    if (is_anode(specs, "enum_def")) {
			emit_enum_def_toplevel(specs);
			return;
		    }
		}
		OH("%s;\n", type.c_str());
		return;
	    }

	    // Declaration with declarators
	    if (is_typedef) {
		OH("%s %s;\n", type.c_str(), emit_declarator_str(decls).c_str());
	    } else {
		// Check if any declarator is a func_decl (function prototype)
		std::string d = emit_declarator_str(decls);
		// Extern declarations go in header
		if (type.find("extern") != std::string::npos) {
		    OH("%s %s;\n", type.c_str(), d.c_str());
		} else {
		    OH("%s %s;\n", type.c_str(), d.c_str());
		}
	    }
	    return;
	}

	// Struct/Union/Enum definitions (direct, not through decl)
	if (is_anode(node, "struct_def")) {
	    emit_struct_def_toplevel(node);
	    return;
	}
	if (is_anode(node, "enum_def")) {
	    emit_enum_def_toplevel(node);
	    return;
	}

	// Class definition → C struct + free functions
	if (is_anode(node, "class_def") || is_anode(node, "class_inherit")) {
	    emit_class_def(node);
	    return;
	}

	// Using / namespace — skip for C output
	if (is_anode(node, "using_ns") || is_anode(node, "using_decl") ||
	    is_anode(node, "prefer") || is_anode(node, "namespace_def")) {
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
    CEmitter() : indent_level(0), tmp_counter(0), sema(nullptr) {}

    std::string emit(gp_tree_node *root, SemaInfo *sema_info = nullptr)
    {
	sema = sema_info;
	header.clear();
	body.clear();

	header += "/* Generated by madc transpiler */\n";
	header += "typedef signed char int8_t;\n";
	header += "typedef unsigned char uint8_t;\n";
	header += "typedef short int16_t;\n";
	header += "typedef unsigned short uint16_t;\n";
	header += "typedef int int32_t;\n";
	header += "typedef unsigned int uint32_t;\n";
	header += "typedef long int64_t;\n";
	header += "typedef unsigned long uint64_t;\n";
	header += "typedef unsigned long size_t;\n";
	header += "typedef long ptrdiff_t;\n";
	header += "#define NULL ((void *)0)\n";
	header += "#define nullptr ((void *)0)\n";
	header += "\n";

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
	header += "extern char *strstr(const char *, const char *);\n";
	header += "extern char *strchr(const char *, int);\n";
	header += "extern char *strncpy(char *, const char *, unsigned long);\n";
	header += "extern char *strcat(char *, const char *);\n";
	header += "extern char *strcpy(char *, const char *);\n";
	header += "extern int strncmp(const char *, const char *, unsigned long);\n";
	header += "extern void *realloc(void *, unsigned long);\n";
	header += "extern int abs(int);\n";
	header += "extern long labs(long);\n";
	header += "extern void exit(int);\n";
	header += "extern void abort(void);\n";
	header += "extern int system(const char *);\n";
	header += "extern int snprintf(char *, unsigned long, const char *, ...);\n";
	header += "extern int sprintf(char *, const char *, ...);\n";
	header += "extern int sscanf(const char *, const char *, ...);\n";
	header += "static const char *version = \"v0.0.1\";\n";
	header += "\n";

	// Stream and container type stubs (opaque pointers in C)
	header += "/* Stream/container type stubs */\n";
	header += "typedef void *ifstream;\n";
	header += "typedef void *ofstream;\n";
	header += "typedef void *fstream;\n";
	header += "typedef void *stringstream;\n";
	header += "typedef void *ostream;\n";
	header += "typedef void *array;\n";  // MadArray
	header += "\n";

	// Additional builtins commonly needed
	header += "extern void puti(int64_t);\n";
	header += "extern void putu(uint64_t);\n";
	header += "extern void putd(double);\n";
	header += "extern void putf(float);\n";
	header += "extern void printstr(const char *);\n";
	header += "extern int getchar(void);\n";
	header += "extern int rand(void);\n";
	header += "extern void srand(unsigned int);\n";
	header += "extern unsigned long time(void *);\n";
	header += "extern int isdigit(int);\n";
	header += "extern int isalpha(int);\n";
	header += "extern int isalnum(int);\n";
	header += "extern int isspace(int);\n";
	header += "extern int toupper(int);\n";
	header += "extern int tolower(int);\n";
	header += "extern double sqrt(double);\n";
	header += "extern double pow(double, double);\n";
	header += "extern double fabs(double);\n";
	header += "extern double floor(double);\n";
	header += "extern double ceil(double);\n";
	header += "extern double log(double);\n";
	header += "extern double sin(double);\n";
	header += "extern double cos(double);\n";
	header += "\n";

	ns_funcs_used.clear();
	emit_top_level(root);

	// Emit extern declarations for all namespace functions used
	if (!ns_funcs_used.empty()) {
	    std::string ns_decls;
	    ns_decls += "/* Namespace function externs (resolved via dlsym) */\n";
	    for (auto &fn : ns_funcs_used)
		ns_decls += "extern long " + fn + "();\n";
	    ns_decls += "\n";
	    // Insert after header, before body
	    header += ns_decls;
	}

	return header + "\n" + body;
    }
};

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

std::string madc_emit_c(gp_tree_node *root, SemaInfo *sema)
{
    CEmitter emitter;
    return emitter.emit(root, sema);
}
