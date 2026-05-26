// madc_sema.cpp -- Semantic analysis walker for Gecko AST
//
// Phase 2 of the Gecko pipeline.  Walks the gp_tree_node AST produced
// by the Gecko GLR parser and populates madc's existing data structures
// (DataDef, Variable, FuncDef, scope chains) so the C emitter (Phase 3)
// has everything it needs.
//
// This file does NOT emit code.  It only builds symbol tables.

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
#include <cstring>
#include <asmjit/x86.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

extern "C" {
#include "gecko.h"
}

using namespace std;
using namespace asmjit;

// -----------------------------------------------------------------------
// Terminal codes — must match GeckoTermCode in madc_grammar.cpp
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
};

// -----------------------------------------------------------------------
// Helpers — access Gecko tree nodes
// -----------------------------------------------------------------------

// Get the string text from a terminal node (IDENT, STRING, etc.)
static string term_text(gp_tree_node *node)
{
	if ( !node || node->type != GP_TERM )
		return "";
	TokenBase *tb = (TokenBase *)node->val.term.attr;
	if ( !tb )
		return "";
	TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
	if ( ti )
		return ti->str;
	// For keyword terminals that carry no TokenIdent, return empty
	return "";
}

// Get integer value from a terminal node
static int64_t term_ival(gp_tree_node *node)
{
	if ( !node || node->type != GP_TERM )
		return 0;
	TokenBase *tb = (TokenBase *)node->val.term.attr;
	return tb ? tb->ival() : 0;
}

// Get double value from a terminal node
static double GP_UNUSED term_dval(gp_tree_node *node)
{
	if ( !node || node->type != GP_TERM )
		return 0.0;
	TokenBase *tb = (TokenBase *)node->val.term.attr;
	return tb ? tb->dval() : 0.0;
}

// Get the TokenBase* from a terminal node
static TokenBase *term_token(gp_tree_node *node)
{
	if ( !node || node->type != GP_TERM )
		return nullptr;
	return (TokenBase *)node->val.term.attr;
}

// Get source location from a terminal node
static void term_loc(gp_tree_node *node, const char *&file, int &line, int &col)
{
	TokenBase *tb = term_token(node);
	if ( tb )
	{
		file = tb->file;
		line = tb->line;
		col  = tb->column;
	}
}

// Check if node is an anode with given name
static bool is_anode(gp_tree_node *node, const char *name)
{
	return node && node->type == GP_ANODE
	    && strcmp(node->val.anode.name, name) == 0;
}

// Check if node is a terminal
static bool is_term(gp_tree_node *node)
{
	return node && node->type == GP_TERM;
}

// Check if node is NIL
static bool is_nil(gp_tree_node *node)
{
	return !node || node->type == GP_NIL;
}

// Get terminal code
static int term_code(gp_tree_node *node)
{
	if ( !node || node->type != GP_TERM )
		return -1;
	return node->val.term.code;
}

// Get child N of an anode (NULL-safe)
static gp_tree_node *child(gp_tree_node *node, int n)
{
	if ( !node || node->type != GP_ANODE )
		return nullptr;
	if ( n >= node->val.anode.children_num )
		return nullptr;
	return node->val.anode.children[n];
}

// Number of children
static int nchildren(gp_tree_node *node)
{
	if ( !node || node->type != GP_ANODE )
		return 0;
	return node->val.anode.children_num;
}

// Get the anode name (empty string if not an anode)
static const char *anode_name(gp_tree_node *node)
{
	if ( !node || node->type != GP_ANODE )
		return "";
	return node->val.anode.name;
}

// -----------------------------------------------------------------------
// SemaScope — lexical scope for variables
// -----------------------------------------------------------------------

class SemaScope
{
public:
	SemaScope *parent;
	map<string, Variable *> locals;
	// The FuncDef being defined, if this is a function body scope
	FuncDef *func;

	SemaScope(SemaScope *p = nullptr)
		: parent(p), func(nullptr) {}

	~SemaScope() {}

	Variable *find_local(const string &name)
	{
		auto it = locals.find(name);
		if ( it != locals.end() )
			return it->second;
		return nullptr;
	}

	Variable *find(const string &name)
	{
		Variable *v = find_local(name);
		if ( v )
			return v;
		if ( parent )
			return parent->find(name);
		return nullptr;
	}

	void add(const string &name, Variable *v)
	{
		locals[name] = v;
	}
};

// -----------------------------------------------------------------------
// SemaWalker — the main semantic analysis walker
// -----------------------------------------------------------------------

class SemaWalker
{
	Program &pgm;
	SemaScope *current_scope;
	int errors;

	// Error reporting
	void sema_error(const string &msg, gp_tree_node *node = nullptr)
	{
		const char *file = nullptr;
		int line = 0, col = 0;

		// Try to extract location from the node or its first terminal child
		if ( node )
		{
			if ( is_term(node) )
			{
				term_loc(node, file, line, col);
			}
			else if ( node->type == GP_ANODE && nchildren(node) > 0 )
			{
				// Walk to find the first terminal for location
				gp_tree_node *c = child(node, 0);
				if ( is_term(c) )
					term_loc(c, file, line, col);
			}
		}

		pgm.add_diagnostic(Program::DiagnosticSeverity::error,
				   Program::DiagnosticPhase::parser,
				   msg, file, line, col);
		++errors;
	}

