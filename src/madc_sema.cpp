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

static bool is_anode(gp_tree_node *node, const char *name)
{
    return node && node->type == GP_ANODE &&
	   strcmp(node->val.anode.name, name) == 0;
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
    // Type classification — direct from terminal codes, O(1).
    // No string building, no string::find(). The terminal code
    // already encodes the type — just switch on it.
    // ---------------------------------------------------------------

    static TypeClass classify_terminal(int code)
    {
	switch (code) {
	case GT_CHAR:     return TC_CHAR;
	case GT_FLOAT:
	case GT_DOUBLE:   return TC_DOUBLE;
	case GT_STRING_T: return TC_STRING;
	case GT_VOID:     return TC_VOID;
	default:          return TC_INT;
	}
    }

    // Classify a declaration_specifiers (qual chain) node.
    // Walks the chain looking for the type-bearing terminal.
    // Storage-class and qualifier terminals are skipped.
    TypeClass classify_specs(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return TC_INT;

	if (node->type == GP_TERM) {
	    int code = term_code(node);
	    // Skip non-type terminals (qualifiers, storage class)
	    switch (code) {
	    case GT_CONST: case GT_STATIC: case GT_EXTERN:
	    case GT_TYPEDEF: case GT_SIGNED: case GT_UNSIGNED:
	    case 282: /*VOLATILE*/ case 283: /*REGISTER*/
	    case 284: /*INLINE*/ case 293: /*VIRTUAL*/
		return TC_INT;  // not the type — caller keeps looking
	    case GT_IDENT: {
		string name = term_text(node);
		if (info.class_names.count(name)) return TC_CLASS;
		return TC_INT;
	    }
	    default:
		return classify_terminal(code);
	    }
	}

	if (node->type != GP_ANODE) return TC_INT;
	const char *nm = anode_name(node);

	// qual(specifier, rest) — find the actual type terminal
	if (strcmp(nm, "qual") == 0) {
	    TypeClass left = classify_specs(child(node, 0));
	    if (left != TC_INT) return left;
	    return classify_specs(child(node, 1));
	}

	if (strcmp(nm, "struct_ref") == 0 || strcmp(nm, "struct_def") == 0)
	    return TC_INT;
	if (strcmp(nm, "enum_ref") == 0 || strcmp(nm, "enum_def") == 0)
	    return TC_INT;
	if (strcmp(nm, "type_name") == 0)
	    return classify_specs(child(node, 0));
	if (strcmp(nm, "vector_type") == 0 || strcmp(nm, "set_type") == 0 ||
	    strcmp(nm, "list_type") == 0 || strcmp(nm, "map_type") == 0)
	    return TC_PTR;

	return TC_INT;
    }

    // Check if a qual chain contains a typedef keyword
    bool has_typedef(gp_tree_node *node)
    {
	if (!node || node->type == GP_NIL) return false;
	if (node->type == GP_TERM) return term_code(node) == GT_TYPEDEF;
	if (is_anode(node, "qual"))
	    return has_typedef(child(node, 0)) || has_typedef(child(node, 1));
	return false;
    }

    // ---------------------------------------------------------------
    // Name extraction from declarators
    // ---------------------------------------------------------------

    string extract_name(gp_tree_node *node)
    {
	if (!node) return "";
	if (node->type == GP_TERM) return term_text(node);
	const char *nm = anode_name(node);
	if (strcmp(nm, "ptr_decl") == 0)
	    return extract_name(child(node, 1));
	if (strcmp(nm, "ref_decl") == 0)
	    return extract_name(child(node, 0));
	if (strcmp(nm, "func_decl") == 0)
	    return extract_name(child(node, 0));
	if (strcmp(nm, "array_decl") == 0)
	    return extract_name(child(node, 0));
	if (strcmp(nm, "init_decl") == 0)
	    return extract_name(child(node, 0));
	if (nchildren(node) > 0)
	    return extract_name(child(node, 0));
	return "";
    }

    // Check if declarator has a pointer (ptr_decl at any level)
    bool has_pointer(gp_tree_node *node)
    {
	if (!node || node->type != GP_ANODE) return false;
	return is_anode(node, "ptr_decl");
    }

    // Check if declarator is a function declarator
    bool is_func_decl(gp_tree_node *node)
    {
	if (!node) return false;
	if (is_anode(node, "func_decl")) return true;
	if (is_anode(node, "ptr_decl"))
	    return is_func_decl(child(node, 1));
	return false;
    }

    // ---------------------------------------------------------------
    // Declaration walking
    // ---------------------------------------------------------------

    // Extract class name from a specs node (for var→class mapping).
    // Only called when classify_specs returned TC_CLASS.
    string extract_class_name(gp_tree_node *specs)
    {
	if (!specs) return "";
	if (specs->type == GP_TERM && term_code(specs) == GT_IDENT)
	    return term_text(specs);
	if (is_anode(specs, "qual")) {
	    string left = extract_class_name(child(specs, 0));
	    if (!left.empty()) return left;
	    return extract_class_name(child(specs, 1));
	}
	return "";
    }

    void collect_decl_vars(gp_tree_node *specs, TypeClass tc,
			   gp_tree_node *decls)
    {
	if (is_nil(decls)) return;

	string vname = extract_name(decls);
	if (!vname.empty()) {
	    if (tc == TC_CLASS) {
		string cname = extract_class_name(specs);
		info.var_class_map[vname] = cname;
		info.var_types[vname] = TC_CLASS;
	    } else {
		TypeClass effective_tc = tc;
		if (has_pointer(decls) && effective_tc == TC_CHAR)
		    effective_tc = TC_STRING;
		info.var_types[vname] = effective_tc;
	    }
	}

	const char *nm = anode_name(decls);
	if (strcmp(nm, "decl_list") == 0) {
	    collect_decl_vars(specs, tc, child(decls, 0));
	    collect_decl_vars(specs, tc, child(decls, 1));
	} else if (strcmp(nm, "init_decl") == 0) {
	    collect_decl_vars(specs, tc, child(decls, 0));
	}
    }

    void walk_decl(gp_tree_node *node, bool is_global)
    {
	gp_tree_node *specs = child(node, 0);
	gp_tree_node *decls = child(node, 1);

	if (has_typedef(specs)) {
	    string alias = extract_name(decls);
	    if (!alias.empty())
		info.typedef_names.insert(alias);
	    return;
	}

	TypeClass tc = classify_specs(specs);

	if (is_func_decl(decls)) {
	    string fname = extract_name(decls);
	    if (!fname.empty()) {
		TypeClass ftc = tc;
		if (has_pointer(decls) && ftc == TC_CHAR)
		    ftc = TC_STRING;
		info.func_ret_types[fname] = ftc;
	    }
	    return;
	}

	collect_decl_vars(specs, tc, decls);
    }

    // ---------------------------------------------------------------
    // Parameter walking — collect param types
    // ---------------------------------------------------------------

    void walk_params(gp_tree_node *node)
    {
	if (is_nil(node)) return;
	const char *nm = anode_name(node);

	if (strcmp(nm, "param") == 0) {
	    gp_tree_node *pspecs = child(node, 0);
	    gp_tree_node *decl = child(node, 1);
	    if (!is_nil(decl)) {
		string pname = extract_name(decl);
		if (!pname.empty()) {
		    TypeClass tc = classify_specs(pspecs);
		    if (has_pointer(decl) && tc == TC_CHAR)
			tc = TC_STRING;
		    info.var_types[pname] = tc;
		}
	    }
	} else if (strcmp(nm, "param_list") == 0) {
	    walk_params(child(node, 0));
	    walk_params(child(node, 1));
	} else if (strcmp(nm, "param_va") == 0) {
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

	string func_name = extract_name(decl);
	TypeClass tc = classify_specs(specs);

	if (has_pointer(decl) && tc == TC_CHAR)
	    tc = TC_STRING;

	if (!func_name.empty())
	    info.func_ret_types[func_name] = tc;

	// Collect parameter types
	if (is_anode(decl, "func_decl"))
	    walk_params(child(decl, 1));
	else if (is_anode(decl, "ptr_decl") &&
		 is_anode(child(decl, 1), "func_decl"))
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

	if (is_anode(node, "class_def")) {
	    class_name = term_text(child(node, 0));
	    body_node = child(node, 1);
	} else if (is_anode(node, "class_inherit")) {
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
	if (is_anode(node, "class_body")) {
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
	const char *nm = anode_name(node);

	// Field declaration
	if (strcmp(nm, "decl") == 0) {
	    gp_tree_node *fspecs = child(node, 0);
	    gp_tree_node *decls = child(node, 1);
	    TypeClass ftc = classify_specs(fspecs);
	    collect_class_fields(decls, ftc, ci);
	    return;
	}

	// Method definition
	if (strcmp(nm, "method") == 0) {
	    gp_tree_node *mspecs = child(node, 0);
	    gp_tree_node *decl = child(node, 1);
	    string method_name = extract_name(decl);
	    if (!method_name.empty()) {
		ci.methods.insert(method_name);
		TypeClass tc = classify_specs(mspecs);
		if (has_pointer(decl) && tc == TC_CHAR) tc = TC_STRING;
		ci.method_ret_types[method_name] = tc;
		string mangled = class_name + "__" + method_name;
		info.func_ret_types[mangled] = tc;
	    }
	    walk_block(child(node, 2));
	    return;
	}

	// Method prototype (no body)
	if (strcmp(nm, "method_proto") == 0) {
	    gp_tree_node *mspecs = child(node, 0);
	    gp_tree_node *decl = child(node, 1);
	    string method_name = extract_name(decl);
	    if (!method_name.empty()) {
		ci.methods.insert(method_name);
		TypeClass tc = classify_specs(mspecs);
		if (has_pointer(decl) && tc == TC_CHAR) tc = TC_STRING;
		ci.method_ret_types[method_name] = tc;
		string mangled = class_name + "__" + method_name;
		info.func_ret_types[mangled] = tc;
	    }
	    return;
	}

	// Constructor
	if (strcmp(nm, "ctor") == 0) {
	    info.classes_with_ctor.insert(class_name);
	    ci.has_ctor = true;
	    // Walk body for local var types
	    walk_block(child(node, 2));
	    return;
	}

	// Destructor
	if (strcmp(nm, "dtor") == 0) {
	    info.classes_with_dtor.insert(class_name);
	    ci.has_dtor = true;
	    // Walk body for local var types
	    walk_block(child(node, 1));
	    return;
	}

	// Operator method
	if (strcmp(nm, "oper_method") == 0) {
	    // Walk body for local var types
	    walk_block(child(node, 3));
	    return;
	}

	// Access specifier — skip
    }

    void collect_class_fields(gp_tree_node *node, TypeClass tc,
			      SemaClassInfo &ci)
    {
	if (is_nil(node)) return;

	string fname = extract_name(node);
	if (!fname.empty()) {
	    TypeClass ftc = tc;
	    if (has_pointer(node) && ftc == TC_CHAR)
		ftc = TC_STRING;
	    ci.fields.insert(fname);
	    ci.field_types[fname] = ftc;
	}

	const char *nm = anode_name(node);
	if (strcmp(nm, "decl_list") == 0) {
	    collect_class_fields(child(node, 0), tc, ci);
	    collect_class_fields(child(node, 1), tc, ci);
	} else if (strcmp(nm, "init_decl") == 0) {
	    collect_class_fields(child(node, 0), tc, ci);
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
	if (is_anode(node, "enum_list")) {
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
	else if (is_anode(node, "enum_assign"))
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
	if (is_anode(node, "block")) {
	    walk_stmt_list(child(node, 0));
	    return;
	}
	walk_stmt(node);
    }

    void walk_stmt_list(gp_tree_node *node)
    {
	if (is_nil(node)) return;
	if (is_anode(node, "stmt_list")) {
	    walk_stmt_list(child(node, 0));
	    walk_stmt(child(node, 1));
	    return;
	}
	walk_stmt(node);
    }

    void walk_stmt(gp_tree_node *node)
    {
	if (is_nil(node)) return;
	const char *nm = anode_name(node);

	if (strcmp(nm, "decl") == 0) {
	    walk_decl(node, false);
	    return;
	}
	if (strcmp(nm, "ctor_decl") == 0) {
	    gp_tree_node *cspecs = child(node, 0);
	    string vname = term_text(child(node, 1));
	    TypeClass tc = classify_specs(cspecs);
	    if (!vname.empty()) {
		if (tc == TC_CLASS) {
		    info.var_class_map[vname] = extract_class_name(cspecs);
		    info.var_types[vname] = TC_CLASS;
		} else {
		    info.var_types[vname] = tc;
		}
	    }
	    return;
	}
	if (strcmp(nm, "block") == 0) {
	    walk_block(node);
	    return;
	}
	if (strcmp(nm, "if") == 0) {
	    walk_stmt(child(node, 1));
	    return;
	}
	if (strcmp(nm, "if_else") == 0) {
	    walk_stmt(child(node, 1));
	    walk_stmt(child(node, 2));
	    return;
	}
	if (strcmp(nm, "while") == 0) {
	    walk_stmt(child(node, 1));
	    return;
	}
	if (strcmp(nm, "do_while") == 0) {
	    walk_stmt(child(node, 0));
	    return;
	}
	if (strcmp(nm, "for") == 0) {
	    walk_stmt(child(node, 0));  // init may declare vars
	    walk_stmt(child(node, 3));  // body
	    return;
	}
	if (strcmp(nm, "for_decl") == 0) {
	    walk_decl(child(node, 0), false);  // declaration in for-init
	    walk_stmt(child(node, 3));  // body
	    return;
	}
	if (strcmp(nm, "switch") == 0) {
	    walk_stmt(child(node, 1));
	    return;
	}
	if (strcmp(nm, "try") == 0) {
	    walk_stmt(child(node, 0));
	    walk_stmt(child(node, 1));
	    return;
	}
	if (strcmp(nm, "catch") == 0) {
	    // catch(type, declarator, body)
	    gp_tree_node *cspecs = child(node, 0);
	    gp_tree_node *decl = child(node, 1);
	    string vname = extract_name(decl);
	    if (!vname.empty())
		info.var_types[vname] = classify_specs(cspecs);
	    walk_stmt(child(node, 2));
	    return;
	}
	if (strcmp(nm, "catch_all") == 0) {
	    walk_stmt(child(node, 0));
	    return;
	}
	if (strcmp(nm, "catch_list") == 0) {
	    walk_stmt(child(node, 0));
	    walk_stmt(child(node, 1));
	    return;
	}
	if (strcmp(nm, "stmt_list") == 0) {
	    walk_stmt_list(node);
	    return;
	}
	if (strcmp(nm, "label") == 0) {
	    walk_stmt(child(node, 1));
	    return;
	}
	if (strcmp(nm, "case") == 0) {
	    walk_stmt(child(node, 1));
	    return;
	}
	if (strcmp(nm, "case_range") == 0) {
	    walk_stmt(child(node, 2));
	    return;
	}
	if (strcmp(nm, "default") == 0) {
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

	const char *nm = anode_name(node);

	// Translation unit — walk children
	if (strcmp(nm, "tu") == 0) {
	    for (int i = 0; i < nchildren(node); ++i)
		walk(child(node, i));
	    return;
	}

	if (strcmp(nm, "func_def") == 0) {
	    walk_func_def(node);
	    return;
	}

	if (strcmp(nm, "decl") == 0) {
	    walk_decl(node, true);
	    return;
	}

	if (strcmp(nm, "class_def") == 0 || strcmp(nm, "class_inherit") == 0) {
	    walk_class_def(node);
	    return;
	}

	if (strcmp(nm, "struct_def") == 0) {
	    walk_struct_def(node);
	    return;
	}

	if (strcmp(nm, "enum_def") == 0) {
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
