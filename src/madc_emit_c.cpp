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
#include "madc_anode.h"

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
    std::map<std::string, TypeClass> local_var_types;

    // Per-function local class var map overlay
    std::map<std::string, std::string> local_var_class_map;

    // Class vars in current scope needing destructor injection (LIFO)
    std::vector<std::string> scope_class_vars;

    // Look up variable type: local overlay first, then sema
    TypeClass lookup_var_type(const std::string &name) {
	auto it = local_var_types.find(name);
	if (it != local_var_types.end()) return it->second;
	if (sema) return sema->get_var_type(name);
	return TC_INT;
    }

    // Look up function return type: sema first
    TypeClass lookup_func_ret_type(const std::string &name) {
	if (sema) return sema->get_func_ret_type(name);
	return TC_INT;
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

	int an = an_code(node);

	// qual(specifier, rest) — recursive declaration_specifiers chain
	if (an == AN_QUAL) {
	    std::string spec = emit_type(child(node, 0));
	    gp_tree_node *rest = child(node, 1);
	    if (is_nil(rest)) return spec;
	    std::string r = emit_type(rest);
	    if (spec.empty()) return r;
	    if (r.empty()) return spec;
	    return spec + " " + r;
	}

	// struct/union reference: struct foo
	if (an == AN_STRUCT_REF) {
	    std::string su = term_text(child(node, 0));  // "struct" or "union"
	    std::string nm = term_text(child(node, 1));
	    return su + " " + nm;
	}

	// struct/union definition (inline): struct foo { ... }
	if (an == AN_STRUCT_DEF) {
	    std::string su = term_text(child(node, 0));
	    gp_tree_node *nm = child(node, 1);
	    std::string sname = is_nil(nm) ? "" : (" " + term_text(nm));
	    std::string result = su + sname + " { ";
	    result += emit_struct_body_inline(child(node, 2));
	    result += " }";
	    return result;
	}

	// enum reference: enum foo
	if (an == AN_ENUM_REF)
	    return "enum " + term_text(child(node, 0));

	// enum definition (inline) — emit the full body
	if (an == AN_ENUM_DEF) {
	    gp_tree_node *nm = child(node, 0);
	    std::string ename = is_nil(nm) ? "" : (" " + term_text(nm));
	    std::string result = "enum" + ename + " { ";
	    result += emit_enum_body_inline(child(node, 1));
	    result += " }";
	    return result;
	}

	// type_name(specifier_qualifier_list, abstract_declarator_opt)
	if (an == AN_TYPE_NAME) {
	    std::string base = emit_type(child(node, 0));
	    gp_tree_node *abs = child(node, 1);
	    if (is_nil(abs)) return base;
	    return base + " " + emit_abstract_declarator(abs);
	}

	// container types
	if (an == AN_VECTOR_TYPE || an == AN_SET_TYPE || an == AN_LIST_TYPE)
	    return "void *";  // TODO: proper container support
	if (an == AN_MAP_TYPE)
	    return "void *";

	// Legacy compatibility
	if (an == AN_QUAL_LIST) {
	    return emit_type(child(node, 0)) + " " + emit_type(child(node, 1));
	}

	return "int";
    }

    // Inline struct/union body (for typedef struct { ... } name;)
    std::string emit_struct_body_inline(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";
	if (is_an(node, AN_STRUCT_BODY))
	    return emit_struct_body_inline(child(node, 0)) + " " +
		   emit_struct_field_inline(child(node, 1));
	return emit_struct_field_inline(node);
    }

    std::string emit_struct_field_inline(gp_tree_node *node)
    {
	if (!node) return "";
	if (is_an(node, AN_STRUCT_FIELD)) {
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
	if (is_an(node, AN_ENUM_LIST))
	    return emit_enum_body_inline(child(node, 0)) + ", " +
		   emit_enum_val_inline(child(node, 1));
	return emit_enum_val_inline(node);
    }

    std::string emit_enum_val_inline(gp_tree_node *node)
    {
	if (!node) return "";
	if (node->type == GP_TERM)
	    return term_text(node);
	if (is_an(node, AN_ENUM_ASSIGN))
	    return term_text(child(node, 0)) + " = " + emit_expr(child(node, 1));
	return "";
    }

    // ---------------------------------------------------------------
    // Pointer emission — convert pointer chain to string of *'s
    // ---------------------------------------------------------------

    std::string emit_pointer(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";

	if (is_an(node, AN_STAR)) {
	    gp_tree_node *quals = child(node, 0);
	    if (!is_nil(quals))
		return "* " + emit_type(quals) + " ";
	    return "*";
	}
	if (is_an(node, AN_STARS)) {
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
	    int an = an_code(node);
	    if (an == AN_STAR || an == AN_STARS)
		return emit_pointer(node);
	    if (an == AN_ABS_REF)
		return "*";  // refs → pointers in C
	    if (an == AN_ABS_PTR)
		return emit_pointer(child(node, 0));
	    if (an == AN_ABS_FUNC)
		return "(*)(void)";  // function pointer abstract
	    if (an == AN_ABS_ARRAY)
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
	int an = an_code(node);
	if (an == AN_PTR_DECL)
	    return extract_name(child(node, 1));
	if (an == AN_REF_DECL)
	    return extract_name(child(node, 0));
	if (an == AN_FUNC_DECL)
	    return extract_name(child(node, 0));
	if (an == AN_ARRAY_DECL)
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

	int an = an_code(node);

	if (an == AN_PTR_DECL)
	    return emit_pointer(child(node, 0)) + emit_declarator_str(child(node, 1));

	if (an == AN_REF_DECL)
	    return "*" + emit_declarator_str(child(node, 0));  // refs → pointers in C

	if (an == AN_FUNC_DECL) {
	    gp_tree_node *dd = child(node, 0);
	    std::string nm = emit_declarator_str(dd);
	    std::string params = emit_param_list(child(node, 1));
	    // Wrap in parens if the direct declarator has a pointer
	    // (needed for function pointer declarators: int (*fp)(int))
	    if (is_an(dd, AN_PTR_DECL) || is_an(dd, AN_REF_DECL))
		return "(" + nm + ")(" + params + ")";
	    return nm + "(" + params + ")";
	}

	if (an == AN_ARRAY_DECL) {
	    std::string nm = emit_declarator_str(child(node, 0));
	    gp_tree_node *sz = child(node, 1);
	    if (is_nil(sz)) return nm + "[]";
	    return nm + "[" + emit_expr(sz) + "]";
	}

	if (an == AN_VLA_DECL)
	    return emit_declarator_str(child(node, 0)) + "[*]";

	// init_decl — declarator = initializer
	if (an == AN_INIT_DECL)
	    return emit_declarator_str(child(node, 0)) + " = " + emit_expr(child(node, 1));

	// decl_list / field_list — multiple declarators
	if (an == AN_DECL_LIST || an == AN_FIELD_LIST)
	    return emit_declarator_str(child(node, 0)) + ", " + emit_declarator_str(child(node, 1));

	// bitfield
	if (an == AN_BITFIELD) {
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

    TypeClass infer_expr_cout_type(gp_tree_node *node)
    {
	if (!node) return TC_INT;

	// Terminal
	if (node->type == GP_TERM) {
	    int code = term_code(node);
	    if (code == GT_STRING) return TC_STRING;
	    if (code == GT_REAL) return TC_DOUBLE;
	    if (code == GT_CHAR_LIT) return TC_CHAR;
	    if (code == GT_IDENT) return lookup_var_type(term_text(node));
	    return TC_INT;
	}

	if (node->type != GP_ANODE) return TC_INT;
	int an = an_code(node);

	// Deref: *p where p is char* → char
	if (an == AN_DEREF) {
	    TypeClass inner = infer_expr_cout_type(child(node, 0));
	    if (inner == TC_STRING) return TC_CHAR;  // deref string/char* → char
	    return TC_INT;
	}

	// Subscript: a[i] where a is char* → char
	if (an == AN_SUBSCRIPT) {
	    TypeClass inner = infer_expr_cout_type(child(node, 0));
	    if (inner == TC_STRING) return TC_CHAR;
	    return TC_INT;
	}

	// Cast — check target type
	if (an == AN_CAST) {
	    std::string ct = emit_type(child(node, 0));
	    return classify_type(ct);
	}

	// Function call — check return type
	if (an == AN_CALL) {
	    std::string fn = extract_name(child(node, 0));
	    return lookup_func_ret_type(fn);
	}

	// Parenthesized
	if (an == AN_PAREN)
	    return infer_expr_cout_type(child(node, 0));

	// Arithmetic — pointer arithmetic preserves pointer type
	if (an == AN_ADD || an == AN_SUB) {
	    TypeClass l = infer_expr_cout_type(child(node, 0));
	    TypeClass r = infer_expr_cout_type(child(node, 1));
	    if (l == TC_STRING || r == TC_STRING) return TC_STRING;  // ptr + int = ptr
	    if (l == TC_DOUBLE || r == TC_DOUBLE) return TC_DOUBLE;
	    return TC_INT;
	}
	if (an == AN_MUL || an == AN_DIV) {
	    TypeClass l = infer_expr_cout_type(child(node, 0));
	    TypeClass r = infer_expr_cout_type(child(node, 1));
	    if (l == TC_DOUBLE || r == TC_DOUBLE) return TC_DOUBLE;
	    return TC_INT;
	}

	// Assignment — type of LHS
	if (an == AN_ASSIGN)
	    return infer_expr_cout_type(child(node, 0));

	// Address-of — produces a pointer
	if (an == AN_ADDROF) {
	    TypeClass inner = infer_expr_cout_type(child(node, 0));
	    if (inner == TC_CHAR) return TC_STRING;  // &char → char*
	    return TC_INT;  // &int → int* (print as int)
	}

	// Member access — default to int for now
	if (an == AN_MEMBER || an == AN_ARROW_MEMBER)
	    return TC_INT;

	// Ternary — type of true branch
	if (an == AN_TERNARY)
	    return infer_expr_cout_type(child(node, 1));

	// Pre/post inc/dec — same as operand
	if (an == AN_PRE_INC || an == AN_PRE_DEC ||
	    an == AN_POST_INC || an == AN_POST_DEC)
	    return infer_expr_cout_type(child(node, 0));

	// Neg — same as operand
	if (an == AN_NEG)
	    return infer_expr_cout_type(child(node, 0));

	return TC_INT;
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
	if (is_an(node, AN_PAREN))
	    return is_cout_chain(child(node, 0));
	return false;
    }

    // Check if a bsl chain is rooted at "cout"
    static bool is_cout_chain(gp_tree_node *node)
    {
	if (!node) return false;
	if (node->type == GP_TERM) return is_cout(node);
	if (is_an(node, AN_PAREN))
	    return is_cout_chain(child(node, 0));
	if (!is_an(node, AN_BSL)) return false;
	gp_tree_node *lhs = child(node, 0);
	return is_cout(lhs) || is_cout_chain(lhs);
    }

    // Collect all values from a cout << chain (left to right)
    void collect_cout_values(gp_tree_node *node,
			     std::vector<gp_tree_node *> &vals)
    {
	if (!node) return;
	if (is_an(node, AN_PAREN)) {
	    collect_cout_values(child(node, 0), vals);
	    return;
	}
	if (!is_an(node, AN_BSL)) return;

	gp_tree_node *lhs = child(node, 0);
	gp_tree_node *rhs = child(node, 1);

	// Recurse into left side (cout or more bsl's)
	if (is_an(lhs, AN_BSL) && is_cout_chain(lhs))
	    collect_cout_values(lhs, vals);
	else if (is_an(lhs, AN_PAREN) && is_cout_chain(lhs))
	    collect_cout_values(child(lhs, 0), vals);
	// else: lhs is "cout" — skip it

	// Add right side as a value
	vals.push_back(rhs);
    }

    // Emit a cout chain as calls to C++ stream wrappers.
    // cout << x << y << endl  →  __std_cout_*(x), __std_cout_*(y), __std_cout_endl()
    // The wrappers call real std::cout << operators, preserving iostream formatting.
    std::string emit_cout_chain(gp_tree_node *node)
    {
	std::vector<gp_tree_node *> vals;
	collect_cout_values(node, vals);

	std::string result;
	for (size_t i = 0; i < vals.size(); i++) {
	    gp_tree_node *v = vals[i];
	    if (i > 0) result += ", ";

	    // Stream manipulators (endl, flush, hex, oct, etc.)
	    if (v->type == GP_TERM && term_code(v) == GT_IDENT) {
		std::string manip = cout_manipulator(term_text(v));
		if (!manip.empty()) {
		    result += manip;
		    continue;
		}
	    }

	    std::string e = emit_expr(v);

	    // Dispatch by terminal type or inferred expression type
	    if (v->type == GP_TERM) {
		int code = term_code(v);
		if (code == GT_STRING)
		    result += "__std_cout_str(\"" + c_escape(term_text(v)) + "\")";
		else if (code == GT_CHAR_LIT)
		    result += "__std_cout_char(" + e + ")";
		else if (code == GT_REAL)
		    result += "__std_cout_double(" + e + ")";
		else if (code == GT_INTEGER)
		    result += "__std_cout_int(" + e + ")";
		else {
		    // Identifier — use type info to pick the right wrapper
		    TypeClass tc = lookup_var_type(term_text(v));
		    result += cout_call_for_type(tc, e);
		}
	    } else {
		// Complex expression — infer type
		TypeClass etc = infer_expr_cout_type(v);
		if (!e.empty() && e[0] == '"')
		    result += "__std_cout_str(" + e + ")";
		else
		    result += cout_call_for_type(etc, e);
	    }
	}

	return "(" + result + ")";
    }

    // Map TypeClass to the appropriate __std_cout_* wrapper call
    std::string cout_call_for_type(TypeClass tc, const std::string &expr)
    {
	switch (tc) {
	case TC_STDSTR: return "__std_cout_stdstr(" + expr + ")";
	case TC_STRING: return "__std_cout_str(" + expr + ")";
	case TC_CHAR:   return "__std_cout_char(" + expr + ")";
	case TC_DOUBLE: return "__std_cout_double(" + expr + ")";
	default:        return "__std_cout_int(" + expr + ")";
	}
    }

    // Map iostream manipulator names to wrapper calls
    static std::string cout_manipulator(const std::string &name)
    {
	if (name == "endl")        return "__std_cout_endl()";
	if (name == "flush")       return "__std_cout_flush()";
	if (name == "hex")         return "__std_cout_hex()";
	if (name == "oct")         return "__std_cout_oct()";
	if (name == "dec")         return "__std_cout_dec()";
	if (name == "fixed")       return "__std_cout_fixed()";
	if (name == "scientific")  return "__std_cout_scientific()";
	if (name == "left")        return "__std_cout_left()";
	if (name == "right")       return "__std_cout_right()";
	if (name == "boolalpha")   return "__std_cout_boolalpha()";
	if (name == "noboolalpha") return "__std_cout_noboolalpha()";
	if (name == "showbase")    return "__std_cout_showbase()";
	if (name == "noshowbase")  return "__std_cout_noshowbase()";
	return "";
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

	int an = an_code(node);

	// Parenthesized expression
	if (an == AN_PAREN)
	    return "(" + emit_expr(child(node, 0)) + ")";

	// cout << expr << endl → C output calls
	if (an == AN_BSL && is_cout_chain(node))
	    return emit_cout_chain(node);

	// Binary operators
	struct { int code; const char *c_op; } binops[] = {
	    {AN_ADD, "+"}, {AN_SUB, "-"}, {AN_MUL, "*"}, {AN_DIV, "/"},
	    {AN_MOD, "%"}, {AN_BSL, "<<"}, {AN_BSR, ">>"},
	    {AN_BITAND, "&"}, {AN_BITOR, "|"}, {AN_BITXOR, "^"},
	    {AN_LOR, "||"}, {AN_LAND, "&&"},
	    {AN_EQ, "=="}, {AN_NE, "!="}, {AN_LT, "<"}, {AN_GT, ">"},
	    {AN_LE, "<="}, {AN_GE, ">="}, {AN_THREE_WAY, "-"},
	    {AN_EQ3, "==="},
	    {AN_NONE, nullptr}
	};
	for (int i = 0; binops[i].c_op; i++) {
	    if (an == binops[i].code)
		return "(" + emit_expr(child(node, 0)) + " " +
		       binops[i].c_op + " " +
		       emit_expr(child(node, 1)) + ")";
	}

	// Assignment operators
	struct { int code; const char *c_op; } assigns[] = {
	    {AN_ASSIGN, "="}, {AN_ADD_ASSIGN, "+="}, {AN_SUB_ASSIGN, "-="},
	    {AN_MUL_ASSIGN, "*="}, {AN_DIV_ASSIGN, "/="}, {AN_MOD_ASSIGN, "%="},
	    {AN_BSL_ASSIGN, "<<="}, {AN_BSR_ASSIGN, ">>="}, {AN_BAND_ASSIGN, "&="},
	    {AN_BOR_ASSIGN, "|="}, {AN_XOR_ASSIGN, "^="},
	    {AN_NONE, nullptr}
	};
	for (int i = 0; assigns[i].c_op; i++) {
	    if (an == assigns[i].code)
		return emit_expr(child(node, 0)) + " " +
		       assigns[i].c_op + " " +
		       emit_expr(child(node, 1));
	}

	// Unary operators
	if (an == AN_NEG)      return "(-" + emit_expr(child(node, 0)) + ")";
	if (an == AN_POS)      return "(+" + emit_expr(child(node, 0)) + ")";
	if (an == AN_LNOT)     return "(!" + emit_expr(child(node, 0)) + ")";
	if (an == AN_BNOT)     return "(~" + emit_expr(child(node, 0)) + ")";
	if (an == AN_DEREF)    return "(*" + emit_expr(child(node, 0)) + ")";
	if (an == AN_ADDROF)   return "(&" + emit_expr(child(node, 0)) + ")";
	if (an == AN_PRE_INC)  return "(++" + emit_expr(child(node, 0)) + ")";
	if (an == AN_PRE_DEC)  return "(--" + emit_expr(child(node, 0)) + ")";
	if (an == AN_POST_INC) return "(" + emit_expr(child(node, 0)) + "++)";
	if (an == AN_POST_DEC) return "(" + emit_expr(child(node, 0)) + "--)";

	// Ternary
	if (an == AN_TERNARY)
	    return "(" + emit_expr(child(node, 0)) + " ? " +
		   emit_expr(child(node, 1)) + " : " +
		   emit_expr(child(node, 2)) + ")";

	// Function call
	if (an == AN_CALL) {
	    gp_tree_node *callee = child(node, 0);
	    std::string args = emit_arg_list(child(node, 1));

	    // Handle method calls: call(member(obj, method), args)
	    if (is_an(callee, AN_MEMBER) || is_an(callee, AN_ARROW_MEMBER)) {
		std::string obj = emit_expr(child(callee, 0));
		std::string method = term_text(child(callee, 1));
		bool is_arrow = is_an(callee, AN_ARROW_MEMBER);

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
	if (an == AN_MEMBER)
	    return emit_expr(child(node, 0)) + "." + term_text(child(node, 1));
	if (an == AN_ARROW_MEMBER)
	    return emit_expr(child(node, 0)) + "->" + term_text(child(node, 1));

	// Subscript
	if (an == AN_SUBSCRIPT)
	    return emit_expr(child(node, 0)) + "[" + emit_expr(child(node, 1)) + "]";

	// Namespace call: ns::func(args)
	if (an == AN_NS_CALL) {
	    std::string ns = term_text(child(node, 0));
	    std::string func = term_text(child(node, 1));
	    std::string args = emit_arg_list(child(node, 2));
	    std::string mangled = "__" + ns_prefix(ns) + "_" + func;
	    ns_funcs_used.insert(mangled);
	    return mangled + "(" + args + ")";
	}
	if (an == AN_NS_NAME) {
	    std::string ns = term_text(child(node, 0));
	    std::string func = term_text(child(node, 1));
	    std::string mangled = "__" + ns_prefix(ns) + "_" + func;
	    ns_funcs_used.insert(mangled);
	    return mangled;
	}

	// Cast: (type)expr
	if (an == AN_CAST)
	    return "((" + emit_type(child(node, 0)) + ")" +
		   emit_expr(child(node, 1)) + ")";

	// sizeof
	if (an == AN_SIZEOF_TYPE)
	    return "sizeof(" + emit_type(child(node, 0)) + ")";
	if (an == AN_SIZEOF_EXPR)
	    return "sizeof(" + emit_expr(child(node, 0)) + ")";

	// new/delete
	if (an == AN_NEW_PLAIN)
	    return "malloc(sizeof(" + term_text(child(node, 0)) + "_t))";
	if (an == AN_NEW_CTOR)
	    return "malloc(sizeof(" + term_text(child(node, 0)) + "_t))";

	// Comma operator
	if (an == AN_COMMA)
	    return "(" + emit_expr(child(node, 0)) + ", " +
		   emit_expr(child(node, 1)) + ")";

	// Compound literal
	if (an == AN_COMPOUND_LIT)
	    return "((" + emit_type(child(node, 0)) + "){" +
		   emit_init_list(child(node, 1)) + "})";

	// Initializer list in expression context
	if (an == AN_INIT_LIST)
	    return "{" + emit_init_list(child(node, 0)) + "}";
	if (an == AN_EMPTY_INIT)
	    return "{0}";

	// Fallback: if it has children, try first child
	if (nchildren(node) > 0)
	    return emit_expr(child(node, 0));

	return "/* unknown expr: " + std::string(anode_name(node)) + " */0";
    }

    // Emit argument list (flattened from nested arg_list nodes)
    std::string emit_arg_list(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";
	if (node->type == GP_TERM) return emit_expr(node);
	if (!is_an(node, AN_ARG_LIST))
	    return emit_expr(node);
	return emit_arg_list(child(node, 0)) + ", " +
	       emit_expr(child(node, 1));
    }

    // Emit initializer list
    std::string emit_init_list(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";

	// desig_init(designation_opt, initializer)
	if (is_an(node, AN_DESIG_INIT)) {
	    gp_tree_node *desig = child(node, 0);
	    std::string val = emit_init_item(child(node, 1));
	    if (!is_nil(desig))
		return emit_designator(desig) + " = " + val;
	    return val;
	}

	// init_seq(prev_list, designation_opt, initializer)
	if (is_an(node, AN_INIT_SEQ)) {
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
	if (is_an(node, AN_INIT_LIST))
	    return "{" + emit_init_list(child(node, 0)) + "}";
	if (is_an(node, AN_EMPTY_INIT))
	    return "{0}";
	return emit_expr(node);
    }

    std::string emit_designator(gp_tree_node *node)
    {
	if (!node) return "";
	if (is_an(node, AN_MEMBER_DESIG))
	    return "." + term_text(child(node, 0));
	if (is_an(node, AN_INDEX_DESIG))
	    return "[" + emit_expr(child(node, 0)) + "]";
	if (is_an(node, AN_DESIG_CHAIN))
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

	int an = an_code(node);

	// Statement list
	if (an == AN_STMT_LIST) {
	    emit_stmt(child(node, 0));
	    emit_stmt(child(node, 1));
	    return;
	}

	// Expression statement
	if (an == AN_EXPR_STMT) {
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
	if (an == AN_BLOCK) {
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
	if (an == AN_DECL) {
	    emit_decl_stmt(node);
	    return;
	}

	// Constructor-call declaration: ClassName var(args);
	if (an == AN_CTOR_DECL) {
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
	if (an == AN_IF) {
	    emit_indent();
	    O("if (%s)\n", emit_expr(child(node, 0)).c_str());
	    indent_level++;
	    emit_stmt(child(node, 1));
	    indent_level--;
	    return;
	}
	if (an == AN_IF_ELSE) {
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
	if (an == AN_WHILE) {
	    emit_indent();
	    O("while (%s)\n", emit_expr(child(node, 0)).c_str());
	    indent_level++;
	    emit_stmt(child(node, 1));
	    indent_level--;
	    return;
	}

	// Do-while
	if (an == AN_DO_WHILE) {
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
	if (an == AN_FOR) {
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
	if (an == AN_FOR_DECL) {
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
	if (an == AN_RETURN_VAL) {
	    // Call destructors in LIFO order
	    for (int i = (int)scope_class_vars.size() - 1; i >= 0; i--) {
		size_t sep = scope_class_vars[i].find('|');
		if (sep != std::string::npos) {
		    std::string vn = scope_class_vars[i].substr(0, sep);
		    std::string cn = scope_class_vars[i].substr(sep + 1);
		    emit_indent();
		    if (cn == "__string")
			O("__madc_string_destruct(%s);\n", vn.c_str());
		    else
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
	if (an == AN_RETURN_MULTI) {
	    emit_indent();
	    O("return %s;\n", emit_expr(child(node, 0)).c_str());
	    return;
	}

	// Break / Continue
	if (an == AN_BREAK)    { emit_indent(); O("break;\n"); return; }
	if (an == AN_CONTINUE) { emit_indent(); O("continue;\n"); return; }

	// Goto / Label
	if (an == AN_GOTO) {
	    emit_indent();
	    O("goto %s;\n", term_text(child(node, 0)).c_str());
	    return;
	}
	if (an == AN_GOTO_INDIRECT) {
	    emit_indent();
	    O("goto *%s;\n", emit_expr(child(node, 0)).c_str());
	    return;
	}
	if (an == AN_LABEL) {
	    // label has children: [ident, statement]
	    O("%s:;\n", term_text(child(node, 0)).c_str());
	    emit_stmt(child(node, 1));
	    return;
	}

	// Switch
	if (an == AN_SWITCH) {
	    emit_indent();
	    O("switch (%s)\n", emit_expr(child(node, 0)).c_str());
	    emit_stmt(child(node, 1));
	    return;
	}

	// Case / Default (labeled statements)
	if (an == AN_CASE) {
	    emit_indent();
	    O("case %s:\n", emit_expr(child(node, 0)).c_str());
	    indent_level++;
	    emit_stmt(child(node, 1));
	    indent_level--;
	    return;
	}
	if (an == AN_CASE_RANGE) {
	    emit_indent();
	    O("case %s ... %s:\n", emit_expr(child(node, 0)).c_str(),
	      emit_expr(child(node, 1)).c_str());
	    indent_level++;
	    emit_stmt(child(node, 2));
	    indent_level--;
	    return;
	}
	if (an == AN_DEFAULT) {
	    emit_indent();
	    O("default:\n");
	    indent_level++;
	    emit_stmt(child(node, 0));
	    indent_level--;
	    return;
	}

	// Try/catch
	if (an == AN_TRY) {
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
	if (an == AN_THROW_EXPR) {
	    emit_indent();
	    O("/* throw */ ;\n");
	    return;
	}
	if (an == AN_THROW) {
	    emit_indent();
	    O("/* throw */ ;\n");
	    return;
	}

	// Delete
	if (an == AN_DELETE) {
	    emit_indent();
	    O("free(%s);\n", emit_expr(child(node, 0)).c_str());
	    return;
	}

	// Defer
	if (an == AN_DEFER) {
	    emit_indent();
	    O("/* defer */ ;\n");
	    return;
	}

	// Fallback
	emit_indent();
	O("/* TODO stmt: %s */\n", anode_name(node));
    }

    // ---------------------------------------------------------------
    // Declaration emission (inside function bodies)
    // ---------------------------------------------------------------

    // Classify a type string for cout format inference
    static TypeClass classify_type(const std::string &type)
    {
	if (type.find("char") != std::string::npos) {
	    if (type.find("*") != std::string::npos) return TC_STRING;
	    return TC_CHAR;
	}
	if (type.find("const char") != std::string::npos) return TC_STRING;
	if (type.find("float") != std::string::npos) return TC_DOUBLE;
	if (type.find("double") != std::string::npos) return TC_DOUBLE;
	return TC_INT;
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
		local_var_types[vname] = TC_CLASS;  // class objects print as int
	    } else {
		bool has_ptr = is_an(decls, AN_PTR_DECL);
		TypeClass tc = classify_type(type);
		if (has_ptr && tc == TC_CHAR) tc = TC_STRING;  // char * → string
		local_var_types[vname] = tc;
	    }
	}
	// Handle decl_list / init_decl
	if (is_an(decls, AN_DECL_LIST) || is_an(decls, AN_INIT_DECL)) {
	    track_decl_types(type, child(decls, 0));
	    if (nchildren(decls) > 1)
		track_decl_types(type, child(decls, 1));
	}
    }

    // Track parameter types for cout format inference
    void track_param_types(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;
	int an = an_code(node);
	if (an == AN_PARAM) {
	    std::string type = emit_type(child(node, 0));
	    gp_tree_node *decl = child(node, 1);
	    if (!is_nil(decl)) track_decl_types(type, decl);
	} else if (an == AN_PARAM_LIST) {
	    track_param_types(child(node, 0));
	    track_param_types(child(node, 1));
	} else if (an == AN_PARAM_VA) {
	    track_param_types(child(node, 0));
	}
    }

    // Check if a declaration_specifiers chain contains GT_STRING_T
    static bool is_string_type(gp_tree_node *specs)
    {
	if (!specs) return false;
	if (specs->type == GP_TERM) return term_code(specs) == 318;
	if (an_code(specs) == AN_QUAL)
	    return is_string_type(child(specs, 0)) || is_string_type(child(specs, 1));
	return false;
    }

    // Emit a string variable declaration with runtime management
    void emit_string_decl(gp_tree_node *decls)
    {
	if (is_nil(decls)) return;

	int an = an_code(decls);

	// init_decl(name, initializer) — string x = "literal";
	if (an == AN_INIT_DECL) {
	    std::string vname = extract_name(child(decls, 0));
	    std::string init_expr = emit_expr(child(decls, 1));
	    if (!vname.empty()) {
		emit_indent();
		O("char %s[MADC_STRING_SIZE];\n", vname.c_str());
		emit_indent();
		O("__madc_string_construct(%s);\n", vname.c_str());
		emit_indent();
		O("__madc_string_assign_cstr(%s, %s);\n",
		  vname.c_str(), init_expr.c_str());
		local_var_types[vname] = TC_STDSTR;
		scope_class_vars.push_back(vname + "|__string");
	    }
	    return;
	}

	// decl_list — multiple declarators
	if (an == AN_DECL_LIST) {
	    emit_string_decl(child(decls, 0));
	    emit_string_decl(child(decls, 1));
	    return;
	}

	// Plain identifier — string x; (no initializer)
	if (decls->type == GP_TERM) {
	    std::string vname = term_text(decls);
	    if (!vname.empty()) {
		emit_indent();
		O("char %s[MADC_STRING_SIZE];\n", vname.c_str());
		emit_indent();
		O("__madc_string_construct(%s);\n", vname.c_str());
		local_var_types[vname] = TC_STDSTR;
		scope_class_vars.push_back(vname + "|__string");
	    }
	    return;
	}
    }

    void emit_decl_stmt(gp_tree_node *node)
    {
	// decl(declaration_specifiers, init_declarator_list_opt)
	gp_tree_node *specs = child(node, 0);
	gp_tree_node *decls = child(node, 1);

	// String type: managed std::string with runtime wrappers
	if (is_string_type(specs)) {
	    emit_string_decl(decls);
	    return;
	}

	std::string type = emit_type(specs);

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

	if (is_an(decls, AN_FUNC_DECL) && is_known_class(clean_type)) {
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
	if (!is_an(node, AN_DECL)) return emit_expr(node);
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

	int an = an_code(node);

	// param(declaration_specifiers, declarator_or_abstract)
	if (an == AN_PARAM) {
	    std::string type = emit_type(child(node, 0));
	    gp_tree_node *decl = child(node, 1);
	    if (is_nil(decl)) return type;
	    // The declarator may have pointer, name, etc.
	    std::string d = emit_declarator_str(decl);
	    if (d.empty()) return type;
	    return type + " " + d;
	}

	// param_list(prev, next_param)
	if (an == AN_PARAM_LIST)
	    return emit_param_list(child(node, 0)) + ", " +
		   emit_param_list(child(node, 1));

	// param_va(param_list) — variadic
	if (an == AN_PARAM_VA)
	    return emit_param_list(child(node, 0)) + ", ...";

	// type_name used as param (abstract)
	if (an == AN_TYPE_NAME)
	    return emit_type(node);

	// qual (declaration_specifiers used as param type)
	if (an == AN_QUAL)
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
	if (is_an(inner, AN_PTR_DECL)) {
	    ptr_str = emit_pointer(child(inner, 0));
	    inner = child(inner, 1);
	}
	if (is_an(inner, AN_REF_DECL)) {
	    ptr_str = "&";
	    inner = child(inner, 0);
	}

	// inner should be func_decl(name, params) or just identifier
	std::string func_name;
	std::string params;

	if (is_an(inner, AN_FUNC_DECL)) {
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
	if (is_an(node, AN_STRUCT_BODY)) {
	    emit_struct_body(child(node, 0));
	    emit_struct_field(child(node, 1));
	    return;
	}
	emit_struct_field(node);
    }

    void emit_struct_field(gp_tree_node *node)
    {
	if (!node) return;
	if (is_an(node, AN_STRUCT_FIELD)) {
	    std::string type = emit_type(child(node, 0));
	    gp_tree_node *decls = child(node, 1);
	    OH("    %s %s;\n", type.c_str(), emit_declarator_str(decls).c_str());
	    return;
	}
	// Nested struct/union/enum definitions
	if (is_an(node, AN_STRUCT_DEF)) {
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
	if (is_an(node, AN_ENUM_LIST)) {
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
	else if (is_an(node, AN_ENUM_ASSIGN))
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

	if (is_an(node, AN_CLASS_DEF)) {
	    // class_def(name, class_body)
	    class_name = term_text(child(node, 0));
	    body_node = child(node, 1);
	} else if (is_an(node, AN_CLASS_INHERIT)) {
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
	if (is_an(node, AN_CLASS_BODY)) {
	    collect_class_fields(child(node, 0));
	    gp_tree_node *item = child(node, 1);
	    if (is_an(item, AN_DECL)) {
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
	if (is_an(node, AN_CLASS_BODY)) {
	    emit_class_fields(child(node, 0));
	    emit_class_field_item(child(node, 1));
	    return;
	}
	emit_class_field_item(node);
    }

    void emit_class_field_item(gp_tree_node *node)
    {
	if (!node) return;
	// Only emit field declarations, skip methods/ctors/access specs
	if (is_an(node, AN_DECL)) {
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
	if (is_an(node, AN_CLASS_BODY)) {
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
	int an = an_code(node);

	// method(declaration_specifiers, declarator, compound_statement)
	if (an == AN_METHOD) {
	    std::string prev_class = current_class;
	    current_class = class_name;

	    std::string ret_type = emit_type(child(node, 0));
	    gp_tree_node *decl = child(node, 1);
	    gp_tree_node *body_node = child(node, 2);

	    // Extract method name and params from declarator
	    std::string method_name;
	    std::string params;
	    gp_tree_node *inner = decl;
	    if (is_an(inner, AN_FUNC_DECL)) {
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
	if (an == AN_CTOR) {
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
	if (an == AN_DTOR) {
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
	if (is_an(node, AN_TU)) {
	    emit_top_level(child(node, 0));
	    emit_top_level(child(node, 1));
	    return;
	}

	// Function definition
	if (is_an(node, AN_FUNC_DEF)) {
	    emit_func_def(node);
	    return;
	}

	// Top-level declaration (globals, prototypes, typedefs)
	if (is_an(node, AN_DECL)) {
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
		    while (is_an(s, AN_QUAL)) {
			gp_tree_node *c0 = child(s, 0);
			if (is_an(c0, AN_STRUCT_DEF)) {
			    emit_struct_def_toplevel(c0);
			    return;
			}
			if (is_an(c0, AN_ENUM_DEF)) {
			    emit_enum_def_toplevel(c0);
			    return;
			}
			s = child(s, 1);
		    }
		    // Single specifier
		    if (is_an(specs, AN_STRUCT_DEF)) {
			emit_struct_def_toplevel(specs);
			return;
		    }
		    if (is_an(specs, AN_ENUM_DEF)) {
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
	if (is_an(node, AN_STRUCT_DEF)) {
	    emit_struct_def_toplevel(node);
	    return;
	}
	if (is_an(node, AN_ENUM_DEF)) {
	    emit_enum_def_toplevel(node);
	    return;
	}

	// Class definition → C struct + free functions
	if (is_an(node, AN_CLASS_DEF) || is_an(node, AN_CLASS_INHERIT)) {
	    emit_class_def(node);
	    return;
	}

	// Using / namespace — skip for C output
	if (is_an(node, AN_USING_NS) || is_an(node, AN_USING_DECL) ||
	    is_an(node, AN_PREFER) || is_an(node, AN_NAMESPACE_DEF)) {
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

	// String runtime (manages std::string objects from C)
	header += "#define MADC_STRING_SIZE 32\n";
	header += "extern void __madc_string_construct(void *);\n";
	header += "extern void __madc_string_destruct(void *);\n";
	header += "extern void __madc_string_assign_cstr(void *, const char *);\n";
	header += "extern void __madc_string_assign(void *, void *);\n";
	header += "extern const char *__madc_string_cstr(void *);\n";
	header += "extern long __madc_string_length(void *);\n";
	header += "extern void __madc_string_append_cstr(void *, const char *);\n";
	header += "extern void __madc_string_append(void *, void *);\n";
	header += "extern void __std_cout_stdstr(void *);\n";
	header += "\n";

	// C++ iostream wrappers (call real std::cout << operators)
	header += "extern void __std_cout_str(const char *);\n";
	header += "extern void __std_cout_int(long);\n";
	header += "extern void __std_cout_uint(unsigned long);\n";
	header += "extern void __std_cout_char(int);\n";
	header += "extern void __std_cout_double(double);\n";
	header += "extern void __std_cout_endl(void);\n";
	header += "extern void __std_cout_flush(void);\n";
	header += "extern void __std_cout_hex(void);\n";
	header += "extern void __std_cout_oct(void);\n";
	header += "extern void __std_cout_dec(void);\n";
	header += "extern void __std_cout_fixed(void);\n";
	header += "extern void __std_cout_scientific(void);\n";
	header += "extern void __std_cout_left(void);\n";
	header += "extern void __std_cout_right(void);\n";
	header += "extern void __std_cout_boolalpha(void);\n";
	header += "extern void __std_cout_noboolalpha(void);\n";
	header += "extern void __std_cout_showbase(void);\n";
	header += "extern void __std_cout_noshowbase(void);\n";
	header += "extern void __std_cout_setw(int);\n";
	header += "extern void __std_cout_setprecision(int);\n";
	header += "extern void __std_cout_setfill(int);\n";
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