	void sema_warning(const string &msg, gp_tree_node *node = nullptr)
	{
		const char *file = nullptr;
		int line = 0, col = 0;
		if ( node && is_term(node) )
			term_loc(node, file, line, col);

		pgm.add_diagnostic(Program::DiagnosticSeverity::warning,
				   Program::DiagnosticPhase::parser,
				   msg, file, line, col);
	}

	// ---------------------------------------------------------------
	// Scope management
	// ---------------------------------------------------------------

	void push_scope()
	{
		SemaScope *s = new SemaScope(current_scope);
		current_scope = s;
	}

	void pop_scope()
	{
		if ( !current_scope )
			return;
		SemaScope *p = current_scope->parent;
		delete current_scope;
		current_scope = p;
	}

	// ---------------------------------------------------------------
	// Type resolution — map AST type nodes to DataDef*
	// ---------------------------------------------------------------

	DataDef *resolve_type(gp_tree_node *node)
	{
		if ( !node )
			return &ddVOID;

		// Terminal type keywords — direct mapping by code
		if ( is_term(node) )
		{
			switch ( term_code(node) )
			{
			case GT_VOID:     return &ddVOID;
			case GT_BOOL:     return &ddBOOL;
			case GT_CHAR:     return &ddCHAR;
			case GT_SHORT:    return &ddINT16;
			case GT_INT:      return &ddINT32;
			case GT_LONG:     return &ddINT64;
			case GT_FLOAT:    return &ddFLOAT;
			case GT_DOUBLE:   return &ddDOUBLE;
			case GT_STRING_T: return &ddSTRING;
			case GT_INT8:     return &ddINT8;
			case GT_INT16:    return &ddINT16;
			case GT_INT32:    return &ddINT32;
			case GT_INT64:    return &ddINT64;
			case GT_UINT8:    return &ddUINT8;
			case GT_UINT16:   return &ddUINT16;
			case GT_UINT32:   return &ddUINT32;
			case GT_UINT64:   return &ddUINT64;
			case GT_IDENT:
			{
				// User-defined type name — look up in datadef_map
				string name = term_text(node);
				auto it = pgm.datadef_map.find(name);
				if ( it != pgm.datadef_map.end() )
					return it->second;
				auto it2 = pgm.struct_map.find(name);
				if ( it2 != pgm.struct_map.end() )
					return it2->second;
				sema_error("unknown type '" + name + "'", node);
				return &ddVOID;
			}
			default:
				return &ddVOID;
			}
		}

		// NIL node means void / absent
		if ( is_nil(node) )
			return &ddVOID;

		// Anode type wrappers
		const char *nm = anode_name(node);

		if ( strcmp(nm, "ptr_type") == 0 )
		{
			DataDef *base = resolve_type(child(node, 0));
			// Look up or create a pointer-to-base type
			auto it = pgm.ptr_type_cache.find(base);
			if ( it != pgm.ptr_type_cache.end() )
				return it->second;
			DataDefPTR *pdd = new DataDefPTR(*base);
			pgm.ptr_type_cache[base] = pdd;
			return pdd;
		}

		if ( strcmp(nm, "ref_type") == 0 )
		{
			DataDef *base = resolve_type(child(node, 0));
			// Reference types — set reftype on a copy
			// For sema purposes, references resolve to the base type
			// with rtReference marker.  The emitter handles the rest.
			return base;
		}

		if ( strcmp(nm, "const_type") == 0 )
		{
			// const qualifier — for sema, resolve the inner type
			return resolve_type(child(node, 1));
		}

		if ( strcmp(nm, "qual_type") == 0 )
		{
			// Qualified type (signed/unsigned + inner type)
			gp_tree_node *qual = child(node, 0);
			gp_tree_node *inner = child(node, 1);
			DataDef *base = resolve_type(inner);

			if ( is_term(qual) && term_code(qual) == GT_UNSIGNED )
			{
				// Map signed -> unsigned variants
				if ( base == &ddCHAR  )  return &ddUINT8;
				if ( base == &ddINT8  )  return &ddUINT8;
				if ( base == &ddINT16 )  return &ddUINT16;
				if ( base == &ddINT32 )  return &ddUINT32;
				if ( base == &ddINT64 )  return &ddUINT64;
				// "unsigned" alone = unsigned int
				if ( base == &ddVOID  )  return &ddUINT32;
			}
			if ( is_term(qual) && term_code(qual) == GT_SIGNED )
			{
				// signed char -> int8, etc.
				if ( base == &ddUINT8  ) return &ddINT8;
				if ( base == &ddCHAR   ) return &ddINT8;
				if ( base == &ddUINT16 ) return &ddINT16;
				if ( base == &ddUINT32 ) return &ddINT32;
				if ( base == &ddUINT64 ) return &ddINT64;
				if ( base == &ddVOID   ) return &ddINT32;
			}
			return base;
		}

		if ( strcmp(nm, "struct_type") == 0 )
		{
			string name = term_text(child(node, 1));
			auto it = pgm.struct_map.find(name);
			if ( it != pgm.struct_map.end() )
				return it->second;
			auto it2 = pgm.datadef_map.find(name);
			if ( it2 != pgm.datadef_map.end() )
				return it2->second;
			sema_error("unknown struct '" + name + "'", child(node, 1));
			return &ddVOID;
		}

		if ( strcmp(nm, "class_type") == 0 )
		{
			string name = term_text(child(node, 1));
			auto it = pgm.datadef_map.find(name);
			if ( it != pgm.datadef_map.end() )
				return it->second;
			sema_error("unknown class '" + name + "'", child(node, 1));
			return &ddVOID;
		}

		// vector_type, map_type — stub for now
		if ( strcmp(nm, "vector_type") == 0 || strcmp(nm, "map_type") == 0 )
		{
			DBG(cerr << "sema: TODO container type '" << nm << "'" << endl);
			return &ddVOID;
		}

		sema_error(string("unrecognized type node '") + nm + "'", node);
		return &ddVOID;
	}

