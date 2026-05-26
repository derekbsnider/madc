// madc_sema.cpp — Semantic pre-pass for Gecko AST
//
// Phase 2 of the Gecko pipeline.  Walks the gp_tree_node AST produced
// by the Gecko GLR parser and collects type information into a SemaInfo
// struct.  The C emitter (Phase 3) queries SemaInfo for:
//   - Variable types (for cout format inference)
//   - Function return types (for call expression type inference)
//   - Class names, fields, methods, ctor/dtor presence
//   - Typedef names (for type vs identifier disambiguation)
//
// This file does NOT emit code.  It only builds symbol tables.

#include <cstdio>
#include <cstring>
#include <iostream>
#include <sstream>
#include <fstream>
#include <string>
#include <map>
#include <set>
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

#include "madc_anode.h"
#include "madc_sema.h"

using namespace std;

// -----------------------------------------------------------------------
// Gecko terminal codes — must match madc_grammar.cpp
// -----------------------------------------------------------------------

enum SemaTermCode {
    GT_IDENT      = 256,
    GT_INTEGER    = 257,
    GT_REAL       = 258,
    GT_STRING     = 259,
    GT_CHAR_LIT   = 260,

    GT_VOID       = 310,
    GT_BOOL       = 311,
    GT_CHAR       = 312,
    GT_SHORT      = 313,
    GT_INT        = 314,
    GT_LONG       = 315,
    GT_FLOAT      = 316,
    GT_DOUBLE     = 317,
    GT_STRING_T   = 318,
    GT_INT8       = 319,
    GT_INT16      = 320,
    GT_INT32      = 321,
    GT_INT64      = 322,
    GT_UINT8      = 323,
    GT_UINT16     = 324,
    GT_UINT32     = 325,
    GT_UINT64     = 326,

    GT_SIGNED     = 285,
    GT_UNSIGNED   = 286,

    GT_TYPEDEF    = 277,
    GT_EXTERN     = 280,
    GT_STATIC     = 279,
    GT_CONST      = 281,
};

// -----------------------------------------------------------------------
// Helpers — access Gecko tree nodes
// -----------------------------------------------------------------------

