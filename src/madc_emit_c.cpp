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
#include <unordered_map>
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

// c2mir reserves C++ keywords even in C11 mode — prefix them with _
static std::string safe_ident(const std::string &name)
{
    static const std::set<std::string> reserved = {
	"class", "new", "delete", "this", "virtual", "template",
	"namespace", "using", "throw", "catch", "try", "private",
	"public", "protected", "operator"
    };
    if (reserved.count(name))
	return "_" + name;
    return name;
}

// Gecko terminal codes (must match madc_grammar.cpp)
enum {
    GT_IDENT    = 256,
    GT_INTEGER  = 257,
    GT_REAL     = 258,
    GT_STRING   = 259,
    GT_CHAR_LIT = 260,

    // Multi-char operators
    GT_EQ       = 350,
    GT_NE       = 351,
    GT_LE       = 352,
    GT_GE       = 353,
    GT_THREE_WAY= 373,
};

// Map operator symbol string to function suffix
static std::string op_suffix(const std::string &sym)
{
    if (sym == "==")  return "op_eq";
    if (sym == "!=")  return "op_ne";
    if (sym == "<")   return "op_lt";
    if (sym == ">")   return "op_gt";
    if (sym == "<=")  return "op_le";
    if (sym == ">=")  return "op_ge";
    if (sym == "+")   return "op_add";
    if (sym == "-")   return "op_sub";
    if (sym == "*")   return "op_mul";
    if (sym == "/")   return "op_div";
    if (sym == "%")   return "op_mod";
    if (sym == "<=>") return "op_cmp";
    return "op_unknown";
}

// Convert an operator terminal node to its string symbol
static std::string op_terminal_to_str(gp_tree_node *node)
{
    if (!node || node->type != GP_TERM) return "";
    int code = node->val.term.code;
    switch (code) {
    case '+':      return "+";
    case '-':      return "-";
    case '*':      return "*";
    case '/':      return "/";
    case '%':      return "%";
    case '<':      return "<";
    case '>':      return ">";
    case GT_EQ:    return "==";
    case GT_NE:    return "!=";
    case GT_LE:    return "<=";
    case GT_GE:    return ">=";
    case GT_THREE_WAY: return "<=>";
    default:       return "";
    }
}