	// ---------------------------------------------------------------
	// Parameter list walking — returns a vector of (type, name) pairs
	// ---------------------------------------------------------------

	struct ParamInfo
	{
		DataDef *type;
		string name;
		bool is_ref;
		bool is_const;
	};

	void collect_params(gp_tree_node *node, vector<ParamInfo> &out)
	{
		if ( is_nil(node) )
			return;

		if ( is_anode(node, "param_list") )
		{
			collect_params(child(node, 0), out);
			collect_params(child(node, 2), out);
			return;
		}

		if ( is_anode(node, "param") )
		{
			ParamInfo pi;
			pi.is_ref = false;
			pi.is_const = false;

			gp_tree_node *type_node = child(node, 0);
			gp_tree_node *name_node = child(node, 1);

			// Check for reference type
			if ( is_anode(type_node, "ref_type") )
			{
				pi.is_ref = true;
				pi.type = resolve_type(child(type_node, 0));
			}
			else if ( is_anode(type_node, "const_type") )
			{
				pi.is_const = true;
				gp_tree_node *inner = child(type_node, 1);
				if ( is_anode(inner, "ref_type") )
				{
					pi.is_ref = true;
					pi.type = resolve_type(child(inner, 0));
				}
				else
				{
					pi.type = resolve_type(inner);
				}
			}
			else
			{
				pi.type = resolve_type(type_node);
			}

			pi.name = is_nil(name_node) ? "" : term_text(name_node);
			out.push_back(pi);
			return;
		}

		// Single terminal like "void" — void params
		if ( is_term(node) && term_code(node) == GT_VOID )
			return;

		DBG(cerr << "sema: unexpected param node type '" << anode_name(node) << "'" << endl);
	}

	// ---------------------------------------------------------------
	// Function definition
	// ---------------------------------------------------------------

	void walk_func_def(gp_tree_node *node)
	{
		// func_def: [0]=return_type, [1]=name, [2]=params_or_NIL, [3]=block
		gp_tree_node *ret_node   = child(node, 0);
		gp_tree_node *name_node  = child(node, 1);
		gp_tree_node *param_node = child(node, 2);
		gp_tree_node *body_node  = child(node, 3);

		DataDef *ret_type = resolve_type(ret_node);
		string func_name = term_text(name_node);

		if ( func_name.empty() )
		{
			sema_error("function definition with no name", name_node);
			return;
		}

		// Create FuncDef
		FuncDef *fd = new FuncDef(*ret_type);
		fd->name = func_name;

		// Parse parameters
		vector<ParamInfo> params;
		collect_params(param_node, params);

		for ( auto &pi : params )
		{
			fd->parameters.push_back(pi.type);
			fd->ref_params.push_back(pi.is_ref);
			fd->const_params.push_back(pi.is_const);
		}

		// Register in funcdef_map
		pgm.funcdef_map[func_name] = fd;

		DBG(cerr << "sema: registered function '" << func_name
		         << "' returning " << ret_type->name
		         << " with " << params.size() << " params" << endl);

		// Walk the body in a new scope
		push_scope();
		current_scope->func = fd;

		// Add parameters to scope
		for ( auto &pi : params )
		{
			if ( pi.name.empty() )
				continue;
			uint32_t flags = vfPARAM | vfLOCAL;
			Variable *v = new Variable(pi.name, *pi.type, 1, nullptr, false);
			v->flags = flags;
			current_scope->add(pi.name, v);
		}

		// Walk body statements
		walk_block(body_node);

		pop_scope();
	}

	// ---------------------------------------------------------------
	// Function prototype (forward declaration)
	// ---------------------------------------------------------------

	void walk_func_proto(gp_tree_node *node)
	{
		// func_proto: [0]=return_type, [1]=name, [2]=params
		gp_tree_node *ret_node   = child(node, 0);
		gp_tree_node *name_node  = child(node, 1);
		gp_tree_node *param_node = child(node, 2);

		DataDef *ret_type = resolve_type(ret_node);
		string func_name = term_text(name_node);

		if ( func_name.empty() )
			return;

		// Only register if not already defined
		if ( pgm.funcdef_map.find(func_name) != pgm.funcdef_map.end() )
			return;

		FuncDef *fd = new FuncDef(*ret_type);
		fd->name = func_name;

		vector<ParamInfo> params;
		collect_params(param_node, params);
		for ( auto &pi : params )
		{
			fd->parameters.push_back(pi.type);
			fd->ref_params.push_back(pi.is_ref);
			fd->const_params.push_back(pi.is_const);
		}

		pgm.funcdef_map[func_name] = fd;

		DBG(cerr << "sema: registered prototype '" << func_name << "'" << endl);
	}

