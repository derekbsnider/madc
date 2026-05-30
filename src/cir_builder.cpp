/* cir_builder.cpp — CirBuilder implementation.
 *
 * Builds cir_node AST trees that are simultaneously:
 *   1. Valid c2mir node_t trees (passable to c2mir_compile_tree)
 *   2. madc AST trees with origin pointers, typedef names, etc.
 */

#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <iostream>
#include <sstream>
#include <fstream>
#include <stdint.h>
#include <dlfcn.h>
#include <cstdlib>
#include <typeinfo>
#include <cxxabi.h>


#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "cir_builder.h"

extern "C" {
#include "c2mir/c2mir_api.h"
}

extern thread_local bool madc_verbose;

// Derived source position: a node's position IS its origin token's position
// (the single source of truth). No absolute offset is stored on the node, so a
// future switch to relative token positions changes only the token layer.
const char *cir_node::src_file()   const { return origin ? origin->file   : NULL; }
int         cir_node::src_line()   const { return origin ? origin->line   : 0; }
int         cir_node::src_column() const { return origin ? origin->column : 0; }

// -----------------------------------------------------------------------
// CirBuilder core
// -----------------------------------------------------------------------

CirBuilder::CirBuilder(c2m_ctx_t c2m_ctx) : c2m(c2m_ctx), m_prog(NULL) {}
CirBuilder::~CirBuilder() {}

cir_node *CirBuilder::make(c2mir_node_code_t code, TokenBase *origin)
{
	cir_node *cn = arena.alloc();
	cn->base.code = (node_code_t)code;
	cn->base.uid = c2mir_next_uid(c2m);
	cn->base.attr = NULL;
	c2mir_init_node_ops((node_t)cn);

	cn->origin = origin;
	cn->datadef = NULL;
	cn->typedef_name = NULL;
	cn->src_lang = cslC;  // default; caller can override

	// Position is derived from origin (see cir_node::src_*); here we only feed
	// c2mir's own (absolute) position store so diagnostics work.
	if (origin)
		set_pos(cn, origin);

	return cn;
}

node_t CirBuilder::error_node(const char *reason, TokenBase *origin)
{
	cir_node *cn = make(N_IGNORE, origin);
	cn->error_msg = arena.intern(reason ? reason : "error");
	return cn->as_node();
}

// Human-readable description of a parse token, for internal-error diagnostics.
// madc is open source — there are no internals to hide — so name the concrete
// TokenBase subclass via RTTI (e.g. "TokenMember") plus the lexeme it carries,
// which is far more actionable for a contributor than raw enum ordinals.
static std::string describe_token(TokenBase *tb)
{
	if (!tb)
		return "<null token>";
	int status = 0;
	char *dem = abi::__cxa_demangle(typeid(*tb).name(), NULL, NULL, &status);
	std::string desc = (status == 0 && dem) ? dem : typeid(*tb).name();
	free(dem);
	// Append the lexeme for tokens that carry one (identifiers, keywords,
	// strings, literals all derive from TokenIdent) — the single most useful
	// disambiguator. Operators are already identified by their class name.
	if (TokenIdent *idt = dynamic_cast<TokenIdent *>(tb)) {
		if (!idt->str.empty()) desc += " '" + idt->str + "'";
	}
	return desc;
}

void CirBuilder::set_pos(cir_node *cn, const char *file, int line, int col)
{
	c2mir_pos_t pos = { file, line, col };
	c2mir_set_node_pos(c2m, cn->as_node(), pos);
}

void CirBuilder::set_pos(cir_node *cn, TokenBase *tb)
{
	if (tb)
		set_pos(cn, tb->file, tb->line, tb->column);
}

// -----------------------------------------------------------------------
// Leaf node builders
// -----------------------------------------------------------------------

node_t CirBuilder::id(const char *name, TokenBase *origin)
{
	cir_node *cn = make(N_ID, origin);
	const char *interned = c2mir_uniq_str(c2m, name, strlen(name) + 1);
	cn->base.u.s.s = interned;
	cn->base.u.s.len = strlen(name) + 1;
	return cn->as_node();
}

node_t CirBuilder::integer(long val, TokenBase *origin)
{
	// c2m types N_I as `int` (32-bit) and N_L as `long` (64-bit). A value
	// outside signed-32 range must be N_L, or c2m truncates+sign-extends it
	// to int — e.g. __builtin_bswap64(0x0123456789abcdef) lost its high word.
	bool fits_int = (val >= (long)INT32_MIN && val <= (long)INT32_MAX);
	cir_node *cn = make(fits_int ? N_I : N_L, origin);
	cn->base.u.l = val;
	return cn->as_node();
}

node_t CirBuilder::real(double val, TokenBase *origin)
{
	cir_node *cn = make(N_D, origin);
	cn->base.u.d = val;
	return cn->as_node();
}

node_t CirBuilder::ch(long val, TokenBase *origin)
{
	cir_node *cn = make(N_CH, origin);
	cn->base.u.ch = (c2mir_char)val;
	return cn->as_node();
}

node_t CirBuilder::str(const char *s, size_t len, TokenBase *origin)
{
	cir_node *cn = make(N_STR, origin);
	const char *interned = c2mir_uniq_str(c2m, s, len);
	cn->base.u.s.s = interned;
	cn->base.u.s.len = len;
	return cn->as_node();
}

node_t CirBuilder::ignore()
{
	cir_node *cn = make(N_IGNORE, NULL);
	return cn->as_node();
}

// -----------------------------------------------------------------------
// Composite node builders
// -----------------------------------------------------------------------

node_t CirBuilder::list()
{
	cir_node *cn = make(N_LIST, NULL);
	return cn->as_node();
}

node_t CirBuilder::simple(c2mir_node_code_t code, TokenBase *origin)
{
	cir_node *cn = make(code, origin);
	return cn->as_node();
}

node_t CirBuilder::append(node_t parent, node_t child)
{
	return c2mir_op_append(c2m, parent, child);
}

node_t CirBuilder::node1(c2mir_node_code_t code, node_t op1, TokenBase *origin)
{
	cir_node *cn = make(code, origin);
	node_t n = cn->as_node();
	c2mir_op_append(c2m, n, op1);
	return n;
}

node_t CirBuilder::node2(c2mir_node_code_t code, node_t op1, node_t op2, TokenBase *origin)
{
	cir_node *cn = make(code, origin);
	node_t n = cn->as_node();
	c2mir_op_append(c2m, n, op1);
	c2mir_op_append(c2m, n, op2);
	return n;
}

node_t CirBuilder::node3(c2mir_node_code_t code, node_t op1, node_t op2, node_t op3, TokenBase *origin)
{
	cir_node *cn = make(code, origin);
	node_t n = cn->as_node();
	c2mir_op_append(c2m, n, op1);
	c2mir_op_append(c2m, n, op2);
	c2mir_op_append(c2m, n, op3);
	return n;
}

node_t CirBuilder::node4(c2mir_node_code_t code, node_t op1, node_t op2, node_t op3, node_t op4, TokenBase *origin)
{
	cir_node *cn = make(code, origin);
	node_t n = cn->as_node();
	c2mir_op_append(c2m, n, op1);
	c2mir_op_append(c2m, n, op2);
	c2mir_op_append(c2m, n, op3);
	c2mir_op_append(c2m, n, op4);
	return n;
}

node_t CirBuilder::node5(c2mir_node_code_t code, node_t op1, node_t op2, node_t op3, node_t op4, node_t op5, TokenBase *origin)
{
	cir_node *cn = make(code, origin);
	node_t n = cn->as_node();
	c2mir_op_append(c2m, n, op1);
	c2mir_op_append(c2m, n, op2);
	c2mir_op_append(c2m, n, op3);
	c2mir_op_append(c2m, n, op4);
	c2mir_op_append(c2m, n, op5);
	return n;
}

// -----------------------------------------------------------------------
// Type builders
// -----------------------------------------------------------------------

// A user-defined class (`class Foo { ... };`) lowers to a plain C struct
// `struct Foo`, matching the Cfront C++->C model. It is btClass with the
// generic dtRESERVED rawtype — distinct from the builtin opaque-object
// "classes" (std::string, the streams) which carry their own dt* rawtype
// and are lowered as runtime buffers, not C structs. Returns the class
// DataDef when `dd` is such a user class, else NULL.
static DataDefCLASS *as_user_class(DataDef *dd)
{
	if (!dd) return NULL;
	if (dd->basetype() != BaseType::btClass) return NULL;
	if (dd->rawtype() != DataType::dtRESERVED) return NULL;
	return dynamic_cast<DataDefCLASS *>(dd);
}

node_t CirBuilder::pointer()
{
	return node1(N_POINTER, list());
}

// Append type specifier nodes for a DataDef into a LIST node.
void CirBuilder::append_type_specs(node_t lst, DataDef *dd)
{
	if (!dd) { append(lst, simple(N_INT)); return; }

	// C99 _Complex: emit base-type spec(s) followed by N_COMPLEX.
	// c2mir natively supports _Complex (spec list e.g. [N_DOUBLE, N_COMPLEX]).
	// DataDefCOMPLEX IS-A DataDefSTRUCT, so callers must route it here rather
	// than into the N_STRUCT path (which would emit a bogus `struct double _Complex`).
	if (dd->is_complex()) {
		append_type_specs(lst, ((DataDefCOMPLEX *)dd)->element_type);
		append(lst, simple(N_COMPLEX));
		return;
	}

	DataType dt = dd->rawtype();
	switch (dt) {
	case DataType::dtVOID:   append(lst, simple(N_VOID)); break;
	case DataType::dtCHAR:   append(lst, simple(N_CHAR)); break;
	case DataType::dtINT16:  append(lst, simple(N_SHORT)); break;
	case DataType::dtINT32:  append(lst, simple(N_INT)); break;
	case DataType::dtINT64:  append(lst, simple(N_LONG)); break;
	case DataType::dtUINT8:
		append(lst, simple(N_UNSIGNED));
		append(lst, simple(N_CHAR));
		break;
	case DataType::dtUINT16:
		append(lst, simple(N_UNSIGNED));
		append(lst, simple(N_SHORT));
		break;
	case DataType::dtUINT32:
		append(lst, simple(N_UNSIGNED));
		append(lst, simple(N_INT));
		break;
	case DataType::dtUINT64:
		append(lst, simple(N_UNSIGNED));
		append(lst, simple(N_LONG));
		break;
	case DataType::dtFLOAT:  append(lst, simple(N_FLOAT)); break;
	case DataType::dtDOUBLE: append(lst, simple(N_DOUBLE)); break;
	case DataType::dtSTRING:
		// A string OBJECT uses string_storage_decl, not this path. If a bare
		// string spec still reaches here (e.g. a struct member — Phase 3),
		// emit `char` so it degrades to a byte type, never a 32-bit-truncating
		// `int`. (Struct-member string objects are not yet lowered.)
		append(lst, simple(N_CHAR));
		break;
	default:
		append(lst, simple(N_INT));
	}
}

// ---------------------------------------------------------------------------
// std::string object lowering
// ---------------------------------------------------------------------------
// A madc `string` is a real std::string object, matching g++/clang++ (Rule #1):
// an 8-aligned opaque buffer + ctor/dtor calls to the runtime wrappers in
// madc_mir_backend.cpp (string_construct*/string_destruct/string_cstr). Only a
// declared string OBJECT becomes an object; a bare string literal stays a
// const char* (it never reaches here — TokenString emits str()).

bool CirBuilder::is_string_object(DataDef *dd)
{
	// dtSTRING is the value type `string`; dtSTRINGref (`string&`) and
	// dtSTRINGptr are separate and handled on the param/pointer paths.
	return dd && dd->is_string() && !dd->is_pointer();
}

bool CirBuilder::is_string_object_value(TokenBase *arg)
{
	// A string OBJECT value is a DECLARED `string` variable — an lvalue with
	// constructed storage. Many other expressions are typed dtSTRING but are
	// const char* values, NOT objects: string literals, lifted `__literal__`
	// vars, and char*-valued expressions like a ternary of literals
	// (`cond ? "a" : "b"`). So require a genuine variable token; everything
	// else flows through the const char* path (str()/char* operator<<).
	TokenVar *tv = dynamic_cast<TokenVar *>(arg);
	if (!tv)
		return false;
	// A declared `string` object (dtSTRING) OR a `string&` reference parameter
	// (dtSTRINGref) — both denote a std::string object the front end can take
	// the address of. (A by-value `string` param is also dtSTRING.)
	DataType dt = tv->var.type ? tv->var.type->rawtype() : DataType::dtVOID;
	if (dt != DataType::dtSTRING && dt != DataType::dtSTRINGref)
		return false;
	if (tv->var.name.compare(0, 11, "__literal__") == 0)
		return false;
	if (tv->var.is_constant())
		return false;
	return true;
}

size_t CirBuilder::string_obj_words() const
{
	// cir_builder is itself C++, so it knows the real ABI size/alignment of
	// std::string. A long[] buffer is naturally 8-aligned and >= the object
	// size, so placement-new of std::string into it is well-aligned without an
	// explicit alignment attribute.
	return (sizeof(std::string) + sizeof(long) - 1) / sizeof(long);
}

// N_TYPE node for a (void*) cast: TYPE(LIST(VOID), DECL(IGNORE, LIST(POINTER))).
// The spec is a plain LIST (NOT N_SHARE — that wraps SPEC_DECL specs only;
// matches the cast at translate_expr and the param TYPE in need_output_extern).
node_t CirBuilder::void_ptr_type()
{
	node_t spec = list();
	append(spec, simple(N_VOID));
	node_t decl_list = list();
	append(decl_list, pointer());
	return node2(N_TYPE, spec, node2(N_DECL, ignore(), decl_list));
}

// `long <name>[words];` — opaque, 8-aligned storage for a C++ runtime object,
// tagged with __attribute__((cleanup(dtor_sym))). This is the shared mechanism
// behind every monomorphic runtime object madc lowers to a buffer + ctor/dtor
// (std::string, MadArray, …): the madc c2mir fork calls dtor_sym(&name)
// automatically at EVERY scope exit (fall-through, return, break, continue,
// goto) — proper RAII without per-exit dtor injection in the front end. We
// build the N_ATTR node directly (no source preprocessing, so the glibc
// `__attribute__`-emptying issue doesn't apply).
node_t CirBuilder::obj_storage_decl(const char *name, size_t words,
				    const char *dtor_sym, TokenBase *origin)
{
	node_t spec = list();
	append(spec, simple(N_LONG, origin));
	node_t share = node1(N_SHARE, spec);
	node_t decl_list = list();
	append(decl_list, node3(N_ARR, ignore(), list(),
				integer((long)words, origin)));
	node_t decl = node2(N_DECL, id(name, origin), decl_list);
	need_output_extern(dtor_sym, false, { { {N_VOID}, true } });
	node_t attr_args = list();
	append(attr_args, id(dtor_sym, origin));
	node_t attrs = list();
	append(attrs, node2(N_ATTR, id("cleanup", origin), attr_args, origin));
	node_t sd = simple(N_SPEC_DECL, origin);
	append(sd, share);
	append(sd, decl);
	append(sd, attrs);      // __attribute__((cleanup(dtor_sym)))
	append(sd, ignore());   // asm
	append(sd, ignore());   // initializer — none; the ctor call does it
	CIR_NODE(sd)->synth_from_origin = true;
	return sd;
}

// `dtor_sym((void*)name)` as a block-item expression-statement — the default
// (no-argument) constructor call for a runtime object lowered by obj_storage_decl.
node_t CirBuilder::obj_default_ctor_call(const char *name, const char *ctor_sym,
					 TokenBase *origin)
{
	need_output_extern(ctor_sym, true, { { {N_VOID}, true } });
	node_t args = list();
	append(args, string_obj_addr(name, origin));
	node_t call = node2(N_CALL, id(ctor_sym, origin), args, origin);
	CIR_NODE(call)->synth_from_origin = true;
	node_t stmt = node2(N_EXPR, list(), call, origin);
	CIR_NODE(stmt)->synth_from_origin = true;
	return stmt;
}

// `long <name>[NWORDS];` — the opaque, 8-aligned storage for a string object.
node_t CirBuilder::string_storage_decl(const char *name, TokenBase *origin)
{
	return obj_storage_decl(name, string_obj_words(), "string_destruct", origin);
}

// ---- MadArray (`array`) object lowering ----
// A madc `array` is a real MadArray C++ object — same model as std::string:
// an 8-aligned opaque buffer + madarray_construct/madarray_destruct (the
// wrappers in madc_mir_backend.cpp). Unlike a string it needs no const char*
// coercion: an array argument is always passed by pointer, and the buffer's
// long[] name decays to that pointer naturally at the call site.
bool CirBuilder::is_array_object(DataDef *dd)
{
	return dd && dd->rawtype() == DataType::dtARRAY && !dd->is_pointer();
}

size_t CirBuilder::array_obj_words() const
{
	return (sizeof(MadArray) + sizeof(long) - 1) / sizeof(long);
}

node_t CirBuilder::array_storage_decl(const char *name, TokenBase *origin)
{
	return obj_storage_decl(name, array_obj_words(), "madarray_destruct", origin);
}

node_t CirBuilder::array_ctor_call(const char *name, TokenBase *origin)
{
	return obj_default_ctor_call(name, "madarray_construct", origin);
}

// ---- STL container (vector/map/set) object lowering ----
// A typed STL container (vector<T>/map<K,V>/set<T>) is a real C++ object, the
// same opaque-buffer model as std::string / MadArray: an 8-aligned long[]
// buffer + a placement-new ctor wrapper, destructed at every scope exit via
// __attribute__((cleanup(dtor))). The runtime wrapper family is monomorphic per
// (kind, element/value type): vector<int> -> vector_int_*, vector<string> ->
// vector_str_*, map<string,int> -> map_str_int_*, map<string,string> ->
// map_str_str_*, set<string> -> set_str_*, set<int> -> set_int_*. The container
// is always passed to its wrappers by address ((void*)&buffer).
bool CirBuilder::is_container_object(DataDef *dd)
{
	if (!dd || dd->is_pointer())
		return false;
	DataType dt = dd->rawtype();
	return dt == DataType::dtVECTOR || dt == DataType::dtMAP
	    || dt == DataType::dtSET;
}

bool CirBuilder::container_obj_info(DataDef *dd, const char *&ctor_sym,
				    const char *&dtor_sym, size_t &words)
{
	if (!is_container_object(dd))
		return false;
	if (DataDefVECTOR *v = dynamic_cast<DataDefVECTOR *>(dd)) {
		bool str = v->element_type && v->element_type->is_string();
		ctor_sym = str ? "vector_str_construct" : "vector_int_construct";
		dtor_sym = str ? "vector_str_destruct"  : "vector_int_destruct";
		words = (sizeof(std::vector<int64_t>) + sizeof(long) - 1) / sizeof(long);
		return true;
	}
	if (DataDefMAP *m = dynamic_cast<DataDefMAP *>(dd)) {
		bool str = m->val_type && m->val_type->is_string();
		ctor_sym = str ? "map_str_str_construct" : "map_str_int_construct";
		dtor_sym = str ? "map_str_str_destruct"  : "map_str_int_destruct";
		words = (sizeof(std::map<std::string, int64_t>) + sizeof(long) - 1) / sizeof(long);
		return true;
	}
	if (DataDefSET *s = dynamic_cast<DataDefSET *>(dd)) {
		bool str = s->element_type && s->element_type->is_string();
		ctor_sym = str ? "set_str_construct" : "set_int_construct";
		dtor_sym = str ? "set_str_destruct"  : "set_int_destruct";
		words = (sizeof(std::set<std::string>) + sizeof(long) - 1) / sizeof(long);
		return true;
	}
	return false;
}

node_t CirBuilder::container_storage_decl(const char *name, DataDef *dd,
					  TokenBase *origin)
{
	const char *ctor_sym, *dtor_sym;
	size_t words;
	if (!container_obj_info(dd, ctor_sym, dtor_sym, words))
		return error_node("container_storage_decl on non-container type", origin);
	return obj_storage_decl(name, words, dtor_sym, origin);
}

node_t CirBuilder::container_ctor_call(const char *name, DataDef *dd,
				       TokenBase *origin)
{
	const char *ctor_sym, *dtor_sym;
	size_t words;
	if (!container_obj_info(dd, ctor_sym, dtor_sym, words))
		return error_node("container_ctor_call on non-container type", origin);
	return obj_default_ctor_call(name, ctor_sym, origin);
}