// Map AN_* binary operator code to the operator symbol string
static std::string an_to_op_sym(int an)
{
    switch (an) {
    case AN_EQ:  return "==";
    case AN_NE:  return "!=";
    case AN_LT:  return "<";
    case AN_GT:  return ">";
    case AN_LE:  return "<=";
    case AN_GE:  return ">=";
    case AN_ADD: return "+";
    case AN_SUB: return "-";
    case AN_MUL: return "*";
    case AN_DIV: return "/";
    case AN_MOD: return "%";
    case AN_THREE_WAY: return "<=>";
    default:     return "";
    }
}

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

    // When true, use calloc instead of malloc for new expressions
    bool prefer_calloc = false;

    // Try block nesting depth — for unique __try_ctx variable names
    int try_depth = 0;
    // Nesting level for try blocks — used to determine whether objects
    // should persist to function scope (level 0) or be destroyed at
    // try scope (level > 0).
    int try_nesting = 0;

    // Guard variables for try-body objects: maps "varname|classname"
    // to a guard variable name. Function-scope dtors check the guard.
    std::map<std::string, std::string> try_dtor_guards;

    // Per-function va_list variable names (for va_start/va_end emission)
    std::set<std::string> va_list_vars;

    // Last named parameter of the current variadic function (for va_start)
    std::string current_func_last_param;

    // Per-function reference parameters (name → true means it's a ref/pointer)
    // Variables in this set are emitted as (*name) in expressions.
    std::set<std::string> ref_vars;

    // Per-program map: function name → vector<bool> of ref param positions
    // Used at call sites to emit &arg for ref parameters.
    std::map<std::string, std::vector<bool>> func_ref_param_map;

    // Per-program map: function name → (return_type, param_list)
    // Used for auto type inference on function pointer assignments.
    struct FuncSig {
	std::string ret_type;   // e.g. "int"
	std::string param_list; // e.g. "int, int" or "int x" — types only
    };
    std::map<std::string, FuncSig> func_signatures;

    // Global string variables — collected during emit_top_level,
    // emitted as __madc_init/cleanup_globals() after all top-level decls.
    struct GlobalStringVar {
	std::string name;
	std::string init_expr;  // empty if no initializer
    };
    std::vector<GlobalStringVar> global_strings;

    // Top-level expression statements (assignments, etc.) captured for
    // emission inside __madc_init_globals().
    std::vector<std::string> global_init_stmts;

    // Set to true while emitting the body of main() so that return
    // statements inject __madc_cleanup_globals().
    bool emitting_main;

    // Depth counter for multi-dimensional char array initialization.
    // Incremented at each `{...}` nesting level inside a char[] initializer.
    // When >= 1, string literals in init items are expanded to explicit bytes
    // to work around a c2mir bug where string literal offsets are wrong in
    // 3D+ char arrays.
    int char_array_init_depth = 0;

    // VLA param side effects: expressions from VLA parameter sizes that
    // need to be evaluated at function entry to preserve side effects.
    // E.g. `int sub(int i, int arr[i++])` → emit `(void)(i++);` in body.
    std::vector<std::string> vla_param_side_effects;

    // VLA typedef tracking: typedef name → size expression string.
    // When a VLA typedef like `typedef int c[n+2]` is encountered inside
    // a function, instead of emitting the typedef (which c2mir rejects),
    // we store the computed size expression and replace sizeof(c) with it.
    std::map<std::string, std::string> vla_typedef_sizes;

    // Check if a name is a global string variable tracked in global_strings
    bool is_global_string(const std::string &name) {
	for (auto &gsv : global_strings)
	    if (gsv.name == name) return true;
	return false;
    }

    // Look up variable type: local overlay first, then global_strings,
    // then sema.  Global string vars tracked in global_strings always
    // return TC_CLASS (sema mis-classifies them as TC_STRING because it
    // maps GT_STRING_T → "const char *" internally).
    TypeClass lookup_var_type(const std::string &name) {
	auto it = local_var_types.find(name);
	if (it != local_var_types.end()) return it->second;
	if (is_global_string(name)) return TC_CLASS;
	// Inside class methods: check class field types
	if (!current_class.empty() && current_class_fields.count(name) && sema) {
	    const SemaClassInfo *ci = sema->get_class(current_class);
	    if (ci) {
		auto fit = ci->field_types.find(name);
		if (fit != ci->field_types.end()) return fit->second;
	    }
	}
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
	if (sema) {
	    std::string cls = sema->get_var_class(name);
	    if (!cls.empty()) return cls;
	}
	if (is_global_string(name)) return "string";
	return "";
    }

    // Collect catch clauses from catch_list tree into a flat vector
    void collect_catch_clauses(gp_tree_node *node,
			       std::vector<gp_tree_node *> &out)
    {
	if (!node) return;
	int an = an_code(node);
	if (an == AN_CATCH || an == AN_CATCH_ALL) {
	    out.push_back(node);
	    return;
	}
	if (an == AN_CATCH_LIST) {
	    collect_catch_clauses(child(node, 0), out);
	    collect_catch_clauses(child(node, 1), out);
	    return;
	}
	// Fallback: might be a single clause directly
	out.push_back(node);
    }

    // Determine the __madc_throw_* function for a throw expression type
    std::string throw_func_for_type(TypeClass tc)
    {
	switch (tc) {
	case TC_DOUBLE: return "__madc_throw_double";
	case TC_STRING: return "__madc_throw_cstr";
	default:        return "__madc_throw_int";
	}
    }

    // Map catch type string to exception type constant (1=int, 2=double, 3=cstr)
    int exception_type_for_catch(const std::string &type_str)
    {
	if (type_str.find("double") != std::string::npos ||
	    type_str.find("float") != std::string::npos)
	    return 2;
	if (type_str.find("char") != std::string::npos &&
	    type_str.find("*") != std::string::npos)
	    return 3;
	if (type_str == "const char *")
	    return 3;
	return 1;  // int, long, etc.
    }

    // Map exception type constant to the retrieval function
    std::string exception_getter_for_type(int exc_type)
    {
	switch (exc_type) {
	case 2:  return "__madc_exception_double";
	case 3:  return "__madc_exception_cstr";
	default: return "__madc_exception_int";
	}
    }

    // Emit the contents of a compound statement WITHOUT braces
    // (unwraps AN_BLOCK to keep declarations at enclosing scope)
    void emit_stmt_unwrapped(gp_tree_node *node)
    {
	if (!node) return;
	if (is_an(node, AN_BLOCK)) {
	    emit_stmt(child(node, 0));
	    return;
	}
	emit_stmt(node);
    }

    // Check if a statement node is a declaration (needs hoisting out
    // of setjmp if-blocks to keep variables in scope for dtors).
    bool is_decl_stmt(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return false;
	int an = an_code(node);
	return an == AN_DECL || an == AN_CTOR_DECL;
    }

    // Emit ONLY the variable declaration part of a ctor decl
    // (struct Foo a;) without the constructor call. Used for hoisting.
    void emit_ctor_decl_only(gp_tree_node *node)
    {
	std::string type = emit_type(child(node, 0));
	std::string vname = term_text(child(node, 1));
	emit_indent();
	O("%s %s;\n", type.c_str(), vname.c_str());
	// Track type info
	std::string clean_type = type;
	if (clean_type.substr(0, 7) == "struct ")
	    clean_type = clean_type.substr(7);
	if (is_known_class(clean_type))
	    local_var_class_map[vname] = clean_type;
    }

    // Emit ONLY the constructor call part of a ctor decl
    // (Foo__Foo(&a, args);) without the variable declaration.
    // If guard_var is non-empty, set the guard after construction and
    // register it for guarded function-scope cleanup.
    void emit_ctor_call_only(gp_tree_node *node, const std::string &guard_var = "")
    {
	std::string type = emit_type(child(node, 0));
	std::string vname = term_text(child(node, 1));
	std::string args = emit_arg_list(child(node, 2));
	std::string clean_type = type;
	if (clean_type.substr(0, 7) == "struct ")
	    clean_type = clean_type.substr(7);
	emit_indent();
	O("%s__%s(&%s, %s);\n", clean_type.c_str(), clean_type.c_str(),
	  vname.c_str(), args.c_str());
	if (class_has_dtor(clean_type)) {
	    std::string key = vname + "|" + clean_type;
	    scope_class_vars.push_back(key);
	    if (!guard_var.empty()) {
		// Set guard to 1 after successful construction
		emit_indent();
		O("%s = 1;\n", guard_var.c_str());
		try_dtor_guards[key] = guard_var;
	    }
	}
    }


    // Collect all statements from a compound statement's stmt_list,
    // flattening left-recursive AN_STMT_LIST nodes into a flat vector.
    void collect_stmts(gp_tree_node *node,
		       std::vector<gp_tree_node *> &out)
    {
	if (!node || node->type == GP_NIL) return;
	if (is_an(node, AN_STMT_LIST)) {
	    collect_stmts(child(node, 0), out);
	    collect_stmts(child(node, 1), out);
	    return;
	}
	out.push_back(node);
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
	for (size_t i = 0; i < s.size(); i++) {
	    unsigned char c = (unsigned char)s[i];
	    unsigned char next = (i + 1 < s.size()) ? (unsigned char)s[i + 1] : 0;
	    switch (c) {
	    case '\n': out += "\\n"; break;
	    case '\r': out += "\\r"; break;
	    case '\t': out += "\\t"; break;
	    case '\\': out += "\\\\"; break;
	    case '"':  out += "\\\""; break;
	    case '\0':
		// Use 3-digit octal so a following digit is not consumed as part
		// of the escape sequence (e.g. "\0002" would be parsed as \000
		// followed by 2, not as octal \002).
		out += "\\000";
		break;
	    default:
		if (c < 32) {
		    char buf[16];
		    // Use \xNN hex escape, but if the next char is a hex digit
		    // we must terminate and reopen the string literal so the
		    // compiler does not extend the escape sequence.
		    bool next_is_hex = isxdigit(next) != 0;
		    if (next_is_hex) {
			snprintf(buf, sizeof(buf), "\\x%02x\" \"", c);
		    } else {
			snprintf(buf, sizeof(buf), "\\x%02x", c);
		    }
		    out += buf;
		} else {
		    out += (char)c;
		}
		break;
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
	if (name == "puti")             return "madc_puti";
	if (name == "putu")             return "madc_putu";
	if (name == "putd")             return "madc_putd";
	if (name == "putf")             return "madc_putf";
	if (name == "printstr")         return "madc_printstr";
	// JIT-backend va helpers → standard C11 equivalents
	if (name == "__madc_vsprintf")  return "vsprintf";
	if (name == "__madc_vsnprintf") return "vsnprintf";
	if (name == "__madc_vprintf")   return "vprintf";
	if (name == "__madc_vfprintf")  return "vfprintf";
	// C++ string-to-number → C equivalents (args already coerced via string_cstr)
	if (name == "stoi")             return "atoi";
	if (name == "stol")             return "atol";
	if (name == "stof" || name == "stod") return "atof";
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

	// typeof — emit as __typeof__ for c2mir/GCC compatibility
	if (an == AN_TYPEOF_EXPR)
	    return "__typeof__(" + emit_expr(child(node, 0)) + ")";
	if (an == AN_TYPEOF_TYPE)
	    return "__typeof__(" + emit_type(child(node, 0)) + ")";

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
	    return safe_ident(term_text(node));
	if (is_an(node, AN_ENUM_ASSIGN))
	    return safe_ident(term_text(child(node, 0))) + " = " + emit_expr(child(node, 1));
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

    // Count the number of array dimensions in a declarator chain.
    // E.g. a[2][3][9] → 3, a[2] → 1, a → 0.
    static int array_dims(gp_tree_node *node) {
	if (!node || node->type != GP_ANODE) return 0;
	int an = an_code(node);
	if (an == AN_ARRAY_DECL)
	    return 1 + array_dims(child(node, 0));
	if (an == AN_INIT_DECL)
	    return array_dims(child(node, 0));
	if (an == AN_DECL_LIST)
	    return array_dims(child(node, 0));
	return 0;
    }

    // Return true if an expression subtree contains any GT_IDENT terminal,
    // meaning it references a runtime variable and is not a compile-time
    // constant.  Used to detect VLA size expressions.
    static bool expr_has_ident(gp_tree_node *node) {
	if (!node) return false;
	if (node->type == GP_TERM)
	    return term_code(node) == GT_IDENT;
	if (node->type == GP_ANODE) {
	    int an = an_code(node);
	    // sizeof(expr) and sizeof(type) are compile-time constants —
	    // identifiers inside them don't make the expression non-constant.
	    if (an == AN_SIZEOF_EXPR || an == AN_SIZEOF_TYPE)
		return false;
	    for (int i = 0; i < nchildren(node); i++)
		if (expr_has_ident(child(node, i)))
		    return true;
	}
	return false;
    }

    // Check whether a declarator contains any VLA dimension (an
    // AN_ARRAY_DECL whose size expression contains GT_IDENT).
    static bool declarator_has_vla(gp_tree_node *node) {
	if (!node || node->type != GP_ANODE) return false;
	int an = an_code(node);
	if (an == AN_ARRAY_DECL) {
	    gp_tree_node *sz = child(node, 1);
	    if (sz && !is_nil(sz) && expr_has_ident(sz))
		return true;
	    return declarator_has_vla(child(node, 0));
	}
	if (an == AN_INIT_DECL)
	    return declarator_has_vla(child(node, 0));
	return false;
    }

    // Return true if node is a plain function declarator (AN_FUNC_DECL at the
    // top level, not wrapped in AN_PTR_DECL which would make it a function
    // pointer).  Used to skip local function prototype entries in mixed
    // declarations like `float fx(), a, b, c;`.
    static bool is_plain_func_decl(gp_tree_node *node) {
	if (!node || node->type != GP_ANODE) return false;
	return an_code(node) == AN_FUNC_DECL;
    }

    // Collect all leaf declarators from an AN_DECL_LIST tree into a vector.
    static void collect_decl_list(gp_tree_node *node,
				  std::vector<gp_tree_node *> &out) {
	if (!node || node->type == GP_NIL) return;
	if (node->type == GP_ANODE && an_code(node) == AN_DECL_LIST) {
	    collect_decl_list(child(node, 0), out);
	    collect_decl_list(child(node, 1), out);
	} else {
	    out.push_back(node);
	}
    }

    // Emit a complete declarator (pointer + name + array/func suffixes)
    // Returns: "**name" or "name[10]" or "(*name)(int, int)" etc.
    std::string emit_declarator_str(gp_tree_node *node)
    {
	if (!node) return "";
	if (node->type == GP_TERM) return safe_ident(term_text(node));

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
	    gp_tree_node *dd = child(node, 0);
	    std::string nm = emit_declarator_str(dd);
	    gp_tree_node *sz = child(node, 1);
	    // Wrap in parens if the direct declarator has a pointer:
	    // int (*a)[2] needs parens to distinguish from int *a[2]
	    if (is_an(dd, AN_PTR_DECL) || is_an(dd, AN_REF_DECL))
		nm = "(" + nm + ")";
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

	// Deref: *p where p is char* or char[] → char
	if (an == AN_DEREF) {
	    TypeClass inner = infer_expr_cout_type(child(node, 0));
	    if (inner == TC_STRING) return TC_CHAR;  // deref string/char* → char
	    if (inner == TC_CHAR)   return TC_CHAR;  // deref char[] → char
	    return TC_INT;
	}

	// Subscript: a[i] where a is char* or char[] → char
	if (an == AN_SUBSCRIPT) {
	    TypeClass inner = infer_expr_cout_type(child(node, 0));
	    if (inner == TC_STRING) return TC_CHAR;
	    // char array (TC_CHAR from sema for char[N]): subscripting still gives char
	    if (inner == TC_CHAR) return TC_CHAR;
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
	    // char array (TC_CHAR) + int decays to char* (TC_STRING)
	    if (l == TC_CHAR || r == TC_CHAR) return TC_STRING;
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

	// Member access — look up field type in struct/class info
	if (an == AN_MEMBER || an == AN_ARROW_MEMBER) {
	    std::string field = term_text(child(node, 1));
	    if (!field.empty() && sema) {
		// Check struct field types
		TypeClass tc = sema->get_struct_field_type(field);
		if (tc != TC_INT) return tc;
		// Check class field types
		for (auto &kv : sema->class_info) {
		    auto fit = kv.second.field_types.find(field);
		    if (fit != kv.second.field_types.end())
			return fit->second;
		}
	    }
	    return TC_INT;
	}

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

    // Check if a node is an ostream identifier (cout or cerr)
    bool is_ostream_ident(gp_tree_node *node)
    {
	if (!node) return false;
	// Namespace-qualified: std::cout, std::cerr
	if (node->type == GP_ANODE && an_code(node) == AN_NS_NAME) {
	    std::string func = term_text(child(node, 1));
	    if (func == "cout" || func == "cerr") return true;
	    return false;
	}
	if (node->type != GP_TERM || term_code(node) != GT_IDENT)
	    return false;
	std::string id = term_text(node);
	if (id == "cout" || id == "cerr") return true;
	// Stream-typed variables (ofstream, fstream, stringstream) are ostreams
	std::string cls = lookup_var_class(id);
	return is_ostream_class(cls);
    }

    // Check if a node is an istream identifier (cin or ifstream/fstream var)
    bool is_istream_ident(gp_tree_node *node)
    {
	if (!node || node->type != GP_TERM || term_code(node) != GT_IDENT)
	    return false;
	std::string id = term_text(node);
	if (id == "cin") return true;
	std::string cls = lookup_var_class(id);
	return is_istream_class(cls);
    }

    // Get the ostream expression for a stream identifier
    std::string ostream_for_ident(gp_tree_node *node)
    {
	if (!node) return "__madc_cout";
	// Namespace-qualified: std::cout, std::cerr
	if (node->type == GP_ANODE && an_code(node) == AN_NS_NAME) {
	    std::string func = term_text(child(node, 1));
	    if (func == "cerr") return "__madc_cerr";
	    return "__madc_cout";
	}
	if (node->type == GP_TERM) {
	    std::string id = term_text(node);
	    if (id == "cerr") return "__madc_cerr";
	    if (id == "cout") return "__madc_cout";
	    // Stream variable — pass the buffer directly
	    std::string cls = lookup_var_class(id);
	    if (is_ostream_class(cls)) return id;
	}
	return "__madc_cout";
    }

    // Get the istream expression for a stream identifier
    std::string istream_for_ident(gp_tree_node *node)
    {
	if (node && node->type == GP_TERM) {
	    std::string id = term_text(node);
	    if (id == "cin") return "__madc_cin";
	    std::string cls = lookup_var_class(id);
	    if (is_istream_class(cls)) return id;
	}
	return "__madc_cin";
    }

    // Check if a node is the identifier "cout" (backward compat helper)
    bool is_cout(gp_tree_node *node)
    {
	return is_ostream_ident(node);
    }

    // Check if a bsl chain is rooted at an ostream
    bool is_cout_chain(gp_tree_node *node)
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

    // Find the ostream identifier at the root of a bsl chain
    gp_tree_node *find_stream_root(gp_tree_node *node)
    {
	if (!node) return nullptr;
	if (is_ostream_ident(node)) return node;
	if (is_an(node, AN_PAREN)) return find_stream_root(child(node, 0));
	if (is_an(node, AN_BSL)) return find_stream_root(child(node, 0));
	if (is_an(node, AN_NS_NAME)) return is_ostream_ident(node) ? node : nullptr;
	return nullptr;
    }

    // Emit an ostream << chain as calls to C++ stream wrappers.
    std::string emit_cout_chain(gp_tree_node *node)
    {
	std::vector<gp_tree_node *> vals;
	collect_cout_values(node, vals);

	// Determine which stream (cout or cerr)
	gp_tree_node *root = find_stream_root(node);
	std::string os = ostream_for_ident(root);

	std::string result;
	for (size_t i = 0; i < vals.size(); i++) {
	    gp_tree_node *v = vals[i];
	    if (i > 0) result += ", ";

	    // Stream manipulators (endl, flush, hex, oct, etc.)
	    if (v->type == GP_TERM && term_code(v) == GT_IDENT) {
		std::string manip = ostream_manipulator(os, term_text(v));
		if (!manip.empty()) {
		    result += manip;
		    continue;
		}
	    }
	    // Namespace-qualified manipulators: std::endl, std::flush, etc.
	    if (v->type == GP_ANODE && an_code(v) == AN_NS_NAME) {
		std::string manip = ostream_manipulator(os, term_text(child(v, 1)));
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
		    result += "streamout_cstr(" + os + ", \"" + c_escape(term_text(v)) + "\")";
		else if (code == GT_CHAR_LIT)
		    result += "streamout_char(" + os + ", " + e + ")";
		else if (code == GT_REAL)
		    result += "streamout_double(" + os + ", " + e + ")";
		else if (code == GT_INTEGER)
		    result += "streamout_int64(" + os + ", " + e + ")";
		else {
		    TypeClass tc = lookup_var_type(term_text(v));
		    result += ostream_call_for_type(os, tc, e);
		}
	    } else {
		TypeClass etc = infer_expr_cout_type(v);
		if (!e.empty() && e[0] == '"')
		    result += "streamout_cstr(" + os + ", " + e + ")";
		else
		    result += ostream_call_for_type(os, etc, e);
	    }
	}

	return "(" + result + ")";
    }

    // Map TypeClass to the appropriate streamout_* wrapper call
    std::string ostream_call_for_type(const std::string &os,
				      TypeClass tc, const std::string &expr)
    {
	switch (tc) {
	case TC_CLASS:  return "streamout_string(" + os + ", " + expr + ")";
	case TC_STRING: return "streamout_cstr(" + os + ", " + expr + ")";
	case TC_CHAR:   return "streamout_char(" + os + ", " + expr + ")";
	case TC_DOUBLE: return "streamout_double(" + os + ", " + expr + ")";
	default:        return "streamout_int64(" + os + ", " + expr + ")";
	}
    }

    // Map iostream manipulator names to ostream wrapper calls
    static std::string ostream_manipulator(const std::string &os,
					   const std::string &name)
    {
	if (name == "endl")        return "streamout_endl(" + os + ")";
	if (name == "flush")       return "streamout_flush(" + os + ")";
	if (name == "hex")         return "streamout_hex(" + os + ")";
	if (name == "oct")         return "streamout_oct(" + os + ")";
	if (name == "dec")         return "streamout_dec(" + os + ")";
	if (name == "fixed")       return "streamout_fixed(" + os + ")";
	if (name == "scientific")  return "streamout_scientific(" + os + ")";
	if (name == "left")        return "streamout_left(" + os + ")";
	if (name == "right")       return "streamout_right(" + os + ")";
	if (name == "boolalpha")   return "streamout_boolalpha(" + os + ")";
	if (name == "noboolalpha") return "streamout_noboolalpha(" + os + ")";
	if (name == "showbase")    return "streamout_showbase(" + os + ")";
	if (name == "noshowbase")  return "streamout_noshowbase(" + os + ")";
	return "";
    }

    // ---------------------------------------------------------------
    // cin >> var  →  istream wrapper calls
    // ---------------------------------------------------------------

    bool is_cin(gp_tree_node *node)
    {
	if (!node) return false;
	if (node->type == GP_TERM && term_code(node) == GT_IDENT)
	    return is_istream_ident(node);
	if (is_an(node, AN_PAREN))
	    return is_cin_chain(child(node, 0));
	return false;
    }

    // Find the istream identifier at the root of a >> chain
    gp_tree_node *find_istream_root(gp_tree_node *node)
    {
	if (!node) return nullptr;
	if (is_istream_ident(node)) return node;
	if (is_an(node, AN_PAREN)) return find_istream_root(child(node, 0));
	if (is_an(node, AN_BSR)) return find_istream_root(child(node, 0));
	return nullptr;
    }

    bool is_cin_chain(gp_tree_node *node)
    {
	if (!node) return false;
	if (node->type == GP_TERM) return is_cin(node);
	if (is_an(node, AN_PAREN))
	    return is_cin_chain(child(node, 0));
	if (!is_an(node, AN_BSR)) return false;
	return is_cin(child(node, 0)) || is_cin_chain(child(node, 0));
    }

    void collect_cin_targets(gp_tree_node *node,
			     std::vector<gp_tree_node *> &targets)
    {
	if (!node) return;
	if (is_an(node, AN_PAREN)) {
	    collect_cin_targets(child(node, 0), targets);
	    return;
	}
	if (!is_an(node, AN_BSR)) return;
	gp_tree_node *lhs = child(node, 0);
	gp_tree_node *rhs = child(node, 1);
	if (is_an(lhs, AN_BSR) && is_cin_chain(lhs))
	    collect_cin_targets(lhs, targets);
	else if (is_an(lhs, AN_PAREN) && is_cin_chain(lhs))
	    collect_cin_targets(child(lhs, 0), targets);
	targets.push_back(rhs);
    }

    std::string emit_cin_chain(gp_tree_node *node)
    {
	std::vector<gp_tree_node *> targets;
	collect_cin_targets(node, targets);

	// Determine which istream (cin or ifstream variable)
	gp_tree_node *root = find_istream_root(node);
	std::string is = istream_for_ident(root);

	std::string result;
	for (size_t i = 0; i < targets.size(); i++) {
	    gp_tree_node *t = targets[i];
	    if (i > 0) result += ", ";
	    std::string var = emit_expr(t);
	    // Determine type and emit appropriate istream call
	    if (t->type == GP_TERM && term_code(t) == GT_IDENT) {
		TypeClass tc = lookup_var_type(term_text(t));
		switch (tc) {
		case TC_CLASS:
		    result += "streamin_string(" + is + ", " + var + ")";
		    break;
		case TC_STRING:
		    result += "streamin_char(" + is + ", " + var + ")";
		    break;
		case TC_CHAR:
		    result += "streamin_char(" + is + ", &" + var + ")";
		    break;
		case TC_DOUBLE:
		    result += "streamin_double(" + is + ", &" + var + ")";
		    break;
		default:
		    result += "streamin_int(" + is + ", &" + var + ")";
		    break;
		}
	    } else {
		result += "streamin_int(" + is + ", &" + var + ")";
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
	// GLR ambiguity — use first alternative
	if (node->type == GP_ALT) return emit_expr(node->val.alt.first);
	if (node->type == GP_OPT) return emit_expr(node->val.opt.first);

	// Terminal: literal or identifier
	if (node->type == GP_TERM) {
	    int code = term_code(node);
	    if (code == GT_IDENT) {
		std::string id = term_text(node);
		// Inside class methods: unqualified members → __this->member
		if (!current_class.empty() &&
		    current_class_fields.count(id) &&
		    !local_var_types.count(id))  // don't transform local vars
		    return "__this->" + safe_ident(id);
		// Reference parameters are pointers; dereference on access.
		if (ref_vars.count(id))
		    return "(*" + safe_ident(id) + ")";
		return safe_ident(id);
	    }
	    if (code == GT_INTEGER) {
		// If the original token has source_text (e.g. "0U", "0xFFU"),
		// use it verbatim so unsigned suffixes are preserved.  This is
		// critical for expressions like ~0U where the signedness of the
		// literal determines the result type.
		TokenBase *tb_int = (TokenBase *)node->val.term.attr;
		TokenInt  *ti_int = tb_int ? dynamic_cast<TokenInt *>(tb_int) : nullptr;
		if (ti_int && !ti_int->source_text.empty())
		    return ti_int->source_text;
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

	// cout << expr << endl → ostream wrapper calls
	if (an == AN_BSL && is_cout_chain(node))
	    return emit_cout_chain(node);

	// cin >> var >> var → istream wrapper calls
	if (an == AN_BSR && is_cin_chain(node))
	    return emit_cin_chain(node);

	// Operator overload dispatch: if LHS is a class with that operator,
	// emit ClassName__op_XX(&var, rhs) instead of (var op rhs)
	{
	    std::string sym = an_to_op_sym(an);
	    if (!sym.empty()) {
		gp_tree_node *lhs = child(node, 0);
		if (lhs && lhs->type == GP_TERM && term_code(lhs) == GT_IDENT) {
		    std::string lname = term_text(lhs);
		    std::string cls = lookup_var_class(lname);
		    if (!cls.empty()) {
			auto cit = class_operators.find(cls);
			if (cit != class_operators.end() &&
			    cit->second.count(sym)) {
			    std::string mangled = cls + "__" + op_suffix(sym);
			    std::string rhs = emit_expr(child(node, 1));
			    return mangled + "(&" + lname + ", " + rhs + ")";
			}
		    }
		}
	    }
	}

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

	// String assignment: s = "hello" → string_assign_cstr(s, "hello")
	//                    s = t      → string_assign(s, t)
	if (an == AN_ASSIGN) {
	    gp_tree_node *lhs = child(node, 0);
	    gp_tree_node *rhs = child(node, 1);
	    // va_start macro expands to: ap = __va_args
	    // Detect this and emit proper va_start(ap, last_param) instead.
	    if (lhs && lhs->type == GP_TERM && term_code(lhs) == GT_IDENT &&
		rhs && rhs->type == GP_TERM && term_code(rhs) == GT_IDENT &&
		term_text(rhs) == "__va_args") {
		std::string ap = term_text(lhs);
		va_list_vars.insert(ap);
		std::string last = current_func_last_param;
		if (last.empty()) last = "/* last_param */";
		return "va_start(" + ap + ", " + last + ")";
	    }
	    if (lhs && lhs->type == GP_TERM && term_code(lhs) == GT_IDENT) {
		std::string lname = term_text(lhs);
		if (lookup_var_type(lname) == TC_CLASS &&
		    lookup_var_class(lname) == "string") {
		    std::string lhs_str = emit_expr(lhs);
		    if (rhs && rhs->type == GP_TERM && term_code(rhs) == GT_STRING) {
			return "string_assign_cstr(" + lhs_str + ", \"" +
			       c_escape(term_text(rhs)) + "\")";
		    }
		    // RHS is another string variable or expression
		    std::string rhs_str = emit_expr(rhs);
		    return "string_assign(" + lhs_str + ", " + rhs_str + ")";
		}
	    }
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
	    // Namespace calls (call(ns_name(...), args)): don't coerce strings
	    bool is_ns = is_an(callee, AN_NS_NAME);
	    // For plain identifier calls, extract function name early so we
	    // can emit &arg for ref parameters.
	    std::string early_func_name;
	    if (!is_ns && callee && callee->type == GP_TERM &&
		term_code(callee) == GT_IDENT)
		early_func_name = term_text(callee);
	    std::string args = is_ns ? emit_arg_list_raw(child(node, 1))
				     : emit_arg_list_ref(child(node, 1), early_func_name);

	    // Handle method calls: call(member(obj, method), args)
	    if (is_an(callee, AN_MEMBER) || is_an(callee, AN_ARROW_MEMBER)) {
		std::string obj = emit_expr(child(callee, 0));
		std::string method = term_text(child(callee, 1));
		bool is_arrow = is_an(callee, AN_ARROW_MEMBER);

		// String methods: c_str() → string_cstr(obj)
		// (obj is a char[MADC_STRING_SIZE] buffer holding a placement-new'd std::string)
		if (method == "c_str") return "string_cstr(" + obj + ")";
		// String methods: length()/size() → strlen()
		if (method == "length" || method == "size")
		    return "strlen(" + obj + ")";

		// Stream method dispatch: obj.method(args) → prefix_method(obj, args)
		std::string obj_name = term_text(child(callee, 0));
		std::string cls = lookup_var_class(obj_name);
		if (cls == "ofstream" || cls == "ifstream" ||
		    cls == "fstream" || cls == "stringstream") {
		    std::string prefix;
		    if (cls == "stringstream") prefix = "sstream";
		    else prefix = cls;
		    std::string wrapper = prefix + "_" + method;
		    if (args.empty())
			return wrapper + "(" + obj + ")";
		    return wrapper + "(" + obj + ", " + args + ")";
		}
		if (!cls.empty()) {
		    // Check for virtual dispatch
		    if (sema && sema->is_virtual_method(cls, method)) {
			// Virtual call: obj->__vptr->method(obj)
			std::string addr = is_arrow ? obj : ("&" + obj);
			std::string vptr_access = is_arrow
			    ? (obj + "->__vptr->" + method)
			    : (obj + ".__vptr->" + method);
			// Cast addr to base vtable type
			std::string vtbl_cls = sema->vtable_class(cls);
			if (!vtbl_cls.empty() && vtbl_cls != cls) {
			    if (is_arrow)
				addr = "(struct " + vtbl_cls + " *)" + obj;
			    else
				addr = "(struct " + vtbl_cls + " *)&" + obj;
			}
			if (args.empty())
			    return vptr_access + "(" + addr + ")";
			return vptr_access + "(" + addr + ", " + args + ")";
		    }

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
		    // Cast to base class type when method is inherited
		    if (owner != cls && !is_arrow)
			addr = "(struct " + owner + " *)&" + obj;
		    else if (owner != cls && is_arrow)
			addr = "(struct " + owner + " *)" + obj;
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

	    // getline(stream, string) → streamin_getline(stream, string)
	    if (func == "getline") {
		// Re-emit args without string coercion (need raw buffer, not c_str)
		std::string raw_args = emit_arg_list(child(node, 1), false);
		return "streamin_getline(" + raw_args + ")";
	    }

	    // to_string(str, val) → __std_to_string(str, val)
	    // First arg is a string buffer (not coerced to cstr)
	    if (func == "to_string") {
		std::string raw_args = emit_arg_list(child(node, 1), false);
		return "__std_to_string(" + raw_args + ")";
	    }

	    return func + "(" + args + ")";
	}

	// Member access
	if (an == AN_MEMBER)
	    return emit_expr(child(node, 0)) + "." + safe_ident(term_text(child(node, 1)));
	if (an == AN_ARROW_MEMBER)
	    return emit_expr(child(node, 0)) + "->" + safe_ident(term_text(child(node, 1)));

	// Subscript
	if (an == AN_SUBSCRIPT)
	    return emit_expr(child(node, 0)) + "[" + emit_expr(child(node, 1)) + "]";

	// Namespace call: ns::func(args)
	if (an == AN_NS_CALL) {
	    std::string ns = term_text(child(node, 0));
	    std::string func = term_text(child(node, 1));
	    std::string args = emit_arg_list_raw(child(node, 2));  // no coerce — ns funcs take std::string*
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
	if (an == AN_CAST) {
	    // va_end(ap) expands to ((void)(ap)) — detect and emit va_end
	    gp_tree_node *cast_type = child(node, 0);
	    gp_tree_node *cast_expr = child(node, 1);
	    std::string type_str = emit_type(cast_type);
	    if (type_str == "void" && cast_expr) {
		// Unwrap optional paren: (ap) → ap
		gp_tree_node *inner_e = cast_expr;
		if (inner_e->type == GP_ANODE && an_code(inner_e) == AN_PAREN)
		    inner_e = child(inner_e, 0);
		if (inner_e && inner_e->type == GP_TERM &&
		    term_code(inner_e) == GT_IDENT) {
		    std::string vname = term_text(inner_e);
		    if (va_list_vars.count(vname))
			return "va_end(" + vname + ")";
		}
	    }
	    return "((" + type_str + ")" + emit_expr(cast_expr) + ")";
	}

	// sizeof
	if (an == AN_SIZEOF_TYPE) {
	    std::string st = emit_type(child(node, 0));
	    // Check for VLA typedef — replace sizeof(vla_type) with
	    // the precomputed size variable
	    auto vit = vla_typedef_sizes.find(st);
	    if (vit != vla_typedef_sizes.end())
		return "_sizeof_" + st;
	    return "sizeof(" + st + ")";
	}
	if (an == AN_SIZEOF_EXPR)
	    return "sizeof(" + emit_expr(child(node, 0)) + ")";

	// va_arg(expr, type)
	if (an == AN_VA_ARG)
	    return "va_arg(" + emit_expr(child(node, 0)) + ", " +
		   emit_type(child(node, 1)) + ")";

	// new/delete
	if (an == AN_NEW_PLAIN || an == AN_NEW_CTOR) {
	    std::string cls = term_text(child(node, 0));
	    std::string st = is_known_class(cls) ? "struct " + cls : cls;
	    if (prefer_calloc)
		return "((" + st + " *)calloc(1, sizeof(" + st + ")))";
	    return "((" + st + " *)malloc(sizeof(" + st + ")))";
	}

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
    // Find the deepest identifier in a single-child pass-through chain
    static gp_tree_node *unwrap_to_ident(gp_tree_node *node)
    {
	while (node) {
	    if (node->type == GP_TERM) return node;
	    if (node->type == GP_ANODE && nchildren(node) == 1)
		node = child(node, 0);
	    else
		return node;
	}
	return nullptr;
    }

    // Emit an expression, optionally coercing managed strings to const char *
    std::string emit_arg_maybe_coerce(gp_tree_node *node, bool coerce)
    {
	std::string e = emit_expr(node);
	if (!coerce) return e;
	// Check if the expression resolves to a managed string identifier
	gp_tree_node *leaf = unwrap_to_ident(node);
	if (leaf && leaf->type == GP_TERM && term_code(leaf) == GT_IDENT) {
	    TypeClass tc = lookup_var_type(term_text(leaf));
	    if (tc == TC_CLASS && lookup_var_class(term_text(leaf)) == "string")
		return "string_cstr(" + e + ")";
	}
	return e;
    }

    // Emit an arg for namespace calls — string literals become stack
    // string objects constructed inline (same lifecycle as any C++ object).
    std::string emit_ns_arg(gp_tree_node *node)
    {
	std::string e = emit_expr(node);
	if (node && node->type == GP_TERM && term_code(node) == GT_STRING) {
	    // Emit a temporary stack string for the literal
	    std::string tmp = tmp_var();
	    emit_indent();
	    O("char %s[MADC_STRING_SIZE];\n", tmp.c_str());
	    emit_indent();
	    O("string_construct_cstr(%s, \"%s\");\n", tmp.c_str(), c_escape(term_text(node)).c_str());
	    // Track for destruction
	    scope_class_vars.push_back(tmp + "|string");
	    return tmp;
	}
	return e;
    }

    // Raw arg list for namespace calls — no coercion, but wrap string literals
    std::string emit_arg_list_raw(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";
	if (node->type == GP_TERM) return emit_ns_arg(node);
	if (!is_an(node, AN_ARG_LIST))
	    return emit_ns_arg(node);
	return emit_arg_list_raw(child(node, 0)) + ", " +
	       emit_ns_arg(child(node, 1));
    }

    // Regular arg list with string coercion
    std::string emit_arg_list(gp_tree_node *node, bool coerce = true)
    {
	if (!node || node->type == GP_NIL) return "";
	if (node->type == GP_TERM) return emit_arg_maybe_coerce(node, coerce);
	if (!is_an(node, AN_ARG_LIST))
	    return emit_arg_maybe_coerce(node, coerce);
	return emit_arg_list(child(node, 0), coerce) + ", " +
	       emit_arg_maybe_coerce(child(node, 1), coerce);
    }

    // Emit a single arg, prepending & if the param at position idx is a ref.
    // When the arg itself is a ref_var we already have (*x); strip that and
    // use the underlying pointer directly (i.e. emit just the name).
    std::string emit_arg_maybe_ref(gp_tree_node *node, bool is_ref)
    {
	if (!is_ref) return emit_arg_maybe_coerce(node, true);
	// Arg should be passed as a pointer.  If the expression is a plain
	// identifier that is itself a ref_var, it is already a pointer — pass
	// it directly.  Otherwise, take its address.
	gp_tree_node *leaf = unwrap_to_ident(node);
	if (leaf && leaf->type == GP_TERM && term_code(leaf) == GT_IDENT) {
	    std::string id = term_text(leaf);
	    if (ref_vars.count(id))
		return safe_ident(id);  // already a pointer, pass through
	    return "&" + safe_ident(id);
	}
	// Non-identifier expression (e.g. array element, member):
	// emit the expression and prepend &
	return "&(" + emit_expr(node) + ")";
    }

    // Arg list with ref-param awareness: func_name used to look up
    // which parameters expect a pointer (ref) vs a value.
    std::string emit_arg_list_ref(gp_tree_node *node,
				  const std::string &func_name,
				  int &idx)
    {
	if (!node || node->type == GP_NIL) return "";
	auto it = func_ref_param_map.find(func_name);
	const std::vector<bool> *refs = (it != func_ref_param_map.end())
					? &it->second : nullptr;
	if (node->type == GP_TERM) {
	    bool is_ref = refs && idx < (int)refs->size() && (*refs)[idx];
	    idx++;
	    return emit_arg_maybe_ref(node, is_ref);
	}
	if (!is_an(node, AN_ARG_LIST)) {
	    bool is_ref = refs && idx < (int)refs->size() && (*refs)[idx];
	    idx++;
	    return emit_arg_maybe_ref(node, is_ref);
	}
	std::string left = emit_arg_list_ref(child(node, 0), func_name, idx);
	std::string right;
	{
	    bool is_ref = refs && idx < (int)refs->size() && (*refs)[idx];
	    idx++;
	    right = emit_arg_maybe_ref(child(node, 1), is_ref);
	}
	return left + ", " + right;
    }

    // Entry point for ref-aware arg list emission.
    std::string emit_arg_list_ref(gp_tree_node *node,
				  const std::string &func_name)
    {
	int idx = 0;
	return emit_arg_list_ref(node, func_name, idx);
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

    // Expand a string literal to a braced byte-list initializer.
    // E.g. "hi" → {104, 105, 0}
    // Used inside multi-dimensional char array inits to work around a
    // c2mir bug where string literal offsets in 3D+ arrays are wrong.
    static std::string string_to_bytes(const std::string &s) {
	std::string out = "{";
	for (size_t i = 0; i < s.size(); i++) {
	    char buf[16];
	    snprintf(buf, sizeof(buf), "%d", (unsigned char)s[i]);
	    out += buf;
	    out += ", ";
	}
	out += "0}";
	return out;
    }

    // emit_init_list variant that treats string literals as char-array
    // sub-initializers (expands them to byte lists).  Used for the inner
    // dimension of a 3D+ char array (e.g. char b[2][3][9] = {<here>}).
    std::string emit_init_list_chararray(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";
	if (is_an(node, AN_DESIG_INIT)) {
	    gp_tree_node *desig = child(node, 0);
	    std::string val = emit_init_item_chararray(child(node, 1));
	    if (!is_nil(desig))
		return emit_designator(desig) + " = " + val;
	    return val;
	}
	if (is_an(node, AN_INIT_SEQ)) {
	    std::string prev = emit_init_list_chararray(child(node, 0));
	    gp_tree_node *desig = child(node, 1);
	    std::string val = emit_init_item_chararray(child(node, 2));
	    std::string item;
	    if (!is_nil(desig))
		item = emit_designator(desig) + " = " + val;
	    else
		item = val;
	    return prev + ", " + item;
	}
	// Leaf — expand if it's a string literal
	if (node->type == GP_TERM && term_code(node) == GT_STRING) {
	    TokenBase *tb = (TokenBase *)node->val.term.attr;
	    TokenIdent *ti = tb ? dynamic_cast<TokenIdent *>(tb) : nullptr;
	    if (ti) return string_to_bytes(ti->str);
	}
	return emit_expr(node);
    }

    std::string emit_init_item_chararray(gp_tree_node *node)
    {
	if (!node) return "0";
	if (is_an(node, AN_INIT_LIST))
	    return "{" + emit_init_list_chararray(child(node, 0)) + "}";
	if (is_an(node, AN_EMPTY_INIT))
	    return "{0}";
	if (node->type == GP_TERM && term_code(node) == GT_STRING) {
	    TokenBase *tb = (TokenBase *)node->val.term.attr;
	    TokenIdent *ti = tb ? dynamic_cast<TokenIdent *>(tb) : nullptr;
	    if (ti) return string_to_bytes(ti->str);
	}
	return emit_expr(node);
    }

    std::string emit_init_item(gp_tree_node *node)
    {
	if (!node) return "0";
	if (is_an(node, AN_INIT_LIST)) {
	    // If we are in a char-array init context, inner lists must also
	    // expand string literals.
	    if (char_array_init_depth >= 1) {
		char_array_init_depth++;
		std::string inner = emit_init_list(child(node, 0));
		char_array_init_depth--;
		return "{" + inner + "}";
	    }
	    return "{" + emit_init_list(child(node, 0)) + "}";
	}
	if (is_an(node, AN_EMPTY_INIT))
	    return "{0}";
	// When inside a char array initializer (depth >= 1), expand string
	// literals to explicit byte arrays to work around a c2mir bug where
	// string literals in 3D+ char arrays are mis-placed by 8 bytes.
	if (char_array_init_depth >= 1 &&
	    node->type == GP_TERM && term_code(node) == GT_STRING) {
	    TokenBase *tb = (TokenBase *)node->val.term.attr;
	    TokenIdent *ti = tb ? dynamic_cast<TokenIdent *>(tb) : nullptr;
	    if (ti)
		return string_to_bytes(ti->str);
	}
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

    // Check if a GP_ALT's first branch is a declaration whose "type" is
    // actually a known function name — i.e. the GLR parser mis-read a
    // function call like `print_shape(&r)` as a declaration.  When that
    // happens we prefer the second alternative (the expression statement).
    bool alt_first_is_func_as_decl(gp_tree_node *alt)
    {
	gp_tree_node *first = alt->val.alt.first;
	if (!first || first->type != GP_ANODE) return false;
	if (an_code(first) != AN_DECL) return false;
	gp_tree_node *specs = child(first, 0);
	if (!specs) return false;
	// Simple case: the specifier is a single identifier
	if (specs->type == GP_TERM && term_code(specs) == GT_IDENT) {
	    std::string name = term_text(specs);
	    if (sema && sema->func_ret_types.count(name))
		return true;
	}
	return false;
    }

    // Emit dtor call for one scope_class_vars entry
    void emit_one_dtor(const std::string &vn, const std::string &cn)
    {
	emit_indent();
	emit_one_dtor_inline(vn, cn);
	O("\n");
    }

    // Emit dtor call inline (no indent/newline — for use inside if blocks)
    void emit_one_dtor_inline(const std::string &vn, const std::string &cn)
    {
	if (cn == "string")
	    O("string_destruct(%s);", vn.c_str());
	else if (cn == "array")
	    O("madarray_destruct(%s);", vn.c_str());
	else if (cn == "ofstream" || cn == "ifstream" || cn == "fstream")
	    O("%s_destruct(%s);", cn.c_str(), vn.c_str());
	else if (cn == "stringstream")
	    O("sstream_destruct(%s);", vn.c_str());
	else
	    O("%s__dtor(&%s);", cn.c_str(), vn.c_str());
    }

    // Emit dtor calls for try-body objects in catch handler.
    // Uses guard variables to handle partially-constructed state
    // (e.g. throw before all ctors complete). Also clears guards
    // so function-scope dtors won't double-destruct.
    void emit_try_body_dtors_guarded(size_t scope_save)
    {
	for (int i = (int)scope_class_vars.size() - 1; i >= (int)scope_save; i--) {
	    std::string key = scope_class_vars[i];
	    size_t sep = key.find('|');
	    if (sep == std::string::npos) continue;
	    std::string vn = key.substr(0, sep);
	    std::string cn = key.substr(sep + 1);
	    auto git = try_dtor_guards.find(key);
	    if (git != try_dtor_guards.end()) {
		// Guarded: only destruct if guard is set, then clear it
		emit_indent();
		O("if (%s) { ", git->second.c_str());
		if (cn == "string")
		    O("string_destruct(%s);", vn.c_str());
		else if (cn == "array")
		    O("madarray_destruct(%s);", vn.c_str());
		else if (cn == "ofstream" || cn == "ifstream" || cn == "fstream")
		    O("%s_destruct(%s);", cn.c_str(), vn.c_str());
		else if (cn == "stringstream")
		    O("sstream_destruct(%s);", vn.c_str());
		else
		    O("%s__dtor(&%s);", cn.c_str(), vn.c_str());
		O(" %s = 0; }\n", git->second.c_str());
	    } else {
		emit_one_dtor(vn, cn);
	    }
	}
    }

    // Emit a try/catch block using the SJLJ exception runtime.
    //
    // Declarations from the try body are hoisted before setjmp so that
    // variables remain visible for destructor calls in both the normal
    // and exception paths.
    //
    // Normal path: dtors are NOT called at try-body exit — they persist
    // to function scope (matching madc JIT semantics).
    // Exception path: catch handler calls dtors for try-body objects,
    // then removes them from scope_class_vars.
    void emit_try_catch(gp_tree_node *node)
    {
	int ctx_id = try_depth++;
	bool nested = (try_nesting > 0);
	try_nesting++;
	gp_tree_node *try_body = child(node, 0);
	gp_tree_node *catch_list = child(node, 1);

	// Collect catch clauses
	std::vector<gp_tree_node *> catches;
	collect_catch_clauses(catch_list, catches);

	// Save scope_class_vars size to track objects constructed in try body
	size_t scope_save = scope_class_vars.size();

	// Collect all statements from the try body compound statement.
	gp_tree_node *body_stmts = try_body;
	if (is_an(try_body, AN_BLOCK))
	    body_stmts = child(try_body, 0);
	std::vector<gp_tree_node *> stmts;
	collect_stmts(body_stmts, stmts);

	// Declarations are emitted at the enclosing scope level (no
	// extra block) so variables remain visible for function-scope
	// destructor calls and guard checks.

	// Pass 1: emit declarations before setjmp (hoisted).
	// For CTOR_DECL, emit variable declaration + guard variable.
	// For regular decls (including strings), emit fully.
	std::vector<std::string> ctor_guard_names;
	for (size_t i = 0; i < stmts.size(); i++) {
	    if (an_code(stmts[i]) == AN_CTOR_DECL) {
		emit_ctor_decl_only(stmts[i]);
		// Emit a guard variable for dtor tracking. Use static so
		// the value survives longjmp (c2mir doesn't honour volatile
		// across setjmp/longjmp). Reset to 0 at entry.
		std::string gname = "__try_alive_" + std::to_string(ctx_id)
		    + "_" + std::to_string(ctor_guard_names.size());
		emit_indent();
		O("static int %s = 0;\n", gname.c_str());
		emit_indent();
		O("%s = 0;\n", gname.c_str());
		ctor_guard_names.push_back(gname);
	    } else if (an_code(stmts[i]) == AN_DECL) {
		emit_stmt(stmts[i]);
	    }
	}

	// Declare try context
	emit_indent();
	O("__madc_try_ctx_t __try_ctx_%d;\n", ctx_id);
	emit_indent();
	O("__madc_try_push(&__try_ctx_%d);\n", ctx_id);

	// setjmp branch: 0 = normal (try body), non-zero = exception (catch)
	emit_indent();
	O("if (_setjmp(((char *)&__try_ctx_%d)) == 0) {\n", ctx_id);
	indent_level++;

	// Pass 2: emit constructor calls (with guards) and non-declaration
	// statements inside the if block
	int ctor_idx = 0;
	for (size_t i = 0; i < stmts.size(); i++) {
	    if (an_code(stmts[i]) == AN_CTOR_DECL) {
		std::string guard = ctor_idx < (int)ctor_guard_names.size()
		    ? ctor_guard_names[ctor_idx] : "";
		emit_ctor_call_only(stmts[i], guard);
		ctor_idx++;
	    } else if (!is_decl_stmt(stmts[i]))
		emit_stmt(stmts[i]);
	}

	// Save scope_class_vars for the catch handler before potentially
	// resizing on the normal exit path (nested try).
	std::vector<std::string> saved_scope = scope_class_vars;

	// Normal exit: for nested try blocks, destroy objects here
	// (they can't survive to function scope since they're in a
	// nested block). For top-level try, let them persist to
	// function-scope cleanup via scope_class_vars + guards.
	if (nested) {
	    // Call dtors in LIFO order and remove from scope_class_vars
	    for (int i = (int)scope_class_vars.size() - 1; i >= (int)scope_save; i--) {
		std::string key = scope_class_vars[i];
		size_t sep = key.find('|');
		if (sep != std::string::npos) {
		    std::string vn = key.substr(0, sep);
		    std::string cn = key.substr(sep + 1);
		    auto git = try_dtor_guards.find(key);
		    if (git != try_dtor_guards.end()) {
			emit_indent();
			O("if (%s) { ", git->second.c_str());
			emit_one_dtor_inline(vn, cn);
			O(" %s = 0; }\n", git->second.c_str());
		    } else {
			emit_one_dtor(vn, cn);
		    }
		}
	    }
	    scope_class_vars.resize(scope_save);
	}

	emit_indent();
	O("__madc_try_pop();\n");
	indent_level--;
	emit_indent();

	// Restore scope_class_vars for catch handler emission so that
	// the guarded dtor calls reference the right entries.
	scope_class_vars = saved_scope;

	// Catch handler(s) — emit guarded dtors for try-body objects.
	// Guards are cleared so function-scope cleanup won't double-destruct.
	if (catches.size() == 1 && is_an(catches[0], AN_CATCH_ALL)) {
	    // Single catch(...)
	    O("} else {\n");
	    indent_level++;
	    emit_try_body_dtors_guarded(scope_save);
	    emit_stmt_unwrapped(child(catches[0], 0));
	    emit_indent();
	    O("__madc_exception_clear();\n");
	    indent_level--;
	} else if (catches.size() == 1 && is_an(catches[0], AN_CATCH)) {
	    // Single typed catch
	    gp_tree_node *c = catches[0];
	    std::string type_str = emit_type(child(c, 0));
	    std::string vname = extract_name(child(c, 1));
	    int exc_type = exception_type_for_catch(type_str);
	    O("} else {\n");
	    indent_level++;
	    // Destroy try-body objects (exception unwinding)
	    emit_try_body_dtors_guarded(scope_save);
	    // Declare catch variable and retrieve exception value
	    emit_indent();
	    O("%s %s = %s();\n", type_str.c_str(), vname.c_str(),
	      exception_getter_for_type(exc_type).c_str());
	    emit_stmt_unwrapped(child(c, 2));
	    emit_indent();
	    O("__madc_exception_clear();\n");
	    indent_level--;
	} else {
	    // Multiple catch clauses — dispatch by exception type
	    O("} else {\n");
	    indent_level++;
	    // Destroy try-body objects (exception unwinding)
	    emit_try_body_dtors_guarded(scope_save);
	    emit_indent();
	    O("int __exc_type_%d = __madc_exception_type();\n", ctx_id);
	    bool first = true;
	    for (size_t i = 0; i < catches.size(); i++) {
		gp_tree_node *c = catches[i];
		if (is_an(c, AN_CATCH_ALL)) {
		    // catch(...) — always last, acts as else
		    emit_indent();
		    if (!first) O("} else {\n");
		    else O("{\n");
		    indent_level++;
		    emit_stmt_unwrapped(child(c, 0));
		    emit_indent();
		    O("__madc_exception_clear();\n");
		    indent_level--;
		} else if (is_an(c, AN_CATCH)) {
		    std::string type_str = emit_type(child(c, 0));
		    std::string vname = extract_name(child(c, 1));
		    int exc_type = exception_type_for_catch(type_str);
		    emit_indent();
		    if (first)
			O("if (__exc_type_%d == %d) {\n", ctx_id, exc_type);
		    else
			O("} else if (__exc_type_%d == %d) {\n", ctx_id, exc_type);
		    indent_level++;
		    emit_indent();
		    O("%s %s = %s();\n", type_str.c_str(), vname.c_str(),
		      exception_getter_for_type(exc_type).c_str());
		    emit_stmt_unwrapped(child(c, 2));
		    emit_indent();
		    O("__madc_exception_clear();\n");
		    indent_level--;
		}
		first = false;
	    }
	    emit_indent();
	    O("}\n");
	    indent_level--;
	}

	// Close the if/else
	emit_indent();
	O("}\n");
	// For nested try, also resize scope_class_vars in the catch
	// path (entries were already guarded-destructed above)
	if (nested)
	    scope_class_vars.resize(scope_save);
	try_nesting--;
    }

    void emit_stmt(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return;
	// GLR ambiguity — prefer expression statement when the "type" is
	// actually a known function name
	if (node->type == GP_ALT) {
	    if (alt_first_is_func_as_decl(node) && node->val.alt.second)
		emit_stmt(node->val.alt.second);
	    else
		emit_stmt(node->val.alt.first);
	    return;
	}
	if (node->type == GP_OPT) { emit_stmt(node->val.opt.first); return; }

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
	    bool need_dtors = !scope_class_vars.empty() ||
			     (emitting_main && !global_strings.empty());
	    // When there are dtor calls to emit, wrap the whole block in braces
	    // so that a bare `if (cond) return expr;` doesn't become an
	    // unbraced multi-statement sequence where only the first statement
	    // is guarded by the condition.
	    if (need_dtors) {
		emit_indent();
		O("{\n");
		indent_level++;
	    }
	    // Call destructors in LIFO order, checking guards for
	    // try-body objects (already destroyed on exception path).
	    for (int i = (int)scope_class_vars.size() - 1; i >= 0; i--) {
		std::string key = scope_class_vars[i];
		size_t sep = key.find('|');
		if (sep != std::string::npos) {
		    std::string vn = key.substr(0, sep);
		    std::string cn = key.substr(sep + 1);
		    auto git = try_dtor_guards.find(key);
		    if (git != try_dtor_guards.end()) {
			// Guarded: only call dtor if guard is set
			emit_indent();
			O("if (%s) ", git->second.c_str());
			if (cn == "string")
			    O("string_destruct(%s);\n", vn.c_str());
			else if (cn == "array")
			    O("madarray_destruct(%s);\n", vn.c_str());
			else if (cn == "ofstream" || cn == "ifstream" ||
				 cn == "fstream")
			    O("%s_destruct(%s);\n", cn.c_str(), vn.c_str());
			else if (cn == "stringstream")
			    O("sstream_destruct(%s);\n", vn.c_str());
			else
			    O("%s__dtor(&%s);\n", cn.c_str(), vn.c_str());
		    } else {
			emit_indent();
			if (cn == "string")
			    O("string_destruct(%s);\n", vn.c_str());
			else if (cn == "array")
			    O("madarray_destruct(%s);\n", vn.c_str());
			else if (cn == "ofstream" || cn == "ifstream" ||
				 cn == "fstream")
			    O("%s_destruct(%s);\n", cn.c_str(), vn.c_str());
			else if (cn == "stringstream")
			    O("sstream_destruct(%s);\n", vn.c_str());
			else
			    O("%s__dtor(&%s);\n", cn.c_str(), vn.c_str());
		    }
		}
	    }
	    // In main(), call __madc_cleanup_globals() before returning
	    if (emitting_main && !global_strings.empty()) {
		emit_indent();
		O("__madc_cleanup_globals();\n");
	    }
	    emit_indent();
	    gp_tree_node *val = child(node, 0);
	    if (is_nil(val))
		O("return;\n");
	    else
		O("return %s;\n", emit_expr(val).c_str());
	    if (need_dtors) {
		indent_level--;
		emit_indent();
		O("}\n");
	    }
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
	    emit_try_catch(node);
	    return;
	}

	// Throw with expression
	if (an == AN_THROW_EXPR) {
	    gp_tree_node *expr = child(node, 0);
	    std::string val = emit_expr(expr);
	    TypeClass tc = infer_expr_cout_type(expr);
	    emit_indent();
	    O("%s(%s);\n", throw_func_for_type(tc).c_str(), val.c_str());
	    return;
	}
	// Bare throw (rethrow)
	if (an == AN_THROW) {
	    emit_indent();
	    O("__madc_rethrow();\n");
	    return;
	}

	// Delete — call destructor before freeing
	if (an == AN_DELETE) {
	    std::string expr = emit_expr(child(node, 0));
	    // Look up class name for the variable
	    std::string cls = lookup_var_class(expr);
	    if (!cls.empty() && class_has_dtor(cls)) {
		emit_indent();
		O("%s__dtor(%s);\n", cls.c_str(), expr.c_str());
	    }
	    emit_indent();
	    O("free(%s);\n", expr.c_str());
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
	    if (!is_nil(decl)) {
		track_decl_types(type, decl);
		// If the declarator is a ref_decl, add the param name to ref_vars
		// so that uses of it in the function body emit (*name).
		if (is_an(decl, AN_REF_DECL)) {
		    std::string nm = extract_name(child(decl, 0));
		    if (!nm.empty()) ref_vars.insert(nm);
		}
	    }
	} else if (an == AN_PARAM_LIST) {
	    track_param_types(child(node, 0));
	    track_param_types(child(node, 1));
	} else if (an == AN_PARAM_VA) {
	    track_param_types(child(node, 0));
	}
    }

    // Collect vector<bool> of which params are refs for a param list node.
    // Also registers the result in func_ref_param_map under func_name.
    void collect_ref_params(const std::string &fname, gp_tree_node *plist)
    {
	std::vector<bool> refs;
	collect_ref_params_into(plist, refs);
	if (!refs.empty())
	    func_ref_param_map[fname] = refs;
    }

    void collect_ref_params_into(gp_tree_node *node, std::vector<bool> &refs)
    {
	if (!node || node->type == GP_NIL) return;
	int an = an_code(node);
	if (an == AN_PARAM) {
	    gp_tree_node *decl = child(node, 1);
	    refs.push_back(!is_nil(decl) && is_an(decl, AN_REF_DECL));
	} else if (an == AN_PARAM_LIST) {
	    collect_ref_params_into(child(node, 0), refs);
	    collect_ref_params_into(child(node, 1), refs);
	} else if (an == AN_PARAM_VA) {
	    collect_ref_params_into(child(node, 0), refs);
	}
    }

    // Check if a declaration_specifiers chain contains a stream type
    // Returns the stream kind name or "" if not a stream.
    static std::string stream_type_name(gp_tree_node *specs)
    {
	if (!specs) return "";
	if (specs->type == GP_TERM) {
	    int code = term_code(specs);
	    if (code == 341) return "ofstream";
	    if (code == 340) return "ifstream";
	    if (code == 342) return "fstream";
	    if (code == 343) return "stringstream";
	    return "";
	}
	if (an_code(specs) == AN_QUAL) {
	    std::string s = stream_type_name(child(specs, 0));
	    if (!s.empty()) return s;
	    return stream_type_name(child(specs, 1));
	}
	return "";
    }

    // Check if a variable name refers to a stream type via local_var_class_map
    bool is_stream_var(const std::string &name)
    {
	std::string cls = lookup_var_class(name);
	return cls == "ofstream" || cls == "ifstream" ||
	       cls == "fstream" || cls == "stringstream";
    }

    // Check if a stream class is an ostream (supports <<)
    static bool is_ostream_class(const std::string &cls)
    {
	return cls == "ofstream" || cls == "fstream" || cls == "stringstream";
    }

    // Check if a stream class is an istream (supports >>)
    static bool is_istream_class(const std::string &cls)
    {
	return cls == "ifstream" || cls == "fstream";
    }

    // Get the wrapper prefix for a stream class (e.g. "ofstream" → "ofstream_")
    // stringstream uses "sstream_" prefix
    static std::string stream_wrapper_prefix(const std::string &cls)
    {
	if (cls == "stringstream") return "sstream_";
	return cls + "_";
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

    // Check if a declaration_specifiers chain is the "array" typedef (MadArray)
    static bool is_array_type(gp_tree_node *specs)
    {
	if (!specs) return false;
	if (specs->type == GP_TERM)
	    return term_code(specs) == GT_IDENT && term_text(specs) == "array";
	if (an_code(specs) == AN_QUAL)
	    return is_array_type(child(specs, 0)) || is_array_type(child(specs, 1));
	return false;
    }

    // Collect global string declarations — emit storage in header and
    // record name + optional initializer for __madc_init_globals().
    void collect_global_string_decls(gp_tree_node *decls)
    {
	if (is_nil(decls)) return;

	int an = an_code(decls);

	// decl_list — multiple declarators
	if (an == AN_DECL_LIST) {
	    collect_global_string_decls(child(decls, 0));
	    collect_global_string_decls(child(decls, 1));
	    return;
	}

	// init_decl(name, initializer) — string x = "literal";
	if (an == AN_INIT_DECL) {
	    std::string vname = extract_name(child(decls, 0));
	    std::string init = emit_expr(child(decls, 1));
	    if (!vname.empty()) {
		OH("char %s[MADC_STRING_SIZE];\n", vname.c_str());
		GlobalStringVar gsv;
		gsv.name = vname;
		gsv.init_expr = init;
		global_strings.push_back(gsv);
	    }
	    return;
	}

	// Plain identifier — string x; (no initializer)
	if (decls->type == GP_TERM) {
	    std::string vname = term_text(decls);
	    if (!vname.empty()) {
		OH("char %s[MADC_STRING_SIZE];\n", vname.c_str());
		GlobalStringVar gsv;
		gsv.name = vname;
		global_strings.push_back(gsv);
	    }
	    return;
	}
    }

    // Emit __madc_init_globals() and __madc_cleanup_globals() functions.
    // Called after all top-level declarations have been emitted.
    void emit_global_init_cleanup()
    {
	if (global_strings.empty() && global_init_stmts.empty()) return;

	// Forward declarations in header
	OH("static void __madc_init_globals(void);\n");
	OH("static void __madc_cleanup_globals(void);\n");
	// Guard flag — prevents double-cleanup if main() has both an
	// explicit return and falls through (or has multiple return paths).
	OH("static int __madc_globals_live = 0;\n");

	// Init function body
	O("static void __madc_init_globals(void)\n");
	O("{\n");
	O("\t__madc_globals_live = 1;\n");
	for (auto &gsv : global_strings) {
	    if (gsv.init_expr.empty())
		O("\tstring_construct(%s);\n", gsv.name.c_str());
	    else
		O("\tstring_construct_cstr(%s, %s);\n",
		  gsv.name.c_str(), gsv.init_expr.c_str());
	}
	for (auto &stmt : global_init_stmts)
	    O("\t%s\n", stmt.c_str());
	O("}\n\n");

	// Cleanup function body (reverse order, idempotent via guard)
	O("static void __madc_cleanup_globals(void)\n");
	O("{\n");
	O("\tif (!__madc_globals_live) return;\n");
	O("\t__madc_globals_live = 0;\n");
	for (int i = (int)global_strings.size() - 1; i >= 0; i--)
	    O("\tstring_destruct(%s);\n", global_strings[i].name.c_str());
	O("}\n\n");
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
		O("string_construct_cstr(%s, %s);\n",
		  vname.c_str(), init_expr.c_str());
		local_var_types[vname] = TC_CLASS;
		local_var_class_map[vname] = "string";
		scope_class_vars.push_back(vname + "|string");
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
		O("string_construct(%s);\n", vname.c_str());
		local_var_types[vname] = TC_CLASS;
		local_var_class_map[vname] = "string";
		scope_class_vars.push_back(vname + "|string");
	    }
	    return;
	}
    }

    // Emit a MadArray variable declaration with runtime lifecycle
    void emit_array_decl(gp_tree_node *decls)
    {
	if (is_nil(decls)) return;

	int an = an_code(decls);

	// init_decl(name, initializer) — unusual for arrays
	if (an == AN_INIT_DECL) {
	    std::string vname = extract_name(child(decls, 0));
	    if (!vname.empty()) {
		emit_indent();
		O("char %s[MADC_ARRAY_SIZE];\n", vname.c_str());
		emit_indent();
		O("madarray_construct(%s);\n", vname.c_str());
		local_var_types[vname] = TC_CLASS;
		local_var_class_map[vname] = "array";
		scope_class_vars.push_back(vname + "|array");
	    }
	    return;
	}

	// decl_list — multiple declarators
	if (an == AN_DECL_LIST) {
	    emit_array_decl(child(decls, 0));
	    emit_array_decl(child(decls, 1));
	    return;
	}

	// Plain identifier — array x;
	if (decls->type == GP_TERM) {
	    std::string vname = term_text(decls);
	    if (!vname.empty()) {
		emit_indent();
		O("char %s[MADC_ARRAY_SIZE];\n", vname.c_str());
		emit_indent();
		O("madarray_construct(%s);\n", vname.c_str());
		local_var_types[vname] = TC_CLASS;
		local_var_class_map[vname] = "array";
		scope_class_vars.push_back(vname + "|array");
	    }
	    return;
	}
    }

    // Emit a stream variable declaration: char buf[SIZE]; type_construct(buf);
    void emit_stream_decl(const std::string &stream_class, gp_tree_node *decls)
    {
	if (is_nil(decls)) return;

	// Determine size macro and wrapper prefix
	std::string size_macro;
	std::string prefix = stream_wrapper_prefix(stream_class);
	if (stream_class == "ofstream")      size_macro = "MADC_OFSTREAM_SIZE";
	else if (stream_class == "ifstream") size_macro = "MADC_IFSTREAM_SIZE";
	else if (stream_class == "fstream")  size_macro = "MADC_FSTREAM_SIZE";
	else                                 size_macro = "MADC_SSTREAM_SIZE";

	int an = an_code(decls);

	// init_decl(name, initializer) — stream with initializer (unusual)
	if (an == AN_INIT_DECL) {
	    std::string vname = extract_name(child(decls, 0));
	    if (!vname.empty()) {
		emit_indent();
		O("char %s[%s];\n", vname.c_str(), size_macro.c_str());
		emit_indent();
		O("%sconstruct(%s);\n", prefix.c_str(), vname.c_str());
		local_var_types[vname] = TC_CLASS;
		local_var_class_map[vname] = stream_class;
		scope_class_vars.push_back(vname + "|" + stream_class);
	    }
	    return;
	}

	// decl_list — multiple declarators
	if (an == AN_DECL_LIST) {
	    emit_stream_decl(stream_class, child(decls, 0));
	    emit_stream_decl(stream_class, child(decls, 1));
	    return;
	}

	// Plain identifier — ofstream f;
	if (decls->type == GP_TERM) {
	    std::string vname = term_text(decls);
	    if (!vname.empty()) {
		emit_indent();
		O("char %s[%s];\n", vname.c_str(), size_macro.c_str());
		emit_indent();
		O("%sconstruct(%s);\n", prefix.c_str(), vname.c_str());
		local_var_types[vname] = TC_CLASS;
		local_var_class_map[vname] = stream_class;
		scope_class_vars.push_back(vname + "|" + stream_class);
	    }
	    return;
	}
    }

    // Reconstruct function call arguments from a declarator that was
    // mis-parsed as a declaration.  ptr_decl(*, name) → "&name",
    // plain terminal → "name", decl_list(a, b) → "a, b".
    std::string reconstruct_call_args(gp_tree_node *node)
    {
	if (!node) return "";
	if (node->type == GP_TERM)
	    return safe_ident(term_text(node));
	int an = an_code(node);
	if (an == AN_PTR_DECL) {
	    // ptr_decl(pointer, declarator) — the pointer means &
	    return "&" + reconstruct_call_args(child(node, nchildren(node) - 1));
	}
	if (an == AN_REF_DECL) {
	    // ref_decl(declarator) — the & means address-of in call context
	    return "&" + reconstruct_call_args(child(node, 0));
	}
	if (an == AN_DECL_LIST) {
	    return reconstruct_call_args(child(node, 0)) + ", " +
		   reconstruct_call_args(child(node, 1));
	}
	// Fallback: emit as expression
	return emit_expr(node);
    }

    void emit_decl_stmt(gp_tree_node *node)
    {
	// decl(declaration_specifiers, init_declarator_list_opt)
	gp_tree_node *specs = child(node, 0);
	gp_tree_node *decls = child(node, 1);

	// GLR mis-parse: function call parsed as declaration.
	// e.g. `print_shape(&r)` → decl(qual(print_shape,nil), ptr_decl(*, r))
	// Detect: type specifier resolves to a single identifier that is a
	// known function (in sema->func_ret_types) but NOT a known type/class.
	{
	    // Unwrap AN_QUAL chain to find the bare identifier
	    gp_tree_node *s = specs;
	    while (s && s->type == GP_ANODE && an_code(s) == AN_QUAL) {
		// qual(child0, child1) — if child1 is nil, child0 is the leaf
		gp_tree_node *c1 = child(s, 1);
		if (is_nil(c1)) { s = child(s, 0); break; }
		s = child(s, 0);  // keep unwrapping
	    }
	    if (s && s->type == GP_TERM && term_code(s) == GT_IDENT &&
		!is_nil(decls)) {
		std::string tname = term_text(s);
		if (sema && sema->func_ret_types.count(tname) &&
		    !is_known_class(tname)) {
		    // Reconstruct as function call: ptr_decl(*, name) → &name
		    std::string args = reconstruct_call_args(decls);
		    emit_indent();
		    O("%s(%s);\n", tname.c_str(), args.c_str());
		    return;
		}
	    }
	}

	// Stream type: managed fstream/ifstream/ofstream/stringstream
	std::string sclass = stream_type_name(specs);
	if (!sclass.empty()) {
	    emit_stream_decl(sclass, decls);
	    return;
	}

	// Array type: managed MadArray with runtime wrappers
	if (is_array_type(specs)) {
	    emit_array_decl(decls);
	    return;
	}

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

	// C++ `auto` → infer type from initializer.
	// Currently handles: auto var = funcname → function pointer.
	if (type == "auto" && is_an(decls, AN_INIT_DECL)) {
	    gp_tree_node *init_expr = child(decls, 1);
	    std::string vname = extract_name(child(decls, 0));
	    if (init_expr && init_expr->type == GP_TERM &&
		term_code(init_expr) == GT_IDENT) {
		std::string fname = term_text(init_expr);
		auto sit = func_signatures.find(fname);
		if (sit != func_signatures.end()) {
		    // Emit: ret_type (*var)(params) = funcname;
		    const FuncSig &sig = sit->second;
		    std::string p = sig.param_list.empty() ? "void" : sig.param_list;
		    emit_indent();
		    O("%s (*%s)(%s) = %s;\n", sig.ret_type.c_str(),
		      vname.c_str(), p.c_str(), fname.c_str());
		    // Track as function pointer for call sites
		    local_var_types[vname] = TC_INT;
		    return;
		}
	    }
	    // Fallback: auto with non-function initializer → int
	    type = "int";
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

	// Pointer-to-class with new: `Box *b = new Box(3,4);`
	// Emit: struct Box *b = (struct Box *)malloc(sizeof(struct Box));
	//       Box__Box(b, 3, 4);
	if (is_an(decls, AN_INIT_DECL)) {
	    gp_tree_node *declarator = child(decls, 0);
	    gp_tree_node *initializer = child(decls, 1);
	    bool is_new_ctor = is_an(initializer, AN_NEW_CTOR);
	    bool is_new_plain = is_an(initializer, AN_NEW_PLAIN);
	    if ((is_new_ctor || is_new_plain) && is_an(declarator, AN_PTR_DECL)) {
		std::string vname = extract_name(declarator);
		std::string new_cls = term_text(child(initializer, 0));
		std::string alloc = emit_expr(initializer);
		emit_indent();
		O("%s *%s = %s;\n", type.c_str(), vname.c_str(), alloc.c_str());
		// Emit constructor call with proper args
		if (is_new_ctor) {
		    std::string args = emit_arg_list(child(initializer, 1));
		    emit_indent();
		    if (args.empty())
			O("%s__%s(%s);\n", new_cls.c_str(), new_cls.c_str(),
			  vname.c_str());
		    else
			O("%s__%s(%s, %s);\n", new_cls.c_str(), new_cls.c_str(),
			  vname.c_str(), args.c_str());
		} else if (class_has_ctor(new_cls)) {
		    emit_indent();
		    O("%s__%s(%s);\n", new_cls.c_str(), new_cls.c_str(),
		      vname.c_str());
		}
		local_var_class_map[vname] = new_cls;
		// Don't push to scope_class_vars — delete handles dtor for heap objects
		return;
	    }
	}

	// Mixed declaration: `float fx(), a, b, c;` — split into function
	// declarators (already prototyped globally, skip them) and variable
	// declarators (emit normally).
	if (is_an(decls, AN_DECL_LIST)) {
	    std::vector<gp_tree_node *> all_decls;
	    collect_decl_list(decls, all_decls);
	    std::vector<gp_tree_node *> var_decls;
	    for (gp_tree_node *d : all_decls)
		if (!is_plain_func_decl(d))
		    var_decls.push_back(d);
	    if (var_decls.size() < all_decls.size()) {
		// There were function declarators — emit only the variable ones.
		if (!var_decls.empty()) {
		    std::string decl_str;
		    for (size_t i = 0; i < var_decls.size(); i++) {
			if (i > 0) decl_str += ", ";
			decl_str += emit_declarator_str(var_decls[i]);
		    }
		    emit_indent();
		    O("%s %s;\n", type.c_str(), decl_str.c_str());
		}
		return;
	    }
	    // No function declarators — fall through to normal emit.
	}

	// VLA lowering: convert variable-length array declarations to
	// __builtin_alloca.  Only for local (non-global) declarations.
	if (indent_level > 0 && declarator_has_vla(decls)) {
	    // VLA typedef: `typedef int c[n+2]` → record sizeof and suppress
	    if (type.find("typedef") != std::string::npos) {
		emit_vla_typedef(type, decls);
		return;
	    }
	    emit_vla_alloca(type, decls);
	    // Still need to track the variable name below
	    goto post_decl_emit;
	}

	// For 3D+ char arrays, set the depth flag before emitting so that
	// string literal sub-initializers are expanded to byte lists.
	{
	    bool is_char_type = (type.find("char") != std::string::npos &&
				 type.find('*') == std::string::npos);
	    if (is_char_type && array_dims(decls) >= 3)
		char_array_init_depth = 1;
	    emit_indent();
	    O("%s %s;\n", type.c_str(), emit_declarator_str(decls).c_str());
	    char_array_init_depth = 0;
	}

	post_decl_emit:
	// Inject constructor call for class variables (no-args ctor)
	std::string vname = extract_name(decls);

	// Track va_list variables so we can emit proper va_start/va_end
	if (type == "va_list" && !vname.empty())
	    va_list_vars.insert(vname);
	if (!vname.empty() && class_has_ctor(clean_type)) {
	    emit_indent();
	    O("%s__%s(&%s);\n", clean_type.c_str(), clean_type.c_str(),
	      vname.c_str());
	    if (class_has_dtor(clean_type))
		scope_class_vars.push_back(vname + "|" + clean_type);
	}
    }

    // ---------------------------------------------------------------
    // VLA lowering: emit __builtin_alloca for variable-length arrays
    // ---------------------------------------------------------------

    // Collect array dimensions from a declarator chain.
    // Returns the dimension expressions (as emitted C strings) from
    // outermost to innermost, and sets `name` to the base variable name.
    void collect_vla_dims(gp_tree_node *node,
			  std::vector<std::string> &dims,
			  std::vector<bool> &dim_is_vla,
			  std::string &name)
    {
	if (!node) return;
	if (node->type == GP_TERM) {
	    name = term_text(node);
	    return;
	}
	if (node->type != GP_ANODE) return;
	int an = an_code(node);
	if (an == AN_ARRAY_DECL) {
	    gp_tree_node *sz = child(node, 1);
	    if (sz && !is_nil(sz)) {
		dims.push_back(emit_expr(sz));
		dim_is_vla.push_back(expr_has_ident(sz));
	    } else {
		dims.push_back("");
		dim_is_vla.push_back(false);
	    }
	    collect_vla_dims(child(node, 0), dims, dim_is_vla, name);
	} else if (an == AN_INIT_DECL) {
	    collect_vla_dims(child(node, 0), dims, dim_is_vla, name);
	} else {
	    name = extract_name(node);
	}
    }

    void emit_vla_alloca(const std::string &type, gp_tree_node *decls)
    {
	std::vector<std::string> dims;
	std::vector<bool> dim_is_vla;
	std::string name;
	collect_vla_dims(decls, dims, dim_is_vla, name);

	// dims are collected outermost-first due to how the AST nests
	// AN_ARRAY_DECL nodes.  E.g. int M[a][b] gives dims = {"b","a"}
	// (innermost first from the recursive descent) — actually reverse
	// because the outer array wraps the inner.  Let's verify the order:
	// AN_ARRAY_DECL(AN_ARRAY_DECL(name, b), a) → first call gets a,
	// recurse gets b.  So dims = {"a", "b"} = outermost first.

	int ndims = (int)dims.size();
	if (ndims == 0) {
	    // Shouldn't happen — fallback to normal emit
	    emit_indent();
	    O("%s %s;\n", type.c_str(), emit_declarator_str(decls).c_str());
	    return;
	}

	// Build the total size expression: d0 * d1 * ... * sizeof(type)
	std::string size_expr;
	for (int i = 0; i < ndims; i++) {
	    if (!dims[i].empty()) {
		if (!size_expr.empty()) size_expr += " * ";
		size_expr += "(" + dims[i] + ")";
	    }
	}
	if (!size_expr.empty())
	    size_expr += " * ";
	size_expr += "sizeof(" + type + ")";

	if (ndims == 1) {
	    // 1D VLA: int *name = (int *)__builtin_alloca(size);
	    emit_indent();
	    O("%s *%s = (%s *)__builtin_alloca(%s);\n",
	      type.c_str(), name.c_str(), type.c_str(), size_expr.c_str());
	} else {
	    // Multi-dim VLA: use array-of-pointers approach.
	    // For int M[a][b]:
	    //   int **M = (int **)__builtin_alloca(a * sizeof(int *));
	    //   { int *_M_data = (int *)__builtin_alloca(a * b * sizeof(int));
	    //     for (int _i = 0; _i < a; _i++) M[_i] = _M_data + _i * b; }
	    //
	    // For 3D+ we only handle the common 2D case properly;
	    // deeper dimensions are flattened (subscript behavior may differ).

	    // Generate pointer type: int ** for 2D, int *** for 3D, etc.
	    std::string ptr_stars;
	    for (int i = 0; i < ndims; i++) ptr_stars += "*";

	    // Outer pointer array allocation
	    emit_indent();
	    O("%s %s%s = (%s %s)__builtin_alloca((%s) * sizeof(%s %s));\n",
	      type.c_str(), ptr_stars.c_str(), name.c_str(),
	      type.c_str(), ptr_stars.c_str(),
	      dims[0].c_str(),
	      type.c_str(), std::string(ptr_stars.size() - 1, '*').c_str());

	    // Data allocation (flat)
	    std::string data_var = "_" + name + "_data";
	    emit_indent();
	    O("%s *%s = (%s *)__builtin_alloca(%s);\n",
	      type.c_str(), data_var.c_str(), type.c_str(), size_expr.c_str());

	    if (ndims == 2) {
		// Row pointer setup: for (int _i=0; _i<a; _i++) M[_i] = data + _i*b;
		std::string idx = "_" + name + "_i";
		emit_indent();
		O("for (int %s = 0; %s < (%s); %s++) %s[%s] = %s + %s * (%s);\n",
		  idx.c_str(), idx.c_str(), dims[0].c_str(),
		  idx.c_str(), name.c_str(), idx.c_str(),
		  data_var.c_str(), idx.c_str(), dims[1].c_str());
	    } else {
		// 3D+ — set up first-level pointers only; deeper levels
		// remain flat.  This is a best-effort fallback.
		std::string idx = "_" + name + "_i";
		// Compute stride for first dim (product of remaining dims)
		std::string stride;
		for (int i = 1; i < ndims; i++) {
		    if (!stride.empty()) stride += " * ";
		    stride += "(" + dims[i] + ")";
		}
		emit_indent();
		O("for (int %s = 0; %s < (%s); %s++) %s[%s] = (%s %s)(%s + %s * %s);\n",
		  idx.c_str(), idx.c_str(), dims[0].c_str(),
		  idx.c_str(), name.c_str(), idx.c_str(),
		  type.c_str(), std::string(ptr_stars.size() - 1, '*').c_str(),
		  data_var.c_str(), idx.c_str(), stride.c_str());
	    }
	}
    }

    // Handle VLA typedef: `typedef int c[n+2]`
    // Instead of emitting the typedef (c2mir rejects VLA types),
    // record the computed size expression so that sizeof(c) can
    // be replaced with the runtime value.
    // Check if an expression has side effects (++, --, function calls).
    static bool expr_has_side_effects(gp_tree_node *node) {
	if (!node) return false;
	if (node->type == GP_ANODE) {
	    int an = an_code(node);
	    if (an == AN_POST_INC || an == AN_POST_DEC ||
		an == AN_PRE_INC || an == AN_PRE_DEC ||
		an == AN_CALL)
		return true;
	    for (int i = 0; i < nchildren(node); i++)
		if (expr_has_side_effects(child(node, i)))
		    return true;
	}
	return false;
    }

    // Collect VLA param size expressions that have side effects.
    // These need to be evaluated at function entry.
    void collect_vla_param_side_effects(gp_tree_node *node) {
	if (!node || node->type != GP_ANODE) return;
	int an = an_code(node);
	if (an == AN_ARRAY_DECL) {
	    gp_tree_node *sz = child(node, 1);
	    if (sz && !is_nil(sz) && expr_has_side_effects(sz)) {
		vla_param_side_effects.push_back(emit_expr(sz));
	    }
	    collect_vla_param_side_effects(child(node, 0));
	}
    }

    void emit_vla_typedef(const std::string &type, gp_tree_node *decls)
    {
	// Extract base type (strip "typedef ")
	std::string base = type;
	size_t pos = base.find("typedef ");
	if (pos != std::string::npos)
	    base.erase(pos, 8);
	// Trim leading/trailing whitespace
	while (!base.empty() && base[0] == ' ') base.erase(0, 1);
	while (!base.empty() && base.back() == ' ') base.pop_back();

	// Collect dimensions and name
	std::vector<std::string> dims;
	std::vector<bool> dim_is_vla;
	std::string name;
	collect_vla_dims(decls, dims, dim_is_vla, name);

	// Build size expression: d0 * d1 * ... * sizeof(base_type)
	std::string size_expr;
	for (size_t i = 0; i < dims.size(); i++) {
	    if (!dims[i].empty()) {
		if (!size_expr.empty()) size_expr += " * ";
		size_expr += "(" + dims[i] + ")";
	    }
	}
	if (!size_expr.empty())
	    size_expr += " * ";
	size_expr += "sizeof(" + base + ")";

	// Store for sizeof() replacement
	vla_typedef_sizes[name] = size_expr;

	// Emit a size_t variable to hold the computed size at runtime
	emit_indent();
	O("unsigned long _sizeof_%s = %s;\n", name.c_str(), size_expr.c_str());
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
	    // VLA array params decay to pointers in C:
	    // int arr[n] → int *arr, int arr[n][m] → int (*arr)[m] or int **arr
	    if (declarator_has_vla(decl)) {
		std::string name = extract_name(decl);
		int nd = array_dims(decl);
		// Capture VLA size expressions with side effects (i++, fn())
		collect_vla_param_side_effects(decl);
		if (nd <= 1)
		    return type + " *" + name;
		// Multi-dim VLA param: decay outermost dim to pointer
		std::string stars;
		for (int i = 0; i < nd; i++) stars += "*";
		return type + " " + stars + name;
	    }
	    // The declarator may have pointer, name, etc.
	    std::string d = emit_declarator_str(decl);
	    // If emit_declarator_str returned empty, the declarator may be
	    // an abstract declarator (unnamed pointer param like "void *").
	    // Fall back to emit_abstract_declarator to pick up the pointer.
	    if (d.empty()) {
		d = emit_abstract_declarator(decl);
		if (d.empty()) return type;
	    }
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

    // Extract the last named parameter from a parameter list node.
    // Used to generate va_start(ap, last_param).
    std::string extract_last_named_param(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";
	int an = an_code(node);
	// param_va(param_list) — variadic: descend into the param_list
	if (an == AN_PARAM_VA)
	    return extract_last_named_param(child(node, 0));
	// param_list(prev, next) — rightmost is last
	if (an == AN_PARAM_LIST) {
	    std::string r = extract_last_named_param(child(node, 1));
	    if (!r.empty()) return r;
	    return extract_last_named_param(child(node, 0));
	}
	// param(type, declarator)
	if (an == AN_PARAM) {
	    gp_tree_node *decl = child(node, 1);
	    if (!is_nil(decl)) return extract_name(decl);
	    return "";
	}
	return "";
    }

    void emit_func_def(gp_tree_node *node)
    {
	// func_def(declaration_specifiers, declarator, compound_statement)
	local_var_types.clear();  // fresh scope for each function
	local_var_class_map.clear();
	scope_class_vars.clear();
	try_depth = 0;
	try_nesting = 0;
	try_dtor_guards.clear();
	va_list_vars.clear();
	vla_typedef_sizes.clear();
	vla_param_side_effects.clear();
	current_func_last_param.clear();
	ref_vars.clear();
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
	    gp_tree_node *plist = child(inner, 1);
	    track_param_types(plist);
	    collect_ref_params(func_name, plist);
	    params = emit_param_list(plist);
	    // If this is a variadic function, record last named param for va_start
	    if (is_an(plist, AN_PARAM_VA))
		current_func_last_param = extract_last_named_param(plist);
	} else {
	    func_name = term_text(inner);
	}

	std::string full_ret = ret_type;
	if (!ptr_str.empty())
	    full_ret += " " + ptr_str;

	// Record function signature for auto type inference
	func_signatures[func_name] = {full_ret, params};

	// Forward declaration in header
	OH("%s %s(%s);\n", full_ret.c_str(), func_name.c_str(),
	   params.empty() ? "void" : params.c_str());

	// Function body
	O("%s %s(%s)\n", full_ret.c_str(), func_name.c_str(),
	  params.empty() ? "void" : params.c_str());

	indent_level = 0;
	// For main(), inject __madc_init_globals() as first statement,
	// set flag so return statements also get __madc_cleanup_globals(),
	// and inject __madc_cleanup_globals() before the implicit end of main.
	bool is_main = (func_name == "main");
	if (is_main && !global_strings.empty()) {
	    emitting_main = true;
	    O("{\n");
	    indent_level++;
	    O("\t__madc_init_globals();\n");
	    // Inject VLA param side-effect expressions
	    for (const auto &se : vla_param_side_effects) {
		emit_indent();
		O("(void)(%s);\n", se.c_str());
	    }
	    // Emit body statements (skip outer block wrapper to avoid double braces)
	    gp_tree_node *stmts = is_an(body_node, AN_BLOCK) ? child(body_node, 0) : body_node;
	    emit_stmt(stmts);
	    // Inject cleanup before implicit fall-through end of main()
	    O("\t__madc_cleanup_globals();\n");
	    indent_level--;
	    O("}\n");
	    emitting_main = false;
	} else if (!vla_param_side_effects.empty()) {
	    // Inject VLA param side-effect expressions at function entry
	    O("{\n");
	    indent_level++;
	    for (const auto &se : vla_param_side_effects) {
		emit_indent();
		O("(void)(%s);\n", se.c_str());
	    }
	    gp_tree_node *stmts = is_an(body_node, AN_BLOCK) ? child(body_node, 0) : body_node;
	    emit_stmt(stmts);
	    indent_level--;
	    O("}\n");
	} else {
	    emit_stmt(body_node);
	}
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
	    OH("    %s", safe_ident(term_text(node)).c_str());
	else if (is_an(node, AN_ENUM_ASSIGN))
	    OH("    %s = %s", safe_ident(term_text(child(node, 0))).c_str(),
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

    // Per-class operator overloads: class_name → set of operator symbols
    std::map<std::string, std::set<std::string>> class_operators;

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

	// Determine vtable info for this class
	std::string vtable_owner;  // class that declares the vtable struct
	const SemaClassInfo *ci_vt = nullptr;
	if (sema) {
	    // Find the root class with virtual methods
	    vtable_owner = sema->vtable_class(class_name);
	    if (!vtable_owner.empty())
		ci_vt = sema->get_class(vtable_owner);
	}

	// Emit vtable struct (only for the class that first declares virtuals)
	if (ci_vt && vtable_owner == class_name && ci_vt->has_virtuals()) {
	    OH("struct %s_vtable {\n", class_name.c_str());
	    for (size_t i = 0; i < ci_vt->virtual_methods.size(); i++) {
		const std::string &vm = ci_vt->virtual_methods[i];
		auto rit = ci_vt->virtual_ret_types.find(vm);
		std::string rt = (rit != ci_vt->virtual_ret_types.end()) ? rit->second : "int";
		auto pit = ci_vt->virtual_param_types.find(vm);
		std::string pt = (pit != ci_vt->virtual_param_types.end()) ? pit->second : "";
		std::string params = "struct " + class_name + " *";
		if (!pt.empty())
		    params += ", " + pt;
		OH("    %s (*%s)(%s);\n", rt.c_str(), vm.c_str(), params.c_str());
	    }
	    OH("};\n\n");
	}

	// First pass: emit the struct with fields only
	OH("struct %s {\n", class_name.c_str());
	// Add __vptr as first field if this class hierarchy has virtuals
	std::string vptr_field;
	if (ci_vt) {
	    vptr_field = "    struct " + vtable_owner + "_vtable *__vptr;\n";
	    // Only emit __vptr if not already inherited from base
	    if (base_class.empty() || vtable_owner == class_name)
		OH("%s", vptr_field.c_str());
	}
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
	// Store emitted fields including __vptr for inheritance
	std::string all_fields;
	if (ci_vt && (base_class.empty() || vtable_owner == class_name))
	    all_fields = vptr_field;
	if (!base_class.empty()) {
	    auto bit = emitted_class_fields.find(base_class);
	    if (bit != emitted_class_fields.end())
		all_fields += bit->second;
	}
	all_fields += field_text;
	emitted_class_fields[class_name] = all_fields;
	OH("};\n\n");

	// Second pass: emit methods as free functions
	// Forward-declare vtable instance (ctor references it before definition)
	if (ci_vt) {
	    O("static struct %s_vtable %s_vtable_instance;\n",
	      vtable_owner.c_str(), class_name.c_str());
	}
	emit_class_methods(class_name, body_node);

	// Emit vtable instance for this class (if it has virtuals or inherits them)
	if (ci_vt && sema) {
	    const SemaClassInfo *this_ci = sema->get_class(class_name);
	    if (this_ci) {
		emit_vtable_instance(class_name, vtable_owner, *ci_vt, *this_ci);
	    }
	}

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

	    // Strip /* virtual */ comment from return type
	    std::string virt_prefix = "/* virtual */ ";
	    if (ret_type.substr(0, virt_prefix.size()) == virt_prefix)
		ret_type = ret_type.substr(virt_prefix.size());

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

	    // Check for base class — need to chain base ctor
	    std::string base_ctor_call;
	    if (sema) {
		auto bit = sema->class_bases.find(class_name);
		if (bit != sema->class_bases.end()) {
		    std::string base = bit->second;
		    base_ctor_call = base + "__" + base +
			"((struct " + base + " *)__this);\n";
		}
	    }

	    // If class has vtable, inject vptr initialization at top of ctor body
	    std::string vptr_init;
	    if (sema) {
		std::string vtbl_cls = sema->vtable_class(class_name);
		if (!vtbl_cls.empty())
		    vptr_init = "__this->__vptr = &" + class_name + "_vtable_instance;\n";
	    }

	    bool need_inject = !base_ctor_call.empty() || !vptr_init.empty();
	    if (need_inject && is_an(body_node, AN_BLOCK)) {
		// Emit block manually to inject base ctor / vptr init at the top
		emit_indent();
		O("{\n");
		indent_level++;
		if (!base_ctor_call.empty()) {
		    emit_indent();
		    O("%s", base_ctor_call.c_str());
		}
		if (!vptr_init.empty()) {
		    emit_indent();
		    O("%s", vptr_init.c_str());
		}
		emit_stmt(child(body_node, 0));
		indent_level--;
		emit_indent();
		O("}\n");
	    } else {
		emit_stmt(body_node);
	    }
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

	    // Check for base class — need to chain base dtor
	    std::string base_class;
	    if (sema) {
		auto bit = sema->class_bases.find(class_name);
		if (bit != sema->class_bases.end())
		    base_class = bit->second;
	    }

	    if (!base_class.empty() && is_an(body_node, AN_BLOCK)) {
		// Emit block manually so we can inject base dtor before closing }
		emit_indent();
		O("{\n");
		indent_level++;
		emit_stmt(child(body_node, 0));
		// Chain to base class destructor
		emit_indent();
		O("%s__dtor((struct %s *)__this);\n",
		  base_class.c_str(), base_class.c_str());
		indent_level--;
		emit_indent();
		O("}\n");
	    } else {
		emit_stmt(body_node);
	    }
	    O("\n");

	    current_class = prev_class;
	    return;
	}

	// oper_method(ret_type, op_symbol, params, body)
	if (an == AN_OPER_METHOD) {
	    std::string prev_class = current_class;
	    current_class = class_name;

	    std::string ret_type = emit_type(child(node, 0));
	    gp_tree_node *op_node = child(node, 1);
	    std::string params = emit_param_list(child(node, 2));
	    gp_tree_node *body_node = child(node, 3);

	    // Get operator symbol and compute mangled name
	    std::string op_sym = op_terminal_to_str(op_node);
	    std::string suffix = op_suffix(op_sym);
	    std::string mangled = class_name + "__" + suffix;

	    // Track this operator for call site rewriting
	    class_operators[class_name].insert(op_sym);

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
    }

    // Emit a static vtable instance for a class
    void emit_vtable_instance(const std::string &class_name,
			      const std::string &vtable_owner,
			      const SemaClassInfo &vtable_ci,
			      const SemaClassInfo &this_ci)
    {
	O("static struct %s_vtable %s_vtable_instance = {\n",
	  vtable_owner.c_str(), class_name.c_str());
	for (size_t i = 0; i < vtable_ci.virtual_methods.size(); i++) {
	    const std::string &vm = vtable_ci.virtual_methods[i];
	    // Determine which class provides the implementation
	    std::string impl_class = class_name;
	    if (sema) {
		// Walk from this class up to find who implements this method
		std::string check = class_name;
		bool found = false;
		while (!check.empty()) {
		    const SemaClassInfo *ci = sema->get_class(check);
		    if (ci && ci->methods.count(vm)) {
			impl_class = check;
			found = true;
			break;
		    }
		    auto bit = sema->class_bases.find(check);
		    if (bit != sema->class_bases.end())
			check = bit->second;
		    else
			break;
		}
		if (!found) impl_class = vtable_owner;
	    }
	    std::string mangled = impl_class + "__" + vm;
	    // Cast to vtable function pointer type
	    auto rit = vtable_ci.virtual_ret_types.find(vm);
	    std::string rt = (rit != vtable_ci.virtual_ret_types.end()) ? rit->second : "int";
	    auto pit = vtable_ci.virtual_param_types.find(vm);
	    std::string pt = (pit != vtable_ci.virtual_param_types.end()) ? pit->second : "";
	    std::string params = "struct " + vtable_owner + " *";
	    if (!pt.empty())
		params += ", " + pt;
	    O("    (%s (*)(%s))%s,\n", rt.c_str(), params.c_str(), mangled.c_str());
	}
	O("};\n\n");
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
	    gp_tree_node *specs = child(node, 0);
	    gp_tree_node *decls = child(node, 1);

	    // Global string variable — emit storage array; track for init/cleanup
	    if (is_string_type(specs)) {
		collect_global_string_decls(decls);
		return;
	    }

	    std::string type = emit_type(specs);

	    // Check if this is a typedef
	    bool is_typedef = (type.find("typedef") != std::string::npos);

	    if (is_nil(decls)) {
		// Type-only decl (struct/enum definition with no variable)
		// Check if type contains a struct/enum definition
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
		// Suppress recovered error expressions like "hello;" that
		// have no real type keywords — just an identifier as type.
		// Real type-only decls (struct, enum defs) are handled above.
		if (!type.empty() && type.find(' ') == std::string::npos) {
		    // Single-word "type" with no declarator — likely a
		    // grammar recovery of a top-level expression statement.
		    return;
		}
		OH("%s;\n", type.c_str());
		return;
	    }

	    // Declaration with declarators
	    if (is_typedef) {
		// Suppress va_list typedef — we use <stdarg.h> in the preamble
		std::string decl_name = emit_declarator_str(decls);
		if (decl_name == "va_list") return;
		OH("%s %s;\n", type.c_str(), decl_name.c_str());
	    } else {
		// For multi-dimensional char arrays (3+ dims), set the depth
		// flag so that string literal initializers in sub-lists are
		// expanded to explicit byte arrays (c2mir bug workaround).
		bool is_char_type = (type.find("char") != std::string::npos &&
				     type.find('*') == std::string::npos);
		if (is_char_type && array_dims(decls) >= 3)
		    char_array_init_depth = 1;
		std::string d = emit_declarator_str(decls);
		char_array_init_depth = 0;
		// Suppress bare-identifier "declarations" with empty type —
		// these result from grammar error-recovery on top-level
		// assignment statements (e.g. `hello = "x";` at file scope).
		if (type.empty() && decls->type == GP_TERM) return;
		// Extern declarations go in header
		if (type.find("extern") != std::string::npos) {
		    OH("%s %s;\n", type.c_str(), d.c_str());
		} else {
		    OH("%s %s;\n", type.c_str(), d.c_str());
		}
	    }
	    return;
	}

	// Top-level expression statement (e.g. global assignment recovered
	// from a parse error on "hello = ...;" at file scope).
	// Capture as a global init statement.
	if (is_an(node, AN_EXPR_STMT) || is_an(node, AN_ASSIGN)) {
	    std::string s = emit_expr(node);
	    if (!s.empty())
		global_init_stmts.push_back(s + ";");
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
	    is_an(node, AN_NAMESPACE_DEF)) {
	    return;
	}

	// prefer calloc; — set flag for new expressions
	if (is_an(node, AN_PREFER)) {
	    std::string pref = term_text(child(node, 0));
	    if (pref == "calloc")
		prefer_calloc = true;
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
    CEmitter() : indent_level(0), tmp_counter(0), sema(nullptr), emitting_main(false) {}

    std::string emit(gp_tree_node *root, SemaInfo *sema_info = nullptr)
    {
	sema = sema_info;
	header.clear();
	body.clear();

	header += "/* Generated by madc transpiler */\n";
	header += "#include <stdarg.h>\n";
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
	header += "extern int *__errno_location(void);\n";
	header += "extern void *fopen(const char *, const char *);\n";
	header += "extern int fclose(void *);\n";
	header += "extern int system(const char *);\n";
	header += "extern int snprintf(char *, unsigned long, const char *, ...);\n";
	header += "extern int sprintf(char *, const char *, ...);\n";
	header += "extern int sscanf(const char *, const char *, ...);\n";
	header += "extern int vsprintf(char *, const char *, va_list);\n";
	header += "extern int vsnprintf(char *, unsigned long, const char *, va_list);\n";
	header += "extern int vprintf(const char *, va_list);\n";
	header += "extern int vfprintf(void *, const char *, va_list);\n";
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

	// Object sizes — computed from sizeof() so they're correct for
	// whatever libstdc++/libc++ is linked. Portable across ABIs.
	{
	    char __sz[512];
	    snprintf(__sz, sizeof(__sz),
		"#define MADC_STRING_SIZE %zu\n"
		"#define MADC_OFSTREAM_SIZE %zu\n"
		"#define MADC_IFSTREAM_SIZE %zu\n"
		"#define MADC_FSTREAM_SIZE %zu\n"
		"#define MADC_SSTREAM_SIZE %zu\n"
		"#define MADC_ARRAY_SIZE %zu\n",
		sizeof(std::string),
		sizeof(std::ofstream),
		sizeof(std::ifstream),
		sizeof(std::fstream),
		sizeof(std::stringstream),
		sizeof(MadArray));
	    header += __sz;
	}
	header += "extern void *string_construct(void *);\n";
	header += "extern void string_destruct(void *);\n";
	header += "extern void *string_construct_cstr(void *, const char *);\n";
	header += "extern void string_assign(void *, void *);\n";
	header += "extern void string_assign_cstr(void *, const char *);\n";
	header += "extern const char *string_cstr(void *);\n";
	header += "extern long string_length(void *);\n";
	header += "extern void string_append(void *, void *);\n";
	header += "extern void string_append_cstr(void *, const char *);\n";
	header += "extern void __std_to_string(void *, long);\n";
	// ostream wrappers — mirrors streamout_string/cstr/numeric from legacy
	header += "extern void streamout_string(void *, void *);\n";
	header += "extern void streamout_cstr(void *, const char *);\n";
	header += "extern void streamout_int64(void *, long);\n";
	header += "extern void streamout_uint64(void *, unsigned long);\n";
	header += "extern void streamout_char(void *, int);\n";
	header += "extern void streamout_double(void *, double);\n";
	header += "extern void streamout_endl(void *);\n";
	header += "extern void streamout_flush(void *);\n";
	header += "extern void streamout_hex(void *);\n";
	header += "extern void streamout_oct(void *);\n";
	header += "extern void streamout_dec(void *);\n";
	header += "extern void streamout_fixed(void *);\n";
	header += "extern void streamout_scientific(void *);\n";
	header += "extern void streamout_left(void *);\n";
	header += "extern void streamout_right(void *);\n";
	header += "extern void streamout_boolalpha(void *);\n";
	header += "extern void streamout_noboolalpha(void *);\n";
	header += "extern void streamout_showbase(void *);\n";
	header += "extern void streamout_noshowbase(void *);\n";
	header += "extern void streamout_setw(void *, int);\n";
	header += "extern void streamout_setprecision(void *, int);\n";
	header += "extern void streamout_setfill(void *, int);\n";
	// istream wrappers
	header += "extern void streamin_int(void *, long *);\n";
	header += "extern void streamin_uint(void *, unsigned long *);\n";
	header += "extern void streamin_double(void *, double *);\n";
	header += "extern void streamin_char(void *, char *);\n";
	header += "extern void streamin_string(void *, void *);\n";
	header += "extern void streamin_getline(void *, void *);\n";
	header += "\n";

	// fstream runtime wrappers
	header += "extern void *ofstream_construct(void *);\n";
	header += "extern void ofstream_destruct(void *);\n";
	header += "extern void ofstream_open(void *, const char *);\n";
	header += "extern void ofstream_close(void *);\n";
	header += "extern long ofstream_good(void *);\n";
	header += "extern long ofstream_is_open(void *);\n";
	header += "extern void *ifstream_construct(void *);\n";
	header += "extern void ifstream_destruct(void *);\n";
	header += "extern void ifstream_open(void *, const char *);\n";
	header += "extern void ifstream_close(void *);\n";
	header += "extern long ifstream_good(void *);\n";
	header += "extern long ifstream_eof(void *);\n";
	header += "extern long ifstream_is_open(void *);\n";
	header += "extern void *fstream_construct(void *);\n";
	header += "extern void fstream_destruct(void *);\n";
	header += "extern void fstream_open(void *, const char *);\n";
	header += "extern void fstream_close(void *);\n";
	header += "extern void *sstream_construct(void *);\n";
	header += "extern void sstream_destruct(void *);\n";
	header += "extern const char *sstream_str(void *);\n";
	header += "extern void sstream_str_set(void *, const char *);\n";
	header += "extern void printstream(void *);\n";
	header += "\n";

	// MadArray runtime wrappers
	header += "extern void *madarray_construct(void *);\n";
	header += "extern void madarray_destruct(void *);\n";
	header += "extern long madarray_size(void *);\n";
	header += "\n";

	// Stream pointers
	header += "extern void *__madc_cout_ptr(void);\n";
	header += "extern void *__madc_cerr_ptr(void);\n";
	header += "extern void *__madc_cin_ptr(void);\n";
	header += "#define __madc_cout (__madc_cout_ptr())\n";
	header += "#define __madc_cerr (__madc_cerr_ptr())\n";
	header += "#define __madc_cin  (__madc_cin_ptr())\n";
	header += "\n";

	// (old __std_cout_* declarations removed — replaced by streamout_* above)
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

	// Exception handling (SJLJ) runtime
	header += "/* Exception handling runtime (SJLJ) */\n";
	header += "#define __MADC_TRYCTX_SIZE 216\n";
	header += "#define __MADC_TRYCTX_ALIGN 16\n";
	header += "typedef struct { char __buf[__MADC_TRYCTX_SIZE]; } __madc_try_ctx_t;\n";
	header += "extern void *__madc_try_push(void *);\n";
	header += "extern void __madc_try_pop(void);\n";
	header += "extern void __madc_throw_int(long);\n";
	header += "extern void __madc_throw_double(double);\n";
	header += "extern void __madc_throw_cstr(const char *);\n";
	header += "extern void __madc_rethrow(void);\n";
	header += "extern int __madc_exception_type(void);\n";
	header += "extern long __madc_exception_int(void);\n";
	header += "extern double __madc_exception_double(void);\n";
	header += "extern const char *__madc_exception_cstr(void);\n";
	header += "extern void __madc_exception_clear(void);\n";
	header += "extern int _setjmp(void *);\n";
	header += "\n";

	header += "\n";

	ns_funcs_used.clear();
	global_strings.clear();
	global_init_stmts.clear();
	emit_top_level(root);
	emit_global_init_cleanup();

	// Emit extern declarations for all namespace functions used.
	// Known std:: functions get proper typed signatures; others fall back
	// to the generic variadic form.
	if (!ns_funcs_used.empty()) {
	    // Map of mangled name -> typed extern declaration
	    static const std::unordered_map<std::string, std::string> known_ns_sigs = {
		{"__std_stoi",      "extern long __std_stoi(void *);"},
		{"__std_stol",      "extern long __std_stol(void *);"},
		{"__std_stoul",     "extern unsigned long __std_stoul(void *);"},
		{"__std_stof",      "extern double __std_stof(void *);"},
		{"__std_stod",      "extern double __std_stod(void *);"},
		{"__std_stold",     "extern double __std_stold(void *);"},
		{"__std_to_string", "extern void __std_to_string(void *, long);"},
		{"__std_for_each", "extern void __std_for_each(void *, void *);"},
	    };
	    std::string ns_decls;
	    ns_decls += "/* Namespace function externs (resolved via dlsym) */\n";
	    for (auto &fn : ns_funcs_used) {
		auto it = known_ns_sigs.find(fn);
		if (it != known_ns_sigs.end())
		    ns_decls += it->second + "\n";
		else
		    ns_decls += "extern long " + fn + "();\n";
	    }
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