	// ---------------------------------------------------------------
	// Struct definition
	// ---------------------------------------------------------------

	void walk_struct_def(gp_tree_node *node)
	{
		// struct_def: [1]=name, [3]=struct_body  (or [0]=name, [1]=struct_body)
		// The child indices depend on grammar rule expansion.
		// Try common patterns.
		string name;
		gp_tree_node *body = nullptr;

		// Find the name terminal and body anode
		for ( int i = 0; i < nchildren(node); ++i )
		{
			gp_tree_node *c = child(node, i);
			if ( is_term(c) && term_code(c) == GT_IDENT && name.empty() )
				name = term_text(c);
			if ( is_anode(c, "struct_body") )
				body = c;
		}

		if ( name.empty() )
		{
			sema_error("struct definition with no name", node);
			return;
		}

		// Create a DataDefSTRUCT
		DataDefSTRUCT *sdd = new DataDefSTRUCT(name, 0);

		// Walk struct body to collect fields
		if ( body )
			walk_struct_body(body, sdd);

		// Register in struct_map
		pgm.struct_map[name] = sdd;
		// Also register in datadef_map so "struct X" and just "X" both resolve
		pgm.datadef_map[name] = sdd;

		DBG(cerr << "sema: registered struct '" << name
		         << "' with " << sdd->members.size() << " members" << endl);
	}

	void walk_struct_body(gp_tree_node *node, DataDefSTRUCT *sdd)
	{
		if ( is_nil(node) )
			return;

		if ( is_anode(node, "struct_body") )
		{
			// struct_body: [0]=prev, [1]=member
			walk_struct_body(child(node, 0), sdd);
			walk_struct_field(child(node, 1), sdd);
			return;
		}

		// Single field
		walk_struct_field(node, sdd);
	}

	void walk_struct_field(gp_tree_node *node, DataDefSTRUCT *sdd)
	{
		if ( is_nil(node) )
			return;

		if ( is_anode(node, "struct_field") )
		{
			// struct_field: [0]=type, [1]=field_list
			DataDef *ftype = resolve_type(child(node, 0));
			walk_field_list(child(node, 1), sdd, ftype);
			return;
		}

		DBG(cerr << "sema: TODO struct member type '" << anode_name(node) << "'" << endl);
	}

	void walk_field_list(gp_tree_node *node, DataDefSTRUCT *sdd, DataDef *ftype)
	{
		if ( is_nil(node) )
			return;

		// Terminal IDENT — single field name
		if ( is_term(node) && term_code(node) == GT_IDENT )
		{
			string fname = term_text(node);
			sdd->members.push_back(make_pair(fname, ftype));
			sdd->member_counts.push_back(1);
			sdd->member_array_flags.push_back(false);
			sdd->member_offsets.push_back(0);
			sdd->member_bitfields.push_back(DataDefSTRUCT::BitFieldInfo());
			sdd->member_dims.push_back(vector<uint32_t>());
			sdd->member_count_exprs.push_back(nullptr);
			sdd->member_access.push_back(0);
			return;
		}

		if ( is_anode(node, "field_array") )
		{
			// field_array: [0]=name, [1]=size
			string fname = term_text(child(node, 0));
			int64_t count = term_ival(child(node, 1));
			if ( count <= 0 ) count = 1;

			sdd->members.push_back(make_pair(fname, ftype));
			sdd->member_counts.push_back((size_t)count);
			sdd->member_array_flags.push_back(true);
			sdd->member_offsets.push_back(0);
			sdd->member_bitfields.push_back(DataDefSTRUCT::BitFieldInfo());
			vector<uint32_t> dims;
			dims.push_back((uint32_t)count);
			sdd->member_dims.push_back(dims);
			sdd->member_count_exprs.push_back(nullptr);
			sdd->member_access.push_back(0);
			return;
		}

		if ( is_anode(node, "field_ptr") )
		{
			// field_ptr: [0]=name — field declared as pointer
			string fname = term_text(child(node, 0));
			// Create pointer-to-ftype
			DataDef *ptype = ftype;
			auto it = pgm.ptr_type_cache.find(ftype);
			if ( it != pgm.ptr_type_cache.end() )
				ptype = it->second;
			else
			{
				DataDefPTR *pdd = new DataDefPTR(*ftype);
				pgm.ptr_type_cache[ftype] = pdd;
				ptype = pdd;
			}

			sdd->members.push_back(make_pair(fname, ptype));
			sdd->member_counts.push_back(1);
			sdd->member_array_flags.push_back(false);
			sdd->member_offsets.push_back(0);
			sdd->member_bitfields.push_back(DataDefSTRUCT::BitFieldInfo());
			sdd->member_dims.push_back(vector<uint32_t>());
			sdd->member_count_exprs.push_back(nullptr);
			sdd->member_access.push_back(0);
			return;
		}

		if ( is_anode(node, "bitfield") )
		{
			// bitfield: [0]=name, [1]=width
			string fname = term_text(child(node, 0));
			int64_t width = term_ival(child(node, 1));

			DataDefSTRUCT::BitFieldInfo bfi;
			bfi.is_bitfield = true;
			bfi.bit_width = (size_t)width;

			sdd->members.push_back(make_pair(fname, ftype));
			sdd->member_counts.push_back(1);
			sdd->member_array_flags.push_back(false);
			sdd->member_offsets.push_back(0);
			sdd->member_bitfields.push_back(bfi);
			sdd->member_dims.push_back(vector<uint32_t>());
			sdd->member_count_exprs.push_back(nullptr);
			sdd->member_access.push_back(0);
			return;
		}

		// Could be a decl_list for multi-field declarations
		DBG(cerr << "sema: TODO field_list node '" << anode_name(node) << "'" << endl);
	}