// Lower a container method call on a container OBJECT receiver to its runtime
// wrapper. The receiver object's address is passed first as (void*). A string
// key/value/element argument is materialized to a std::string OBJECT via
// string_obj_arg (literal -> scope-temp, object -> by address); a string RESULT
// (vector<string>::at, map<string,string>::get) is filled into a scope-temp
// std::string and returned as a const char* via string_cstr (usable in cout /
// string-assign contexts). Returns NULL for unrecognized receivers/methods so
// the caller falls through to generic member/call handling.
node_t CirBuilder::container_method_call(TokenMember *tm, TokenBase *origin)
{
	const Variable &obj = tm->object;
	if (!is_container_object(obj.type))
		return NULL;

	const std::string &method = tm->var.name;
	node_t self = string_obj_addr(obj.name.c_str(), origin);   // (void*)&container

	// String result via a scope-temp std::string: the wrapper fills *result,
	// then we read it as const char*. Used for vector<string>::at and
	// map<string,string>::get.
	auto str_result = [&](const char *sym,
			      const std::vector<node_t> &tail_args,
			      const std::vector<ExternParam> &tail_params) -> node_t {
		char tname[40];
		snprintf(tname, sizeof(tname), "__madc_cstrtmp_%d", m_strtmp_counter++);
		m_pending_stmts.push_back(string_storage_decl(tname, origin));
		m_pending_stmts.push_back(string_ctor_call(tname, NULL, origin));
		// sym((void*)&tmp, (void*)&container, tail...)
		std::vector<ExternParam> params;
		params.push_back({ {N_VOID}, true });   // result
		params.push_back({ {N_VOID}, true });   // container
		for (auto &p : tail_params) params.push_back(p);
		need_output_extern(sym, true, params);
		node_t a = list();
		append(a, string_obj_addr(tname, origin));
		append(a, self);
		for (node_t t : tail_args) append(a, t);
		node_t call = node2(N_CALL, id(sym, origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		// read the temp as const char*
		need_output_extern("string_cstr", true, { { {N_VOID}, true } });
		// comma-sequence: (fill(...), string_cstr(&tmp))
		node_t cargs = list();
		append(cargs, string_obj_addr(tname, origin));
		node_t cstr = node2(N_CALL, id("string_cstr", origin), cargs, origin);
		CIR_NODE(cstr)->synth_from_origin = true;
		node_t seq = node2(N_COMMA, call, cstr, origin);
		CIR_NODE(seq)->synth_from_origin = true;
		return seq;
	};

	// Helper: build `sym((void*)&container [, extra...])` returning long/void.
	auto simple_call = [&](const char *sym, bool ret_long,
			       const std::vector<node_t> &extra,
			       const std::vector<ExternParam> &extra_params) -> node_t {
		std::vector<ExternParam> params;
		params.push_back({ {N_VOID}, true });
		for (auto &p : extra_params) params.push_back(p);
		need_output_extern(sym, false, params,
				   ret_long ? std::vector<c2mir_node_code_t>{N_LONG}
					    : std::vector<c2mir_node_code_t>());
		node_t a = list();
		append(a, self);
		for (node_t e : extra) append(a, e);
		node_t call = node2(N_CALL, id(sym, origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		return call;
	};

	if (DataDefVECTOR *vd = dynamic_cast<DataDefVECTOR *>(obj.type)) {
		bool str = vd->element_type && vd->element_type->is_string();
		TokenBase *arg0 = tm->parameters.empty() ? NULL : tm->parameters[0];
		if (method == "push_back" && arg0) {
			if (str) {
				node_t a = string_obj_arg(arg0);  // (void*)&str
				return simple_call("vector_str_push_back", false,
						   { a }, { { {N_VOID}, true } });
			}
			return simple_call("vector_int_push_back", false,
					   { translate_expr(arg0) }, { { {N_LONG}, false } });
		}
		if (method == "pop_back")
			return simple_call(str ? "vector_str_pop_back" : "vector_int_pop_back",
					   false, {}, {});
		if (method == "size")
			return simple_call(str ? "vector_str_size" : "vector_int_size",
					   true, {}, {});
		if (method == "empty")
			return simple_call(str ? "vector_str_empty" : "vector_int_empty",
					   true, {}, {});
		if (method == "clear")
			return simple_call(str ? "vector_str_clear" : "vector_int_clear",
					   false, {}, {});
		if (method == "at" && arg0) {
			if (str)
				return str_result("vector_str_at",
						  { translate_expr(arg0) },
						  { { {N_LONG}, false } });
			return simple_call("vector_int_at", true,
					   { translate_expr(arg0) }, { { {N_LONG}, false } });
		}
		return NULL;
	}

	if (DataDefMAP *md = dynamic_cast<DataDefMAP *>(obj.type)) {
		bool vstr = md->val_type && md->val_type->is_string();
		TokenBase *arg0 = tm->parameters.empty() ? NULL : tm->parameters[0];
		TokenBase *arg1 = tm->parameters.size() < 2 ? NULL : tm->parameters[1];
		if (method == "put" && arg0 && arg1) {
			node_t k = string_obj_arg(arg0);   // key is always a string
			if (vstr) {
				node_t v = string_obj_arg(arg1);
				return simple_call("map_str_str_set", false, { k, v },
						   { { {N_VOID}, true }, { {N_VOID}, true } });
			}
			return simple_call("map_str_int_set", false,
					   { k, translate_expr(arg1) },
					   { { {N_VOID}, true }, { {N_LONG}, false } });
		}
		if (method == "get" && arg0) {
			node_t k = string_obj_arg(arg0);
			if (vstr)
				return str_result("map_str_str_get", { k },
						  { { {N_VOID}, true } });
			return simple_call("map_str_int_get", true, { k },
					   { { {N_VOID}, true } });
		}
		if (method == "contains" && arg0) {
			node_t k = string_obj_arg(arg0);
			return simple_call(vstr ? "map_str_str_contains" : "map_str_int_contains",
					   true, { k }, { { {N_VOID}, true } });
		}
		if (method == "erase" && arg0 && !vstr) {
			node_t k = string_obj_arg(arg0);
			return simple_call("map_str_int_erase", false, { k },
					   { { {N_VOID}, true } });
		}
		if (method == "size")
			return simple_call(vstr ? "map_str_str_size" : "map_str_int_size",
					   true, {}, {});
		if (method == "clear" && !vstr)
			return simple_call("map_str_int_clear", false, {}, {});
		return NULL;
	}

	if (DataDefSET *sd = dynamic_cast<DataDefSET *>(obj.type)) {
		bool str = sd->element_type && sd->element_type->is_string();
		TokenBase *arg0 = tm->parameters.empty() ? NULL : tm->parameters[0];
		if (method == "insert" && arg0) {
			if (str) {
				node_t a = string_obj_arg(arg0);
				return simple_call("set_str_insert", false, { a },
						   { { {N_VOID}, true } });
			}
			return simple_call("set_int_insert", false,
					   { translate_expr(arg0) }, { { {N_LONG}, false } });
		}
		if (method == "contains" && arg0) {
			if (str) {
				node_t a = string_obj_arg(arg0);
				return simple_call("set_str_contains", true, { a },
						   { { {N_VOID}, true } });
			}
			return simple_call("set_int_contains", true,
					   { translate_expr(arg0) }, { { {N_LONG}, false } });
		}
		if (method == "erase" && arg0 && str) {
			node_t a = string_obj_arg(arg0);
			return simple_call("set_str_erase", false, { a },
					   { { {N_VOID}, true } });
		}
		if (method == "size")
			return simple_call(str ? "set_str_size" : "set_int_size",
					   true, {}, {});
		if (method == "clear" && str)
			return simple_call("set_str_clear", false, {}, {});
		return NULL;
	}
	return NULL;
}

// Lower a container subscript READ `c[i]` to its runtime getter:
//  vector<int>     nums[i]      -> vector_int_at((void*)&nums, i)            (long)
//  vector<string>  names[i]     -> string_cstr fill from vector_str_at       (const char*)
//  map<string,int> ages[k]      -> map_str_int_get((void*)&ages, (void*)&k)  (long)
//  map<string,str> dict[k]      -> string_cstr fill from map_str_str_get     (const char*)
// Returns NULL when the subscript object is not a container.
node_t CirBuilder::container_subscript_read(TokenSubscript *tsub, TokenBase *origin)
{
	const Variable &obj = tsub->object;
	if (!is_container_object(obj.type))
		return NULL;
	node_t self = string_obj_addr(obj.name.c_str(), origin);   // (void*)&container

	// String-result subscript: fill a scope-temp std::string then read it as
	// const char* (matches container_method_call's str_result).
	auto str_result = [&](const char *sym, node_t key_or_index,
			      ExternParam key_param) -> node_t {
		char tname[40];
		snprintf(tname, sizeof(tname), "__madc_cstrtmp_%d", m_strtmp_counter++);
		m_pending_stmts.push_back(string_storage_decl(tname, origin));
		m_pending_stmts.push_back(string_ctor_call(tname, NULL, origin));
		need_output_extern(sym, true,
				   { { {N_VOID}, true }, { {N_VOID}, true }, key_param });
		node_t a = list();
		append(a, string_obj_addr(tname, origin));
		append(a, self);
		append(a, key_or_index);
		node_t call = node2(N_CALL, id(sym, origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		need_output_extern("string_cstr", true, { { {N_VOID}, true } });
		node_t cargs = list();
		append(cargs, string_obj_addr(tname, origin));
		node_t cstr = node2(N_CALL, id("string_cstr", origin), cargs, origin);
		CIR_NODE(cstr)->synth_from_origin = true;
		node_t seq = node2(N_COMMA, call, cstr, origin);
		CIR_NODE(seq)->synth_from_origin = true;
		return seq;
	};

	if (DataDefVECTOR *vd = dynamic_cast<DataDefVECTOR *>(obj.type)) {
		bool str = vd->element_type && vd->element_type->is_string();
		if (str)
			return str_result("vector_str_at", translate_expr(tsub->index),
					  { {N_LONG}, false });
		need_output_extern("vector_int_at", false,
				   { { {N_VOID}, true }, { {N_LONG}, false } },
				   std::vector<c2mir_node_code_t>{N_LONG});
		node_t a = list();
		append(a, self);
		append(a, translate_expr(tsub->index));
		node_t call = node2(N_CALL, id("vector_int_at", origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		return call;
	}
	if (DataDefMAP *md = dynamic_cast<DataDefMAP *>(obj.type)) {
		bool vstr = md->val_type && md->val_type->is_string();
		node_t k = string_obj_arg(tsub->index);   // key is a string
		if (vstr)
			return str_result("map_str_str_get", k, { {N_VOID}, true });
		need_output_extern("map_str_int_get", false,
				   { { {N_VOID}, true }, { {N_VOID}, true } },
				   std::vector<c2mir_node_code_t>{N_LONG});
		node_t a = list();
		append(a, self);
		append(a, k);
		node_t call = node2(N_CALL, id("map_str_int_get", origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		return call;
	}
	return NULL;
}

// Lower a container subscript WRITE `c[i] = rhs` to its runtime setter:
//  vector<int>     nums[i] = v   -> vector_int_set((void*)&nums, i, v)
//  vector<string>  names[i] = s  -> vector_str_set / _set_cstr
//  map<string,int> ages[k] = v   -> map_str_int_set((void*)&ages, (void*)&k, v)
//  map<string,str> dict[k] = s   -> map_str_str_set / _set_cstr
// Returns NULL when the LHS is not a container subscript (caller falls through
// to the generic assignment path).
node_t CirBuilder::container_subscript_assign(TokenOperator *top, TokenBase *origin)
{
	TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(top->left);
	if (!tsub || !is_container_object(tsub->object.type))
		return NULL;
	const Variable &obj = tsub->object;
	node_t self = string_obj_addr(obj.name.c_str(), origin);
	TokenBase *rhs = top->right;

	if (DataDefVECTOR *vd = dynamic_cast<DataDefVECTOR *>(obj.type)) {
		bool str = vd->element_type && vd->element_type->is_string();
		node_t a = list();
		append(a, self);
		append(a, translate_expr(tsub->index));
		if (str) {
			append(a, string_obj_arg(rhs));   // (void*)&str
			need_output_extern("vector_str_set", false,
				{ { {N_VOID}, true }, { {N_LONG}, false }, { {N_VOID}, true } });
			node_t call = node2(N_CALL, id("vector_str_set", origin), a, origin);
			CIR_NODE(call)->synth_from_origin = true;
			return call;
		}
		append(a, translate_expr(rhs));
		need_output_extern("vector_int_set", false,
			{ { {N_VOID}, true }, { {N_LONG}, false }, { {N_LONG}, false } });
		node_t call = node2(N_CALL, id("vector_int_set", origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		return call;
	}
	if (DataDefMAP *md = dynamic_cast<DataDefMAP *>(obj.type)) {
		bool vstr = md->val_type && md->val_type->is_string();
		node_t k = string_obj_arg(tsub->index);
		node_t a = list();
		append(a, self);
		append(a, k);
		if (vstr) {
			append(a, string_obj_arg(rhs));
			need_output_extern("map_str_str_set", false,
				{ { {N_VOID}, true }, { {N_VOID}, true }, { {N_VOID}, true } });
			node_t call = node2(N_CALL, id("map_str_str_set", origin), a, origin);
			CIR_NODE(call)->synth_from_origin = true;
			return call;
		}
		append(a, translate_expr(rhs));
		need_output_extern("map_str_int_set", false,
			{ { {N_VOID}, true }, { {N_VOID}, true }, { {N_LONG}, false } });
		node_t call = node2(N_CALL, id("map_str_int_set", origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		return call;
	}
	return NULL;
}

// `(void*)<name>` — the buffer address as void*, so the runtime wrappers see
// void* and c2mir emits no pointer/integer cast warning. The array name decays
// to long*; the cast normalizes it to void*.
node_t CirBuilder::string_obj_addr(const char *name, TokenBase *origin)
{
	node_t cast = node2(N_CAST, void_ptr_type(), id(name, origin), origin);
	CIR_NODE(cast)->synth_from_origin = true;
	return cast;
}

// string_construct_cstr((void*)name, <cstr>)  — or string_construct((void*)name)
// when there is no initializer. (A string-from-string copy-init is Phase 2/3.)
node_t CirBuilder::string_ctor_call(const char *name, TokenBase *initexpr,
				    TokenBase *origin)
{
	node_t args = list();
	append(args, string_obj_addr(name, origin));
	const char *sym;
	if (initexpr) {
		append(args, translate_expr(initexpr));   // a const char* value
		sym = "string_construct_cstr";
		need_output_extern(sym, true, { { {N_VOID}, true }, { {N_CHAR}, true } });
	} else {
		sym = "string_construct";
		need_output_extern(sym, true, { { {N_VOID}, true } });
	}
	node_t call = node2(N_CALL, id(sym, origin), args, origin);
	CIR_NODE(call)->synth_from_origin = true;
	// Wrap as an expression-statement (a block item), matching translate_stmt.
	node_t stmt = node2(N_EXPR, list(), call, origin);
	CIR_NODE(stmt)->synth_from_origin = true;
	return stmt;
}

// Coerce a std::string OBJECT argument to const char*: string_cstr((void*)obj).
// For a plain string variable the object address is string_obj_addr(name); for
// any other string-object expression, cast its translated value to void*. This
// is the dtSTRING->dtCHARptr coercion applied at char*-expecting call sites.
node_t CirBuilder::string_cstr_arg(TokenBase *arg)
{
	need_output_extern("string_cstr", true, { { {N_VOID}, true } });
	node_t addr;
	if (TokenVar *tv = dynamic_cast<TokenVar *>(arg))
		addr = string_obj_addr(tv->var.name.c_str(), arg);
	else
		addr = node2(N_CAST, void_ptr_type(), translate_expr(arg), arg);
	node_t a = list();
	append(a, addr);
	node_t call = node2(N_CALL, id("string_cstr", arg), a, arg);
	CIR_NODE(call)->synth_from_origin = true;
	return call;
}

// Coerce an argument to a std::string OBJECT pointer for a string-object
// parameter. A real string object (declared variable / string& param) is passed
// by address. A const char* value (string literal, char* variable, char*-valued
// expression) is materialized into a scope-lived temporary std::string: the
// storage decl (cleanup-tagged) + ctor are pushed to m_pending_stmts so
// translate_block emits them just before the current statement; the temp's
// address becomes the argument. Mirrors the old transpiler's emit_ns_arg.
node_t CirBuilder::string_obj_arg(TokenBase *arg)
{
	if (is_string_object_value(arg)) {
		// A string-object MEMBER (`obj.name`) is an embedded `long[W]` buffer
		// in the struct; its address is `(void*)&(obj.name)`. Translate the
		// member access (yields the FIELD lvalue), take its address, cast.
		if (dynamic_cast<TokenMember *>(arg))
			return node2(N_CAST, void_ptr_type(),
				node1(N_ADDR, translate_expr(arg), arg), arg);
		if (TokenVar *tv = dynamic_cast<TokenVar *>(arg))
			return string_obj_addr(tv->var.name.c_str(), arg);
		return node2(N_CAST, void_ptr_type(), translate_expr(arg), arg);
	}
	// const char* value -> temp std::string object.
	char name[32];
	snprintf(name, sizeof(name), "__madc_strtmp_%d", m_strtmp_counter++);
	m_pending_stmts.push_back(string_storage_decl(name, arg));
	m_pending_stmts.push_back(string_ctor_call(name, arg, arg));
	return string_obj_addr(name, arg);
}

// Lower a std::string method call (s.c_str()/.length()/.size()/.empty()/.clear())
// on a string OBJECT receiver to the matching runtime wrapper, passing the object
// address as (void*). Returns NULL when this is not a recognized string-object
// method call (wrong receiver type, a string literal/lifted-literal receiver, or
// an unknown method) so the caller falls through to generic handling.
node_t CirBuilder::string_method_call(TokenMember *tm, TokenBase *origin)
{
	// Receiver must be a genuine string OBJECT, not a const char* value:
	// exclude non-string types, lifted `__literal__` vars, and constants —
	// the same exclusions is_string_object_value applies to a TokenVar.
	const Variable &obj = tm->object;
	if (!is_string_object(obj.type))
		return NULL;
	if (obj.name.compare(0, 11, "__literal__") == 0)
		return NULL;
	if (obj.is_constant())
		return NULL;

	const std::string &method = tm->var.name;
	node_t addr = string_obj_addr(obj.name.c_str(), origin);

	if (method == "c_str") {
		need_output_extern("string_cstr", true, { { {N_VOID}, true } });
		node_t a = list();
		append(a, addr);
		node_t call = node2(N_CALL, id("string_cstr", origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		return call;
	}
	// length()/size() — std::string size()==length(); both -> string_length,
	// declared returning `long` (NOT void) so the value reads correctly in
	// arithmetic/comparison contexts.
	if (method == "length" || method == "size") {
		need_output_extern("string_length", false, { { {N_VOID}, true } },
				   { N_LONG });
		node_t a = list();
		append(a, addr);
		node_t call = node2(N_CALL, id("string_length", origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		return call;
	}
	// empty() -> (string_length((void*)s) == 0)
	if (method == "empty") {
		need_output_extern("string_length", false, { { {N_VOID}, true } },
				   { N_LONG });
		node_t a = list();
		append(a, addr);
		node_t call = node2(N_CALL, id("string_length", origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		node_t eq = node2(N_EQ, call, integer(0, origin), origin);
		CIR_NODE(eq)->synth_from_origin = true;
		return eq;
	}
	// clear() -> string_clear((void*)s)  (a void statement-expression)
	if (method == "clear") {
		need_output_extern("string_clear", false, { { {N_VOID}, true } });
		node_t a = list();
		append(a, addr);
		node_t call = node2(N_CALL, id("string_clear", origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		return call;
	}
	return NULL;
}

// Build a type specifier LIST. If typedef_alias is set, emit ID("alias")
// instead of raw type nodes — c2mir's checker resolves the typedef.
node_t CirBuilder::type_list(DataDef *dd, const std::string &typedef_alias)
{
	node_t lst = list();

	// If a typedef name is available, emit ID("alias") — matches c2m's behavior
	if (!typedef_alias.empty()) {
		append(lst, id(typedef_alias.c_str()));
		return lst;
	}

	// Struct types: LIST(STRUCT(ID("name"), IGNORE))
	// A user-defined class lowers to `struct ClassName` too (same shape).
	// _Complex is a DataDefSTRUCT subclass but must use the native spec path.
	if (dd && (dd->is_struct() || as_user_class(dd)) && !dd->is_complex()) {
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd);
		if (sdd) {
			node_t sref = node2(N_STRUCT, id(sdd->name.c_str()), ignore());
			append(lst, sref);
			return lst;
		}
	}

	append_type_specs(lst, dd);
	return lst;
}

// Build the N_FUNC(param-list) declarator suffix for a function-pointer's
// target signature. Mirrors func_proto's parameter handling: drops the
// synthetic trailing vararg slot and emits N_DOTS, renders an explicit
// `(void)` only for is_void_params, leaves a bare `()` for K&R-unspecified,
// and reuses param_decl per parameter.
node_t CirBuilder::fnptr_func_node(FuncDef *fd)
{
	node_t param_list = list();
	if (!fd) return node1(N_FUNC, param_list);   // unknown signature -> ()

	size_t nparam = fd->parameters.size();
	if (fd->is_varargs && nparam > 0) nparam--;  // drop the synthetic vararg slot

	if (nparam == 0) {
		if (fd->is_void_params) {
			node_t void_spec = node1(N_LIST, simple(N_VOID));
			node_t void_decl = node2(N_DECL, ignore(), list());
			append(param_list, node2(N_TYPE, void_spec, void_decl));
		}
		// else: bare () — unspecified parameter list (K&R)
	} else {
		for (size_t i = 0; i < nparam; i++)
			append(param_list, param_decl(fd->parameters[i], "", std::string()));
	}
	if (fd->is_varargs)
		append(param_list, simple(N_DOTS));
	return node1(N_FUNC, param_list);
}

// Append a function-pointer's return-type specifiers into `spec_list`, and
// fill `decl_list` with its declarator suffixes in c2m innermost-first order.
// With emit_pointer the result is a pointer-to-function (`int (*fp)(char)` =>
// specs `int`, suffixes [POINTER, FUNC]); without it, a bare function type
// (`typedef int BINOP(int,int)` => [FUNC]). A pointer-returning fn-ptr
// `char *(*fp)(void)` appends the return stars AFTER the FUNC
// ([POINTER, FUNC, POINTER]); an array of fn-ptrs `void (*tab[3])(int)`
// prepends the array dims ([ARR, POINTER, FUNC]).
void CirBuilder::fnptr_decl_pieces(FuncDef *fd, bool emit_pointer,
				   node_t spec_list, node_t decl_list,
				   const std::vector<uint32_t> &lead_dims)
{
	// Return-type specs: peel pointer levels, recording the star count so the
	// stars can be appended as the outermost declarator suffix.
	DataDef *ret_dd = fd ? &fd->returns : NULL;
	int ret_stars = 0;
	while (ret_dd && ret_dd->is_pointer()) {
		DataDefPTR *p = dynamic_cast<DataDefPTR *>(ret_dd);
		if (!p || !p->base_type) break;
		ret_dd = p->base_type;
		ret_stars++;
	}
	if (ret_dd && ret_dd->is_struct() && !ret_dd->is_complex()) {
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(ret_dd);
		if (sdd)
			append(spec_list, node2(N_STRUCT, id(sdd->name.c_str()), ignore()));
		else
			append_type_specs(spec_list, ret_dd);
	} else {
		append_type_specs(spec_list, ret_dd);
	}

	// Suffixes, innermost binding first.
	for (size_t d = 0; d < lead_dims.size(); d++)
		append(decl_list, node3(N_ARR, ignore(), list(), integer(lead_dims[d])));
	if (emit_pointer)
		append(decl_list, pointer());        // the fn-ptr `(*name)`
	append(decl_list, fnptr_func_node(fd));  // the `(params)`
	for (int s = 0; s < ret_stars; s++)
		append(decl_list, pointer());        // return-type `*`
}

// Extra pointer stars an fn-ptr usage carries beyond its typedef alias. The
// alias `DO_FUN` of `typedef void DO_FUN(args)` is a bare function type, so
// `DO_FUN *do_fun` needs one explicit `*`; `UNOP` of `typedef int (*UNOP)(int)`
// already includes the pointer, so `UNOP u` needs none. Unknown alias -> 1
// (the usage is a fn-ptr, so assume the alias is the function form).
int CirBuilder::fnptr_alias_stars(const std::string &alias)
{
	if (alias.empty() || !m_prog) return 1;
	datatype_map_iter it = m_prog->datatype_map.find(alias);
	if (it == m_prog->datatype_map.end() || !it->second) return 1;
	// A Form-2 pointer-to-function typedef already carries the pointer (0
	// extra stars); a Form-1 function typedef does not, so a fn-ptr use of
	// the alias needs one explicit `*`.
	if (DataDefFPTR *afp = dynamic_cast<DataDefFPTR *>(&it->second->definition))
		return afp->ptr_syntax ? 0 : 1;
	return 1;
}

// -----------------------------------------------------------------------
// Declaration builders
// -----------------------------------------------------------------------

static int dd_ptr_depth(DataDef *dd);  // defined below; counts int** -> 2

// Peel DataDefCArray layers off a type, collecting fixed-array dimensions
// outermost-first. `typedef unsigned long T[2]` -> elem=unsigned long,
// dims=[2]. A runtime-sized CArray (count_expr != NULL) stops the peel and
// contributes no dimension (it decays/uses a VLA path elsewhere).
// Returns the innermost element type; appends each fixed dim to `dims`.
static DataDef *peel_carray_dims(DataDef *dd, std::vector<uint32_t> &dims)
{
	while (DataDefCArray *ca = dynamic_cast<DataDefCArray *>(dd)) {
		if (ca->has_runtime_size() || !ca->element_type)
			break;
		dims.push_back((uint32_t)ca->count);
		dd = ca->element_type;
	}
	return dd;
}

// Read element `index` of a static integer fixed-array's pre-baked storage
// (`var->data`), decoded per the element type. The parser writes constant
// initializers for `static`/global integer arrays directly into var->data and
// clears the TokenDecl init_list (store_static_integer_array_value), so the CIR
// path — which only inspects init_list — would otherwise lose the initializer.
// Returns true and sets `out` on success; false for non-integer element types.
static bool read_static_int_array_elem(Variable *var, size_t index, int64_t &out)
{
	if (!var || !var->data || !var->type || !var->type->is_integer())
		return false;
	const char *src = (const char *)var->data + index * var->type->size;
	switch (var->type->rawtype()) {
	case DataType::dtBOOL:
	case DataType::dtCHAR:   out = *(const int8_t *)src;   return true;
	case DataType::dtUINT8:  out = *(const uint8_t *)src;  return true;
	case DataType::dtINT16:
	case DataType::dtINT24:  out = *(const int16_t *)src;  return true;
	case DataType::dtUINT16:
	case DataType::dtUINT24: out = *(const uint16_t *)src; return true;
	case DataType::dtINT32:  out = *(const int32_t *)src;  return true;
	case DataType::dtUINT32: out = *(const uint32_t *)src; return true;
	case DataType::dtINT64:  out = *(const int64_t *)src;  return true;
	case DataType::dtUINT64: out = (int64_t)*(const uint64_t *)src; return true;
	default: return false;
	}
}

node_t CirBuilder::param_decl(DataDef *ptype, const char *pname,
			      const std::string &typedef_alias)
{
	// c2m emits a NAMED parameter as N_SPEC_DECL, but an UNNAMED (abstract)
	// parameter as N_TYPE(specs, DECL(IGNORE, suffixes)). Emitting an unnamed
	// param as N_SPEC_DECL with an IGNORE/empty id passes c2mir's checker but
	// CRASHES its MIR generator (and two empty-named N_ID params would also
	// trip "repeated declaration"). Fn-ptr type params (built via
	// fnptr_func_node) are always unnamed — e.g. `void (*)(CHAR_DATA*, char*)`.
	// wrap() builds the correct node for each case from (specs, suffix-list).
	bool unnamed = !(pname && pname[0]);
	auto wrap = [&](node_t pspec, node_t pdecl_list) -> node_t {
		if (unnamed)
			return node2(N_TYPE, pspec, node2(N_DECL, ignore(), pdecl_list));
		node_t sd = simple(N_SPEC_DECL);
		append(sd, pspec);
		append(sd, node2(N_DECL, id(pname), pdecl_list));
		append(sd, ignore());
		append(sd, ignore());
		append(sd, ignore());
		return sd;
	};

	// std::string / MadArray parameter: a runtime-object value (`string`,
	// `string&`, `array`, `array&`) or its pointer form lowers to `void *` — a
	// pointer to the object. The caller passes the object's address; uses inside
	// the function treat the param as that object. NOTE: a by-value param
	// currently aliases the caller's object (no copy); a copy-temp at the call
	// site is a refinement.
	if (ptype) {
		DataType pdt = ptype->rawtype();
		if (pdt == DataType::dtSTRING || pdt == DataType::dtSTRINGref
		    || pdt == DataType::dtARRAY || pdt == DataType::dtARRAYref
		    || pdt == DataType::dtVECTOR || pdt == DataType::dtVECTORref
		    || pdt == DataType::dtMAP || pdt == DataType::dtMAPref
		    || pdt == DataType::dtSET || pdt == DataType::dtSETref) {
			node_t pspec = list();
			append(pspec, simple(N_VOID));
			node_t pdecl_list = list();
			append(pdecl_list, pointer());
			return wrap(pspec, pdecl_list);
		}
	}

	// Parameter declared via a typedef alias keeps the alias as the type spec
	// (matches c2m and the var/member paths); explicit stars go on the
	// declarator. This correctly renders `HARD_REG_SET *p` (a pointer to an
	// array typedef) as `HARD_REG_SET *p` rather than peeling to `int *p`.
	if (!typedef_alias.empty()) {
		int stars = explicit_star_count(ptype, typedef_alias);
		node_t pspec = type_list(ptype, typedef_alias);
		node_t pdecl_list = list();
		for (int s = 0; s < stars; s++)
			append(pdecl_list, pointer());
		return wrap(pspec, pdecl_list);
	}

	// Function-pointer parameter (no typedef alias): `int (*fp)(int)`.
	if (DataDefFPTR *fp = dynamic_cast<DataDefFPTR *>(ptype)) {
		node_t pspec = list();
		node_t pdecl_list = list();
		fnptr_decl_pieces(fp->target, true, pspec, pdecl_list,
				  std::vector<uint32_t>());
		return wrap(pspec, pdecl_list);
	}

	// Array parameter decay: `T m[N]` -> `T *m`, `T m[N][M]` -> `T (*m)[M]`.
	// A 1-D array param arrives as a CArray; a multi-dim param arrives already
	// decayed by the parser as DataDefPTR(CArray) (`char (*)[M]`). Handle both:
	// peel pointer levels, then any remaining array dims become `T (*m)[dims]`.
	// Without this an array param collapsed to the wrong type (`int *map`), so
	// `map[x][y]` failed ("subscripted value is neither array nor pointer").
	{
		int ptr_levels = 0;
		DataDef *t = ptype;
		while (t && t->is_pointer()) {
			DataDefPTR *p = dynamic_cast<DataDefPTR *>(t);
			if (!p || !p->base_type) break;
			t = p->base_type;
			ptr_levels++;
		}
		std::vector<uint32_t> adims;
		DataDef *elem = peel_carray_dims(t, adims);
		if (!adims.empty()) {
			// A direct CArray param (ptr_levels == 0) decays its outermost
			// dimension to a pointer; an already-decayed DataDefPTR(CArray)
			// keeps its pointer(s) and all remaining dims.
			int decay_ptrs = ptr_levels ? ptr_levels : 1;
			size_t first_dim = ptr_levels ? 0 : 1;
			// Element's own pointer depth (`char *argv[]` -> element char*).
			int elem_stars = 0;
			while (elem && elem->is_pointer()) {
				DataDefPTR *p = dynamic_cast<DataDefPTR *>(elem);
				if (!p || !p->base_type) break;
				elem = p->base_type;
				elem_stars++;
			}
			node_t pspec = type_list(elem);
			node_t pdecl_list = list();
			for (int s = 0; s < elem_stars; s++)
				append(pdecl_list, pointer());          // element pointers (innermost)
			for (int s = 0; s < decay_ptrs; s++)
				append(pdecl_list, pointer());          // explicit / decayed pointer(s)
			for (size_t d = first_dim; d < adims.size(); d++)
				append(pdecl_list, node3(N_ARR, ignore(), list(),
							 integer(adims[d])));
			return wrap(pspec, pdecl_list);
		}
	}

	DataDef *base_dd = ptype;
	bool is_ptr = base_dd && base_dd->is_pointer();
	// Peel ALL pointer levels to the innermost base type. A single peel left
	// `struct node **p` with base = `DataDefPTR(struct node)`, which is not a
	// struct, so type_list fell through to the default `int` spec — dropping
	// the struct/real base type for any multi-level pointer parameter.
	while (base_dd && base_dd->is_pointer()) {
		DataDefPTR *p = dynamic_cast<DataDefPTR *>(base_dd);
		if (!p || !p->base_type) break;
		base_dd = p->base_type;
	}

	node_t pspec = type_list(base_dd);
	node_t pdecl_list = list();
	if (is_ptr) {
		// One '*' per indirection level (int** param -> 2).
		int depth = dd_ptr_depth(ptype);
		for (int s = 0; s < depth; s++)
			append(pdecl_list, pointer());
	}
	return wrap(pspec, pdecl_list);
}

const char *CirBuilder::builtin_output_runtime(const std::string &name)
{
	if (name == "puti")     return "madc_puti";
	if (name == "putu")     return "madc_putu";
	if (name == "putd")     return "madc_putd";
	if (name == "putf")     return "madc_putf";
	if (name == "puts")     return "madc_puts";
	if (name == "printstr") return "madc_printstr";
	return "";
}

void CirBuilder::need_output_extern(const char *symbol, bool ret_ptr,
				    const std::vector<ExternParam> &params,
				    const std::vector<c2mir_node_code_t> &ret_specs)
{
	if (m_output_externs.count(symbol)) return;

	node_t ext_list = list();
	append(ext_list, simple(N_EXTERN));
	// Return base type: N_VOID by default, or the caller-supplied specs
	// (e.g. {N_LONG} for a long-returning runtime fn — a void base would
	// silently truncate/misread a value used in arithmetic/comparison).
	if (ret_specs.empty()) {
		append(ext_list, simple(N_VOID));
	} else {
		for (size_t i = 0; i < ret_specs.size(); i++)
			append(ext_list, simple(ret_specs[i]));
	}
	node_t share = node1(N_SHARE, ext_list);

	node_t param_list = list();
	if (params.empty()) {
		node_t void_spec = node1(N_LIST, simple(N_VOID));
		node_t void_decl = node2(N_DECL, ignore(), list());
		append(param_list, node2(N_TYPE, void_spec, void_decl));
	} else {
		for (size_t i = 0; i < params.size(); i++) {
			node_t specs = list();
			for (size_t j = 0; j < params[i].specs.size(); j++)
				append(specs, simple(params[i].specs[j]));
			node_t pdecl_list = list();
			if (params[i].ptr) append(pdecl_list, pointer());
			node_t pdecl = node2(N_DECL, ignore(), pdecl_list);
			append(param_list, node2(N_TYPE, specs, pdecl));
		}
	}

	node_t func_inner = node1(N_FUNC, param_list);
	node_t decl_list = list();
	append(decl_list, func_inner);
	if (ret_ptr) append(decl_list, pointer());   // returns void*
	node_t decl = node2(N_DECL, id(symbol), decl_list);

	node_t proto = simple(N_SPEC_DECL);
	append(proto, share);
	append(proto, decl);
	append(proto, ignore());
	append(proto, ignore());
	append(proto, ignore());
	m_output_externs[symbol] = proto;
}

// ---- Stream chains (cout << x) ----

CirBuilder::StreamKind CirBuilder::stream_ident_kind(TokenBase *tb)
{
	std::string name;
	if (TokenVar *tv = dynamic_cast<TokenVar *>(tb)) name = tv->var.name;
	else if (TokenIdent *ti = dynamic_cast<TokenIdent *>(tb)) name = ti->str;
	else return SK_NONE;
	// Unqualified `cout` keeps its source name; qualified `std::cout`
	// resolves to the hidden std global `__std_cout` (see add_namespaces()).
	// Recognize both so a qualified stream chain isn't mistaken for a shift.
	if (name == "cout" || name == "__std_cout") return SK_COUT;
	if (name == "cerr" || name == "__std_cerr") return SK_CERR;
	if (name == "clog" || name == "__std_clog") return SK_CLOG;
	if (name == "cin"  || name == "__std_cin")  return SK_CIN;
	return SK_NONE;
}

const char *CirBuilder::stream_object_symbol(StreamKind k)
{
	switch (k) {
	case SK_COUT: return "_ZSt4cout";
	case SK_CERR: return "_ZSt4cerr";
	case SK_CLOG: return "_ZSt4clog";
	case SK_CIN:  return "_ZSt3cin";
	default:      return "_ZSt4cout";
	}
}

// extern char SYM;  (an opaque object whose address we take)
void CirBuilder::need_stream_object(StreamKind k)
{
	const char *sym = stream_object_symbol(k);
	if (m_stream_objects.count(sym)) return;
	m_stream_objects.insert(sym);
	node_t spec = list();
	append(spec, simple(N_EXTERN));
	append(spec, simple(N_CHAR));
	node_t share = node1(N_SHARE, spec);
	node_t decl = node2(N_DECL, id(sym), list());
	node_t sd = simple(N_SPEC_DECL);
	append(sd, share); append(sd, decl);
	append(sd, ignore()); append(sd, ignore()); append(sd, ignore());
	m_stream_object_protos.push_back(sd);
}

// Pick the mangled libstdc++ operator<< overload for a value's type.
const char *CirBuilder::ostream_insert_symbol(DataDef *dd, ExternParam &p_out)
{
	bool is_ptr = dd && dd->is_pointer();
	DataType dt = dd ? dd->rawtype() : DataType::dtINT64;
	// NOTE: a string OBJECT value (operator<<(ostream&, const string&)) is
	// handled directly in translate_stream_chain, which has the token to tell
	// an object apart from a literal. Here `is_string()` means a const char*
	// (string literal / char* value).
	if (dd && dd->is_string()) { p_out = {{N_CHAR}, true}; return "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc"; }
	if (is_ptr && dt == DataType::dtCHAR) { p_out = {{N_CHAR}, true}; return "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc"; }
	if (is_ptr) { p_out = {{N_VOID}, true}; return "_ZNSolsEPKv"; }
	switch (dt) {
	case DataType::dtCHAR:   p_out = {{N_CHAR}, false};  return "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c";
	case DataType::dtFLOAT:
	case DataType::dtDOUBLE: p_out = {{N_DOUBLE}, false}; return "_ZNSolsEd";
	case DataType::dtINT16:
	case DataType::dtINT32:  p_out = {{N_INT}, false};    return "_ZNSolsEi";
	case DataType::dtUINT8:
	case DataType::dtUINT16:
	case DataType::dtUINT32: p_out = {{N_UNSIGNED, N_INT}, false}; return "_ZNSolsEj";
	case DataType::dtINT64:
	case DataType::dtUINT64:
	default:                 p_out = {{N_LONG}, false};   return "_ZNSolsEl";
	}
}

// Lower a C++ ostream chain to direct mangled operator<< calls, one per value.
node_t CirBuilder::translate_stream_chain(TokenOperator *top, StreamKind k, bool is_out)
{
	need_stream_object(k);
	// madc parses `cout << a << b << c` right-associatively as
	// `cout << (a << (b << c))`: the stream object is top->left and the
	// values hang off the right spine (each `<<` node's left is one value,
	// its right is the next value or `<<` node). Collect values in C++
	// (left-to-right) order by walking that right spine.
	std::vector<TokenBase *> vals;
	TokenBase *n = top->right;
	while (TokenOperator *o = dynamic_cast<TokenOperator *>(n)) {
		// A parenthesized `<<`/`>>` is a single value, not a chain link.
		if (o->id() != top->id() || o->is_bracketed()) break;
		vals.push_back(o->left);
		n = o->right;
	}
	if (n) vals.push_back(n);

	node_t result = node1(N_ADDR, id(stream_object_symbol(k)));
	for (size_t i = 0; i < vals.size(); i++) {
		// endl manipulator: _ZNSolsEPFRSoS_E(os, &endl)
		std::string vn;
		if (TokenVar *tv = dynamic_cast<TokenVar *>(vals[i])) vn = tv->var.name;
		else if (TokenIdent *ti = dynamic_cast<TokenIdent *>(vals[i])) vn = ti->str;
		if (vn == "endl" || vn == "__std_endl") {
			const char *MANIP = "_ZNSolsEPFRSoS_E";
			const char *ENDLF = "_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_";
			need_output_extern(MANIP, true, { { {N_VOID}, true }, { {N_VOID}, true } });
			need_output_extern(ENDLF, true, { { {N_VOID}, true } }); // fn; address taken
			node_t args = list();
			append(args, result);
			append(args, node1(N_ADDR, id(ENDLF)));
			result = node2(N_CALL, id(MANIP), args, top);
			continue;
		}
		node_t args = list();
		append(args, result);
		if (is_string_object_value(vals[i])) {
			// operator<<(ostream&, const std::string&) — takes the object
			// pointer, returns ostream& (chains). Literals are NOT objects and
			// fall through to the char* overload below.
			const char *SSYM = "_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKNSt7__cxx1112basic_stringIS4_S5_T1_EE";
			need_output_extern(SSYM, true, { { {N_VOID}, true }, { {N_VOID}, true } });
			// string_obj_arg yields the object's (void*) address for both a
			// plain string variable (&v) and a string MEMBER (&obj.member).
			append(args, string_obj_arg(vals[i]));
			result = node2(N_CALL, id(SSYM), args, top);
			continue;
		}
		DataDef *vdd = vals[i]->datadef();
		ExternParam vp;
		const char *sym = ostream_insert_symbol(vdd, vp);
		need_output_extern(sym, true, { { {N_VOID}, true }, vp });
		append(args, translate_expr(vals[i]));
		result = node2(N_CALL, id(sym), args, top);
	}
	return result;
}

node_t CirBuilder::init_value(TokenBase *elem)
{
	// A NULL element is a designated-initializer GAP: the parser normalizes
	// `.field`/`[index]` designators into positional slots at parse time
	// (parser.cpp: assign_initializer_range / field_index resolution),
	// NULL-filling the slots between explicit values. C semantics zero-fill
	// those gaps. We emit a dense `I 0` rather than N_IGNORE because c2mir
	// rejects N_IGNORE as an initializer value (it crashes in
	// c2mir_compile_tree). This keeps the init list positional+dense, which
	// is semantically identical to C99 sparse designated init.
	// LIMITATION: a gap whose element type is an aggregate (struct/array)
	// would need a nested zero-init list, not a scalar 0; that case is not
	// yet produced by the parser's normalization for the supported tests.
	if (!elem) return integer(0);
	TokenStructLit *sl = dynamic_cast<TokenStructLit *>(elem);
	if (sl) {
		// DEFERRED (CirBuilder-only): an empty brace `{}` produces an empty
		// LIST() value here. For a flexible-array member (`int z[]; ... .z={}`)
		// c2m's text front-end emits INIT(LIST(FIELD_ID(z)), LIST()) — the
		// FIELD_ID designator tells the checker the target is the flex array,
		// so the empty LIST is accepted. Our positional normalization drops
		// the designator, so the empty LIST lands as a scalar/flex slot value
		// and c2mir rejects it ("empty scalar initializer"). Fixing this
		// requires CirBuilder to retain/re-derive designator info for
		// flexible-array empty inits (tests/testflexarrayemptyinit.mad).
		node_t inner = list();
		for (size_t i = 0; i < sl->inits.size(); i++)
			append(inner, node2(N_INIT, list(), init_value(sl->inits[i])));
		return inner;
	}
	return translate_expr(elem);
}

node_t CirBuilder::var_decl(Variable *v, TokenBase *origin)
{
	// std::string object: emit ONLY the opaque storage (`long name[W]`) here.
	// Construction and scope-exit destruction are emitted as separate statements
	// by translate_block (the 1->N C++ lowering); see string_ctor_call/dtor.
	if (is_string_object(v->type))
		return string_storage_decl(v->name.c_str(), origin);

	// MadArray object (`array a;`): same opaque-storage model as std::string.
	// madarray_construct is emitted as a separate statement by translate_block;
	// the cleanup attribute on the storage handles scope-exit destruction.
	if (is_array_object(v->type))
		return array_storage_decl(v->name.c_str(), origin);

	// STL container object (vector<T>/map<K,V>/set<T>): same opaque-storage
	// model. The default ctor is emitted as a separate statement by
	// translate_block; the cleanup attribute handles scope-exit destruction.
	if (is_container_object(v->type))
		return container_storage_decl(v->name.c_str(), v->type, origin);

	DataDef *base_dd = v->type;
	bool is_ptr = base_dd && base_dd->is_pointer();
	// Peel ALL pointer levels to the innermost base type (struct node ** -> the
	// struct, not the intermediate DataDefPTR which type_list would render as
	// `int`). The star count is recovered separately via dd_ptr_depth below.
	while (base_dd && base_dd->is_pointer()) {
		DataDefPTR *p = dynamic_cast<DataDefPTR *>(base_dd);
		if (!p || !p->base_type) break;
		base_dd = p->base_type;
	}

	// A variable whose type is an anonymous aggregate (`struct { ... } x;`)
	// has no tag to forward-reference, so the body must be emitted inline in
	// the variable's own spec: LIST(STRUCT(IGNORE, members)). Without this the
	// builder emits `struct anonymous` (a forward ref to a never-defined tag),
	// leaving the variable with an incomplete type. member_node carries the
	// member array dims via the owning struct.
	DataDefSTRUCT *anon_sdd = NULL;
	if (v->typedef_name.empty() && !is_ptr && base_dd && base_dd->is_struct()
	    && !base_dd->is_complex()) {
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(base_dd);
		if (sdd && sdd->name == "anonymous" && !sdd->members.empty())
			anon_sdd = sdd;
	}

	// Function-pointer variable `int (*fp)(int)` — unless declared via a
	// fn-ptr typedef alias, in which case the alias is the type spec like any
	// other typedef. The dtINT64 rawtype would otherwise emit `long`, erasing
	// the signature; build `ret (*fp)(params)` instead. (Array-of-fn-ptr
	// variables are deferred; SMAUG's tables are struct-arrays with fn-ptr
	// members, handled in member_node.)
	DataDefFPTR *fnptr = v->typedef_name.empty()
			? dynamic_cast<DataDefFPTR *>(v->type) : NULL;
	node_t fnptr_decl_list = NULL;

	// Emit ID("alias") for a variable declared via a typedef (matches c2m).
	// Pointer usages keep the alias as the type spec and carry the explicit
	// stars on the declarator below.
	node_t tl;
	if (fnptr) {
		tl = list();
		if (v->flags & vfSTATIC) append(tl, simple(N_STATIC));
		if (v->flags & vfEXTERN) append(tl, simple(N_EXTERN));
		fnptr_decl_list = list();
		fnptr_decl_pieces(fnptr->target, true, tl, fnptr_decl_list, std::vector<uint32_t>());
	} else if (anon_sdd) {
		node_t ml = list();
		for (size_t i = 0; i < anon_sdd->members.size(); i++)
			append(ml, member_node(anon_sdd->members[i], anon_sdd));
		tl = node1(N_LIST, node2(anon_sdd->union_layout ? N_UNION : N_STRUCT,
					 ignore(), ml));
	} else {
		tl = !v->typedef_name.empty()
				? type_list(v->type, v->typedef_name)
				: type_list(base_dd);
	}

	// Storage class qualifiers (fn-ptr vars handle storage class above).
	if (!fnptr && (v->flags & vfSTATIC)) {
		node_t new_list = list();
		append(new_list, simple(N_STATIC));
		if (base_dd && base_dd->is_struct() && !base_dd->is_complex()) {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(base_dd);
			if (sdd)
				append(new_list, node2(N_STRUCT, id(sdd->name.c_str()), ignore()));
			else
				append_type_specs(new_list, base_dd);
		} else {
			append_type_specs(new_list, base_dd);
		}
		tl = new_list;
	}
	if (!fnptr && (v->flags & vfEXTERN)) {
		// An extern is a forward reference to a symbol defined elsewhere, so
		// emit its type exactly as the definition would — preserve the typedef
		// alias and struct tag. The old append_type_specs path dropped the
		// alias, so `extern bool x` degraded to `extern int x` and conflicted
		// with the `bool x` definition ("incompatible types of x declarations").
		node_t new_list = list();
		append(new_list, simple(N_EXTERN));
		if (!v->typedef_name.empty()) {
			append(new_list, id(v->typedef_name.c_str()));
		} else if (base_dd && base_dd->is_struct() && !base_dd->is_complex()) {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(base_dd);
			if (sdd)
				append(new_list, node2(N_STRUCT, id(sdd->name.c_str()), ignore()));
			else
				append_type_specs(new_list, base_dd);
		} else {
			append_type_specs(new_list, base_dd);
		}
		tl = new_list;
	}

	node_t share = node1(N_SHARE, tl);
	node_t var_id = id(v->name.c_str(), origin);
	node_t decl_list = fnptr ? fnptr_decl_list : list();

	// c2m declarator order: in `T *arr[N]` the `[]` binds tighter than `*`
	// (array of pointers), and c2m's declarator parser appends the pointer
	// ops AFTER the direct-declarator's array ops. So the N_ARR nodes must
	// precede the N_POINTER nodes in the decl list. Emit arrays first, then
	// pointers — matching `direct_declarator`/`declarator` in c2mir.c.
	if (!fnptr && v->is_fixed_array() && !v->dims.empty()) {
		// One N_ARR per dimension, outer dimension first:
		// int m[2][2] -> ARR(...,2) ARR(...,2)
		//
		// When declared via an array typedef alias (`HARD_REG_SET x[2]` with
		// `typedef T HARD_REG_SET[2]`), the parser flattens the typedef's own
		// dims into v->dims, so v->dims = [typedef-dims..., own-dims...]. The
		// alias spec (ID("HARD_REG_SET")) already implies the typedef's dims,
		// so emit only the variable's OWN leading dims here — skip the trailing
		// dims contributed by the alias.
		size_t skip_tail = 0;
		if (!v->typedef_name.empty() && m_prog) {
			datatype_map_iter it = m_prog->datatype_map.find(v->typedef_name);
			if (it != m_prog->datatype_map.end() && it->second) {
				std::vector<uint32_t> tdims;
				peel_carray_dims(&it->second->definition, tdims);
				skip_tail = tdims.size();
			}
		}
		size_t emit_count = v->dims.size() > skip_tail
				? v->dims.size() - skip_tail : 0;
		for (size_t d = 0; d < emit_count; d++) {
			// An unsized extern array (`extern char buf[]`) carries dim 0 —
			// emit `[]` (incomplete type, compatible with the sized
			// definition) rather than `[0]`, which conflicts.
			node_t size = ((v->flags & vfEXTERN) && v->dims[d] == 0)
					? ignore() : integer(v->dims[d]);
			node_t arr = node3(N_ARR, ignore(), list(), size);
			append(decl_list, arr);
		}
	}

	int decl_stars = fnptr ? -1 : explicit_star_count(v->type, v->typedef_name);
	if (fnptr) {
		// fn-ptr declarator suffixes already built by fnptr_decl_pieces.
	} else if (decl_stars >= 0) {
		for (int s = 0; s < decl_stars; s++)
			append(decl_list, pointer());
	} else if (is_ptr) {
		// One '*' per indirection level (int** -> 2). Previously a single
		// pointer() was appended, collapsing int**/T*** to one star.
		int depth = dd_ptr_depth(v->type);
		for (int s = 0; s < depth; s++)
			append(decl_list, pointer());
	}

	node_t var_decl_node = node2(N_DECL, var_id, decl_list);
	node_t init_node = ignore();
	TokenDecl *tdecl = dynamic_cast<TokenDecl *>(origin);
	if (tdecl && (tdecl->has_brace_init || !tdecl->init_list.empty())) {
		// Brace init: LIST(INIT(LIST(), val), ...)
		// Also the string-literal char-array form (`char s[3] = "ab";`):
		// the parser expands the literal into per-char init_list entries
		// without setting has_brace_init, so key on a populated init_list
		// too — the emitted INIT list is identical either way.
		node_t lst = list();
		if (tdecl->init_list.empty() && v->is_fixed_array() && v->data
		    && v->type && v->type->is_integer()) {
			// The parser baked a static/global integer array's constant
			// initializer into v->data and cleared init_list. Reconstruct the
			// INIT entries from the storage so the CIR backend sees them.
			size_t total = v->total_elements();
			for (size_t i = 0; i < total; i++) {
				int64_t ev = 0;
				if (!read_static_int_array_elem(v, i, ev)) { lst = ignore(); break; }
				append(lst, node2(N_INIT, list(), integer(ev)));
			}
		} else {
			for (size_t i = 0; i < tdecl->init_list.size(); i++)
				append(lst, node2(N_INIT, list(), init_value(tdecl->init_list[i])));
		}
		init_node = lst;
	} else if (tdecl && tdecl->initialize) {
		// Scalar init: the parser stores `initialize` as a full
		// assignment AST (TokenAssign: left=var, right=value), but a
		// SPEC_DECL initializer is the bare value, not an ASSIGN. Unwrap
		// to the RHS so the 5th operand matches c2m (e.g. `I 7`).
		TokenBase *init_expr = tdecl->initialize;
		TokenAssign *as = dynamic_cast<TokenAssign *>(init_expr);
		if (as && as->right)
			init_expr = as->right;
		init_node = translate_expr(init_expr);
	}

	node_t spec_decl = simple(N_SPEC_DECL);
	append(spec_decl, share);
	append(spec_decl, var_decl_node);
	// A class instance with a user destructor gets RAII via the cleanup
	// attribute: the c2mir fork calls ClassName___dtor(&v) on every scope
	// exit. Only for a non-pointer value instance (a `Foo *p` pointer owns
	// nothing). The dtor is a user function `void Cls___dtor(struct Cls *)`,
	// and the cleanup mechanism passes &v (a `struct Cls *`) — a type match.
	node_t attr_node = ignore();
	{
		DataDefCLASS *cdd = (!is_ptr) ? as_user_class(base_dd) : NULL;
		if (cdd && class_needs_dtor(cdd)) {
			std::string dtor_sym = cdd->name + "___dtor";
			referenced_funcs.insert(dtor_sym);
			node_t attr_args = list();
			append(attr_args, id(dtor_sym.c_str(), origin));
			node_t attrs = list();
			append(attrs, node2(N_ATTR, id("cleanup", origin),
					    attr_args, origin));
			attr_node = attrs;
		}
	}
	append(spec_decl, attr_node);
	append(spec_decl, ignore());
	append(spec_decl, init_node);
	return spec_decl;
}

// Count pointer-indirection levels of a DataDef (int** -> 2, NODE -> 0).
static int dd_ptr_depth(DataDef *dd)
{
	int depth = 0;
	while (dd && dd->is_pointer()) {
		DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd);
		if (!p) break;
		dd = p->base_type;
		depth++;
	}
	return depth;
}

int CirBuilder::explicit_star_count(DataDef *full_type, const std::string &alias)
{
	if (alias.empty())
		return -1;
	int full_depth = dd_ptr_depth(full_type);
	int base_depth = 0;
	bool fnptr_alias = false;
	if (m_prog) {
		// Resolve the alias to the typedef's own DataDef so its base pointer
		// depth can be subtracted from the usage-site total.
		datatype_map_iter it = m_prog->datatype_map.find(alias);
		if (it != m_prog->datatype_map.end() && it->second) {
			base_depth = dd_ptr_depth(&it->second->definition);
			fnptr_alias = (dynamic_cast<DataDefFPTR *>(
					&it->second->definition) != NULL);
		}
	}
	int stars = full_depth - base_depth;
	if (stars < 0) stars = 0;
	// A function-typedef alias (`typedef void DO_FUN(args)`) names a bare
	// FUNCTION type, so a member/parameter/variable use of it is a
	// pointer-to-function and carries one '*' that the parser does NOT record
	// on the type: it keeps the bare DataDefFPTR (no DataDefPTR wrapper) so the
	// expression parser's fn-ptr-call detection — which keys on
	// `var.type` being DataDefFPTR — keeps working. fnptr_alias_stars adds the
	// missing star for a Form-1 function typedef (1) and none for a Form-2
	// pointer-to-function typedef (0, the pointer is already in the alias).
	// `DO_FUN fn` (a bare-function declaration) never reaches here — it is
	// represented as a FuncDef and emitted on the function path.
	if (fnptr_alias)
		stars += fnptr_alias_stars(alias);
	return stars;
}

node_t CirBuilder::member_node(const memberpair_t &m, DataDefSTRUCT *owner)
{
	DataDef *mtype = m.second;            // full member type, stars included
	const std::string &mtypedef = m.typedef_name;

	// Fixed-array member dimensions, outermost first. The struct stores these
	// in parallel arrays keyed by member name: member_dims for the explicit
	// multi-dim shape, falling back to member_counts (a single flat count) for
	// 1-D array members. A runtime-sized member (member_count_exprs != NULL)
	// is a flexible/VLA-ish member and contributes no constant dimension here.
	std::vector<uint32_t> mdims;
	if (owner) {
		std::string mname = m.first;
		if (owner->m_is_array_decl(mname) && !owner->m_count_expr(mname)) {
			const std::vector<uint32_t> *dv = owner->m_dims(mname);
			if (dv && !dv->empty()) {
				mdims = *dv;
			} else {
				size_t c = owner->m_count(mname);
				if (c >= 1) mdims.push_back((uint32_t)c);
			}
		}
	}

	// Function-pointer member (SMAUG's command/spell tables: `DO_FUN *do_fun`).
	// Without a typedef alias, expand the signature inline
	// (`void (*do_fun)(int,int)`); with one, emit the alias plus the explicit
	// pointer star(s) it carries beyond the alias (`DO_FUN *do_fun`). dtINT64
	// rawtype would otherwise emit a `long` (or, via the alias, a member of
	// bare function type) — both rejected/miscompiled by c2mir.
	if (DataDefFPTR *mfp = dynamic_cast<DataDefFPTR *>(mtype)) {
		node_t mspec;
		node_t mdl = list();
		if (!mtypedef.empty()) {
			mspec = type_list(mtype, mtypedef);
			for (size_t d = 0; d < mdims.size(); d++)
				append(mdl, node3(N_ARR, ignore(), list(), integer(mdims[d])));
			int mstars = fnptr_alias_stars(mtypedef);
			for (int s = 0; s < mstars; s++)
				append(mdl, pointer());
		} else {
			mspec = list();
			fnptr_decl_pieces(mfp->target, true, mspec, mdl, mdims);
		}
		node_t mmember = simple(N_MEMBER, m.origin);
		append(mmember, node1(N_SHARE, mspec));
		append(mmember, node2(N_DECL, id(m.first.c_str(), m.origin), mdl));
		append(mmember, ignore());
		append(mmember, ignore());
		return mmember;
	}

	// A std::string OBJECT member (not a pointer) embeds the opaque string
	// buffer in the struct: `long name[W];`. Same lowering as a local string
	// object (string_storage_decl) but as a struct member. Construction /
	// destruction of an embedded string member is driven by the enclosing
	// class's ctor/dtor, not the cleanup attribute (members have no
	// independent scope). String member access then reads through
	// string_cstr((void*)&obj.name) — handled at the use site.
	if (is_string_object(mtype)) {
		node_t mspec = list();
		append(mspec, simple(N_LONG));
		node_t mdl = list();
		append(mdl, node3(N_ARR, ignore(), list(),
				  integer((long)string_obj_words())));
		node_t mmember = simple(N_MEMBER, m.origin);
		append(mmember, node1(N_SHARE, mspec));
		append(mmember, node2(N_DECL, id(m.first.c_str(), m.origin), mdl));
		append(mmember, ignore());
		append(mmember, ignore());
		return mmember;
	}

	// Type specifier. A member declared via a typedef emits ID("alias")
	// regardless of pointer-ness — the stars belong on the declarator, not
	// the type spec (matches c2m). The star count is the explicit indirection
	// written at the usage site, i.e. the type's total depth minus the
	// typedef's own base depth (handles `typedef T *TP; TP x;` correctly).
	int stars = explicit_star_count(mtype, mtypedef);
	node_t mspec;
	if (!mtypedef.empty()) {
		mspec = type_list(mtype, mtypedef);
	} else {
		DataDef *mbase = mtype;
		if (mbase && mbase->is_pointer()) {
			DataDefPTR *mptr = dynamic_cast<DataDefPTR *>(mbase);
			if (mptr && mptr->base_type) mbase = mptr->base_type;
		}
		mspec = type_list(mbase);
	}

	node_t mshare = node1(N_SHARE, mspec);
	node_t mid = id(m.first.c_str(), m.origin);
	node_t mdecl_list = list();
	// c2m declarator order: in `T *m[N]` the `[]` binds tighter than `*`
	// (array of pointers), so the N_ARR dims must PRECEDE the pointer stars
	// (matches var_decl and c2mir's direct_declarator). Emitting the pointer
	// first instead yields `T (*m)[N]` — a pointer to an array — which
	// mistypes element assignments ("assignment of incompatible value").
	// Array member dimensions: short learned[16] -> ARR(16).
	for (size_t d = 0; d < mdims.size(); d++)
		append(mdecl_list, node3(N_ARR, ignore(), list(), integer(mdims[d])));
	if (!mtypedef.empty()) {
		for (int s = 0; s < stars; s++)
			append(mdecl_list, pointer());
	} else if (mtype && mtype->is_pointer()) {
		append(mdecl_list, pointer());
	}
	node_t mdecl = node2(N_DECL, mid, mdecl_list);

	node_t member = simple(N_MEMBER, m.origin);
	append(member, mshare);
	append(member, mdecl);
	append(member, ignore());
	append(member, ignore());
	return member;
}

node_t CirBuilder::struct_def(DataDefSTRUCT *sdd)
{
	node_t struct_id = id(sdd->name.c_str());
	node_t member_list = list();

	for (size_t i = 0; i < sdd->members.size(); i++) {
		append(member_list, member_node(sdd->members[i], sdd));
	}

	node_t struct_node = node2(N_STRUCT, struct_id, member_list);
	node_t tl = node1(N_LIST, struct_node);

	node_t spec_decl = simple(N_SPEC_DECL);
	append(spec_decl, tl);
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	return spec_decl;
}

node_t CirBuilder::class_vtable_def(DataDefCLASS *cdd)
{
	if (!cdd || !cdd->has_vtable || cdd->vtable_slots.empty())
		return NULL;

	// Initializer: { (void*)C__slot0, (void*)C__slot1, ... }
	node_t inits = list();
	for (const std::string &slot : cdd->vtable_slots) {
		std::string sname = slot;
		Variable *mv = cdd->findMethod(sname);
		if (!mv) {
			// No override visible: a pure/abstract slot. Emit a null
			// pointer (calling it is UB, same as C++ abstract dispatch).
			append(inits, node2(N_INIT, list(), integer(0)));
			continue;
		}
		referenced_funcs.insert(mv->name);
		// (void*)C__slot
		node_t vptr_type = node2(N_TYPE,
			node1(N_LIST, simple(N_VOID)),
			node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		node_t fnref = node2(N_CAST, vptr_type, id(mv->name.c_str()));
		append(inits, node2(N_INIT, list(), fnref));
	}

	// void *ClassName__vtable[] = { ... };
	std::string vname = cdd->name + "__vtable";
	node_t spec = list();
	append(spec, simple(N_VOID));
	node_t dl = list();
	append(dl, node3(N_ARR, ignore(), list(), ignore())); // [] (sized by init)
	append(dl, pointer());
	node_t decl = node2(N_DECL, id(vname.c_str()), dl);
	node_t sd = simple(N_SPEC_DECL);
	append(sd, node1(N_SHARE, spec));
	append(sd, decl);
	append(sd, ignore());
	append(sd, ignore());
	append(sd, inits);
	return sd;
}

node_t CirBuilder::class_struct_def(DataDefCLASS *cdd)
{
	node_t struct_id = id(cdd->name.c_str());
	node_t member_list = list();

	// Virtual classes carry a hidden vtable pointer at offset 0. Emit it
	// first so the C struct layout matches the parser's offsets (which it
	// shifted by 8 to reserve the slot). The parser flattens base *data*
	// members into the derived class but NOT the synthetic __vptr (it is not
	// a real member entry), so every has_vtable class needs __vptr emitted
	// here at offset 0 — including derived classes.
	if (cdd->has_vtable) {
		node_t vptr_spec = list();
		append(vptr_spec, simple(N_VOID));
		node_t vptr_dl = list();
		append(vptr_dl, pointer());
		node_t vptr_member = simple(N_MEMBER);
		append(vptr_member, node1(N_SHARE, vptr_spec));
		append(vptr_member, node2(N_DECL, id("__vptr"), vptr_dl));
		append(vptr_member, ignore());
		append(vptr_member, ignore());
		append(member_list, vptr_member);
	}

	for (size_t i = 0; i < cdd->members.size(); i++)
		append(member_list, member_node(cdd->members[i], cdd));

	node_t struct_node = node2(N_STRUCT, struct_id, member_list);
	node_t tl = node1(N_LIST, struct_node);

	node_t spec_decl = simple(N_SPEC_DECL);
	append(spec_decl, tl);
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	return spec_decl;
}

// Resolve the class behind a (possibly pointer-wrapped) DataDef.
static DataDefCLASS *class_behind(DataDef *dd)
{
	if (!dd) return NULL;
	if (DataDefCLASS *c = as_user_class(dd)) return c;
	if (DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd))
		return as_user_class(p->base_type);
	return NULL;
}

node_t CirBuilder::class_this_arg(TokenMember *tm, DataDefCLASS *&recv_class,
				  TokenBase *origin)
{
	recv_class = NULL;
	// The receiver is either a chained sub-expression (parent_expr) or a bare
	// object variable. Determine its declared type to tell a value receiver
	// (`.`, needs &obj) from a pointer receiver (`->`, pass the pointer).
	DataDef *recv_type;
	node_t recv_node;
	if (tm->parent_expr) {
		recv_type = tm->parent_expr->datadef();
		recv_node = translate_expr(tm->parent_expr);
	} else {
		recv_type = tm->object.type;
		recv_node = id(tm->object.name.c_str(), origin);
	}
	recv_class = class_behind(recv_type);
	if (!recv_class) return NULL;
	bool recv_is_ptr = recv_type && recv_type->is_pointer();
	// Value receiver -> &obj; pointer receiver -> obj.
	return recv_is_ptr ? recv_node : node1(N_ADDR, recv_node, origin);
}

node_t CirBuilder::class_method_call(TokenMember *tm, TokenBase *origin)
{
	DataDefCLASS *recv_class = NULL;
	node_t this_arg = class_this_arg(tm, recv_class, origin);
	if (!recv_class) return NULL;

	// The method's mangled symbol (`ClassName__method`) and signature.
	const std::string &sym = tm->var.name;
	FuncDef *callee = dynamic_cast<FuncDef *>(tm->var.type);

	// An inherited method's __this is typed `Base *`, but the receiver is a
	// `Derived *`. Since base members are flattened at offset 0 (single
	// inheritance), the address is identical — cast the __this pointer to the
	// method's declared owner-class type so c2mir doesn't warn/mistype.
	DataDefCLASS *owner = (callee && !callee->parameters.empty())
				? class_behind(callee->parameters[0]) : NULL;
	if (owner && owner != recv_class)
		this_arg = node2(N_CAST,
			node2(N_TYPE,
			      node1(N_LIST, node2(N_STRUCT, id(owner->name.c_str()),
						  ignore())),
			      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
			this_arg, origin);

	// Build the argument list: hidden __this first, then the explicit args.
	// Coerce each explicit arg to its declared parameter shape (string object
	// / numeric reference), mirroring the free-function call path. The
	// callee's parameter 0 is __this, so explicit arg i maps to parameter i+1.
	node_t args = list();
	append(args, this_arg);
	for (size_t i = 0; i < tm->parameters.size(); i++) {
		TokenBase *arg = tm->parameters[i];
		size_t pi = i + 1;   // +1 to skip __this
		DataDef *pt = (callee && pi < callee->parameters.size())
				? callee->parameters[pi] : NULL;
		DataType pdt = pt ? pt->rawtype() : DataType::dtVOID;
		bool is_ref_param = callee && pi < callee->ref_params.size()
				    && callee->ref_params[pi];
		if (pt && (pdt == DataType::dtSTRING || pdt == DataType::dtSTRINGref))
			append(args, string_obj_arg(arg));
		else if (is_ref_param)
			append(args, node1(N_ADDR, translate_expr(arg), arg));
		else
			append(args, translate_expr(arg));
	}

	// Virtual dispatch: a method declared (or inherited as) virtual is called
	// indirectly through the receiver's vtable rather than by name, so the
	// most-derived override runs. Lower to:
	//   ( (RET(*)(struct Owner*, params))
	//       ((void**)recv->__vptr)[slot] )(args...)
	// recv->__vptr is loaded from a freshly-derived receiver pointer (the
	// this_arg node above is already consumed as the call's first argument).
	std::string mname = (sym.size() > recv_class->name.size() + 2)
				? sym.substr(recv_class->name.size() + 2) : sym;
	int slot = recv_class->vtable_slot(mname);
	if (recv_class->is_virtual_method(mname) && slot >= 0 && callee) {
		DataDefCLASS *vowner = owner ? owner : recv_class;
		// Fresh receiver pointer for the vptr load.
		DataDefCLASS *dummy = NULL;
		node_t recv_for_vptr = class_this_arg(tm, dummy, origin);
		if (vowner && vowner != recv_class)
			recv_for_vptr = node2(N_CAST,
				node2(N_TYPE,
				      node1(N_LIST, node2(N_STRUCT,
					id(vowner->name.c_str()), ignore())),
				      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
				recv_for_vptr, origin);
		// (void**)recv->__vptr
		node_t vptr = node2(N_DEREF_FIELD, recv_for_vptr,
				    id("__vptr", origin));
		// Build the `void **` cast type explicitly (two pointer levels).
		node_t vpp_dl = list();
		append(vpp_dl, pointer());
		append(vpp_dl, pointer());
		node_t vpp_type = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
					node2(N_DECL, ignore(), vpp_dl));
		node_t vtab = node2(N_CAST, vpp_type, vptr, origin);
		// ((void**)...)[slot]
		node_t slotref = node2(N_IND, vtab, integer(slot, origin), origin);
		// Cast the slot to the method's function-pointer type.
		node_t fnptr_type = method_fnptr_type(callee, vowner);
		node_t fn = node2(N_CAST, fnptr_type, slotref, origin);
		return node2(N_CALL, fn, args, origin);
	}

	referenced_funcs.insert(sym);
	return node2(N_CALL, id(sym.c_str(), origin), args, origin);
}

bool CirBuilder::class_has_object_members(DataDefCLASS *cdd)
{
	if (!cdd) return false;
	for (const auto &m : cdd->members)
		if (is_string_object(m.second)) return true;
	return false;
}

// Shared body for member construct/destruct: emit `sym((void*)&__this->m)`
// expression-statements for each embedded string-object member.
static const char *MEMBER_CTOR_SYM = "string_construct";
static const char *MEMBER_DTOR_SYM = "string_destruct";

bool CirBuilder::class_member_construct(DataDefCLASS *cdd,
					std::vector<node_t> &out, TokenBase *origin)
{
	if (!cdd) return false;
	bool any = false;
	for (const auto &m : cdd->members) {
		if (!is_string_object(m.second)) continue;
		need_output_extern(MEMBER_CTOR_SYM, true, { { {N_VOID}, true } });
		node_t fld = node2(N_DEREF_FIELD, id("__this", origin),
				   id(m.first.c_str(), origin));
		node_t addr = node2(N_CAST, void_ptr_type(),
				    node1(N_ADDR, fld, origin), origin);
		node_t a = list();
		append(a, addr);
		node_t call = node2(N_CALL, id(MEMBER_CTOR_SYM, origin), a, origin);
		out.push_back(node2(N_EXPR, list(), call, origin));
		any = true;
	}
	return any;
}

bool CirBuilder::class_member_destruct(DataDefCLASS *cdd,
				       std::vector<node_t> &out, TokenBase *origin)
{
	if (!cdd) return false;
	bool any = false;
	// Destruct in reverse declaration order (mirrors C++ member teardown).
	for (size_t i = cdd->members.size(); i-- > 0; ) {
		const auto &m = cdd->members[i];
		if (!is_string_object(m.second)) continue;
		need_output_extern(MEMBER_DTOR_SYM, false, { { {N_VOID}, true } });
		node_t fld = node2(N_DEREF_FIELD, id("__this", origin),
				   id(m.first.c_str(), origin));
		node_t addr = node2(N_CAST, void_ptr_type(),
				    node1(N_ADDR, fld, origin), origin);
		node_t a = list();
		append(a, addr);
		node_t call = node2(N_CALL, id(MEMBER_DTOR_SYM, origin), a, origin);
		out.push_back(node2(N_EXPR, list(), call, origin));
		any = true;
	}
	return any;
}

bool CirBuilder::class_needs_dtor(DataDefCLASS *cdd)
{
	if (!cdd) return false;
	if (cdd->has_user_dtor) return true;
	if (class_has_object_members(cdd)) return true;
	if (cdd->base_class && class_needs_dtor(cdd->base_class)) return true;
	return false;
}

std::string CirBuilder::class_dtor_symbol(DataDefCLASS *cdd)
{
	return cdd ? cdd->name + "___dtor" : std::string();
}

void CirBuilder::class_instance_member_ctors(const char *inst,
					     DataDefCLASS *cdd, node_t items,
					     TokenBase *origin)
{
	if (!cdd) return;
	for (const auto &m : cdd->members) {
		if (!is_string_object(m.second)) continue;
		need_output_extern(MEMBER_CTOR_SYM, true, { { {N_VOID}, true } });
		// string_construct((void*)&inst.member)
		node_t fld = node2(N_FIELD, id(inst, origin),
				   id(m.first.c_str(), origin));
		node_t addr = node2(N_CAST, void_ptr_type(),
				    node1(N_ADDR, fld, origin), origin);
		node_t a = list();
		append(a, addr);
		node_t call = node2(N_CALL, id(MEMBER_CTOR_SYM, origin), a, origin);
		append(items, node2(N_EXPR, list(), call, origin));
	}
}

node_t CirBuilder::synth_dtor_def(DataDefCLASS *cdd)
{
	if (!cdd || cdd->has_user_dtor || !class_needs_dtor(cdd))
		return NULL;

	// void Class___dtor(struct Class *__this)
	node_t ret_type = node1(N_LIST, simple(N_VOID));
	// A NAMED parameter is an N_SPEC_DECL (matches param_decl); an N_TYPE is
	// only for unnamed/abstract params and would lose the __this name.
	node_t pspec = node1(N_LIST, node2(N_STRUCT, id(cdd->name.c_str()), ignore()));
	node_t param = simple(N_SPEC_DECL);
	append(param, pspec);
	append(param, node2(N_DECL, id("__this"), node1(N_LIST, pointer())));
	append(param, ignore());
	append(param, ignore());
	append(param, ignore());
	node_t param_list = list();
	append(param_list, param);
	node_t func_inner = node1(N_FUNC, param_list);
	node_t decl = node2(N_DECL, id(class_dtor_symbol(cdd).c_str()),
			    node1(N_LIST, func_inner));

	// Body: destruct object members (reverse order), then base dtor.
	std::vector<node_t> stmts;
	class_member_destruct(cdd, stmts, NULL);
	if (cdd->base_class && class_needs_dtor(cdd->base_class)) {
		std::string bsym = class_dtor_symbol(cdd->base_class);
		referenced_funcs.insert(bsym);
		node_t bt = node2(N_TYPE,
			node1(N_LIST, node2(N_STRUCT,
				id(cdd->base_class->name.c_str()), ignore())),
			node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		node_t bcast = node2(N_CAST, bt, id("__this"));
		node_t a = list();
		append(a, bcast);
		node_t call = node2(N_CALL, id(bsym.c_str()), a);
		stmts.push_back(node2(N_EXPR, list(), call));
	}
	node_t items = list();
	for (node_t s : stmts) append(items, s);
	node_t body = node2(N_BLOCK, list(), items);
	return node4(N_FUNC_DEF, ret_type, decl, list(), body);
}

node_t CirBuilder::method_fnptr_type(FuncDef *callee, DataDefCLASS *owner)
{
	// Specs = return type; decl suffixes = [POINTER, FUNC(params), ret*].
	// fnptr_decl_pieces emits the callee's full parameter list (param 0 is the
	// owner-* __this), which is exactly the dynamic call signature.
	node_t spec_list = list();
	node_t decl_list = list();
	fnptr_decl_pieces(callee, true, spec_list, decl_list,
			  std::vector<uint32_t>());
	(void)owner;
	return node2(N_TYPE, spec_list, node2(N_DECL, ignore(), decl_list));
}

node_t CirBuilder::class_ctor_call(Variable *v, DataDefCLASS *cdd,
				   const std::vector<TokenBase *> &ctor_args,
				   TokenBase *origin)
{
	if (!v || !cdd || !cdd->has_user_ctor) return NULL;

	std::string sym = cdd->name + "__" + cdd->name;   // ClassName__ClassName
	// Resolve the ctor's signature for argument coercion (param 0 = __this).
	FuncDef *ctor = NULL;
	auto it = cdd->method_map.find(cdd->name);
	if (it != cdd->method_map.end() && it->second)
		ctor = dynamic_cast<FuncDef *>(it->second->type);

	node_t args = list();
	append(args, node1(N_ADDR, id(v->name.c_str(), origin), origin)); // &v
	for (size_t i = 0; i < ctor_args.size(); i++) {
		TokenBase *arg = ctor_args[i];
		size_t pi = i + 1;   // skip __this
		DataDef *pt = (ctor && pi < ctor->parameters.size())
				? ctor->parameters[pi] : NULL;
		DataType pdt = pt ? pt->rawtype() : DataType::dtVOID;
		bool is_ref_param = ctor && pi < ctor->ref_params.size()
				    && ctor->ref_params[pi];
		if (pt && (pdt == DataType::dtSTRING || pdt == DataType::dtSTRINGref))
			append(args, string_obj_arg(arg));
		else if (is_ref_param)
			append(args, node1(N_ADDR, translate_expr(arg), arg));
		else
			append(args, translate_expr(arg));
	}
	referenced_funcs.insert(sym);
	node_t call = node2(N_CALL, id(sym.c_str(), origin), args, origin);
	return node2(N_EXPR, list(), call, origin);
}

// Map a binary TokenID to its C++ operator spelling (the method-name suffix
// the parser stored, e.g. tkEquals -> "==" -> method "operator=="). Returns
// "" for operators madc does not currently support overloading.
static const char *binop_overload_symbol(TokenID id)
{
	switch (id) {
	case TokenID::tkEquals: return "==";
	case TokenID::tkNotEq:  return "!=";
	case TokenID::tkLT:     return "<";
	case TokenID::tkGT:     return ">";
	case TokenID::tkLE:     return "<=";
	case TokenID::tkGE:     return ">=";
	case TokenID::tkAdd:    return "+";
	case TokenID::tkSub:    return "-";
	case TokenID::tkMul:    return "*";
	case TokenID::tkDiv:    return "/";
	case TokenID::tkMod:    return "%";
	default:                return "";
	}
}

node_t CirBuilder::class_operator_call(TokenOperator *top, TokenBase *origin)
{
	if (!top || !top->left || !top->right) return NULL;
	DataDefCLASS *lcls = as_user_class(top->left->datadef());
	if (!lcls) return NULL;
	const char *opsym = binop_overload_symbol(top->id());
	if (!opsym[0]) return NULL;

	std::string mname = std::string("operator") + opsym;
	Variable *mv = lcls->findMethod(mname);
	if (!mv) return NULL;
	FuncDef *callee = dynamic_cast<FuncDef *>(mv->type);

	std::string sym = lcls->name + "__" + mname;   // ClassName__operator==
	node_t args = list();
	append(args, node1(N_ADDR, translate_expr(top->left), origin));  // &lhs
	// Single explicit RHS argument (operator parameter 1; param 0 = __this).
	{
		DataDef *pt = (callee && callee->parameters.size() > 1)
				? callee->parameters[1] : NULL;
		DataType pdt = pt ? pt->rawtype() : DataType::dtVOID;
		bool refp = callee && callee->ref_params.size() > 1
			    && callee->ref_params[1];
		if (pt && (pdt == DataType::dtSTRING || pdt == DataType::dtSTRINGref))
			append(args, string_obj_arg(top->right));
		else if (refp)
			append(args, node1(N_ADDR, translate_expr(top->right), top->right));
		else
			append(args, translate_expr(top->right));
	}
	referenced_funcs.insert(sym);
	return node2(N_CALL, id(sym.c_str(), origin), args, origin);
}

node_t CirBuilder::typedef_decl(const std::string &alias, DataDef *dd,
				const std::set<std::string> &emitted_structs,
				bool force_incomplete_struct)
{
	if (!dd) return NULL;

	// Function typedef `typedef void DO_FUN(int,int)` (dd = FuncDef) and
	// pointer-to-function typedef `typedef int (*UNOP)(int)` (dd = DataDefFPTR).
	// Both have dtINT64 rawtype and would otherwise emit `typedef long NAME`,
	// erasing the signature; build `typedef ret NAME(params)` /
	// `typedef ret (*NAME)(params)` from the target instead.
	{
		DataDefFPTR *fp_td = dynamic_cast<DataDefFPTR *>(dd);
		FuncDef *fn_td = fp_td ? fp_td->target : dynamic_cast<FuncDef *>(dd);
		if (fn_td) {
			// A Form-1 function typedef `typedef RET NAME(params)` emits no
			// pointer (so `NAME f` is a function decl, `NAME *p` a pointer);
			// a Form-2 pointer typedef `typedef RET (*NAME)(params)` keeps it.
			bool td_ptr = fp_td ? fp_td->ptr_syntax : false;
			node_t sl = list();
			append(sl, simple(N_TYPEDEF));
			node_t dl = list();
			fnptr_decl_pieces(fn_td, td_ptr, sl, dl,
					  std::vector<uint32_t>());
			node_t share = node1(N_SHARE, sl);
			node_t decl = node2(N_DECL, id(alias.c_str()), dl);
			node_t spec_decl = simple(N_SPEC_DECL);
			append(spec_decl, share);
			append(spec_decl, decl);
			append(spec_decl, ignore());
			append(spec_decl, ignore());
			append(spec_decl, ignore());
			return spec_decl;
		}
	}

	node_t tl = list();
	append(tl, simple(N_TYPEDEF));

	// Peel any fixed-array layers: `typedef T NAME[2][3]` carries the element
	// type in DataDefCArray::element_type and the dims as nested CArrays. The
	// element type drives the spec list; the dims become N_ARR declarator
	// suffixes below. Without this the CArray's dtRESERVED rawtype defaults to
	// `int` and the dimensions are dropped entirely.
	std::vector<uint32_t> arr_dims;
	dd = peel_carray_dims(dd, arr_dims);

	// Unwrap pointer for base type
	DataDef *base_dd = dd;
	bool is_ptr = dd->is_pointer();
	DataDefPTR *ptr_dd = is_ptr ? dynamic_cast<DataDefPTR *>(dd) : NULL;
	if (ptr_dd && ptr_dd->base_type)
		base_dd = ptr_dd->base_type;

	// Type specifier
	if (base_dd->is_struct() && !base_dd->is_complex()) {
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(base_dd);
		if (sdd) {
			if (force_incomplete_struct || emitted_structs.count(sdd->name) || sdd->members.empty()) {
				// Forward reference / already-emitted / incomplete:
				// STRUCT(tag, IGNORE). The full body is emitted at the
				// struct's own definition point.
				append(tl, node2(N_STRUCT, id(sdd->name.c_str()), ignore()));
			} else {
				// Emit struct definition inline (typedef struct { ... } NAME)
				node_t struct_id_node = id(sdd->name.c_str());
				node_t ml = list();
				for (size_t i = 0; i < sdd->members.size(); i++) {
					append(ml, member_node(sdd->members[i], sdd));
				}
				append(tl, node2(N_STRUCT, struct_id_node, ml));
			}
		}
	} else {
		append_type_specs(tl, base_dd);
	}

	node_t share = node1(N_SHARE, tl);

	// Declarator: DECL(ID("alias"), LIST([POINTER]))
	node_t alias_id = id(alias.c_str());
	node_t decl_list = list();
	if (is_ptr)
		append(decl_list, pointer());
	// Fixed-array dimensions, outermost first: T NAME[2][3] -> ARR(2) ARR(3).
	for (size_t d = 0; d < arr_dims.size(); d++)
		append(decl_list, node3(N_ARR, ignore(), list(), integer(arr_dims[d])));

	node_t decl = node2(N_DECL, alias_id, decl_list);

	node_t spec_decl = simple(N_SPEC_DECL);
	append(spec_decl, share);
	append(spec_decl, decl);
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	return spec_decl;
}

node_t CirBuilder::func_proto(TokenFunc *tf)
{
	FuncDef *fd = dynamic_cast<FuncDef *>(tf->var.type);
	if (!fd) return NULL;

	DataDef *ret_dd = &fd->returns;
	bool ret_is_ptr = ret_dd && ret_dd->is_pointer();
	DataDefPTR *ret_ptr = ret_is_ptr ? dynamic_cast<DataDefPTR *>(ret_dd) : NULL;
	if (ret_ptr && ret_ptr->base_type) ret_dd = ret_ptr->base_type;

	node_t ret_type = type_list(ret_dd);
	node_t share = node1(N_SHARE, ret_type);

	// Parameters. A variadic function carries a trailing synthetic param
	// (the parser pushes a ddINT64 placeholder when it sees `...`); drop it
	// and emit N_DOTS instead so the prototype is truly variadic.
	node_t param_list = list();
	size_t nparam = fd->parameters.size();
	if (fd->is_varargs && nparam > 0) nparam--;
	if (nparam == 0 && !fd->is_varargs) {
		node_t void_spec = node1(N_LIST, simple(N_VOID));
		node_t void_decl = node2(N_DECL, ignore(), list());
		node_t void_param = node2(N_TYPE, void_spec, void_decl);
		append(param_list, void_param);
	} else {
		for (size_t i = 0; i < nparam; i++) {
			DataDef *ptype = fd->parameters[i];
			const char *pname = "p";
			std::string ptypedef;
			if (tf->method && i < tf->method->parameters.size()) {
				pname = tf->method->parameters[i]->name.c_str();
				ptypedef = tf->method->parameters[i]->typedef_name;
			}
			append(param_list, param_decl(ptype, pname, ptypedef));
		}
	}
	if (fd->is_varargs)
		append(param_list, simple(N_DOTS));

	node_t func_inner = node1(N_FUNC, param_list);

	node_t func_id = id(tf->var.name.c_str(), tf);
	node_t decl_list = list();
	append(decl_list, func_inner);
	if (ret_is_ptr)
		append(decl_list, pointer());

	node_t decl = node2(N_DECL, func_id, decl_list);

	node_t proto = simple(N_SPEC_DECL);
	append(proto, share);
	append(proto, decl);
	append(proto, ignore());
	append(proto, ignore());
	append(proto, ignore());
	return proto;
}

// -----------------------------------------------------------------------
// Expression translation
// -----------------------------------------------------------------------

node_t CirBuilder::translate_expr(TokenBase *tb)
{
	if (!tb) return ignore();

	// Integer literal
	if (tb->type() == TokenType::ttInteger)
		return integer(tb->ival(), tb);

	// Real literal
	if (tb->type() == TokenType::ttReal)
		return real(tb->dval(), tb);

	// Char literal (C char constants emit N_CH; value via ival())
	if (tb->type() == TokenType::ttChar)
		return ch(tb->ival(), tb);

	// new ClassName(args) -> a statement expression that allocates zeroed
	// storage, constructs in place, and yields the typed pointer:
	//   ({ struct C *__newN = (struct C*)calloc(1, sizeof(struct C));
	//      C__C(__newN, args); __newN; })
	// (No ctor call when the class has no user constructor — calloc already
	// zero-initializes, matching value-init of a POD.)
	if (TokenNEW *tn = dynamic_cast<TokenNEW *>(tb)) {
		DataDefCLASS *cdd = tn->alloc_class;
		if (!cdd) return error_node("new without a class type", tb);
		char tmp[32];
		snprintf(tmp, sizeof(tmp), "__new%d", m_strtmp_counter++);
		// struct C * type node, reused for the decl and the cast.
		auto ptr_type = [&]() -> node_t {
			return node2(N_TYPE,
				node1(N_LIST, node2(N_STRUCT,
					id(cdd->name.c_str()), ignore())),
				node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		};
		auto struct_type = [&]() -> node_t {
			return node2(N_TYPE,
				node1(N_LIST, node2(N_STRUCT,
					id(cdd->name.c_str()), ignore())),
				node2(N_DECL, ignore(), list()));
		};
		// calloc(1, sizeof(struct C)) -> void*. Declare the prototype so
		// c2mir does not default it to an implicit int return (which would
		// truncate the 64-bit pointer to 32 bits and crash on deref).
		need_output_extern("calloc", true,
			{ { {N_UNSIGNED, N_LONG}, false },
			  { {N_UNSIGNED, N_LONG}, false } });
		node_t szof = node1(N_SIZEOF, struct_type(), tb);
		node_t calloc_args = list();
		append(calloc_args, integer(1, tb));
		append(calloc_args, szof);
		node_t calloc_call = node2(N_CALL, id("calloc", tb), calloc_args, tb);
		node_t cast_alloc = node2(N_CAST, ptr_type(), calloc_call, tb);
		// struct C *__newN = (struct C*)calloc(...);
		node_t decl = simple(N_SPEC_DECL);
		append(decl, node1(N_SHARE, node1(N_LIST,
			node2(N_STRUCT, id(cdd->name.c_str()), ignore()))));
		append(decl, node2(N_DECL, id(tmp, tb), node1(N_LIST, pointer())));
		append(decl, ignore());
		append(decl, ignore());
		append(decl, cast_alloc);
		node_t items = list();
		append(items, decl);
		// C__C(__newN, args)  (the pointer is already __this-shaped)
		if (cdd->has_user_ctor) {
			std::string csym = cdd->name + "__" + cdd->name;
			FuncDef *ctor = NULL;
			auto it = cdd->method_map.find(cdd->name);
			if (it != cdd->method_map.end() && it->second)
				ctor = dynamic_cast<FuncDef *>(it->second->type);
			referenced_funcs.insert(csym);
			node_t cargs = list();
			append(cargs, id(tmp, tb));
			for (size_t i = 0; i < tn->ctor_args.size(); i++) {
				TokenBase *arg = tn->ctor_args[i];
				size_t pi = i + 1;
				DataDef *pt = (ctor && pi < ctor->parameters.size())
						? ctor->parameters[pi] : NULL;
				DataType pdt = pt ? pt->rawtype() : DataType::dtVOID;
				bool refp = ctor && pi < ctor->ref_params.size()
					    && ctor->ref_params[pi];
				if (pt && (pdt == DataType::dtSTRING
					   || pdt == DataType::dtSTRINGref))
					append(cargs, string_obj_arg(arg));
				else if (refp)
					append(cargs, node1(N_ADDR, translate_expr(arg), arg));
				else
					append(cargs, translate_expr(arg));
			}
			node_t ccall = node2(N_CALL, id(csym.c_str(), tb), cargs, tb);
			append(items, node2(N_EXPR, list(), ccall, tb));
		}
		// __newN;  (value of the statement expression)
		append(items, node2(N_EXPR, list(), id(tmp, tb), tb));
		node_t block = node2(N_BLOCK, list(), items, tb);
		return node1(N_STMTEXPR, block, tb);
	}

	// delete ptr -> ({ C___dtor(ptr); free(ptr); }) — dtor only when the
	// class has a user destructor; free always. Evaluated for effect, no
	// value (delete is a void expression).
	if (TokenDELETE *tdl = dynamic_cast<TokenDELETE *>(tb)) {
		node_t ptr = translate_expr(tdl->expr);
		node_t items = list();
		DataDefCLASS *cdd = tdl->del_class;
		if (cdd && cdd->has_user_dtor) {
			std::string dsym = cdd->name + "___dtor";
			referenced_funcs.insert(dsym);
			node_t dargs = list();
			append(dargs, ptr);
			node_t dcall = node2(N_CALL, id(dsym.c_str(), tb), dargs, tb);
			append(items, node2(N_EXPR, list(), dcall, tb));
			// Re-translate the pointer for free() — a fresh expression node
			// (the same node must not appear twice in the tree).
			ptr = translate_expr(tdl->expr);
		}
		need_output_extern("free", false, { { {N_VOID}, true } });
		node_t fargs = list();
		append(fargs, ptr);
		node_t fcall = node2(N_CALL, id("free", tb), fargs, tb);
		append(items, node2(N_EXPR, list(), fcall, tb));
		// A statement expression needs a trailing value expression; use 0.
		append(items, node2(N_EXPR, list(), integer(0, tb), tb));
		node_t block = node2(N_BLOCK, list(), items, tb);
		return node1(N_STMTEXPR, block, tb);
	}

	// String literal
	if (tb->type() == TokenType::ttString) {
		TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
		if (ti)
			return str(ti->str.c_str(), ti->str.size() + 1, tb);
	}

	// Variable reference
	if (tb->type() == TokenType::ttVariable) {
		TokenVar *tv = dynamic_cast<TokenVar *>(tb);
		if (tv) {
			if (tv->var.is_constant() && tv->var.type &&
			    tv->var.type->is_integer() && !tv->var.type->is_pointer())
				return integer(tv->var.get<int64_t>(), tb);
			if (tv->var.name.compare(0, 11, "__literal__") == 0) {
				const std::string &content = tv->var.name.substr(11);
				return str(content.c_str(), content.size() + 1, tb);
			}
			// Record file-scope (non-local, non-param) references so
			// translate_module can emit extern decls for libc globals
			// (stderr/stdout/stdin) that were registered lazily and never
			// reached top_decls. Locals/params already have in-scope decls.
			if (!(tv->var.flags & vfLOCAL) && !(tv->var.flags & vfPARAM))
				referenced_globals[tv->var.name] = &tv->var;
			// A numeric reference parameter (`int &x`) is lowered to a
			// pointer parameter (`int *x`) by the parser, with vfREFERENCE
			// set for auto-deref. Every value use of the reference reads
			// through the pointer: `x` -> `(*x)`. (String references are a
			// separate object-pointer path handled elsewhere.)
			if ((tv->var.flags & vfREFERENCE) && tv->var.type
			    && tv->var.type->is_pointer())
				return node1(N_DEREF, id(tv->var.name.c_str(), tb), tb);
			return id(tv->var.name.c_str(), tb);
		}
	}

	// Identifier
	if (tb->type() == TokenType::ttIdentifier) {
		TokenIdent *ti = dynamic_cast<TokenIdent *>(tb);
		if (ti)
			return id(ti->str.c_str(), tb);
	}

	// Ternary
	{
		TokenTerQ *tq = dynamic_cast<TokenTerQ *>(tb);
		if (tq)
			return node3(N_COND,
				translate_expr(tq->condition),
				translate_expr(tq->true_expr),
				translate_expr(tq->false_expr), tb);
	}

	// Address-of variable
	{
		TokenAddrOf *ta = dynamic_cast<TokenAddrOf *>(tb);
		if (ta)
			return node1(N_ADDR, id(ta->var.name.c_str(), tb), tb);
	}

	// Address-of expression
	{
		TokenAddrExpr *tae = dynamic_cast<TokenAddrExpr *>(tb);
		if (tae)
			return node1(N_ADDR, translate_expr(tae->expr), tb);
	}

	// Dereference variable
	{
		TokenDeref *td = dynamic_cast<TokenDeref *>(tb);
		if (td)
			return node1(N_DEREF, id(td->var.name.c_str(), tb), tb);
	}

	// Dereference expression
	{
		TokenDerefExpr *tde = dynamic_cast<TokenDerefExpr *>(tb);
		if (tde)
			return node1(N_DEREF, translate_expr(tde->expr), tb);
	}

	// Dereference of a post-incremented/decremented pointer variable:
	// `*p++` / `*p--` -> DEREF(POST_INC|POST_DEC(id(p))). The parser builds a
	// TokenDerefStep for this idiom; without a case here it fell through to the
	// error/IGNORE path, producing an empty operand (`( = ( == ))`).
	{
		TokenDerefStep *tds = dynamic_cast<TokenDerefStep *>(tb);
		if (tds) {
			node_t step = node1(tds->increment ? N_POST_INC : N_POST_DEC,
					    id(tds->var.name.c_str(), tb), tb);
			return node1(N_DEREF, step, tb);
		}
	}

	// Array subscript on a named variable: name[i] (+ multi-dim extras)
	{
		TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(tb);
		if (tsub) {
			// STL container subscript READ (`nums[i]`, `ages[k]`): lower to
			// the runtime getter. A WRITE (`c[i] = v`) is intercepted earlier
			// on the assignment path; this read path is the rvalue use.
			node_t cread = container_subscript_read(tsub, tb);
			if (cread) return cread;
			// A subscript on a lifted string literal (`"X"[0]`) keeps the
			// synthetic `__literal__X` variable as its object. Emit the string
			// literal itself, not a reference to an undefined symbol.
			node_t base;
			if (tsub->object.name.compare(0, 11, "__literal__") == 0) {
				const std::string &content = tsub->object.name.substr(11);
				base = str(content.c_str(), content.size() + 1, tb);
			} else {
				base = id(tsub->object.name.c_str(), tb);
			}
			node_t n = node2(N_IND, base,
				translate_expr(tsub->index), tb);
			// Multi-dim fixed array: a[i][j] -> IND(IND(a,i),j)
			for (size_t k = 0; k < tsub->extra_indices.size(); k++)
				n = node2(N_IND, n, translate_expr(tsub->extra_indices[k]), tb);
			return n;
		}
	}

	// Array subscript on a sub-expression: expr[i] == IND(expr, i)
	{
		TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(tb);
		if (tse)
			return node2(N_IND,
				translate_expr(tse->base_expr),
				translate_expr(tse->index), tb);
	}

	// std::string method call on a string-object receiver
	// (s.c_str()/.length()/.size()/.empty()/.clear()). A TokenCallMethod
	// IS-A TokenMember, so this must run before the generic member-access
	// handler below — otherwise `s.c_str()` mis-lowers to a bare field
	// access `s.c_str`. string_method_call returns NULL for anything it
	// doesn't recognize, so non-string method calls fall through.
	if (tb->type() == TokenType::ttCallMethod) {
		TokenMember *tcm = dynamic_cast<TokenMember *>(tb);
		if (tcm) {
			node_t lowered = string_method_call(tcm, tb);
			if (lowered) return lowered;
			// STL container method call (vector/map/set) on a container
			// object receiver. Returns NULL for non-container receivers.
			lowered = container_method_call(tcm, tb);
			if (lowered) return lowered;
			// User-defined class method call: c.method(args) ->
			// ClassName__method(&c, args). Returns NULL when the
			// receiver is not a user class.
			lowered = class_method_call(tcm, tb);
			if (lowered) return lowered;
		}
	}

	// Struct member access
	{
		TokenMember *tm = dynamic_cast<TokenMember *>(tb);
		if (tm) {
			node_t obj;
			if (tm->parent_expr)
				obj = translate_expr(tm->parent_expr);
			else
				obj = id(tm->object.name.c_str(), tb);
			node_t member = id(tm->var.name.c_str(), tb);
			// `.` requires a struct lvalue; `->` requires a pointer. The
			// object's declared type drives this: a pointer-typed object uses
			// `->` (the parser already marks pointer-arithmetic parents like
			// `(arr+1)` as pointer-typed objects). A *bare* array object
			// (`(Upgrade_items)->m`) decays to a pointer and also needs `->`,
			// UNLESS the access subscripted into the array first (`rt[0].m`),
			// in which case the parent expression yields a struct element and
			// needs `.`.
			bool obj_is_array = !tm->object.dims.empty()
				|| dynamic_cast<DataDefCArray *>(tm->object.type) != NULL;
			bool parent_is_subscript = tm->parent_expr
				&& (dynamic_cast<TokenSubscript *>(tm->parent_expr)
				 || dynamic_cast<TokenSubscriptExpr *>(tm->parent_expr));
			bool ptr_like;
			if (parent_is_subscript) {
				// Subscripting dereferences: `p[i]`/`arr[i]` yields the
				// element value, so use `.` — even when the base `p` is a
				// pointer (`MENU_DATA *m; m[i].field`). Use `->` only if the
				// element itself is a pointer (`T **pp; pp[i]->field`).
				DataDef *pdd = tm->parent_expr->datadef();
				ptr_like = pdd && pdd->is_pointer();
			} else {
				// A pointer object uses `->`; a bare array object decays to a
				// pointer and also uses `->`.
				ptr_like = tm->object.type->is_pointer() || obj_is_array;
			}
			c2mir_node_code_t code = ptr_like ? N_DEREF_FIELD : N_FIELD;
			// Detect & reject the inverse of the bug above: `->` is valid on
			// a pointer OR an array (which decays to a pointer), but invalid
			// on a plain non-pointer struct/union value — gcc rejects that,
			// while c2mir's checker is lenient and miscompiles it into a bad
			// load. Emit an error node so the validity gate reports it with a
			// source location rather than producing broken MIR.
			if (code == N_DEREF_FIELD) {
				DataDef *odd = tm->parent_expr
					? tm->parent_expr->datadef() : tm->object.type;
				bool deref_ok = obj_is_array
					|| (tm->object.type && tm->object.type->is_pointer())
					|| (odd && odd->is_pointer());
				if (!deref_ok && odd && odd->is_struct())
					return error_node("'->' applied to a non-pointer "
						"struct value (use '.')", tb);
			}
			return node2(code, obj, member, tb);
		}
	}

	// sizeof
	{
		TokenTypeQuery *ttq = dynamic_cast<TokenTypeQuery *>(tb);
		if (ttq && ttq->query_type) {
			node_t tl = type_list(ttq->query_type);
			node_t type_decl = node2(N_DECL, ignore(), list());
			node_t type_node = node2(N_TYPE, tl, type_decl);
			c2mir_node_code_t code = ttq->want_alignof ? N_ALIGNOF : N_SIZEOF;
			return node1(code, type_node, tb);
		}
	}

	// Cast
	{
		TokenCast *tc = dynamic_cast<TokenCast *>(tb);
		if (tc) {
			DataDef *cast_dd = tc->cast_type;
			bool cast_is_ptr = cast_dd && cast_dd->is_pointer();
			DataDefPTR *cast_ptr = cast_is_ptr ? dynamic_cast<DataDefPTR *>(cast_dd) : NULL;
			if (cast_ptr && cast_ptr->base_type) cast_dd = cast_ptr->base_type;

			node_t tl = type_list(cast_dd);
			node_t cast_decl_list = list();
			if (cast_is_ptr)
				append(cast_decl_list, pointer());
			node_t type_decl = node2(N_DECL, ignore(), cast_decl_list);
			node_t type_node = node2(N_TYPE, tl, type_decl);
			return node2(N_CAST, type_node, translate_expr(tc->expr), tb);
		}
	}

	// Operators
	if (tb->is_operator()) {
		TokenOperator *top = dynamic_cast<TokenOperator *>(tb);

		// Inc/Dec
		if (tb->id() == TokenID::tkInc || tb->id() == TokenID::tkDec) {
			bool is_post = (top->left != NULL);
			TokenBase *operand_tb = is_post ? top->left : top->right;
			node_t operand = translate_expr(operand_tb);
			c2mir_node_code_t code;
			if (tb->id() == TokenID::tkInc)
				code = is_post ? N_POST_INC : N_INC;
			else
				code = is_post ? N_POST_DEC : N_DEC;
			return node1(code, operand, tb);
		}

		// Unary
		if (top && top->argc() == 1 && top->right) {
			node_t operand = translate_expr(top->right);
			if (tb->id() == TokenID::tkNeg)
				return node2(N_SUB, integer(0, tb), operand, tb);
			c2mir_node_code_t code;
			switch (tb->id()) {
			case TokenID::tkLnot: code = N_NOT; break;
			case TokenID::tkBnot: code = N_BITWISE_NOT; break;
			default:
				DBG(std::cerr << "cir: unhandled unary op " << (int)tb->id() << std::endl);
				code = N_NOT;
			}
			return node1(code, operand, tb);
		}

		// Binary
		if (top && top->left && top->right) {
			// Stream chain: cout << x [<< y ...] -> direct calls on the
			// mangled libstdc++ stream object. Intercept BEFORE the
			// tkBSL->N_LSH (shift) translation below.
			if (tb->id() == TokenID::tkBSL || tb->id() == TokenID::tkBSR) {
				bool is_out = (tb->id() == TokenID::tkBSL);
				TokenBase *leaf = top->left;
				while (TokenOperator *o = dynamic_cast<TokenOperator *>(leaf)) {
					if (o->id() != tb->id()) break;
					leaf = o->left;
				}
				StreamKind k = stream_ident_kind(leaf);
				bool ok = (is_out && (k==SK_COUT||k==SK_CERR||k==SK_CLOG)) || (!is_out && k==SK_CIN);
				if (ok) return translate_stream_chain(top, k, is_out);
			}

			// STL container subscript WRITE: `c[i] = rhs` lowers to the
			// runtime setter (vector_int_set / map_str_int_set / ...).
			// c2mir cannot assign through the long[] buffer; intercept the
			// N_ASSIGN before the generic path. (`c[i] op= rhs` compound
			// assignment is not yet supported — falls through.)
			if (tb->id() == TokenID::tkAssign) {
				node_t cw = container_subscript_assign(top, tb);
				if (cw) return cw;
			}

			// std::string assignment / append on a string OBJECT lvalue:
			// `s = rhs` / `s += rhs` lower to the basic_string operator=/+=
			// runtime wrappers. c2mir cannot assign into the `long[]` buffer,
			// and g++ would emit these operator calls. RHS string OBJECT ->
			// string_assign/append; const char* value -> the _cstr variant.
			if ((tb->id() == TokenID::tkAssign || tb->id() == TokenID::tkAddEq)
			    && is_string_object_value(top->left)) {
				bool is_append = (tb->id() == TokenID::tkAddEq);
				node_t cargs = list();
				append(cargs, string_obj_arg(top->left));   // (void*)&s
				const char *sym;
				if (is_string_object_value(top->right)) {
					append(cargs, string_obj_arg(top->right));  // (void*)&rhs
					sym = is_append ? "string_append" : "string_assign";
					need_output_extern(sym, false,
						{ { {N_VOID}, true }, { {N_VOID}, true } });
				} else {
					append(cargs, translate_expr(top->right));  // const char*
					sym = is_append ? "string_append_cstr" : "string_assign_cstr";
					need_output_extern(sym, false,
						{ { {N_VOID}, true }, { {N_CHAR}, true } });
				}
				node_t scall = node2(N_CALL, id(sym, tb), cargs, tb);
				CIR_NODE(scall)->synth_from_origin = true;
				return scall;
			}

			// Operator overloading on a user-defined class lvalue:
			// `c <op> rhs` where `c`'s class defines `operator<op>` lowers
			// to `ClassName__operator<op>(&c, rhs)`. The parser keeps the
			// raw binary node; the method dispatch is resolved here (like
			// the std::string operator interception above).
			{
				node_t ov = class_operator_call(top, tb);
				if (ov) return ov;
			}

			node_t left = translate_expr(top->left);
			node_t right = translate_expr(top->right);

			c2mir_node_code_t code;
			switch (tb->id()) {
			case TokenID::tkAdd:    code = N_ADD; break;
			case TokenID::tkSub:    code = N_SUB; break;
			case TokenID::tkMul:    code = N_MUL; break;
			case TokenID::tkDiv:    code = N_DIV; break;
			case TokenID::tkMod:    code = N_MOD; break;
			case TokenID::tkAssign: code = N_ASSIGN; break;
			case TokenID::tkEquals: code = N_EQ; break;
			case TokenID::tkNotEq:  code = N_NE; break;
			case TokenID::tkLT:     code = N_LT; break;
			case TokenID::tkLE:     code = N_LE; break;
			case TokenID::tkGT:     code = N_GT; break;
			case TokenID::tkGE:     code = N_GE; break;
			case TokenID::tkBand:   code = N_AND; break;
			case TokenID::tkBor:    code = N_OR; break;
			case TokenID::tkXor:    code = N_XOR; break;
			case TokenID::tkLand:   code = N_ANDAND; break;
			case TokenID::tkLor:    code = N_OROR; break;
			case TokenID::tkBSL:    code = N_LSH; break;
			case TokenID::tkBSR:    code = N_RSH; break;
			case TokenID::tkComma:  code = N_COMMA; break;
			case TokenID::tkAddEq:  code = N_ADD_ASSIGN; break;
			case TokenID::tkSubEq:  code = N_SUB_ASSIGN; break;
			case TokenID::tkMulEq:  code = N_MUL_ASSIGN; break;
			case TokenID::tkDivEq:  code = N_DIV_ASSIGN; break;
			case TokenID::tkModEq:  code = N_MOD_ASSIGN; break;
			case TokenID::tkBandEq: code = N_AND_ASSIGN; break;
			case TokenID::tkBorEq:  code = N_OR_ASSIGN; break;
			case TokenID::tkXorEq:  code = N_XOR_ASSIGN; break;
			case TokenID::tkBSLEq:  code = N_LSH_ASSIGN; break;
			case TokenID::tkBSREq:  code = N_RSH_ASSIGN; break;
			default:
				DBG(std::cerr << "cir: unhandled binary op " << (int)tb->id() << std::endl);
				code = N_ADD;
			}
			return node2(code, left, right, tb);
		}
	}

	// Function call
	if (tb->type() == TokenType::ttCallFunc) {
		TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(tb);
		if (tcf) {
			const char *rt = builtin_output_runtime(tcf->var.name);
			if (rt[0]) {
				static const std::map<std::string, ExternParam> sigs = {
					{"madc_puti",     {{N_LONG}, false}},
					{"madc_putu",     {{N_UNSIGNED, N_LONG}, false}},
					{"madc_putd",     {{N_DOUBLE}, false}},
					{"madc_putf",     {{N_FLOAT}, false}},
					{"madc_puts",     {{N_CHAR}, true}},
					{"madc_printstr", {{N_CHAR}, true}},
				};
				need_output_extern(rt, false, { sigs.at(rt) });
				node_t a = list();
				for (size_t i = 0; i < tcf->parameters.size(); i++) {
					TokenBase *p = tcf->parameters[i];
					// printstr/puts take char*: a std::string object
					// argument must be coerced via string_cstr. String
					// literals are already const char* — pass directly.
					if (is_string_object_value(p))
						append(a, string_cstr_arg(p));
					else
						append(a, translate_expr(p));
				}
				return node2(N_CALL, id(rt, tb), a, tb);
			}
			// Callee selection. A normal call names a function by
			// identifier. A call through a function-pointer held in a
			// sub-expression (a struct member `c.fn`, an array element
			// `arr[i].fn`, etc.) carries that expression in src_node —
			// the parser sets it when `(` follows a TokenMember whose
			// datadef is DataDefFPTR. Emit the member-access node as the
			// callee, not the bare member name: `(*ch->spec_fun)(ch)` must
			// stay an indirect call, not become a call to a (nonexistent)
			// global `spec_fun` (which c2mir-check tolerates as an implicit
			// declaration but MIR-link cannot resolve).
			node_t func_id;
			if (tcf->src_node) {
				func_id = translate_expr(tcf->src_node);
			} else {
				// c2mir intrinsics (e.g. __builtin_va_start) are
				// recognized by name and lowered in-place; emitting an
				// extern prototype for one shadows the intrinsic and
				// turns it into an undefined external symbol at MIR-link.
				// So skip the proto for them. (__builtin_va_arg has its
				// own translation path above.)
				if (tcf->var.name.compare(0, 13, "__builtin_va_") != 0)
					referenced_funcs.insert(tcf->var.name);
				func_id = id(tcf->var.name.c_str(), tb);
			}
			node_t args = list();
			// Coerce arguments to string-OBJECT parameters: a real object
			// is passed by address, a const char* value is materialized into
			// a temporary std::string (string_obj_arg). The callee's declared
			// parameter types come from its FuncDef; varargs / unknown callees
			// fall through to a plain translation.
			FuncDef *callee = dynamic_cast<FuncDef *>(tcf->var.type);
			for (size_t i = 0; i < tcf->parameters.size(); i++) {
				TokenBase *arg = tcf->parameters[i];
				DataDef *pt = (callee && i < callee->parameters.size())
						? callee->parameters[i] : NULL;
				DataType pdt = pt ? pt->rawtype() : DataType::dtVOID;
				bool is_ref_param = callee && i < callee->ref_params.size()
						    && callee->ref_params[i];
				if (pt && (pdt == DataType::dtSTRING
					   || pdt == DataType::dtSTRINGref))
					append(args, string_obj_arg(arg));
				else if (is_ref_param)
					// Numeric reference parameter (`int &x`): the callee
					// takes a pointer, so pass the argument's address. The
					// argument lvalue translates normally (a vfREFERENCE arg
					// re-derefs to its own pointer, and &(*p) folds to p).
					append(args, node1(N_ADDR, translate_expr(arg), arg));
				else
					append(args, translate_expr(arg));
			}
			return node2(N_CALL, func_id, args, tb);
		}
	}

	// va_arg(ap, T) -> __builtin_va_arg(ap, (T *)0). c2mir derives the result
	// type from the 2nd argument's pointer type (it inspects the type, never
	// evaluates the 0); see c2mir.c va_arg handling (2nd arg must be T*).
	if (TokenVaArg *tva = dynamic_cast<TokenVaArg *>(tb)) {
		node_t ap = tva->ap_expr ? translate_expr(tva->ap_expr)
					 : id(tva->ap_var->name.c_str(), tb);
		// Build a (T *)0 carrier: base specs of T plus one extra '*'.
		DataDef *base = tva->target_type;
		int levels = dd_ptr_depth(base) + 1;
		while (base && base->is_pointer()) {
			DataDefPTR *p = dynamic_cast<DataDefPTR *>(base);
			if (!p) break;
			base = p->base_type;
		}
		node_t decl_list = list();
		for (int i = 0; i < levels; i++) append(decl_list, pointer());
		node_t type_node = node2(N_TYPE, type_list(base),
					 node2(N_DECL, ignore(), decl_list));
		node_t typeptr = node2(N_CAST, type_node, integer(0));
		node_t args = list();
		append(args, ap);
		append(args, typeptr);
		return node2(N_CALL, id("__builtin_va_arg", tb), args, tb);
	}

	DBG(std::cerr << "cir: unhandled expr " << describe_token(tb)
		      << " type=" << (int)tb->type()
		      << " id=" << (int)tb->id() << std::endl);
	// Previously this silently became integer(0) — a wrong translation that
	// compiled fine but ran incorrectly. Emit an error node instead so the
	// pre-c2mir gate rejects the tree rather than miscompiling it. Name the
	// concrete token class + lexeme so the failing construct is identifiable
	// (the origin token also carries the source file:line:col, printed by
	// cir_report_errors).
	char buf[160];
	snprintf(buf, sizeof(buf),
		 "unhandled expression: %s (CIR builder has no translation for this "
		 "construct; token type %d, id %d)",
		 describe_token(tb).c_str(), (int)tb->type(), (int)tb->id());
	return error_node(buf, tb);
}

// -----------------------------------------------------------------------
// Statement translation
// -----------------------------------------------------------------------

node_t CirBuilder::translate_return(TokenRETURN *tr)
{
	// `return <void-expr>;` inside a void function (e.g.
	// `return some_void_fn();`) is accepted by gcc — the void expression is
	// evaluated and the function returns — but c2mir rejects "return with a
	// value in function returning void". A void return value is equivalent to
	// a bare return, so lower it to `<expr>; return;`. Require the expression
	// itself to be void-typed: a genuine `return 5;` in a void function stays
	// an error (which gcc reports too).
	if (tr->returns && m_cur_func_returns_void) {
		DataDef *edd = tr->returns->datadef();
		if (edd && !edd->is_pointer() && edd->rawtype() == DataType::dtVOID) {
			node_t items = list();
			append(items, node2(N_EXPR, list(), translate_expr(tr->returns)));
			append(items, node2(N_RETURN, list(), ignore(), tr));
			return node2(N_BLOCK, list(), items);
		}
	}
	node_t expr = tr->returns ? translate_expr(tr->returns) : ignore();
	return node2(N_RETURN, list(), expr, tr);
}

node_t CirBuilder::translate_if(TokenIF *ti)
{
	node_t cond = translate_expr(ti->condition);
	node_t then_body = translate_stmt_required(ti->statement);
	node_t else_body = ti->elsestmt ? translate_stmt_required(ti->elsestmt) : ignore();
	return node4(N_IF, list(), cond, then_body, else_body, ti);
}

node_t CirBuilder::translate_while(TokenBase *tw)
{
	TokenWHILE *w = dynamic_cast<TokenWHILE *>(tw);
	if (!w) return ignore();
	return node3(N_WHILE, list(), translate_expr(w->condition),
		     translate_stmt_required(w->statement), tw);
}

node_t CirBuilder::translate_for(TokenFOR *tf)
{
	// A declaration init (`for (int i = 0; ...)`) parses to a TokenDecl, which
	// translate_expr would mis-handle as a bare variable reference (TokenDecl
	// IS-A TokenVar) — dropping the declaration and leaving `i` undeclared.
	// Emit the proper SPEC_DECL so c2mir sees a declaration in the init slot.
	node_t init;
	if (tf->initialize) {
		TokenDecl *td = dynamic_cast<TokenDecl *>(tf->initialize);
		init = td ? var_decl(&td->var, td) : translate_expr(tf->initialize);
		// Comma-separated init clauses (`for (a=0, b=1; ...)`) keep the first
		// in `initialize` and the rest in init_extras. Fold them into a single
		// left-associative N_COMMA so all run, in order, in the init slot.
		if (!td)
			for (TokenBase *ex : tf->init_extras)
				init = node2(N_COMMA, init, translate_expr(ex));
	} else {
		init = ignore();
	}
	node_t cond = tf->condition ? translate_expr(tf->condition) : ignore();
	node_t incr = tf->increment ? translate_expr(tf->increment) : ignore();
	// Comma-separated increment clauses (`for (...; ...; i++, j--)`): fold the
	// extras into the increment via N_COMMA so each runs every iteration.
	if (tf->increment)
		for (TokenBase *ex : tf->incr_extras)
			incr = node2(N_COMMA, incr, translate_expr(ex));
	node_t body = translate_stmt_required(tf->statement);
	return node5(N_FOR, list(), init, cond, incr, body, tf);
}

// Range-based for over a MadArray: `for (T x : arr) body`.
// The parser already declared `x` in the ENCLOSING scope (so translate_block
// emits its storage + ctor once, and the cleanup attribute destructs it), so
// here we only emit:
//   for (long __fe_i = 0; __fe_i < madarray_size((void*)arr); __fe_i += 1) {
//       <fill x from arr[__fe_i]>;   <body>
//   }
// String element -> php_array_get((void*)x, (void*)arr, __fe_i) (assigns into
// the pre-constructed std::string). Numeric element -> x = php_array_get_int().
node_t CirBuilder::translate_foreach(TokenFOREACH *fe)
{
	if (!fe->container || !fe->elemtype)
		return error_node("range-for missing container or element type", fe);

	// A vector container iterates via vector_int_size/at or vector_str_size/at
	// rather than the MadArray (php_array) helpers. Detect it from the
	// container's declared type.
	DataDef *cdd = fe->container->datadef();
	DataDefVECTOR *vdd = dynamic_cast<DataDefVECTOR *>(cdd);
	if (vdd && cdd && cdd->rawtype() == DataType::dtVECTOR)
		return translate_foreach_vector(fe, vdd);

	// (void*)container — the MadArray object address (the var name decays to
	// its long[] buffer, normalized to void*).
	auto container_addr = [&]() -> node_t {
		return node2(N_CAST, void_ptr_type(), translate_expr(fe->container), fe);
	};

	char idx[32];
	snprintf(idx, sizeof(idx), "__fe_i_%d", m_strtmp_counter++);

	// long __fe_i = 0;
	node_t ispec = list();
	append(ispec, simple(N_LONG, fe));
	node_t init = simple(N_SPEC_DECL, fe);
	append(init, node1(N_SHARE, ispec));
	append(init, node2(N_DECL, id(idx, fe), list()));
	append(init, ignore());
	append(init, ignore());
	append(init, integer(0, fe));

	// __fe_i < madarray_size((void*)container)
	need_output_extern("madarray_size", false, { { {N_VOID}, true } },
			   std::vector<c2mir_node_code_t>{N_LONG});
	node_t size_args = list();
	append(size_args, container_addr());
	node_t size_call = node2(N_CALL, id("madarray_size", fe), size_args, fe);
	node_t cond = node2(N_LT, id(idx, fe), size_call, fe);

	// __fe_i += 1
	node_t incr = node2(N_ADD_ASSIGN, id(idx, fe), integer(1, fe), fe);

	// Body: element fill + user statement.
	node_t body_items = list();
	if (fe->elemtype->rawtype() == DataType::dtSTRING) {
		// php_array_get((void*)x, (void*)container, __fe_i)
		need_output_extern("__php_array_get", true,
				   { { {N_VOID}, true }, { {N_VOID}, true }, { {N_LONG}, false } });
		referenced_funcs.insert("__php_array_get");
		node_t a = list();
		append(a, string_obj_addr(fe->elemname.c_str(), fe));
		append(a, container_addr());
		append(a, id(idx, fe));
		node_t fill = node2(N_CALL, id("__php_array_get", fe), a, fe);
		append(body_items, node2(N_EXPR, list(), fill, fe));
	} else {
		// x = (T)__php_array_get_int((void*)container, __fe_i)
		need_output_extern("__php_array_get_int", false,
				   { { {N_VOID}, true }, { {N_LONG}, false } },
				   std::vector<c2mir_node_code_t>{N_LONG});
		referenced_funcs.insert("__php_array_get_int");
		node_t a = list();
		append(a, container_addr());
		append(a, id(idx, fe));
		node_t getcall = node2(N_CALL, id("__php_array_get_int", fe), a, fe);
		node_t assign = node2(N_ASSIGN, id(fe->elemname.c_str(), fe), getcall, fe);
		append(body_items, node2(N_EXPR, list(), assign, fe));
	}

	node_t user_body = translate_stmt_required(fe->statement);
	if (user_body) append(body_items, user_body);
	node_t body = node2(N_BLOCK, list(), body_items, fe);

	return node5(N_FOR, list(), init, cond, incr, body, fe);
}

node_t CirBuilder::translate_foreach_vector(TokenFOREACH *fe, DataDefVECTOR *vdd)
{
	bool str = vdd->element_type && vdd->element_type->is_string();
	const char *size_sym = str ? "vector_str_size" : "vector_int_size";

	auto container_addr = [&]() -> node_t {
		return node2(N_CAST, void_ptr_type(), translate_expr(fe->container), fe);
	};

	char idx[32];
	snprintf(idx, sizeof(idx), "__fe_i_%d", m_strtmp_counter++);

	// long __fe_i = 0;
	node_t ispec = list();
	append(ispec, simple(N_LONG, fe));
	node_t init = simple(N_SPEC_DECL, fe);
	append(init, node1(N_SHARE, ispec));
	append(init, node2(N_DECL, id(idx, fe), list()));
	append(init, ignore());
	append(init, ignore());
	append(init, integer(0, fe));

	// __fe_i < vector_{int,str}_size((void*)container)
	need_output_extern(size_sym, false, { { {N_VOID}, true } },
			   std::vector<c2mir_node_code_t>{N_LONG});
	node_t size_args = list();
	append(size_args, container_addr());
	node_t size_call = node2(N_CALL, id(size_sym, fe), size_args, fe);
	node_t cond = node2(N_LT, id(idx, fe), size_call, fe);

	// __fe_i += 1
	node_t incr = node2(N_ADD_ASSIGN, id(idx, fe), integer(1, fe), fe);

	// Body: element fill + user statement.
	node_t body_items = list();
	if (str) {
		// vector_str_at((void*)&x, (void*)container, __fe_i) — x is the
		// enclosing-scope string loop var, already constructed.
		need_output_extern("vector_str_at", true,
				   { { {N_VOID}, true }, { {N_VOID}, true }, { {N_LONG}, false } });
		node_t a = list();
		append(a, string_obj_addr(fe->elemname.c_str(), fe));
		append(a, container_addr());
		append(a, id(idx, fe));
		node_t fill = node2(N_CALL, id("vector_str_at", fe), a, fe);
		append(body_items, node2(N_EXPR, list(), fill, fe));
	} else {
		// x = vector_int_at((void*)container, __fe_i)
		need_output_extern("vector_int_at", false,
				   { { {N_VOID}, true }, { {N_LONG}, false } },
				   std::vector<c2mir_node_code_t>{N_LONG});
		node_t a = list();
		append(a, container_addr());
		append(a, id(idx, fe));
		node_t getcall = node2(N_CALL, id("vector_int_at", fe), a, fe);
		node_t assign = node2(N_ASSIGN, id(fe->elemname.c_str(), fe), getcall, fe);
		append(body_items, node2(N_EXPR, list(), assign, fe));
	}

	node_t user_body = translate_stmt_required(fe->statement);
	if (user_body) append(body_items, user_body);
	node_t body = node2(N_BLOCK, list(), body_items, fe);

	return node5(N_FOR, list(), init, cond, incr, body, fe);
}

node_t CirBuilder::translate_do(TokenDO *td)
{
	return node3(N_DO, list(), translate_expr(td->condition),
		     translate_stmt_required(td->statement), td);
}

node_t CirBuilder::translate_switch(TokenSWITCH *ts)
{
	node_t expr = translate_expr(ts->expression);
	node_t block_items = list();

	// Declarations before the first case label (`switch(x){ T v; case ...}`)
	// belong to the switch's compound scope. Emit them first so uses of `v`
	// later in the switch resolve; dropping them left `v` undeclared.
	for (size_t pi = 0; pi < ts->pre_case_stmts.size(); pi++) {
		node_t s = translate_stmt(ts->pre_case_stmts[pi]);
		if (s) append(block_items, s);
	}

	// Emit one case/default: a labeled marker statement (`<label>: 0;`) then
	// every statement in the case translated AS A STATEMENT. The marker
	// carries the label uniformly regardless of the first statement's kind —
	// previously the first non-return/break statement was sent to
	// translate_expr, which mis-flagged `if`/`for`/`while` inside a case as
	// "unhandled expression" (679 such nodes in SMAUG's switch-heavy interp).
	auto emit_case = [&](TokenCASE *tc, bool is_default) {
		node_t label;
		if (is_default) {
			label = simple(N_DEFAULT);
		} else if (tc->range_high) {
			// GNU case range: `case LOW ... HIGH:` -> N_CASE(low, high).
			label = node2(N_CASE, translate_expr(tc->value),
				      translate_expr(tc->range_high), tc);
		} else {
			label = node1(N_CASE, translate_expr(tc->value), tc);
		}
		append(block_items, node2(N_EXPR, node1(N_LIST, label), integer(0)));
		for (size_t si = 0; si < tc->statements.size(); si++) {
			node_t s = translate_stmt(tc->statements[si]);
			if (s) append(block_items, s);
		}
	};

	// The parser keeps the `default:` case in ts->defaultcase (never in
	// ts->cases) and records the position it appeared among the cases in
	// ts->default_index. Emit it at that position so fall-through order is
	// preserved — e.g. `case A: ...; default: ...; case B: ...` stays
	// A, default, B. (The old code only emitted the default when
	// default_index < 0, which the parser never sets, so the default case was
	// silently dropped from every switch — values matching no case fell
	// through the whole switch.)
	bool have_default = (ts->defaultcase != NULL);
	for (size_t ci = 0; ci < ts->cases.size(); ci++) {
		if (have_default && (int)ci == ts->default_index)
			emit_case(ts->defaultcase, true);
		emit_case(ts->cases[ci], false);
	}
	// default at (or past) the end of the case list.
	if (have_default && ts->default_index >= (int)ts->cases.size())
		emit_case(ts->defaultcase, true);

	node_t body = node2(N_BLOCK, list(), block_items);
	return node3(N_SWITCH, list(), expr, body, ts);
}

// rust::match(expr) { p1 | p2 => stmt; _ => stmt; } -> a C switch with an
// implicit break per arm. OR-list patterns become consecutive case labels; the
// `_` arm becomes default. Source order is preserved — in C, explicit case
// values always win over default regardless of where default sits, so a
// mid-list wildcard does not shadow patterns listed after it.
node_t CirBuilder::translate_match(TokenMatch *tm)
{
	if (!tm->expression)
		return error_node("match missing scrutinee expression", tm);

	node_t expr = translate_expr(tm->expression);
	node_t block_items = list();
	for (MatchArm *arm : tm->arms) {
		if (!arm) continue;
		if (arm->is_wildcard) {
			append(block_items, node2(N_EXPR,
				node1(N_LIST, simple(N_DEFAULT)), integer(0), tm));
		} else {
			for (TokenBase *pat : arm->patterns) {
				node_t label = node1(N_CASE, translate_expr(pat), tm);
				append(block_items, node2(N_EXPR,
					node1(N_LIST, label), integer(0)));
			}
		}
		node_t b = translate_stmt(arm->body);
		if (b) append(block_items, b);
		// No fall-through between arms.
		append(block_items, node1(N_BREAK, list(), tm));
	}
	node_t body = node2(N_BLOCK, list(), block_items, tm);
	return node3(N_SWITCH, list(), expr, body, tm);
}

node_t CirBuilder::translate_stmt(TokenBase *tb)
{
	if (!tb) return NULL;

	TokenRETURN *tr = dynamic_cast<TokenRETURN *>(tb);
	if (tr) return translate_return(tr);

	TokenIF *ti = dynamic_cast<TokenIF *>(tb);
	if (ti) return translate_if(ti);

	if (tb->id() == TokenID::tkWHILE)
		return translate_while(tb);

	// Range-based for (TokenFOREACH) is a sibling of TokenFOR, not a subclass,
	// so check it first — dynamic_cast<TokenFOR*> would miss it.
	{ TokenFOREACH *fe = dynamic_cast<TokenFOREACH *>(tb);
	  if (fe) return translate_foreach(fe); }

	TokenFOR *tf = dynamic_cast<TokenFOR *>(tb);
	if (tf) return translate_for(tf);

	{ TokenDO *td = dynamic_cast<TokenDO *>(tb);
	  if (td) return translate_do(td); }

	{ TokenSWITCH *ts = dynamic_cast<TokenSWITCH *>(tb);
	  if (ts) return translate_switch(ts); }

	{ TokenMatch *tm = dynamic_cast<TokenMatch *>(tb);
	  if (tm) return translate_match(tm); }

	// Goto
	{ TokenGOTO *tg = dynamic_cast<TokenGOTO *>(tb);
	  if (tg) return node2(N_GOTO, list(), id(tg->target.c_str(), tb), tb); }

	// Label (handled in translate_block)
	{ TokenLabel *tl = dynamic_cast<TokenLabel *>(tb);
	  if (tl) return NULL; }

	// Declaration
	{ TokenDecl *td = dynamic_cast<TokenDecl *>((TokenBase *)tb);
	  if (td) {
		// A function-local `extern T name;` that resolves to a file-scope
		// global yields a TokenDecl whose var IS that global: file scope, so
		// not vfLOCAL and not vfSTATIC. The parser clears vfEXTERN here, so
		// var_decl would emit a fresh local definition, shadowing the global
		// with uninitialized storage. The global is already emitted at top
		// level and is in scope, so skip the in-function redeclaration.
		// (A `static` local has vfSTATIC and IS emitted in the body.)
		bool is_file_scope_global =
			!(td->var.flags & vfLOCAL) && !(td->var.flags & vfSTATIC);
		if (is_file_scope_global)
			return NULL;
		return var_decl(&td->var, td);
	  } }

	// Break
	if (tb->id() == TokenID::tkBREAK)
		return node1(N_BREAK, list(), tb);

	// Continue
	if (tb->id() == TokenID::tkCONT)
		return node1(N_CONTINUE, list(), tb);

	// Compound statement (block)
	TokenCpnd *tc = dynamic_cast<TokenCpnd *>(tb);
	if (tc) return translate_block(tc);

	// Empty statement (stray ';'): a no-op — skip it (translate_block drops
	// NULLs). Handling it here keeps it from reaching the expression-statement
	// fallthrough and being flagged as an unhandled expression.
	if (tb->id() == TokenID::tkSemi)
		return NULL;

	// Expression statement
	return node2(N_EXPR, list(), translate_expr(tb), tb);
}

// A loop/if body is a required statement operand: c2mir always has a node
// in that slot. translate_stmt returns NULL for an empty `;` (which a block
// drops, matching c2m), so substitute c2mir's empty-statement node here —
// EXPR(LIST, IGNORE) — rather than feeding NULL to c2mir_op_append.
node_t CirBuilder::translate_stmt_required(TokenBase *tb)
{
	node_t s = translate_stmt(tb);
	if (s) return s;
	return node2(N_EXPR, list(), ignore(), tb);
}

node_t CirBuilder::translate_block(TokenCpnd *tc)
{
	node_t empty_list = list();
	node_t items = list();

	TokenFunc *parent_func = dynamic_cast<TokenFunc *>(tc);
	size_t skip = 0;
	if (parent_func) {
		FuncDef *fd = dynamic_cast<FuncDef *>(parent_func->var.type);
		if (fd) skip = fd->parameters.size();
	}

	std::set<std::string> decl_vars;
	for (auto *ts : tc->statements) {
		TokenDecl *td = dynamic_cast<TokenDecl *>((TokenBase *)ts);
		if (td)
			decl_vars.insert(td->var.name);
	}

	for (size_t vi = skip; vi < tc->variables.size(); vi++) {
		Variable *v = tc->variables[vi];
		if (decl_vars.count(v->name)) continue;
		append(items, var_decl(v));
		if (is_string_object(v->type)) {
			// `string b;` (no initializer) — default-construct the object.
			// Destruction is handled by the cleanup attribute on the storage
			// decl (string_storage_decl): c2mir calls string_destruct(&b) at
			// every scope exit, so no manual dtor injection is needed.
			append(items, string_ctor_call(v->name.c_str(), NULL, tc));
		} else if (is_array_object(v->type)) {
			// `array a;` — default-construct the MadArray object. Scope-exit
			// destruction is handled by the cleanup attribute (array_storage_decl).
			append(items, array_ctor_call(v->name.c_str(), tc));
		} else if (is_container_object(v->type)) {
			// `vector<T>/map<K,V>/set<T> c;` — default-construct the container.
			// Scope-exit destruction is via the cleanup attribute.
			append(items, container_ctor_call(v->name.c_str(), v->type, tc));
		} else if (DataDefCLASS *cdd = as_user_class(v->type)) {
			// `Foo f;` — a class instance declared without an explicit
			// constructor-call (no ctor args). Default-construct it; scope-exit
			// destruction is via the cleanup attribute (var_decl). The argful
			// form `Foo f(a,b)` parses to a TokenDecl statement handled below.
			node_t cc = class_ctor_call(v, cdd,
						    std::vector<TokenBase *>(), tc);
			if (cc) append(items, cc);
			// A class with embedded object members but NO user ctor still
			// needs each member constructed (string_construct(&f.member)).
			// (Member destruction is via the synthesized dtor referenced by
			// the cleanup attribute — see class_struct_def / var_decl.)
			else if (class_has_object_members(cdd))
				class_instance_member_ctors(v->name.c_str(), cdd,
							    items, tc);
		}
	}

	// Statements with label handling
	std::vector<std::string> pending_labels;
	for (size_t si = 0; si < tc->statements.size(); si++) {
		TokenBase *stb = (TokenBase *)tc->statements[si];
		TokenLabel *tl = dynamic_cast<TokenLabel *>(stb);
		if (tl) {
			pending_labels.push_back(tl->name);
			continue;
		}

		// std::string object declaration WITH initializer: 1->N lowering.
		// Emit storage + ctor inline here (translate_stmt's var_decl emits only
		// the storage), and track for scope-end destruction. A file-scope global
		// reaching here is handled elsewhere (translate_stmt drops it).
		{
			TokenDecl *sdcl = dynamic_cast<TokenDecl *>(stb);
			bool file_global = sdcl && !(sdcl->var.flags & vfLOCAL)
					&& !(sdcl->var.flags & vfSTATIC);
			if (sdcl && is_string_object(sdcl->var.type) && !file_global) {
				if (!pending_labels.empty()) {
					node_t ll = list();
					for (auto &ln : pending_labels)
						append(ll, node1(N_LABEL, id(ln.c_str())));
					pending_labels.clear();
					append(items, node2(N_EXPR, ll, integer(0)));
				}
				append(items, var_decl(&sdcl->var, sdcl));
				TokenBase *initexpr = sdcl->initialize;
				if (TokenAssign *as = dynamic_cast<TokenAssign *>(initexpr))
					initexpr = as->right;
				if (!initexpr && !sdcl->init_list.empty())
					initexpr = sdcl->init_list[0];
				append(items, string_ctor_call(sdcl->var.name.c_str(),
							       initexpr, sdcl));
				continue;
			}
			// MadArray object declaration: storage (via var_decl) + default ctor.
			if (sdcl && is_array_object(sdcl->var.type) && !file_global) {
				if (!pending_labels.empty()) {
					node_t ll = list();
					for (auto &ln : pending_labels)
						append(ll, node1(N_LABEL, id(ln.c_str())));
					pending_labels.clear();
					append(items, node2(N_EXPR, ll, integer(0)));
				}
				append(items, var_decl(&sdcl->var, sdcl));
				append(items, array_ctor_call(sdcl->var.name.c_str(), sdcl));
				continue;
			}
			// STL container object declaration: storage + default ctor.
			if (sdcl && is_container_object(sdcl->var.type) && !file_global) {
				if (!pending_labels.empty()) {
					node_t ll = list();
					for (auto &ln : pending_labels)
						append(ll, node1(N_LABEL, id(ln.c_str())));
					pending_labels.clear();
					append(items, node2(N_EXPR, ll, integer(0)));
				}
				append(items, var_decl(&sdcl->var, sdcl));
				append(items, container_ctor_call(sdcl->var.name.c_str(),
								  sdcl->var.type, sdcl));
				continue;
			}
			// Class instance declared with constructor args `Foo f(a,b)`:
			// emit the struct storage (+ cleanup attr) then the ctor call
			// threading the parsed ctor_args. The no-arg form `Foo f;` is
			// handled in the variables loop above.
			DataDefCLASS *cdcl = sdcl ? as_user_class(sdcl->var.type) : NULL;
			if (cdcl && !file_global) {
				if (!pending_labels.empty()) {
					node_t ll = list();
					for (auto &ln : pending_labels)
						append(ll, node1(N_LABEL, id(ln.c_str())));
					pending_labels.clear();
					append(items, node2(N_EXPR, ll, integer(0)));
				}
				append(items, var_decl(&sdcl->var, sdcl));
				node_t cc = class_ctor_call(&sdcl->var, cdcl,
							    sdcl->ctor_args, sdcl);
				if (cc) append(items, cc);
				// No user ctor but embedded object members: construct each
				// member in place (string_construct(&inst.member)). With a
				// user ctor, the ctor body's prologue constructs them.
				else if (class_has_object_members(cdcl))
					class_instance_member_ctors(sdcl->var.name.c_str(),
								    cdcl, items, sdcl);
				continue;
			}
		}

		node_t s = translate_stmt(stb);
		if (!s) continue;

		if (!pending_labels.empty()) {
			node_t label_list = list();
			for (auto &lname : pending_labels) {
				node_t lid = id(lname.c_str());
				append(label_list, node1(N_LABEL, lid));
			}
			pending_labels.clear();
			append(items, node2(N_EXPR, label_list, integer(0)));
		}
		// Materialized temporaries (e.g. a std::string built from a literal
		// passed to a string-object parameter) are emitted just before the
		// statement that uses them; their cleanup attribute destructs them at
		// scope exit. See string_obj_arg / m_pending_stmts.
		for (node_t p : m_pending_stmts)
			append(items, p);
		m_pending_stmts.clear();
		append(items, s);
	}

	if (!pending_labels.empty()) {
		node_t label_list = list();
		for (auto &lname : pending_labels) {
			node_t lid = id(lname.c_str());
			append(label_list, node1(N_LABEL, lid));
		}
		append(items, node2(N_EXPR, label_list, integer(0)));
	}

	// No manual scope-exit destruction: each std::string object's storage decl
	// carries __attribute__((cleanup(string_destruct))), so the c2mir fork emits
	// the destructor call on EVERY exit path (fall-through, return, break,
	// continue, goto) — correct RAII including early exits, with no front-end
	// dtor injection. See string_storage_decl.
	return node2(N_BLOCK, empty_list, items, tc);
}

// -----------------------------------------------------------------------
// Function definition
// -----------------------------------------------------------------------

node_t CirBuilder::func_def(TokenFunc *tf)
{
	FuncDef *fd = dynamic_cast<FuncDef *>(tf->var.type);
	if (!fd) return NULL;

	DataDef *ret_dd = &fd->returns;
	bool ret_is_ptr = ret_dd && ret_dd->is_pointer();
	DataDefPTR *ret_ptr = ret_is_ptr ? dynamic_cast<DataDefPTR *>(ret_dd) : NULL;
	if (ret_ptr && ret_ptr->base_type) ret_dd = ret_ptr->base_type;

	// Track whether this function returns void, so translate_return can lower
	// a gcc-accepted `return <expr>;` (void function) to `<expr>; return;`.
	m_cur_func_returns_void = !ret_is_ptr && ret_dd
				  && ret_dd->rawtype() == DataType::dtVOID
				  && !fd->is_multi_return();

	node_t ret_type = type_list(ret_dd);

	// Parameters. A variadic function carries a trailing synthetic param
	// (the parser pushes a ddINT64 placeholder when it sees `...`); drop it
	// and emit N_DOTS instead so the definition is truly variadic.
	node_t param_list = list();
	size_t nparam = fd->parameters.size();
	if (fd->is_varargs && nparam > 0) nparam--;
	if (nparam == 0 && !fd->is_varargs) {
		node_t void_spec = node1(N_LIST, simple(N_VOID));
		node_t void_decl = node2(N_DECL, ignore(), list());
		append(param_list, node2(N_TYPE, void_spec, void_decl));
	} else {
		for (size_t i = 0; i < nparam; i++) {
			const char *pname = "p";
			std::string ptypedef;
			if (tf->method && i < tf->method->parameters.size()) {
				pname = tf->method->parameters[i]->name.c_str();
				ptypedef = tf->method->parameters[i]->typedef_name;
			}
			append(param_list, param_decl(fd->parameters[i], pname, ptypedef));
		}
	}
	if (fd->is_varargs)
		append(param_list, simple(N_DOTS));

	node_t func_inner = node1(N_FUNC, param_list);

	node_t func_id = id(tf->var.name.c_str(), tf);
	node_t decl_list = list();
	append(decl_list, func_inner);
	if (ret_is_ptr)
		append(decl_list, pointer());

	node_t decl = node2(N_DECL, func_id, decl_list);
	node_t body = translate_block((TokenCpnd *)tf);

	// C++ base-class ctor/dtor chaining (single inheritance). The derived
	// ctor implicitly runs the base ctor BEFORE its own body; the derived
	// dtor runs the base dtor AFTER its own body. Base members are flattened
	// at offset 0, so __this (cast to Base *) is the same address. Only chain
	// when the base actually has a user ctor/dtor.
	DataDefCLASS *ocls = tf->method ? tf->method->owner_class : NULL;
	if (ocls) {
		const std::string &fname = tf->var.name;
		bool is_ctor = (fname == ocls->name + "__" + ocls->name);
		bool is_dtor = (fname == ocls->name + "___dtor");
		DataDefCLASS *base = ocls->base_class;
		// Cast __this to `Base *` for a chained base ctor/dtor call.
		auto base_this = [&]() -> node_t {
			node_t t = node2(N_TYPE,
				node1(N_LIST, node2(N_STRUCT,
					id(base->name.c_str()), ignore())),
				node2(N_DECL, ignore(), node1(N_LIST, pointer())));
			return node2(N_CAST, t, id("__this", tf), tf);
		};
		auto base_call = [&](const std::string &bsym) -> node_t {
			referenced_funcs.insert(bsym);
			node_t a = list();
			append(a, base_this());
			node_t call = node2(N_CALL, id(bsym.c_str(), tf), a, tf);
			return node2(N_EXPR, list(), call, tf);
		};
		// Prologue statements (run before the body), in order:
		//   1. base ctor call (ctor of a derived class with a base ctor)
		//   2. __this->__vptr = (void*)ClassName__vtable (ctor of a virtual
		//      class) — set after base construction, matching C++.
		// Epilogue (after the body): base dtor call (dtor with a base dtor).
		std::vector<node_t> prologue, epilogue;
		if (is_ctor && base && base->has_user_ctor)
			prologue.push_back(base_call(base->name + "__" + base->name));
		// Construct embedded object members at ctor entry (after base ctor),
		// destruct them at dtor exit (before base dtor) — C++ member lifetime.
		if (is_ctor)
			class_member_construct(ocls, prologue, tf);
		if (is_dtor)
			class_member_destruct(ocls, epilogue, tf);
		if (is_ctor && ocls->has_vtable) {
			std::string vname = ocls->name + "__vtable";
			node_t vptr_lhs = node2(N_DEREF_FIELD, id("__this", tf),
						id("__vptr", tf));
			node_t vptr_type = node2(N_TYPE,
				node1(N_LIST, simple(N_VOID)),
				node2(N_DECL, ignore(), node1(N_LIST, pointer())));
			node_t vtab = node2(N_CAST, vptr_type, id(vname.c_str(), tf));
			node_t asn = node2(N_ASSIGN, vptr_lhs, vtab, tf);
			prologue.push_back(node2(N_EXPR, list(), asn, tf));
		}
		if (is_dtor && base && base->has_user_dtor)
			epilogue.push_back(base_call(base->name + "___dtor"));

		if (!prologue.empty() || !epilogue.empty()) {
			node_t outer = list();
			for (node_t s : prologue) append(outer, s);
			append(outer, body);
			for (node_t s : epilogue) append(outer, s);
			body = node2(N_BLOCK, list(), outer, tf);
		}
	}

	// Variadic bodies need no function-entry priming: the user's
	// `va_start(ap, last)` macro now lowers directly to the c2mir intrinsic
	// `__builtin_va_start(ap)` (see include/madc/stdarg.h), which initializes
	// the user's own va_list from the frame. The earlier model declared a
	// hidden `__va_args` master here and copied it per use site, but that extra
	// va_list struct copy mis-set reg_save_area in large frames (e.g. SMAUG's
	// bug() with char buf[MAX_STRING_LENGTH]), corrupting the list.

	return node4(N_FUNC_DEF, ret_type, decl, list(), body, tf);
}

// -----------------------------------------------------------------------
// Top-level module translation
// -----------------------------------------------------------------------

node_t CirBuilder::translate_module(Program *prog)
{
	if (!prog) return NULL;
	m_prog = prog;

	node_t module = simple(N_MODULE);
	node_t top_list = list();

	// Collect all TokenFunc entries
	std::vector<TokenFunc *> funcs;
	for (auto it = prog->pending_funcs.begin();
	     it != prog->pending_funcs.end(); ++it) {
		TokenFunc *tf = dynamic_cast<TokenFunc *>(*it);
		if (tf && !tf->is_overridden)
			funcs.push_back(tf);
	}

	// Helper: resolve the (possibly pointer-wrapped) struct behind a DataDef.
	auto struct_behind = [](DataDef *dd) -> DataDefSTRUCT * {
		DataDefSTRUCT *s = dynamic_cast<DataDefSTRUCT *>(dd);
		if (!s) {
			DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd);
			if (p) s = dynamic_cast<DataDefSTRUCT *>(p->base_type);
		}
		return s;
	};

	// Pre-scan: structs that have a bare standalone definition entry
	// (`struct X {...};`). A typedef that references such a struct is a
	// forward reference and must emit STRUCT(tag, IGNORE) — the full body is
	// emitted once at the bare dkStruct, matching c2m's source structure.
	std::set<std::string> struct_def_points;
	for (auto &td : prog->top_decls) {
		if (td.kind == Program::DeclKind::dkStruct ||
		    td.kind == Program::DeclKind::dkUnion) {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(td.dd);
			if (sdd && !sdd->members.empty())
				struct_def_points.insert(sdd->name);
		}
	}

	// Pass 0: top-level declarations in source order (faithful mirror).
	// Stamp each declaration node with the source position the parser
	// recorded (file/line); the token itself isn't in scope at the record
	// site, so position is threaded here. Member nodes carry the real
	// origin token (see member_node).
	auto stamp = [&](node_t n, Program::TopDecl &td) {
		if (!n) return;
		cir_node *cn = CIR_NODE(n);
		if (td.origin) {
			// The origin token is the position source of truth; the node's
			// src_*() derive from it. Also feed c2mir's absolute store.
			cn->origin = td.origin;
			set_pos(cn, td.origin->file, td.origin->line, td.origin->column);
		} else if (td.file) {
			// No origin token captured (rare typedef variants): feed c2mir's
			// store the recorded file/line; node carries no +madc origin.
			set_pos(cn, td.file, td.line, 0);
		}
	};
	std::set<std::string> emitted_structs;
	std::set<std::string> emitted_globals;
	for (auto &td : prog->top_decls) {
		switch (td.kind) {
		case Program::DeclKind::dkTypedef: {
			DataDefSTRUCT *sdd = struct_behind(td.dd);
			// Forward ref iff the struct is defined by a separate bare
			// dkStruct and hasn't been emitted yet -> STRUCT(tag, IGNORE).
			bool forward = sdd && struct_def_points.count(sdd->name)
					   && !emitted_structs.count(sdd->name);
			node_t n = typedef_decl(td.name, td.dd, emitted_structs, forward);
			if (n) { stamp(n, td); append(top_list, n); }
			// A combined `typedef struct X {...} Y;` (no separate bare
			// def) emits the body inline here -> mark it emitted.
			if (sdd && !sdd->members.empty() && !struct_def_points.count(sdd->name))
				emitted_structs.insert(sdd->name);
			break;
		}
		case Program::DeclKind::dkStruct:
		case Program::DeclKind::dkUnion: {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(td.dd);
			if (sdd && !sdd->members.empty() && !emitted_structs.count(sdd->name)) {
				emitted_structs.insert(sdd->name);
				node_t sd = struct_def(sdd);
				if (sd) { stamp(sd, td); append(top_list, sd); }
			}
			break;
		}
		case Program::DeclKind::dkGlobalVar: {
			if (td.var && !dynamic_cast<FuncDef *>(td.var->type)) {
				// Pass the linked TokenDecl as origin so var_decl's
				// existing initializer logic (scalar + brace) emits the
				// global's init in its SPEC_DECL — the single source for
				// the CIR backend (tkProgram->statements is not walked here).
				node_t gd = var_decl(td.var, td.decl);
				if (gd) { stamp(gd, td); append(top_list, gd); }
				emitted_globals.insert(td.var->name);
			}
			break;
		}
		case Program::DeclKind::dkEnum:
			break;
		}
	}

	// Pass 0.5: user-defined class struct definitions. Classes are not in
	// top_decls; they live in struct_map (also keyed under any typedef
	// alias, so dedup by the DataDefCLASS pointer / tag name). Each lowers
	// to `struct ClassName { ... }` — base members are already flattened
	// into the derived class by the parser, so no base-before-derived
	// ordering is required.
	std::set<DataDefCLASS *> emitted_classes;
	for (auto &kv : prog->struct_map) {
		DataDefCLASS *cdd = as_user_class(kv.second);
		if (!cdd) continue;
		if (emitted_classes.count(cdd)) continue;
		if (emitted_structs.count(cdd->name)) continue;
		emitted_classes.insert(cdd);
		emitted_structs.insert(cdd->name);
		node_t cd = class_struct_def(cdd);
		if (cd) append(top_list, cd);
	}

	// Collect user function names.
	std::set<std::string> user_func_names;
	for (TokenFunc *tf : funcs)
		user_func_names.insert(tf->var.name);

	// Translate function bodies first, into a temp list, so referenced_funcs
	// (populated as N_CALL nodes are built) is complete before the prototype
	// pass — letting us emit extern protos for ONLY referenced functions.
	std::vector<node_t> func_def_nodes;
	for (TokenFunc *tf : funcs) {
		node_t fd = func_def(tf);
		if (fd) func_def_nodes.push_back(fd);
	}

	// Pass 0.75: Extern function prototypes — referenced-only (matches c2m,
	// which only declares what #include pulled in).
	for (auto &kv : prog->funcdef_map) {
		const std::string &fname = kv.first;
		FuncDef *fd = kv.second;
		if (!fd || user_func_names.count(fname)) continue;
		if (!referenced_funcs.count(fname)) continue;

		DataDef *ret_dd = &fd->returns;
		bool ret_is_ptr = ret_dd && ret_dd->is_pointer();
		DataDefPTR *ret_ptr = ret_is_ptr ? dynamic_cast<DataDefPTR *>(ret_dd) : NULL;
		if (ret_ptr && ret_ptr->base_type) ret_dd = ret_ptr->base_type;

		// Build: EXTERN + type spec
		node_t ext_list = list();
		append(ext_list, simple(N_EXTERN));
		if (ret_dd && ret_dd->is_struct() && !ret_dd->is_complex()) {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(ret_dd);
			if (sdd)
				append(ext_list, node2(N_STRUCT, id(sdd->name.c_str()), ignore()));
			else
				append_type_specs(ext_list, ret_dd);
		} else {
			append_type_specs(ext_list, ret_dd);
		}
		node_t share = node1(N_SHARE, ext_list);

		// Parameters
		node_t param_list = list();
		if (fd->parameters.empty() && fd->is_void_params) {
			node_t void_spec = node1(N_LIST, simple(N_VOID));
			node_t void_decl = node2(N_DECL, ignore(), list());
			append(param_list, node2(N_TYPE, void_spec, void_decl));
		} else {
			size_t nparam = fd->parameters.size();
			if (fd->is_varargs && nparam > 0) nparam--;
			for (size_t i = 0; i < nparam; i++) {
				char pname[16];
				snprintf(pname, sizeof(pname), "p%zu", i);
				append(param_list, param_decl(fd->parameters[i], pname));
			}
		}
		if (fd->is_varargs)
			append(param_list, simple(N_DOTS));

		node_t func_inner = node1(N_FUNC, param_list);
		node_t func_id_node = id(fname.c_str());
		node_t decl_list = list();
		append(decl_list, func_inner);
		if (ret_is_ptr)
			append(decl_list, pointer());

		node_t decl = node2(N_DECL, func_id_node, decl_list);

		node_t proto = simple(N_SPEC_DECL);
		append(proto, share);
		append(proto, decl);
		append(proto, ignore());
		append(proto, ignore());
		append(proto, ignore());
		append(top_list, proto);
	}

	// Pass 0.78: extern declarations for referenced libc globals.
	// stderr/stdout/stdin (and any future lazily-registered libc global) are
	// added via addGlobal() during expression parsing, so they never appear
	// in prog->top_decls and are never emitted by Pass 0. Without a decl the
	// emitted C fails with "'stderr' undeclared". Emit `extern <type> name;`
	// for every referenced global the top-level pass did not already define.
	for (auto &kv : referenced_globals) {
		const std::string &gname = kv.first;
		Variable *gv = kv.second;
		if (!gv || !gv->type) continue;
		if (emitted_globals.count(gname)) continue;

		// Skip function symbols referenced as values (function pointers):
		// they already have a prototype/definition; an `extern <type> name;`
		// would clash ("redeclared as different kind of symbol").
		if (gv->type->is_function())
			continue;
		{
			DataDefPTR *fp = dynamic_cast<DataDefPTR *>(gv->type);
			if (fp && fp->base_type && fp->base_type->is_function())
				continue;
		}
		if (user_func_names.count(gname) || prog->funcdef_map.count(gname))
			continue;

		DataDef *gdd = gv->type;
		bool g_is_ptr = gdd->is_pointer();
		DataDefPTR *gptr = g_is_ptr ? dynamic_cast<DataDefPTR *>(gdd) : NULL;
		if (gptr && gptr->base_type) gdd = gptr->base_type;

		node_t ext_list = list();
		append(ext_list, simple(N_EXTERN));
		append_type_specs(ext_list, gdd);
		node_t share = node1(N_SHARE, ext_list);

		node_t var_id = id(gname.c_str());
		node_t decl_list = list();
		if (g_is_ptr)
			append(decl_list, pointer());
		node_t decl = node2(N_DECL, var_id, decl_list);

		node_t proto = simple(N_SPEC_DECL);
		append(proto, share);
		append(proto, decl);
		append(proto, ignore());
		append(proto, ignore());
		append(proto, ignore());
		append(top_list, proto);
		emitted_globals.insert(gname);
	}

	// Pass 0.8: output externs (madc_* builtins; libstdc++ stream symbols later)
	for (auto &kv : m_output_externs)
		append(top_list, kv.second);
	for (node_t p : m_stream_object_protos)
		append(top_list, p);

	// Pass 1: Forward declarations
	for (TokenFunc *tf : funcs) {
		if (tf->var.name == "main") continue;
		node_t proto = func_proto(tf);
		if (proto) append(top_list, proto);
	}

	// Pass 1.5: per-class virtual dispatch tables. Emitted after the method
	// prototypes they reference (Pass 1) and before the function definitions
	// (Pass 2) that initialize __vptr from them. Iterate struct_map once more,
	// deduped by class pointer.
	std::set<DataDefCLASS *> emitted_vtables;
	for (auto &kv : prog->struct_map) {
		DataDefCLASS *cdd = as_user_class(kv.second);
		if (!cdd || !cdd->has_vtable) continue;
		if (emitted_vtables.count(cdd)) continue;
		emitted_vtables.insert(cdd);
		node_t vt = class_vtable_def(cdd);
		if (vt) append(top_list, vt);
	}

	// Pass 1.6: synthesized destructors for classes that need a dtor (object
	// members and/or a base dtor) but have no user-written one. Emitted here
	// so the symbol is in scope for the cleanup attribute in function bodies
	// (Pass 2). Deduped by class pointer.
	std::set<DataDefCLASS *> emitted_synth_dtors;
	for (auto &kv : prog->struct_map) {
		DataDefCLASS *cdd = as_user_class(kv.second);
		if (!cdd || cdd->has_user_dtor || !class_needs_dtor(cdd)) continue;
		if (emitted_synth_dtors.count(cdd)) continue;
		emitted_synth_dtors.insert(cdd);
		node_t dd = synth_dtor_def(cdd);
		if (dd) append(top_list, dd);
	}

	// Pass 2: Function definitions (translated above).
	for (node_t fd : func_def_nodes)
		append(top_list, fd);

	append(module, top_list);
	return module;
}

// ---------------------------------------------------------------------------
// cir_dump_nodes — madc-owned walker over the cir_node tree.
// ---------------------------------------------------------------------------
// Prints our enhanced CIR tree directly (not via c2mir's dumper): node type,
// literal payload, and the +madc fields (source position, typedef alias).
// Leaf nodes are codes <= N_ID (payload lives in the union); every other
// node iterates its u.ops children. Every node in a CirBuilder tree is an
// arena-allocated cir_node, so CIR_NODE() is always valid here.

static void cir_dump_node(FILE *f, node_t n, int indent)
{
	if (!n) return;
	for (int i = 0; i < indent; i++) fputc(' ', f);
	fprintf(f, "%s", c2mir_node_code_name((c2mir_node_code_t)n->code));

	switch (n->code) {
	case N_I: case N_L:  fprintf(f, " %lld", (long long)n->u.l); break;
	case N_LL:           fprintf(f, " %lld", (long long)n->u.ll); break;
	case N_U: case N_UL: fprintf(f, " %llu", (unsigned long long)n->u.ul); break;
	case N_ULL:          fprintf(f, " %llu", (unsigned long long)n->u.ull); break;
	case N_F:            fprintf(f, " %g", (double)n->u.f); break;
	case N_D:            fprintf(f, " %g", n->u.d); break;
	case N_CH: case N_CH16: case N_CH32: fprintf(f, " '%d'", (int)n->u.ch); break;
	case N_STR: case N_STR16: case N_STR32:
		fprintf(f, " \"%s\"", n->u.s.s ? n->u.s.s : ""); break;
	case N_ID:           fprintf(f, " %s", n->u.s.s ? n->u.s.s : ""); break;
	default: break;
	}

	cir_node *cn = CIR_NODE(n);
	if (cn->typedef_name) fprintf(f, "  [typedef=%s]", cn->typedef_name);
	if (cn->error_msg) fprintf(f, "  [ERROR: %s]", cn->error_msg);
	if (cn->src_file()) fprintf(f, "  @%s:%d:%d", cn->src_file(), cn->src_line(), cn->src_column());
	fputc('\n', f);

	if (n->code > N_ID) {
		for (int i = 0; ; i++) {
			node_t op = c2mir_node_op(n, i);
			if (!op) break;
			cir_dump_node(f, op, indent + 2);
		}
	}
}

void cir_dump_nodes(FILE *f, node_t tree)
{
	fprintf(f, "=== CIR NODE TREE (+madc) ===\n");
	cir_dump_node(f, tree, 0);
	fprintf(f, "=== END CIR NODE TREE ===\n");
}

// Recursively count (and optionally report) error/incomplete nodes.
static int cir_walk_errors(FILE *f, node_t n, bool report)
{
	if (!n) return 0;
	int count = 0;
	cir_node *cn = CIR_NODE(n);
	if (cn->error_msg) {
		count++;
		if (report && f) {
			fprintf(f, "cir error: %s", cn->error_msg);
			if (cn->src_file())
				fprintf(f, " @%s:%d:%d", cn->src_file(),
					cn->src_line(), cn->src_column());
			fputc('\n', f);
		}
	}
	if (n->code > N_ID) {
		for (int i = 0; ; i++) {
			node_t op = c2mir_node_op(n, i);
			if (!op) break;
			count += cir_walk_errors(f, op, report);
		}
	}
	return count;
}

bool cir_tree_has_error(node_t tree) { return cir_walk_errors(NULL, tree, false) > 0; }
int  cir_report_errors(FILE *f, node_t tree) { return cir_walk_errors(f, tree, true); }