static string term_text(gp_tree_node *node)
{
    if (!node || node->type != GP_TERM) return "";
    TokenBase *tb = (TokenBase *)node->val.term.attr;
    if (!tb) return "";
    TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
    return ti ? ti->str : "";
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

// -----------------------------------------------------------------------
// SemaCollector — walks the AST and populates SemaInfo
// -----------------------------------------------------------------------

class SemaCollector
{
    SemaInfo &info;

    // ---------------------------------------------------------------
    // Type classification — matches the emitter's classify_type()
    // ---------------------------------------------------------------

    static TypeClass classify_type_str(const string &type)
    {
	if (type.find("char") != string::npos) {
	    if (type.find("*") != string::npos) return TC_STRING;
	    return TC_CHAR;
	}
	if (type.find("const char") != string::npos) return TC_STRING;
	if (type.find("float") != string::npos) return TC_DOUBLE;
	if (type.find("double") != string::npos) return TC_DOUBLE;
	return TC_INT;
    }

    // Build a type string from a declaration_specifiers (qual chain)
    string type_string(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return "";

	if (node->type == GP_TERM) {
	    int code = term_code(node);
	    switch (code) {
	    case GT_VOID:     return "void";
	    case GT_BOOL:     return "int";
	    case GT_CHAR:     return "char";
	    case GT_SHORT:    return "short";
	    case GT_INT:      return "int";
	    case GT_LONG:     return "long";
	    case GT_FLOAT:    return "float";
	    case GT_DOUBLE:   return "double";
	    case GT_STRING_T: return "const char *";
	    case GT_INT8:     return "int8_t";
	    case GT_INT16:    return "int16_t";
	    case GT_INT32:    return "int32_t";
	    case GT_INT64:    return "int64_t";
	    case GT_UINT8:    return "uint8_t";
	    case GT_UINT16:   return "uint16_t";
	    case GT_UINT32:   return "uint32_t";
	    case GT_UINT64:   return "uint64_t";
	    case GT_SIGNED:   return "signed";
	    case GT_UNSIGNED: return "unsigned";
	    case GT_TYPEDEF:  return "typedef";
	    case GT_EXTERN:   return "extern";
	    case GT_STATIC:   return "static";
	    case GT_CONST:    return "const";
	    case GT_IDENT:    return term_text(node);
	    default:          return term_text(node);
	    }
	}

	if (node->type != GP_ANODE) return "";

	int an = an_code(node);

	// qual(specifier, rest) — recursive chain
	if (an == AN_QUAL) {
	    string spec = type_string(child(node, 0));
	    gp_tree_node *rest = child(node, 1);
	    if (is_nil(rest)) return spec;
	    string r = type_string(rest);
	    if (spec.empty()) return r;
	    if (r.empty()) return spec;
	    return spec + " " + r;
	}

	// struct_ref(struct/union, name)
	if (an == AN_STRUCT_REF)
	    return "struct " + term_text(child(node, 1));

	// struct_def — just the struct name
	if (an == AN_STRUCT_DEF) {
	    gp_tree_node *sname = child(node, 1);
	    if (!is_nil(sname))
		return "struct " + term_text(sname);
	    return "struct";
	}

	// enum_ref
	if (an == AN_ENUM_REF)
	    return "int";  // enums are ints in C

	// enum_def
	if (an == AN_ENUM_DEF)
	    return "int";

	// type_name(specs, abstract_declarator)
	if (an == AN_TYPE_NAME)
	    return type_string(child(node, 0));

	// Container/stream types → void* for now
	if (an == AN_VECTOR_TYPE || an == AN_SET_TYPE ||
	    an == AN_LIST_TYPE || an == AN_MAP_TYPE)
	    return "void *";

	return "";
    }

    TypeClass classify_specs(gp_tree_node *specs)
    {
	return classify_type_str(type_string(specs));
    }

    // ---------------------------------------------------------------
    // Name extraction from declarators
    // ---------------------------------------------------------------

    string extract_name(gp_tree_node *node)
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
	if (an == AN_INIT_DECL)
	    return extract_name(child(node, 0));
	if (nchildren(node) > 0)
	    return extract_name(child(node, 0));
	return "";
    }

    // Check if declarator has a pointer (ptr_decl at any level)
    bool has_pointer(gp_tree_node *node)
    {
	if (!node || node->type != GP_ANODE) return false;
	return is_an(node, AN_PTR_DECL);
    }

    // Check if declarator is a function declarator
    bool is_func_decl(gp_tree_node *node)
    {
	if (!node) return false;
	if (is_an(node, AN_FUNC_DECL)) return true;
	if (is_an(node, AN_PTR_DECL))
	    return is_func_decl(child(node, 1));
	return false;
    }

    // ---------------------------------------------------------------
    // Declaration walking
    // ---------------------------------------------------------------

    void collect_decl_vars(const string &type_str, TypeClass tc,
			   gp_tree_node *decls, bool is_global)
    {
	if (is_nil(decls)) return;

	string vname = extract_name(decls);
	if (!vname.empty()) {
	    // Check if this is a class type
	    string clean_type = type_str;
	    if (clean_type.substr(0, 7) == "struct ")
		clean_type = clean_type.substr(7);

	    if (info.class_names.count(clean_type)) {
		info.var_class_map[vname] = clean_type;
		info.var_types[vname] = TC_CLASS;
	    } else {
		TypeClass effective_tc = tc;
		if (has_pointer(decls) && effective_tc == TC_CHAR)
		    effective_tc = TC_STRING;  // char * → string
		info.var_types[vname] = effective_tc;
	    }
	}

	// Recurse into decl_list and init_decl
	int an = an_code(decls);
	if (an == AN_DECL_LIST) {
	    collect_decl_vars(type_str, tc, child(decls, 0), is_global);
	    collect_decl_vars(type_str, tc, child(decls, 1), is_global);
	} else if (an == AN_INIT_DECL) {
	    collect_decl_vars(type_str, tc, child(decls, 0), is_global);
	}
    }

    void walk_decl(gp_tree_node *node, bool is_global)
    {
	// decl(declaration_specifiers, init_declarator_list_opt)
	gp_tree_node *specs = child(node, 0);
	gp_tree_node *decls = child(node, 1);

	string type_str = type_string(specs);

	// Check for typedef
	if (type_str.find("typedef") != string::npos) {
	    string alias = extract_name(decls);
	    if (!alias.empty())
		info.typedef_names.insert(alias);
	    return;
	}

	// Check if this is a function prototype (func_decl in declarators)
	if (is_func_decl(decls)) {
	    string fname = extract_name(decls);
	    if (!fname.empty()) {
		TypeClass tc = classify_type_str(type_str);
		// Check if declarator adds a pointer
		if (has_pointer(decls) && tc == TC_CHAR)
		    tc = TC_STRING;  // char * return → string
		info.func_ret_types[fname] = tc;
		info.func_type_strs[fname] = type_str;
	    }
	    return;
	}

	// Regular variable declaration
	TypeClass tc = classify_type_str(type_str);
	collect_decl_vars(type_str, tc, decls, is_global);
    }

    // ---------------------------------------------------------------
    // Parameter walking — collect param types
    // ---------------------------------------------------------------

    void walk_params(gp_tree_node *node)
    {
	if (is_nil(node)) return;
	int an = an_code(node);

	if (an == AN_PARAM) {
	    string type_str = type_string(child(node, 0));
	    gp_tree_node *decl = child(node, 1);
	    if (!is_nil(decl)) {
		string pname = extract_name(decl);
		if (!pname.empty()) {
		    TypeClass tc = classify_type_str(type_str);
		    if (has_pointer(decl) && tc == TC_CHAR)
			tc = TC_STRING;
		    info.var_types[pname] = tc;
		}
	    }
	} else if (an == AN_PARAM_LIST) {
	    walk_params(child(node, 0));
	    walk_params(child(node, 1));
	} else if (an == AN_PARAM_VA) {
	    walk_params(child(node, 0));
	}
    }

    // ---------------------------------------------------------------
    // Function definition
    // ---------------------------------------------------------------

    void walk_func_def(gp_tree_node *node)
    {
	// func_def(declaration_specifiers, declarator, compound_statement)
	gp_tree_node *specs = child(node, 0);
	gp_tree_node *decl = child(node, 1);
	gp_tree_node *body = child(node, 2);

	string type_str = type_string(specs);
	string func_name = extract_name(decl);
	TypeClass tc = classify_type_str(type_str);

	// Check if declarator adds a pointer (e.g. char *func() → ptr_decl)
	if (has_pointer(decl) && tc == TC_CHAR)
	    tc = TC_STRING;  // char * return → string

	if (!func_name.empty()) {
	    info.func_ret_types[func_name] = tc;
	    info.func_type_strs[func_name] = type_str;
	}

	// Collect parameter types
	if (is_an(decl, AN_FUNC_DECL))
	    walk_params(child(decl, 1));
	else if (is_an(decl, AN_PTR_DECL) &&
		 is_an(child(decl, 1), AN_FUNC_DECL))
	    walk_params(child(child(decl, 1), 1));

	// Walk body for local declarations
	walk_block(body);
    }

    // ---------------------------------------------------------------
    // Class definition
    // ---------------------------------------------------------------

    void walk_class_def(gp_tree_node *node)
    {
	string class_name;
	gp_tree_node *body_node = nullptr;
	string base_class;

	if (is_an(node, AN_CLASS_DEF)) {
	    class_name = term_text(child(node, 0));
	    body_node = child(node, 1);
	} else if (is_an(node, AN_CLASS_INHERIT)) {
	    class_name = term_text(child(node, 0));
	    base_class = term_text(child(node, 2));
	    body_node = child(node, 3);
	}

	if (class_name.empty()) return;

	info.class_names.insert(class_name);
	if (!base_class.empty())
	    info.class_bases[class_name] = base_class;

	// Collect fields and methods from class body
	SemaClassInfo &ci = info.class_info[class_name];
	ci.name = class_name;

	// If we have a base class, copy its fields (for member access)
	// but NOT methods (those resolve through the base chain)
	if (!base_class.empty()) {
	    auto bit = info.class_info.find(base_class);
	    if (bit != info.class_info.end()) {
		ci.fields = bit->second.fields;
		ci.field_types = bit->second.field_types;
	    }
	}

	walk_class_body(body_node, class_name, ci);
    }

    void walk_class_body(gp_tree_node *node, const string &class_name,
			 SemaClassInfo &ci)
    {
	if (is_nil(node)) return;
	if (is_an(node, AN_CLASS_BODY)) {
	    walk_class_body(child(node, 0), class_name, ci);
	    walk_class_member(child(node, 1), class_name, ci);
	    return;
	}
	walk_class_member(node, class_name, ci);
    }

    void walk_class_member(gp_tree_node *node, const string &class_name,
			   SemaClassInfo &ci)
    {
	if (!node) return;
	int an = an_code(node);

	// Field declaration
	if (an == AN_DECL) {
	    string type_str = type_string(child(node, 0));
	    gp_tree_node *decls = child(node, 1);
	    collect_class_fields(decls, type_str, ci);
	    return;
	}

	// Method definition
	if (an == AN_METHOD) {
	    string ret_type = type_string(child(node, 0));
	    gp_tree_node *decl = child(node, 1);
	    string method_name = extract_name(decl);
	    if (!method_name.empty()) {
		ci.methods.insert(method_name);
		TypeClass tc = classify_type_str(ret_type);
		ci.method_ret_types[method_name] = tc;
		// Register mangled name in func_ret_types
		string mangled = class_name + "__" + method_name;
		info.func_ret_types[mangled] = tc;
		info.func_type_strs[mangled] = ret_type;
	    }
	    // Walk method body for local var types
	    walk_block(child(node, 2));
	    return;
	}

	// Method prototype (no body)
	if (an == AN_METHOD_PROTO) {
	    string ret_type = type_string(child(node, 0));
	    gp_tree_node *decl = child(node, 1);
	    string method_name = extract_name(decl);
	    if (!method_name.empty()) {
		ci.methods.insert(method_name);
		TypeClass tc = classify_type_str(ret_type);
		ci.method_ret_types[method_name] = tc;
		string mangled = class_name + "__" + method_name;
		info.func_ret_types[mangled] = tc;
	    }
	    return;
	}

	// Constructor
	if (an == AN_CTOR) {
	    info.classes_with_ctor.insert(class_name);
	    ci.has_ctor = true;
	    // Walk body for local var types
	    walk_block(child(node, 2));
	    return;
	}

	// Destructor
	if (an == AN_DTOR) {
	    info.classes_with_dtor.insert(class_name);
	    ci.has_dtor = true;
	    // Walk body for local var types
	    walk_block(child(node, 1));
	    return;
	}

	// Operator method
	if (an == AN_OPER_METHOD) {
	    // Walk body for local var types
	    walk_block(child(node, 3));
	    return;
	}

	// Access specifier — skip
    }

    void collect_class_fields(gp_tree_node *node, const string &type_str,
			      SemaClassInfo &ci)
    {
	if (is_nil(node)) return;

	string fname = extract_name(node);
	if (!fname.empty()) {
	    TypeClass tc = classify_type_str(type_str);
	    if (has_pointer(node) && tc == TC_CHAR)
		tc = TC_STRING;
	    ci.fields.insert(fname);
	    ci.field_types[fname] = tc;
	}

	// Handle decl_list
	int an = an_code(node);
	if (an == AN_DECL_LIST) {
	    collect_class_fields(child(node, 0), type_str, ci);
	    collect_class_fields(child(node, 1), type_str, ci);
	} else if (an == AN_INIT_DECL) {
	    collect_class_fields(child(node, 0), type_str, ci);
	}
    }

    // ---------------------------------------------------------------
    // Struct definition
    // ---------------------------------------------------------------

    void walk_struct_def(gp_tree_node *node)
    {
	gp_tree_node *sname = child(node, 1);
	if (!is_nil(sname)) {
	    string name = term_text(sname);
	    if (!name.empty())
		info.struct_names.insert(name);
	}
	// Walk struct body for field info (future use)
    }

    // ---------------------------------------------------------------
    // Enum definition — register enum values as int constants
    // ---------------------------------------------------------------

    void walk_enum_def(gp_tree_node *node)
    {
	// enum_def(name, enumerator_list)
	gp_tree_node *elist = child(node, 1);
	walk_enum_list(elist);
    }

    void walk_enum_list(gp_tree_node *node)
    {
	if (is_nil(node)) return;
	if (is_an(node, AN_ENUM_LIST)) {
	    walk_enum_list(child(node, 0));
	    walk_enum_val(child(node, 1));
	    return;
	}
	walk_enum_val(node);
    }

    void walk_enum_val(gp_tree_node *node)
    {
	if (!node) return;
	string vname;
	if (node->type == GP_TERM && term_code(node) == GT_IDENT)
	    vname = term_text(node);
	else if (is_an(node, AN_ENUM_ASSIGN))
	    vname = term_text(child(node, 0));
	if (!vname.empty())
	    info.var_types[vname] = TC_INT;  // enum values are ints
    }

    // ---------------------------------------------------------------
    // Typedef
    // ---------------------------------------------------------------

    void walk_typedef(gp_tree_node *node)
    {
	// The decl has "typedef" in its specifiers. Extract the alias name.
	gp_tree_node *decls = child(node, 1);
	string alias = extract_name(decls);
	if (!alias.empty())
	    info.typedef_names.insert(alias);
    }

    // ---------------------------------------------------------------
    // Block / statement walking — collect local variable types
    // ---------------------------------------------------------------

    void walk_block(gp_tree_node *node)
    {
	if (is_nil(node)) return;
	if (is_an(node, AN_BLOCK)) {
	    walk_stmt_list(child(node, 0));
	    return;
	}
	walk_stmt(node);
    }

    void walk_stmt_list(gp_tree_node *node)
    {
	if (is_nil(node)) return;
	if (is_an(node, AN_STMT_LIST)) {
	    walk_stmt_list(child(node, 0));
	    walk_stmt(child(node, 1));
	    return;
	}
	walk_stmt(node);
    }

    void walk_stmt(gp_tree_node *node)
    {
	if (is_nil(node)) return;
	int an = an_code(node);

	if (an == AN_DECL) {
	    walk_decl(node, false);
	    return;
	}
	if (an == AN_CTOR_DECL) {
	    // ctor_decl(type, name, args) — constructor call declaration
	    string type_str = type_string(child(node, 0));
	    string vname = term_text(child(node, 1));
	    string clean_type = type_str;
	    if (clean_type.substr(0, 7) == "struct ")
		clean_type = clean_type.substr(7);
	    if (!vname.empty()) {
		if (info.class_names.count(clean_type)) {
		    info.var_class_map[vname] = clean_type;
		    info.var_types[vname] = TC_INT;
		} else {
		    info.var_types[vname] = classify_type_str(type_str);
		}
	    }
	    return;
	}
	if (an == AN_BLOCK) {
	    walk_block(node);
	    return;
	}
	if (an == AN_IF) {
	    walk_stmt(child(node, 1));
	    return;
	}
	if (an == AN_IF_ELSE) {
	    walk_stmt(child(node, 1));
	    walk_stmt(child(node, 2));
	    return;
	}
	if (an == AN_WHILE) {
	    walk_stmt(child(node, 1));
	    return;
	}
	if (an == AN_DO_WHILE) {
	    walk_stmt(child(node, 0));
	    return;
	}
	if (an == AN_FOR) {
	    walk_stmt(child(node, 0));  // init may declare vars
	    walk_stmt(child(node, 3));  // body
	    return;
	}
	if (an == AN_FOR_DECL) {
	    walk_decl(child(node, 0), false);  // declaration in for-init
	    walk_stmt(child(node, 3));  // body
	    return;
	}
	if (an == AN_SWITCH) {
	    walk_stmt(child(node, 1));
	    return;
	}
	if (an == AN_TRY) {
	    walk_stmt(child(node, 0));
	    walk_stmt(child(node, 1));
	    return;
	}
	if (an == AN_CATCH) {
	    // catch(type, declarator, body)
	    string type_str = type_string(child(node, 0));
	    gp_tree_node *decl = child(node, 1);
	    string vname = extract_name(decl);
	    if (!vname.empty())
		info.var_types[vname] = classify_type_str(type_str);
	    walk_stmt(child(node, 2));
	    return;
	}
	if (an == AN_CATCH_ALL) {
	    walk_stmt(child(node, 0));
	    return;
	}
	if (an == AN_CATCH_LIST) {
	    walk_stmt(child(node, 0));
	    walk_stmt(child(node, 1));
	    return;
	}
	if (an == AN_STMT_LIST) {
	    walk_stmt_list(node);
	    return;
	}
	if (an == AN_LABEL) {
	    walk_stmt(child(node, 1));
	    return;
	}
	if (an == AN_CASE) {
	    walk_stmt(child(node, 1));
	    return;
	}
	if (an == AN_CASE_RANGE) {
	    walk_stmt(child(node, 2));
	    return;
	}
	if (an == AN_DEFAULT) {
	    walk_stmt(child(node, 0));
	    return;
	}
    }

    // ---------------------------------------------------------------
    // Top-level dispatch
    // ---------------------------------------------------------------

    void walk(gp_tree_node *node)
    {
	if (!node) return;

	// Handle GLR alternatives — use first
	if (node->type == GP_ALT) {
	    walk(node->val.alt.first);
	    return;
	}
	if (node->type == GP_OPT) {
	    walk(node->val.opt.first);
	    return;
	}
	if (is_nil(node)) return;
	if (node->type == GP_TERM) return;

	int an = an_code(node);

	// Translation unit — walk children
	if (an == AN_TU) {
	    for (int i = 0; i < nchildren(node); ++i)
		walk(child(node, i));
	    return;
	}

	if (an == AN_FUNC_DEF) {
	    walk_func_def(node);
	    return;
	}

	if (an == AN_DECL) {
	    walk_decl(node, true);
	    return;
	}

	if (an == AN_CLASS_DEF || an == AN_CLASS_INHERIT) {
	    walk_class_def(node);
	    return;
	}

	if (an == AN_STRUCT_DEF) {
	    walk_struct_def(node);
	    return;
	}

	if (an == AN_ENUM_DEF) {
	    walk_enum_def(node);
	    return;
	}

	// Pass-through: single child nodes
	if (node->type == GP_ANODE && nchildren(node) == 1) {
	    walk(child(node, 0));
	    return;
	}

	// Skip: using_ns, namespace_def, etc.
    }

public:
    SemaCollector(SemaInfo &si) : info(si) {}

    void collect(gp_tree_node *root)
    {
	walk(root);
    }
};

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

SemaInfo *madc_sema_collect(gp_tree_node *root)
{
    if (!root) return nullptr;

    SemaInfo *info = new SemaInfo();
    SemaCollector collector(*info);
    collector.collect(root);
    return info;
}

void madc_sema_free(SemaInfo *info)
{
    delete info;
}