	// ---------------------------------------------------------------
	// Enum definition
	// ---------------------------------------------------------------

	void walk_enum_def(gp_tree_node *node)
	{
		string name;
		gp_tree_node *body = nullptr;

		for ( int i = 0; i < nchildren(node); ++i )
		{
			gp_tree_node *c = child(node, i);
			if ( is_term(c) && term_code(c) == GT_IDENT && name.empty() )
				name = term_text(c);
			if ( is_anode(c, "enum_body") || is_anode(c, "enum_list") )
				body = c;
		}

		DBG(cerr << "sema: registered enum '" << name << "'" << endl);

		// Walk enum values and register them as integer constants
		int64_t next_val = 0;
		walk_enum_body(body, name, next_val);
	}

	void walk_enum_body(gp_tree_node *node, const string &enum_name, int64_t &next_val)
	{
		if ( is_nil(node) )
			return;

		if ( is_anode(node, "enum_list") )
		{
			walk_enum_body(child(node, 0), enum_name, next_val);
			walk_enum_body(child(node, 1), enum_name, next_val);
			return;
		}

		if ( is_anode(node, "enum_assign") )
		{
			// enum_assign: [0]=name, [1]=value_expr
			string vname = term_text(child(node, 0));
			// For now, try to get a constant integer value
			gp_tree_node *val_node = child(node, 1);
			if ( is_term(val_node) && term_code(val_node) == GT_INTEGER )
				next_val = term_ival(val_node);

			register_enum_value(vname, next_val);
			++next_val;
			return;
		}

		// Plain identifier (no = value)
		if ( is_term(node) && term_code(node) == GT_IDENT )
		{
			string vname = term_text(node);
			register_enum_value(vname, next_val);
			++next_val;
			return;
		}
	}

	void register_enum_value(const string &name, int64_t val)
	{
		if ( name.empty() )
			return;

		// Register as a constant integer variable
		Variable *v = new Variable(name, ddINT32, 1, nullptr, true);
		v->flags = vfCONSTANT;
		if ( v->data )
			*((int32_t *)v->data) = (int32_t)val;

		// Add to current scope and global scope
		if ( current_scope )
			current_scope->add(name, v);

		// Also register globally via the program's define_map as a constant
		pgm.define_map[name] = to_string(val);

		DBG(cerr << "sema: enum value '" << name << "' = " << val << endl);
	}

	// ---------------------------------------------------------------
	// Typedef
	// ---------------------------------------------------------------

	void walk_typedef(gp_tree_node *node)
	{
		// typedef: [1]=type, [2]=alias
		DataDef *base = resolve_type(child(node, 1));
		string alias = term_text(child(node, 2));

		if ( alias.empty() )
		{
			sema_error("typedef with no alias name", node);
			return;
		}

		pgm.datadef_map[alias] = base;

		DBG(cerr << "sema: typedef '" << alias << "' -> " << base->name << endl);
	}

	// ---------------------------------------------------------------
	// Class definition (stub)
	// ---------------------------------------------------------------

	void walk_class_def(gp_tree_node *node)
	{
		string name;
		for ( int i = 0; i < nchildren(node); ++i )
		{
			gp_tree_node *c = child(node, i);
			if ( is_term(c) && term_code(c) == GT_IDENT && name.empty() )
				name = term_text(c);
		}

		DBG(cerr << "sema: TODO class_def '" << name << "'" << endl);

		// Create a minimal DataDefCLASS for name resolution
		DataDefCLASS *cdd = new DataDefCLASS(name, 0, DataType::dtRESERVED);
		pgm.datadef_map[name] = cdd;
	}

	// ---------------------------------------------------------------
	// Variable declarations
	// ---------------------------------------------------------------

	void walk_decl(gp_tree_node *node)
	{
		// decl: [0]=type, [1]=declarator_list
		DataDef *base_type = resolve_type(child(node, 0));
		walk_declarator_list(child(node, 1), base_type);
	}

	void walk_declarator_list(gp_tree_node *node, DataDef *type)
	{
		if ( is_nil(node) )
			return;

		// decl_list: [0]=prev_list, [2]=declarator
		if ( is_anode(node, "decl_list") )
		{
			walk_declarator_list(child(node, 0), type);
			walk_declarator(child(node, 2), type);
			return;
		}

		// Single declarator
		walk_declarator(node, type);
	}

	Variable *walk_declarator(gp_tree_node *node, DataDef *type)
	{
		if ( is_nil(node) )
			return nullptr;

		// init_decl: [0]=name, [1]=initializer
		if ( is_anode(node, "init_decl") )
		{
			string vname = term_text(child(node, 0));
			if ( vname.empty() )
				return nullptr;

			Variable *v = new Variable(vname, *type, 1, nullptr, false);
			v->flags = vfLOCAL;
			if ( current_scope )
				current_scope->add(vname, v);

			// Walk initializer for type checking (future)
			// walk_expr(child(node, 1));

			DBG(cerr << "sema: decl '" << vname << "' : " << type->name << " (init)" << endl);
			return v;
		}

		// array_decl: [0]=name, [2]=size_expr
		if ( is_anode(node, "array_decl") )
		{
			string vname = term_text(child(node, 0));
			if ( vname.empty() )
				return nullptr;

			Variable *v = new Variable(vname, *type, 1, nullptr, false);
			v->flags = vfLOCAL | vfFIXEDARRAY;

			// Try to get array size as a constant
			gp_tree_node *size_node = child(node, 2);
			if ( is_term(size_node) && term_code(size_node) == GT_INTEGER )
			{
				int64_t sz = term_ival(size_node);
				if ( sz > 0 )
					v->dims.push_back((uint32_t)sz);
			}

			if ( current_scope )
				current_scope->add(vname, v);

			DBG(cerr << "sema: decl '" << vname << "' : " << type->name << "[]" << endl);
			return v;
		}

		// Plain IDENT — uninitialized variable
		if ( is_term(node) && term_code(node) == GT_IDENT )
		{
			string vname = term_text(node);
			if ( vname.empty() )
				return nullptr;

			Variable *v = new Variable(vname, *type, 1, nullptr, false);
			v->flags = vfLOCAL;
			if ( current_scope )
				current_scope->add(vname, v);

			DBG(cerr << "sema: decl '" << vname << "' : " << type->name << endl);
			return v;
		}

		DBG(cerr << "sema: unknown declarator node '" << anode_name(node) << "'" << endl);
		return nullptr;
	}

	// ---------------------------------------------------------------
	// Block / statement walking
	// ---------------------------------------------------------------

	void walk_block(gp_tree_node *node)
	{
		if ( is_nil(node) )
			return;

		if ( is_anode(node, "block") )
		{
			push_scope();
			walk_stmt_list(child(node, 0));
			pop_scope();
			return;
		}

		// Might be a single statement
		walk_stmt(node);
	}

	void walk_stmt_list(gp_tree_node *node)
	{
		if ( is_nil(node) )
			return;

		if ( is_anode(node, "stmt_list") )
		{
			walk_stmt_list(child(node, 0));
			walk_stmt(child(node, 1));
			return;
		}

		// Single statement
		walk_stmt(node);
	}

	void walk_stmt(gp_tree_node *node)
	{
		if ( is_nil(node) )
			return;

		if ( !node )
			return;

		const char *nm = anode_name(node);

		// Declarations inside function bodies
		if ( strcmp(nm, "decl") == 0 )
		{
			walk_decl(node);
			return;
		}

		// Expression statement
		if ( strcmp(nm, "expr_stmt") == 0 )
		{
			// Walk expression for type checking (future)
			// For now, just recurse for any nested declarations
			return;
		}

		// Control flow statements
		if ( strcmp(nm, "if") == 0 )
		{
			// if: [0]=condition, [1]=then_body
			walk_stmt(child(node, 1));
			return;
		}

		if ( strcmp(nm, "if_else") == 0 )
		{
			// if_else: [0]=condition, [1]=then_body, [2]=else_body
			walk_stmt(child(node, 1));
			walk_stmt(child(node, 2));
			return;
		}

		if ( strcmp(nm, "while") == 0 )
		{
			walk_stmt(child(node, 1));
			return;
		}

		if ( strcmp(nm, "do_while") == 0 )
		{
			walk_stmt(child(node, 0));
			return;
		}

		if ( strcmp(nm, "for") == 0 )
		{
			// for: [0]=init, [1]=condition, [2]=increment, [3]=body
			push_scope();
			walk_stmt(child(node, 0));  // init may declare vars
			walk_stmt(child(node, 3));  // body
			pop_scope();
			return;
		}

		if ( strcmp(nm, "switch") == 0 )
		{
			walk_stmt(child(node, 1));
			return;
		}

		if ( strcmp(nm, "block") == 0 )
		{
			walk_block(node);
			return;
		}

		if ( strcmp(nm, "try") == 0 )
		{
			walk_stmt(child(node, 0));  // try body
			walk_stmt(child(node, 1));  // catch list
			return;
		}

		if ( strcmp(nm, "catch") == 0 )
		{
			push_scope();
			// catch: [0]=type, [1]=varname, [2]=body
			DataDef *ctype = resolve_type(child(node, 0));
			string vname = term_text(child(node, 1));
			if ( !vname.empty() )
			{
				Variable *v = new Variable(vname, *ctype, 1, nullptr, false);
				v->flags = vfLOCAL;
				current_scope->add(vname, v);
			}
			walk_stmt(child(node, 2));
			pop_scope();
			return;
		}

		if ( strcmp(nm, "catch_all") == 0 )
		{
			walk_stmt(child(node, 0));
			return;
		}

		// Leaf statements that need no scope work
		if ( strcmp(nm, "return") == 0 || strcmp(nm, "return_val") == 0
		  || strcmp(nm, "return_multi") == 0
		  || strcmp(nm, "break") == 0 || strcmp(nm, "continue") == 0
		  || strcmp(nm, "goto") == 0 || strcmp(nm, "label") == 0
		  || strcmp(nm, "defer") == 0 || strcmp(nm, "delete") == 0
		  || strcmp(nm, "throw_expr") == 0 )
		{
			return;
		}

		// stmt_list at statement level (e.g., case body)
		if ( strcmp(nm, "stmt_list") == 0 )
		{
			walk_stmt_list(node);
			return;
		}

		// Unknown — may be an expression or a node we haven't handled yet
		DBG(cerr << "sema: unhandled stmt node '" << nm << "'" << endl);
	}

	// ---------------------------------------------------------------
	// Expression type inference (basic)
	// ---------------------------------------------------------------

	DataDef *infer_expr_type(gp_tree_node *node)
	{
		if ( !node )
			return &ddVOID;

		// Terminal literals
		if ( is_term(node) )
		{
			switch ( term_code(node) )
			{
			case GT_INTEGER:  return &ddINT32;
			case GT_REAL:     return &ddDOUBLE;
			case GT_STRING:   return &ddSTRING;
			case GT_CHAR_LIT: return &ddCHAR;
			case GT_IDENT:
			{
				string name = term_text(node);
				// Look up in scope chain
				if ( current_scope )
				{
					Variable *v = current_scope->find(name);
					if ( v )
						return v->type;
				}
				// Look up in global function map
				auto it = pgm.funcdef_map.find(name);
				if ( it != pgm.funcdef_map.end() )
					return it->second;
				return &ddINT32;  // unknown identifier defaults to int
			}
			default:
				return &ddVOID;
			}
		}

		if ( is_nil(node) )
			return &ddVOID;

		const char *nm = anode_name(node);

		// Arithmetic binary ops — use C promotion rules (simplified)
		if ( strcmp(nm, "add") == 0 || strcmp(nm, "sub") == 0
		  || strcmp(nm, "mul") == 0 || strcmp(nm, "div") == 0
		  || strcmp(nm, "mod") == 0 )
		{
			DataDef *lhs = infer_expr_type(child(node, 0));
			DataDef *rhs = infer_expr_type(child(node, 1));
			if ( lhs->is_real() || rhs->is_real() )
				return &ddDOUBLE;
			if ( lhs == &ddINT64 || rhs == &ddINT64 )
				return &ddINT64;
			return &ddINT32;
		}

		// Shift and bitwise ops — always integer
		if ( strcmp(nm, "bsl") == 0 || strcmp(nm, "bsr") == 0
		  || strcmp(nm, "bitand") == 0 || strcmp(nm, "bitor") == 0
		  || strcmp(nm, "bitxor") == 0 )
		{
			DataDef *lhs = infer_expr_type(child(node, 0));
			if ( lhs == &ddINT64 )
				return &ddINT64;
			return &ddINT32;
		}

		// Comparison ops — always int (boolean)
		if ( strcmp(nm, "eq") == 0 || strcmp(nm, "ne") == 0
		  || strcmp(nm, "lt") == 0 || strcmp(nm, "gt") == 0
		  || strcmp(nm, "le") == 0 || strcmp(nm, "ge") == 0
		  || strcmp(nm, "lor") == 0 || strcmp(nm, "land") == 0 )
		{
			return &ddINT32;
		}

		// Unary ops
		if ( strcmp(nm, "neg") == 0 || strcmp(nm, "bnot") == 0 )
			return infer_expr_type(child(node, 0));

		if ( strcmp(nm, "lnot") == 0 )
			return &ddINT32;

		if ( strcmp(nm, "pre_inc") == 0 || strcmp(nm, "pre_dec") == 0
		  || strcmp(nm, "post_inc") == 0 || strcmp(nm, "post_dec") == 0 )
			return infer_expr_type(child(node, 0));

		// Deref
		if ( strcmp(nm, "deref") == 0 )
		{
			DataDef *inner = infer_expr_type(child(node, 0));
			if ( inner->is_pointer() )
			{
				DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(inner);
				if ( pdd && pdd->base_type )
					return pdd->base_type;
			}
			return &ddINT32;
		}

		// Address-of
		if ( strcmp(nm, "addrof") == 0 )
		{
			DataDef *inner = infer_expr_type(child(node, 0));
			auto it = pgm.ptr_type_cache.find(inner);
			if ( it != pgm.ptr_type_cache.end() )
				return it->second;
			DataDefPTR *pdd = new DataDefPTR(*inner);
			pgm.ptr_type_cache[inner] = pdd;
			return pdd;
		}

		// Assignment — type of LHS
		if ( strcmp(nm, "assign") == 0 || strcmp(nm, "add_assign") == 0
		  || strcmp(nm, "sub_assign") == 0 || strcmp(nm, "mul_assign") == 0
		  || strcmp(nm, "div_assign") == 0 || strcmp(nm, "mod_assign") == 0 )
			return infer_expr_type(child(node, 0));

		// Cast
		if ( strcmp(nm, "cast") == 0 )
			return resolve_type(child(node, 0));

		// Ternary
		if ( strcmp(nm, "ternary") == 0 )
			return infer_expr_type(child(node, 1));

		// sizeof — always size_t / uint64
		if ( strcmp(nm, "sizeof_type") == 0 || strcmp(nm, "sizeof_expr") == 0 )
			return &ddUINT64;

		// Function call
		if ( strcmp(nm, "call") == 0 )
		{
			DataDef *fn = infer_expr_type(child(node, 0));
			if ( fn->is_function() )
				return &((FuncDef *)fn)->returns;
			return &ddINT32;
		}

		// Member access
		if ( strcmp(nm, "member") == 0 || strcmp(nm, "arrow_member") == 0 )
		{
			// TODO: resolve struct member type
			return &ddINT32;
		}

		// Subscript
		if ( strcmp(nm, "subscript") == 0 )
		{
			DataDef *base = infer_expr_type(child(node, 0));
			if ( base->is_pointer() )
			{
				DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(base);
				if ( pdd && pdd->base_type )
					return pdd->base_type;
			}
			return &ddINT32;
		}

		// Paren — transparent
		if ( strcmp(nm, "paren") == 0 )
			return infer_expr_type(child(node, 0));

		// Comma — type of right operand
		if ( strcmp(nm, "comma") == 0 )
			return infer_expr_type(child(node, 1));

		// String literal
		if ( strcmp(nm, "compound_lit") == 0 )
			return resolve_type(child(node, 0));

		// new
		if ( strcmp(nm, "new_ctor") == 0 || strcmp(nm, "new_plain") == 0 )
		{
			string tname = term_text(child(node, 0));
			auto it = pgm.datadef_map.find(tname);
			if ( it != pgm.datadef_map.end() )
			{
				DataDef *base = it->second;
				auto pit = pgm.ptr_type_cache.find(base);
				if ( pit != pgm.ptr_type_cache.end() )
					return pit->second;
				DataDefPTR *pdd = new DataDefPTR(*base);
				pgm.ptr_type_cache[base] = pdd;
				return pdd;
			}
			return &ddVOIDptr;
		}

		return &ddVOID;
	}

	// ---------------------------------------------------------------
	// Top-level dispatch
	// ---------------------------------------------------------------

	void walk(gp_tree_node *node)
	{
		if ( !node )
			return;

		// Handle alternative nodes (GLR ambiguities)
		if ( node->type == GP_ALT )
		{
			// Use the first alternative
			walk(node->val.alt.first);
			return;
		}

		if ( node->type == GP_OPT )
		{
			walk(node->val.opt.first);
			return;
		}

		// NIL — nothing to do
		if ( is_nil(node) )
			return;

		// Terminal at top level — skip
		if ( is_term(node) )
			return;

		const char *nm = anode_name(node);

		// Translation unit — walk all children
		if ( strcmp(nm, "tu") == 0 )
		{
			walk_tu(node);
			return;
		}

		if ( strcmp(nm, "func_def") == 0 )
		{
			walk_func_def(node);
			return;
		}

		if ( strcmp(nm, "func_proto") == 0 )
		{
			walk_func_proto(node);
			return;
		}

		if ( strcmp(nm, "struct_def") == 0 )
		{
			walk_struct_def(node);
			return;
		}

		if ( strcmp(nm, "class_def") == 0 || strcmp(nm, "class_inherit") == 0 )
		{
			walk_class_def(node);
			return;
		}

		if ( strcmp(nm, "enum_def") == 0 )
		{
			walk_enum_def(node);
			return;
		}

		if ( strcmp(nm, "typedef") == 0 )
		{
			walk_typedef(node);
			return;
		}

		if ( strcmp(nm, "decl") == 0 )
		{
			walk_decl(node);
			return;
		}

		if ( strcmp(nm, "using_ns") == 0 )
		{
			string ns = term_text(child(node, 2));
			DBG(cerr << "sema: using namespace '" << ns << "'" << endl);
			return;
		}

		// Statement-level nodes at top level (e.g., expression statements)
		if ( strcmp(nm, "expr_stmt") == 0 || strcmp(nm, "stmt_list") == 0 )
		{
			walk_stmt(node);
			return;
		}

		DBG(cerr << "sema: unhandled top-level node '" << nm << "'" << endl);
	}

	void walk_tu(gp_tree_node *node)
	{
		if ( is_nil(node) )
			return;

		if ( is_anode(node, "tu") )
		{
			for ( int i = 0; i < nchildren(node); ++i )
				walk(child(node, i));
			return;
		}

		// Single top-level item
		walk(node);
	}

public:
	SemaWalker(Program &p)
		: pgm(p), current_scope(nullptr), errors(0)
	{}

	~SemaWalker()
	{
		// Clean up any remaining scopes (shouldn't happen on success)
		while ( current_scope )
			pop_scope();
	}

	bool analyze(gp_tree_node *root)
	{
		errors = 0;

		// Create the global scope
		push_scope();

		// Walk the AST
		walk(root);

		// Pop global scope
		pop_scope();

		return errors == 0;
	}
};

// -----------------------------------------------------------------------
// Public API
// -----------------------------------------------------------------------

bool madc_sema_analyze(Program &pgm, gp_tree_node *root)
{
	if ( !root )
	{
		pgm.add_diagnostic(Program::DiagnosticSeverity::error,
				   Program::DiagnosticPhase::parser,
				   "sema: NULL AST root", nullptr, 0, 0);
		return false;
	}

	SemaWalker walker(pgm);
	return walker.analyze(root);
}
