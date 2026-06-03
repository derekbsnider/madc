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
#include <algorithm>
#include <iostream>
#include <sstream>
#include <fstream>
#include <stdint.h>
#include <setjmp.h>
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
#include "madc_mangle.h"

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

std::string CirBuilder::var_emit_name(const Variable &v) const
{
	if (v.storage_alias_name.empty() || !m_prog)
		return v.name;
	// storage_alias_name is overloaded: an __attribute__((alias("data")))
	// DATA alias names another DEFINED file-scope variable, while an asm-label
	// on a function (`void f() asm("g")`) also lands here. Only the data-alias
	// case is resolved at the cir layer (the symbol IS the target's storage);
	// a function/asm-label keeps its own emitted name (its existing call/ptr
	// path already handles the label), so rewrite ONLY when the alias resolves
	// to a real, distinct, non-function Variable.
	if (v.type && v.type->is_function())
		return v.name;
	Variable *target =
		m_prog->resolve_global_storage_variable(const_cast<Variable *>(&v));
	if (target && target != &v && !target->name.empty()
	    && !(target->type && target->type->is_function()))
		return target->name;
	return v.name;
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

node_t CirBuilder::integer_typed(int64_t val, DataDef *dd, TokenBase *origin)
{
	// Choose the c2mir literal node code from the constant's own type so
	// its signedness and width survive into c2mir's usual-arithmetic
	// conversions. Without this every literal was N_I/N_L (always signed),
	// so e.g. `0xffffffffull` lowered to a signed `long` and a comparison
	// against a negative int chose the wrong (signed) result. Picking the
	// code from is_unsigned()/size mirrors GCC's literal-typing: the
	// operand self-determines its type, the operator/conversion follows.
	if (!dd || !dd->is_integer())
		return integer((long)val, origin);
	bool uns = dd->is_unsigned();
	bool wide = (dd->size > 4);   // 8-byte long / long long
	c2mir_node_code_t code;
	if (wide)
		code = uns ? N_UL : N_L;
	else
		code = uns ? N_U  : N_I;
	cir_node *cn = make(code, origin);
	switch (code) {
	case N_U:   cn->base.u.ul = (c2mir_ulong)(uint32_t)val; break;
	case N_UL:  cn->base.u.ul = (c2mir_ulong)(uint64_t)val; break;
	default:    cn->base.u.l  = (c2mir_long)val;            break;
	}
	return cn->as_node();
}

node_t CirBuilder::real(double val, TokenBase *origin)
{
	cir_node *cn = make(N_D, origin);
	cn->base.u.d = val;
	return cn->as_node();
}

// Single-precision real literal (`1.0f`) — c2mir's N_F constant node.
node_t CirBuilder::real_float(float val, TokenBase *origin)
{
	cir_node *cn = make(N_F, origin);
	cn->base.u.f = val;
	return cn->as_node();
}

// An IMAGINARY literal (`1.0i`, `2.0iF`, `3i`) — the lexer types it as a
// DataDefCOMPLEX whose element_type carries the float/double/long-double width.
// c2mir models such a literal as an N_CF/N_CD/N_CLD node whose stored value is
// the IMAGINARY part (real part = 0): the same nodes c2mir's own lexer emits for
// an `i`-suffixed constant. Emitting a bare N_D here (dropping the `i`) made
// `5.0 + 7.0i` fold to the real `12.0` — the imaginary part vanished.
node_t CirBuilder::complex_literal(double val, DataDef *complex_dd, TokenBase *origin)
{
	DataDef *elem = NULL;
	if (DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(complex_dd))
		elem = cdd->element_type;
	DataType edt = elem ? elem->rawtype() : DataType::dtDOUBLE;
	if (edt == DataType::dtFLOAT) {
		cir_node *cn = make(N_CF, origin);
		cn->base.u.f = (float)val;
		return cn->as_node();
	}
	if (elem && elem->size > 8) {        // long double element
		cir_node *cn = make(N_CLD, origin);
		cn->base.u.ld = (long double)val;
		return cn->as_node();
	}
	cir_node *cn = make(N_CD, origin);
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
	// std::string now ALSO carries the generic dtRESERVED class tag (P2.14), but
	// it is NOT a plain user class: it lowers to an opaque runtime buffer with
	// mangled libstdc++ ctor/dtor/method symbols, not a C struct. Route it via
	// as_object_class instead. Recognized by identity, not by a special tag.
	if (is_std_string(dd)) return NULL;
	return dynamic_cast<DataDefCLASS *>(dd);
}

// A runtime-object class that flows through the class model but is NOT a
// dtRESERVED user class: std::string (std::string). It IS-A DataDefCLASS (opaque
// storage sized by object_class_words, ctor/dtor/methods bound to mangled
// libstdc++ symbols via emit_symbol). Used to route std::string declarations,
// construction and destruction through the uniform class path, replacing the
// legacy std::string `long[]`+wrapper lowering.
//
// There is exactly ONE populated std::string class object: ddSTRING (it owns the
// ctors, dtor, methods and operators, all bound to mangled libstdc++ symbols).
// A `string&` parameter is typed by the separate ddSTRINGref singleton, which
// carries NO methods — so for ANY std::string identity (value OR reference) this
// canonicalizes to ddSTRING. That gives every caller (method/operator dispatch,
// sizing, ctor/dtor) the one class that actually has the members; a string&
// receiver dispatches std::string's operators exactly like a string value
// (P2.8b). The value-vs-reference storage distinction is carried by is_pointer()
// / the vfREFERENCE Variable flag, not by which class object is returned. This
// folds in the former canonical_string_class shim — no separate canonicalize step.
static DataDefCLASS *as_object_class(DataDef *dd)
{
	if (!dd) return NULL;
	if (dd->basetype() != BaseType::btClass) return NULL;
	if (dd->is_pointer()) return NULL;
	if (!is_std_string(dd)) return NULL;
	return &ddSTRING;
}

// Either an ordinary user class or a runtime-object class (std::string) — both
// lower to a real C struct and use the class ctor/dtor/cleanup machinery.
static DataDefCLASS *as_class_instance(DataDef *dd)
{
	if (DataDefCLASS *c = as_user_class(dd)) return c;
	return as_object_class(dd);
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
	// std::string is the value type `string`; std::string& (`string&`) and
	// std::string* are separate and handled on the param/pointer paths.
	return dd && dd->is_string() && !dd->is_pointer();
}

bool CirBuilder::is_string_object_value(TokenBase *arg)
{
	// A string OBJECT value is a DECLARED `string` variable — an lvalue with
	// constructed storage. Many other expressions are typed std::string but are
	// const char* values, NOT objects: string literals, lifted `__literal__`
	// vars, and char*-valued expressions like a ternary of literals
	// (`cond ? "a" : "b"`). So require a genuine variable token; everything
	// else flows through the const char* path (str()/char* operator<<).
	// A string-OBJECT element of a container (`v[i]` where v's operator[]
	// returns a string&) is a real string object reached by address. A string
	// element of a raw `string*`/`string[]` array (`keys[i]`) is likewise a
	// real string object.
	if (is_string_subscript(arg) || is_string_array_subscript(arg))
		return true;
	// A string `operator+` expression (`a + b`, `a + "lit"`, chained `a+b+c`)
	// is a by-value std::string RVALUE — a real object materialized into a
	// cleanup-tagged temp by class_operator_call. It reads as a string object so
	// `string c = a + b` copy-constructs and `cout << (a+b)` prints it.
	if (is_string_operator_plus(arg))
		return true;
	TokenVar *tv = dynamic_cast<TokenVar *>(arg);
	if (!tv)
		return false;
	// A declared `string` object (std::string) OR a `string&` reference parameter
	// (std::string&) — both denote a std::string object the front end can take
	// the address of. (A by-value `string` param is also std::string.)
	if (!is_std_string(tv->var.type))
		return false;
	if (tv->var.name.compare(0, 11, "__literal__") == 0)
		return false;
	// A const-qualified compile-time char* constant (`const char* x = "lit"`,
	// std::string-typed) is a const char* VALUE, not an object — skip it. But a
	// `const string&` reference parameter (std::string&, OR vfREFERENCE) is a real
	// std::string object passed by address; the const qualifies the binding, not
	// the storage, so it stays an object (P2.8b). Only exclude a NON-reference
	// constant.
	bool is_ref = is_std_string_ref(tv->var.type)
		      || (tv->var.flags & vfREFERENCE);
	if (tv->var.is_constant() && !is_ref)
		return false;
	return true;
}

// Is `arg` a std::string `operator+` expression (`a + b`, `a + "lit"`, or a
// chained `a + b + c`)? Recognized when it is a tkAdd whose LHS is a genuine
// string OBJECT (not a literal/char*) AND whose RHS is string-like (a string
// object or a const char* / std::string value). Such an expression binds the
// std::string operator+ and yields a by-value string object. A chained
// `(a+b)+c` recurses through the LHS (itself a string operator+). EXCLUDED:
// `"lit" + n` / `label + i` (char* pointer arithmetic — LHS is not an object),
// and `s + n` (string + numeric — RHS not string-like; no such operator+).
// This MUST mirror the tkAdd guard in class_operator_call so the two agree.
bool CirBuilder::is_string_operator_plus(TokenBase *arg)
{
	TokenOperator *top = dynamic_cast<TokenOperator *>(arg);
	if (!top || top->id() != TokenID::tkAdd) return false;
	if (!top->left || !top->right) return false;
	bool lhs_obj = is_string_object_value(top->left);
	DataDef *rdd = top->right->datadef();
	// RHS char* test uses type() (unstripped), NOT rawtype(): rawtype() strips
	// the pointer offset (dtCHARptr -= 10000 -> dtINT8), so `rawtype()==dtCHARptr`
	// is always false. A bare string literal is now const char* (dtCHARptr), so
	// `s + "lit"` reaches here as RHS char* and must bind string_concat.
	bool rhs_strlike = is_string_object_value(top->right)
			   || (rdd && (rdd->is_string()
				       || (rdd->is_pointer()
					   && rdd->type() == DataType::dtCHARptr)));
	return lhs_obj && rhs_strlike;
}

// A CALL to a madc-COMPILED function returning a std::string by value (std::string,
// non-pointer). func_def lowers such a function through the __retbuf ABI, so the
// call site must materialize the result into a temp it owns. Gated on
// m_user_func_names: an external / native function with a std::string return (e.g.
// __std_to_string, php::/perl:: helpers) keeps its own ABI and is NOT rewritten —
// matching it here would inject a bogus __retbuf arg ("too many arguments").
// The FuncDef behind a CALL token: either the called function directly, or the
// TARGET signature of a function-pointer variable being called indirectly
// (`auto f = [...]; f()`). Both use the same __retbuf ABI for object returns.
static FuncDef *call_target_funcdef(TokenCallFunc *tcf)
{
	if (!tcf) return NULL;
	if (FuncDef *fd = dynamic_cast<FuncDef *>(tcf->var.type))
		return fd;
	if (DataDefFPTR *fp = dynamic_cast<DataDefFPTR *>(tcf->var.type))
		return fp->target;
	return NULL;
}

bool CirBuilder::is_string_returning_call(TokenBase *arg)
{
	TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(arg);
	if (!tcf) return false;
	FuncDef *fd = call_target_funcdef(tcf);
	if (!fd) return false;
	if (!is_std_string(&fd->returns) || fd->returns.is_pointer())
		return false;
	// A direct call is gated on m_user_func_names (only madc-emitted bodies use
	// the __retbuf ABI; external string-returning fns keep their own). A call
	// THROUGH a fn-ptr whose target is itself a madc-emitted retbuf function
	// (e.g. a hoisted lambda) carries that ABI in its rendered type, so the
	// indirect call must materialize the temp regardless of the variable name.
	if (dynamic_cast<DataDefFPTR *>(tcf->var.type))
		return true;
	return m_user_func_names && m_user_func_names->count(tcf->var.name) > 0;
}

// A CALL to a madc-COMPILED function returning a NON-TRIVIAL user class by value.
// Such a call is lowered (func_def/func_proto) through the SAME __retbuf ABI as a
// string return, so the call site must materialize the result into a caller temp
// it owns (object_call_temp_addr). Gated on m_user_func_names like the string
// predicate: an external/native class-returning fn keeps its own ABI and must
// NOT be rewritten (a bogus __retbuf arg would be "too many arguments"). std::
// string and trivial structs are excluded by class_return_via_retbuf.
DataDefCLASS *CirBuilder::object_returning_call_class(TokenBase *arg)
{
	TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(arg);
	if (!tcf) return NULL;
	FuncDef *fd = call_target_funcdef(tcf);
	if (!fd) return NULL;
	DataDefCLASS *cdd = class_return_via_retbuf(&fd->returns);
	if (!cdd) return NULL;
	// As in is_string_returning_call: a fn-ptr call inherits the retbuf ABI
	// from its rendered target type; a direct call is gated on m_user_func_names.
	if (dynamic_cast<DataDefFPTR *>(tcf->var.type))
		return cdd;
	if (!(m_user_func_names && m_user_func_names->count(tcf->var.name) > 0))
		return NULL;
	return cdd;
}

size_t CirBuilder::string_obj_words() const
{
	// cir_builder is itself C++, so it knows the real ABI size/alignment of
	// std::string. A long[] buffer is naturally 8-aligned and >= the object
	// size, so placement-new of std::string into it is well-aligned without an
	// explicit alignment attribute.
	return (sizeof(std::string) + sizeof(long) - 1) / sizeof(long);
}

// Words of opaque storage for a runtime-object class — one with a concrete ABI
// size (cdd->size) but no madc data members (std::string). cir_builder is itself
// C++, so cdd->size already holds the real sizeof(std::string). Returns 0 for an
// ordinary user class (its struct is built from its declared members).
size_t CirBuilder::object_class_words(DataDefCLASS *cdd) const
{
	if (!cdd || !cdd->members.empty()) return 0;
	if (cdd->size == 0) return 0;
	return (cdd->size + sizeof(long) - 1) / sizeof(long);
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

node_t CirBuilder::char_ptr_type()
{
	node_t spec = list();
	append(spec, simple(N_CHAR));
	node_t decl_list = list();
	append(decl_list, pointer());
	return node2(N_TYPE, spec, node2(N_DECL, ignore(), decl_list));
}

// N_TYPE cast node for an arbitrary pointer DataDef (e.g. `void *`, `T **`).
// Peels the pointer levels to the ultimate pointee, then re-applies that many
// `*` declarators atop the pointee's spec — the same idiom va_arg uses.
node_t CirBuilder::ptr_type_node(DataDef *dd)
{
	int levels = 0;
	DataDef *base = dd;
	while (base && base->is_pointer()) {
		DataDefPTR *p = dynamic_cast<DataDefPTR *>(base);
		if (!p) break;
		base = p->base_type;
		levels++;
	}
	if (levels == 0)
		return void_ptr_type();
	node_t decl_list = list();
	for (int i = 0; i < levels; i++) append(decl_list, pointer());
	return node2(N_TYPE, type_list(base),
		     node2(N_DECL, ignore(), decl_list));
}

// N_TYPE node for a `struct Cls *` cast target (matches class_struct_def's
// `struct Cls` spec + one pointer level). Used for derived->base upcasts.
node_t CirBuilder::class_ptr_type(DataDefCLASS *cdd)
{
	node_t spec = list();
	append(spec, node2(N_STRUCT, id(cdd->name.c_str()), ignore()));
	node_t decl_list = list();
	append(decl_list, pointer());
	return node2(N_TYPE, spec, node2(N_DECL, ignore(), decl_list));
}

// GNU C allows pointer arithmetic on `void *`, treating the element size as 1
// (i.e. as if the pointer were `char *`). c2mir rejects it ("pointer to
// incomplete type as an operand of +/-"), so when madc emits `+`/`-` on a
// `void *` operand it casts that operand to `char *` first — the size-1
// semantics GCC uses. Scoped to `void *` only: madc represents many in-flight
// container/forward-declared structs with is_complete==false even though they
// have a real size, so widening this predicate to "any incomplete pointee"
// wrongly rewrites legitimate sized-pointer arithmetic.
static bool is_size1_pointer(DataDef *dd)
{
	if (!dd || !dd->is_pointer()) return false;
	DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd);
	if (!p || !p->base_type) return false;
	return p->base_type->rawtype() == DataType::dtVOID;
}

// The single-level pointee user-class of `dd` when `dd` is a `Cls *` pointing
// at a real (dtRESERVED) user class; NULL otherwise. std::string and the stream
// classes are deliberately excluded (they never participate in inheritance).
static DataDefCLASS *pointee_user_class(DataDef *dd)
{
	if (!dd || !dd->is_pointer()) return NULL;
	DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd);
	if (!p) return NULL;
	return as_user_class(p->base_type);
}

// The user-class a class-pointer-valued expression yields. `new B()` carries
// its allocated class directly (TokenNEW::alloc_class, never reflected in
// datadef()); any other expression is read through its `Cls *` datadef pointee.
static DataDefCLASS *expr_pointee_class(TokenBase *tb)
{
	if (!tb) return NULL;
	if (TokenNEW *tn = dynamic_cast<TokenNEW *>(tb))
		if (!tn->placement)
			return as_user_class(tn->alloc_class);
	return pointee_user_class(tb->datadef());
}

// Derived->base pointer conversion. When `lhs_dd` is `Base*` and the RHS
// expression `rhs` yields a `Derived*` with Derived deriving from (but not
// equal to) Base, wrap `value` in an explicit `(Base*)` cast so the emitted C
// is clean. C++ permits the implicit upcast (single inheritance: base subobject
// at offset 0, __vptr first), so the value is unchanged — this only makes the
// conversion explicit and silences c2mir's "incompatible types in assignment to
// a pointer". Returns `value` unchanged when no upcast applies. Downcasts and
// multiple inheritance are out of scope (see P2.6).
node_t CirBuilder::upcast_class_ptr(node_t value, DataDef *lhs_dd, TokenBase *rhs,
				    TokenBase *origin)
{
	DataDefCLASS *base = pointee_user_class(lhs_dd);
	DataDefCLASS *derived = expr_pointee_class(rhs);
	if (!base || !derived || base == derived) return value;
	if (!derived->is_or_derives_from(base)) return value;
	// Adjust the pointer to the base subobject. Single inheritance / primary base
	// = offset 0 (a plain cast, byte-identical to before); a secondary or virtual
	// base sits at a non-zero offset, so emit (Base*)((char*)value + offset).
	size_t off = derived->base_offset_of(base);
	node_t adj = value;
	if (off != 0 && off != (size_t)-1) {
		node_t charp = node2(N_CAST,
			node2(N_TYPE, node1(N_LIST, simple(N_CHAR)),
			      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
			value, origin);
		adj = node2(N_ADD, charp, integer((long)off), origin);
	}
	return node2(N_CAST, class_ptr_type(base), adj, origin);
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

// libstdc++ std::string (__cxx11) member symbols, GENERATED by the Itanium
// mangler (madc_mangle, substitution-aware) from the canonical libstdc++ type —
// NOT hand-written. madc is Cfront: std::string operations lower to DIRECT calls
// on these real libstdc++ members (the object is a real libstdc++ std::string),
// resolved against libstdc++ by the dlsym(_Z…) import resolver — same model as
// the cout `<<` path. Computed once at static init; the const char* aliases
// point into the cached std::strings (which live for the program). Adding a new
// std::string method here is one mangler call, not a hand-written wrapper.
static const std::string STR_TYPE     = std_string_type();
static const std::string STR_CTOR0_s  = itanium_mangle_ctor_sub(STR_TYPE, {});
static const std::string STR_CTOR_S_s = itanium_mangle_ctor_sub(STR_TYPE, {"const char*", "const std::allocator<char>&"});
static const std::string STR_CTOR_CP_s= itanium_mangle_ctor_sub(STR_TYPE, {"const " + STR_TYPE + "&"});
static const std::string STR_DTOR_s   = itanium_mangle_dtor_sub(STR_TYPE);
static const std::string STR_CSTR_s   = itanium_mangle_member_sub(STR_TYPE, "c_str", {}, true);
static const std::string STR_SIZE_s   = itanium_mangle_member_sub(STR_TYPE, "size", {}, true);
static const std::string STR_LENGTH_s = itanium_mangle_member_sub(STR_TYPE, "length", {}, true);
static const std::string STR_CLEAR_s  = itanium_mangle_member_sub(STR_TYPE, "clear", {}, false);
static const std::string STR_ASGN_S_s = itanium_mangle_operator_sub(STR_TYPE, "=", {"const char*"}, false);
static const std::string STR_ASGN_C_s = itanium_mangle_operator_sub(STR_TYPE, "=", {"const " + STR_TYPE + "&"}, false);
static const std::string STR_APP_S_s  = itanium_mangle_operator_sub(STR_TYPE, "+=", {"const char*"}, false);
static const std::string STR_APP_C_s  = itanium_mangle_operator_sub(STR_TYPE, "+=", {"const " + STR_TYPE + "&"}, false);
static const char *const STR_CTOR0  = STR_CTOR0_s.c_str();
static const char *const STR_CTOR_S = STR_CTOR_S_s.c_str();
static const char *const STR_CTOR_CP= STR_CTOR_CP_s.c_str();
static const char *const STR_DTOR   = STR_DTOR_s.c_str();
static const char *const STR_CSTR   = STR_CSTR_s.c_str();
static const char *const STR_SIZE   = STR_SIZE_s.c_str();
static const char *const STR_LENGTH = STR_LENGTH_s.c_str();
static const char *const STR_CLEAR  = STR_CLEAR_s.c_str();
static const char *const STR_ASGN_S = STR_ASGN_S_s.c_str();
static const char *const STR_ASGN_C = STR_ASGN_C_s.c_str();
static const char *const STR_APP_S  = STR_APP_S_s.c_str();
static const char *const STR_APP_C  = STR_APP_C_s.c_str();

// Hidden return-slot pointer parameter for a by-value string-returning function.
const char *CirBuilder::RETBUF_NAME = "__retbuf";

// `long <name>[NWORDS];` — the opaque, 8-aligned storage for a string object.
// The cleanup attribute names the REAL libstdc++ dtor (it takes only `this`),
// so c2mir destructs the object via the genuine std::string destructor.
node_t CirBuilder::string_storage_decl(const char *name, TokenBase *origin)
{
	return obj_storage_decl(name, string_obj_words(), STR_DTOR, origin);
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

// ---- std::stringstream object lowering ----
bool CirBuilder::is_sstream_object(DataDef *dd)
{
	return dd && dd->rawtype() == DataType::dtSSTREAM && !dd->is_pointer();
}

size_t CirBuilder::sstream_obj_words() const
{
	return (sizeof(std::stringstream) + sizeof(long) - 1) / sizeof(long);
}

node_t CirBuilder::sstream_storage_decl(const char *name, TokenBase *origin)
{
	return obj_storage_decl(name, sstream_obj_words(), "sstream_destruct", origin);
}

node_t CirBuilder::sstream_ctor_call(const char *name, TokenBase *origin)
{
	return obj_default_ctor_call(name, "sstream_construct", origin);
}

// `(void*)&<name>` — the string object's address as void*, so the mangled
// libstdc++ members / runtime wrappers see a pointer to the object. A string
// local/global is a `struct string` instance, so its address is `&name` (NOT
// array decay). Used for synthetic temporaries (struct locals) — for a named
// program variable use string_var_addr, which honours pointer-stored params.
node_t CirBuilder::string_obj_addr(const char *name, TokenBase *origin)
{
	node_t cast = node2(N_CAST, void_ptr_type(),
			    node1(N_ADDR, id(name, origin), origin), origin);
	CIR_NODE(cast)->synth_from_origin = true;
	return cast;
}

// The object address of a NAMED string variable as void*. A by-value `string`
// parameter and a `string&` reference are lowered to a `void*` holding the
// object's address (param_decl), so the variable's value IS the address —
// `(void*)name`. A string local/global is a `struct string` instance whose
// address is `(void*)&name`. (Mirrors the param vs instance storage split.)
node_t CirBuilder::string_var_addr(const Variable &v, TokenBase *origin)
{
	node_t cast = node2(N_CAST, void_ptr_type(), object_var_addr(v, origin), origin);
	CIR_NODE(cast)->synth_from_origin = true;
	return cast;
}

// Construct the std::string object via the real libstdc++ ctor:
//   no init        -> basic_string()              C1Ev
//   string init    -> basic_string(const string&) C1ERKS4_  (copy ctor)
//   const char*    -> basic_string(const char*, const allocator&) C1EPKcRKS3_
// std::allocator<char> is empty/stateless, so the allocator& arg is a harmless
// dummy — we reuse the object's own address.
node_t CirBuilder::string_ctor_call(const char *name, TokenBase *initexpr,
				    TokenBase *origin)
{
	node_t args = list();
	append(args, string_obj_addr(name, origin));   // this
	const char *sym;
	if (initexpr && is_string_object_value(initexpr)) {
		append(args, string_obj_arg(initexpr));    // const string&
		sym = STR_CTOR_CP;
		need_output_extern(sym, false, { { {N_VOID}, true }, { {N_VOID}, true } });
	} else if (initexpr) {
		append(args, translate_expr(initexpr));        // const char*
		append(args, string_obj_addr(name, origin));   // dummy const allocator&
		sym = STR_CTOR_S;
		need_output_extern(sym, false,
				   { { {N_VOID}, true }, { {N_CHAR}, true }, { {N_VOID}, true } });
	} else {
		sym = STR_CTOR0;
		need_output_extern(sym, false, { { {N_VOID}, true } });
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
// is the std::string->dtCHARptr coercion applied at char*-expecting call sites.
node_t CirBuilder::string_cstr_arg(TokenBase *arg)
{
	need_output_extern(STR_CSTR, true, { { {N_VOID}, true } });
	node_t addr;
	if (TokenVar *tv = dynamic_cast<TokenVar *>(arg))
		addr = string_obj_addr(tv->var.name.c_str(), arg);
	else
		addr = node2(N_CAST, void_ptr_type(), translate_expr(arg), arg);
	node_t a = list();
	append(a, addr);
	node_t call = node2(N_CALL, id(STR_CSTR, arg), a, arg);
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
		// A string-OBJECT element (`v[i]`): the bare operator[] call IS the
		// element's address (a string*); cast to void*. Must use the bare call,
		// not translate_expr (which derefs to the 32-byte object lvalue).
		if (TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(arg))
			if (is_string_subscript(arg))
				return node2(N_CAST, void_ptr_type(),
					class_subscript_addr(tsub, arg), arg);
		// A raw string-array element (`keys[i]`, keys is a string*/string[]
		// local, param, or member array): translate_expr yields the element
		// lvalue; its address is the string object pointer.
		if (is_string_array_subscript(arg))
			return node2(N_CAST, void_ptr_type(),
				node1(N_ADDR, translate_expr(arg), arg), arg);
		// A string-object MEMBER (`obj.name`) is an embedded `long[W]` buffer
		// in the struct; its address is `(void*)&(obj.name)`. Translate the
		// member access (yields the FIELD lvalue), take its address, cast.
		if (dynamic_cast<TokenMember *>(arg))
			return node2(N_CAST, void_ptr_type(),
				node1(N_ADDR, translate_expr(arg), arg), arg);
		if (TokenVar *tv = dynamic_cast<TokenVar *>(arg))
			return string_var_addr(tv->var, arg);
		return node2(N_CAST, void_ptr_type(), translate_expr(arg), arg);
	}
	// A string-returning CALL is a string-object rvalue: materialize it into a
	// scope temp (the callee copy-constructs the result into the temp via the
	// __retbuf ABI) and pass the temp's address. Must precede the const char*
	// path: the call result is a `struct string`, not a const char*.
	if (is_string_returning_call(arg))
		return string_call_temp_addr(arg, arg);

	// const char* value -> temp std::string object.
	char name[32];
	snprintf(name, sizeof(name), "__madc_strtmp_%d", m_strtmp_counter++);
	m_pending_stmts.push_back(string_storage_decl(name, arg));
	m_pending_stmts.push_back(string_ctor_call(name, arg, arg));
	return string_obj_addr(name, arg);
}

// Translate a call's explicit arguments into `args`, coercing string-object
// parameters (passed by address, literals materialized to temps) and numeric
// reference parameters (passed by address). Shared by the normal call path and
// string_call_temp_addr. Does NOT inject hidden params (__this / __retbuf).
void CirBuilder::build_call_args(TokenCallFunc *tcf, node_t args)
{
	// Resolve the callee signature for both direct calls (FuncDef) AND indirect
	// calls through a function pointer / lambda variable (DataDefFPTR -> target).
	// Without the fptr fallback, a `string`-by-value parameter of a lambda is
	// invisible here, so a const char* literal argument is passed raw instead of
	// being materialized into a temp std::string -> the callee reads garbage.
	FuncDef *callee = call_target_funcdef(tcf);
	for (size_t i = 0; i < tcf->parameters.size(); i++) {
		TokenBase *arg = tcf->parameters[i];
		DataDef *pt = (callee && i < callee->parameters.size())
				? callee->parameters[i] : NULL;
		bool is_ref_param = callee && i < callee->ref_params.size()
				    && callee->ref_params[i];
		if (is_std_string(pt))
			append(args, string_obj_arg(arg));
		else if (is_ref_param)
			// Numeric reference parameter (`int &x`): the callee takes a
			// pointer, so pass the argument's address. The argument lvalue
			// translates normally (a vfREFERENCE arg re-derefs to its own
			// pointer, and &(*p) folds to p).
			append(args, node1(N_ADDR, translate_expr(arg), arg));
		else
			// Derived->base pointer argument (`B*` arg -> `A*` parameter):
			// make the implicit upcast explicit so c2mir does not warn.
			append(args, upcast_class_ptr(translate_expr(arg), pt, arg, arg));
	}
	// GNU nested-function / [&]-lambda capture forwarding: a call to a capturing
	// callee passes the address of each captured enclosing variable as a hidden
	// trailing argument, matching the `T *name` capture parameters func_def
	// appended. The captured Variable is in scope at this call site (the call
	// lives in the defining function). If THIS function also captured that same
	// variable (nested-in-nested), forward its capture pointer param directly
	// (it already holds &var); otherwise take its address here.
	if (callee && !callee->captured_vars.empty()) {
		for (Variable *cv : callee->captured_vars) {
			if (!cv) continue;
			if (m_cur_captured_fd && m_cur_capture_set.count(cv)) {
				note_capture(cv);
				append(args, id(cv->name.c_str(), tcf));
			} else {
				append(args, node1(N_ADDR, id(cv->name.c_str(), tcf), tcf));
			}
		}
	}
}

// Materialize a by-value string-returning CALL into a cleanup-tagged temp via
// the __retbuf ABI: declare `struct string __t;` (raw storage, cleanup-tagged),
// emit the call as a VOID call `f((struct string*)&__t, <args>)` whose callee
// copy-constructs the result into *__retbuf, and return the temp's (void*)
// address. Pushes both the storage decl and the call to m_pending_stmts.
// `call_tok` is the TokenCallFunc (so args can be retranslated with __retbuf).
// `struct string <name> __attribute__((cleanup(STR_DTOR)));` — raw storage for a
// string object filled by a return-slot/placement write (no ctor / initializer),
// destructed via the cleanup attribute. Pushes the decl to m_pending_stmts and
// returns the generated temp name. Shared by string_call_temp_addr (__retbuf ABI)
// and the by-value operator+ path (string_concat out-slot).
void CirBuilder::string_temp_decl(char *name_buf, size_t buf_sz, TokenBase *origin)
{
	snprintf(name_buf, buf_sz, "__madc_strtmp_%d", m_strtmp_counter++);

	node_t share = node1(N_SHARE, type_list(&ddSTRING));   // struct string
	node_t decl = node2(N_DECL, id(name_buf, origin), list());
	need_output_extern(STR_DTOR, false, { { {N_VOID}, true } });
	node_t attr_args = list();
	append(attr_args, id(STR_DTOR, origin));
	node_t attrs = list();
	append(attrs, node2(N_ATTR, id("cleanup", origin), attr_args, origin));
	node_t sd = simple(N_SPEC_DECL, origin);
	append(sd, share);
	append(sd, decl);
	append(sd, attrs);   // __attribute__((cleanup(STR_DTOR)))
	append(sd, ignore()); // asm
	append(sd, ignore()); // initializer — none; filled by the return-slot write
	CIR_NODE(sd)->synth_from_origin = true;
	m_pending_stmts.push_back(sd);
}

node_t CirBuilder::string_call_temp_addr(TokenBase *call_tok, TokenBase *origin)
{
	TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(call_tok);
	char name[32];
	// `struct string __t __attribute__((cleanup(STR_DTOR)));` — raw storage; the
	// callee copy-constructs into it, so no default ctor / initializer.
	string_temp_decl(name, sizeof(name), origin);

	// Void call: f((struct string*)&__t, <args>). The first arg is the return
	// slot (__retbuf). string_obj_addr yields (void*)&__t — the param is a
	// `struct string *`, and a void* converts implicitly.
	if (tcf) {
		referenced_funcs.insert(tcf->var.name);
		node_t cargs = list();
		append(cargs, string_obj_addr(name, origin));   // __retbuf = &__t
		build_call_args(tcf, cargs);
		node_t call = node2(N_CALL, id(tcf->var.name.c_str(), origin), cargs, origin);
		CIR_NODE(call)->synth_from_origin = true;
		m_pending_stmts.push_back(node2(N_EXPR, list(), call, origin));
	}

	return string_obj_addr(name, origin);   // (void*)&__t
}

// Materialize a NON-TRIVIAL class-returning CALL into a cleanup-tagged temp of
// that class, via the __retbuf ABI (mirrors string_call_temp_addr). Declares
// `struct Cls __t __attribute__((cleanup(Cls___dtor)));` (var_decl attaches the
// cleanup), emits the void call `f(&__t, <args>)` whose callee copy-constructs
// the result into *__retbuf, and returns the temp's (void*) address. Pushes the
// decl and the call to m_pending_stmts.
node_t CirBuilder::object_call_temp_addr(TokenBase *call_tok, DataDefCLASS *cdd,
					 TokenBase *origin)
{
	TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(call_tok);
	char name[40];
	snprintf(name, sizeof(name), "__madc_objtmp_%d", m_strtmp_counter++);

	// Temp object variable of the class type; var_decl emits the storage with a
	// cleanup(Cls___dtor) attribute (RAII scope-exit destruction).
	Variable *tmp = new Variable(name, *cdd, 1, NULL, false);
	tmp->flags |= vfLOCAL;
	m_pending_stmts.push_back(var_decl(tmp, origin));

	if (tcf) {
		referenced_funcs.insert(tcf->var.name);
		node_t cargs = list();
		append(cargs, node1(N_ADDR, id(name, origin), origin));   // __retbuf = &__t
		build_call_args(tcf, cargs);
		node_t call = node2(N_CALL, id(tcf->var.name.c_str(), origin), cargs, origin);
		CIR_NODE(call)->synth_from_origin = true;
		m_pending_stmts.push_back(node2(N_EXPR, list(), call, origin));
	}

	// The temp's (void*) address — the call's object-lvalue result.
	node_t addr = node2(N_CAST, void_ptr_type(),
			    node1(N_ADDR, id(name, origin), origin), origin);
	CIR_NODE(addr)->synth_from_origin = true;
	return addr;
}

// (std::string method calls — s.c_str()/.length()/.size()/.empty()/.clear() —
// now route through the uniform class path: CirBuilder::class_method_call ->
// emit_symbol_method_call, which calls the mangled libstdc++ member bound via
// FuncDef::emit_symbol. The legacy string_method_call interception was deleted
// in A4b Surface 2; empty() is still lowered to size()==0 there.)

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
	// A user-defined class lowers to `struct ClassName` too (same shape), as does
	// a runtime-object class (std::string -> `struct string`).
	// _Complex is a DataDefSTRUCT subclass but must use the native spec path.
	if (dd && (dd->is_struct() || as_class_instance(dd)) && !dd->is_complex()) {
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd);
		if (sdd) {
			// Tag kind MUST match the definition (struct vs union), or
			// c2mir rejects it ("kind of tag X unmatched"). A union-typed
			// reference emits N_UNION.
			node_t sref = node2(sdd->union_layout ? N_UNION : N_STRUCT,
					    id(sdd->name.c_str()), ignore());
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
// Mirror func_proto's __retbuf decision for a fn-ptr TARGET signature: a
// by-value std::string OR non-trivial class return (not pointer/ref/multi)
// uses the __retbuf ABI. Returns the returned object type (ddSTRING or the
// class) so the fn-ptr renders `void (*)(T*, params)`; NULL otherwise.
DataDef *CirBuilder::fnptr_retbuf_type(FuncDef *fd)
{
	if (!fd) return NULL;
	DataDef *ret_dd = &fd->returns;
	if (ret_dd->is_pointer() || fd->returns_ref || fd->is_multi_return())
		return NULL;
	if (is_std_string(ret_dd))
		return &ddSTRING;
	if (DataDefCLASS *obj = class_return_via_retbuf(ret_dd))
		return (DataDef *)obj;
	return NULL;
}

node_t CirBuilder::fnptr_func_node(FuncDef *fd)
{
	node_t param_list = list();
	if (!fd) return node1(N_FUNC, param_list);   // unknown signature -> ()

	// A by-value object-returning target uses the __retbuf ABI: a hidden
	// leading `T* __retbuf` param (abstract/unnamed in a fn-ptr type).
	DataDef *retbuf_dd = fnptr_retbuf_type(fd);
	if (retbuf_dd) {
		node_t pspec = type_list(retbuf_dd);
		node_t pdecl = list();
		append(pdecl, pointer());
		append(param_list, node2(N_TYPE, pspec, node2(N_DECL, ignore(), pdecl)));
	}

	size_t nparam = fd->parameters.size();
	if (fd->is_varargs && nparam > 0) nparam--;  // drop the synthetic vararg slot

	if (nparam == 0) {
		if (fd->is_void_params && !retbuf_dd) {
			node_t void_spec = node1(N_LIST, simple(N_VOID));
			node_t void_decl = node2(N_DECL, ignore(), list());
			append(param_list, node2(N_TYPE, void_spec, void_decl));
		}
		// else: bare () — unspecified parameter list (K&R), or retbuf-only
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
	// A by-value object-returning target uses the __retbuf ABI: the C return
	// type is `void` (the object travels through the hidden `T* __retbuf`
	// param injected by fnptr_func_node), so render `void` here and skip the
	// pointer-peel / struct-spec logic.
	if (fnptr_retbuf_type(fd)) {
		append_type_specs(spec_list, &ddVOID);
		for (size_t d = 0; d < lead_dims.size(); d++)
			append(decl_list, node3(N_ARR, ignore(), list(), integer(lead_dims[d])));
		if (emit_pointer)
			append(decl_list, pointer());
		append(decl_list, fnptr_func_node(fd));
		return;
	}

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
			append(spec_list, node2(sdd->union_layout ? N_UNION : N_STRUCT, id(sdd->name.c_str()), ignore()));
		else
			append_type_specs(spec_list, ret_dd);
	} else if (ret_dd && ret_dd->is_object()) {
		// A TRIVIAL class returned BY VALUE (no dtor -> not the __retbuf ABI,
		// handled above) is lowered as a native `struct <ClassName>` return;
		// append_type_specs would mis-render the class's dtRESERVED rawtype as
		// `int` (the bug that made `auto g = [](){ S s; ...; return s; }`
		// produce `int (*g)()`).
		append(spec_list, node2(N_STRUCT, id(ret_dd->name.c_str()), ignore()));
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

// The hidden return-slot parameter `struct <Cls> *__retbuf` of a by-value
// object-returning function. A named pointer parameter: N_SPEC_DECL with the
// returned class/struct spec (bare LIST, like param_decl's pspec) and a DECL
// carrying the name plus one pointer suffix. `retdd` is the returned type
// (ddSTRING for a string-returning fn, the user class for a class-returning fn).
node_t CirBuilder::retbuf_param(DataDef *retdd, TokenBase *origin)
{
	node_t pspec = type_list(retdd ? retdd : &ddSTRING);   // LIST(STRUCT(ID(name), IGNORE))
	node_t pdecl_list = list();
	append(pdecl_list, pointer());
	node_t sd = simple(N_SPEC_DECL, origin);
	append(sd, pspec);
	append(sd, node2(N_DECL, id(RETBUF_NAME, origin), pdecl_list));
	append(sd, ignore());
	append(sd, ignore());
	append(sd, ignore());
	return sd;
}

node_t CirBuilder::multi_return_unpack(TokenAssign *as, TokenBase *origin)
{
	// `a, b, ... := f(args)` — f uses the multi-return __retbuf ABI (void return +
	// leading `long *__retbuf`; values written to __retbuf[0..N-1]). Emit a caller
	// buffer `long __mret_K[N]`, the call `f(__mret_K, args)` (the array decays to
	// the long* __retbuf), and `multi_vars[i] = __mret_K[i]` for i>=1 — all into
	// m_pending_stmts (the block loop flushes them BEFORE this decl statement; the
	// i>=1 targets are scope vars auto-declared at block top, so assigning them
	// before this decl is well-formed). Return __mret_K[0] as multi_vars[0]'s init.
	TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(as->right);
	if (!tcf)
		return as->right ? translate_expr(as->right) : ignore();
	size_t n = as->multi_vars.size();
	char nm[32];
	snprintf(nm, sizeof nm, "__mret_%d", m_mret_counter++);

	// long __mret_K[n];
	node_t decl_list = list();
	append(decl_list, node3(N_ARR, ignore(), list(), integer((int64_t)n, origin)));
	node_t bufdecl = simple(N_SPEC_DECL, origin);
	append(bufdecl, type_list(&ddINT64));
	append(bufdecl, node2(N_DECL, id(nm, origin), decl_list));
	append(bufdecl, ignore());
	append(bufdecl, ignore());
	append(bufdecl, ignore());
	m_pending_stmts.push_back(bufdecl);

	// f(__mret_K, args...);  — __retbuf passed first. build_call_args may queue
	// arg temps to m_pending_stmts; they land after bufdecl and before the call.
	referenced_funcs.insert(tcf->var.name);
	node_t cargs = list();
	append(cargs, id(nm, origin));
	build_call_args(tcf, cargs);
	node_t call = node2(N_CALL, id(tcf->var.name.c_str(), origin), cargs, origin);
	CIR_NODE(call)->synth_from_origin = true;
	m_pending_stmts.push_back(node2(N_EXPR, list(), call, origin));

	// multi_vars[i] = __mret_K[i];  for i >= 1
	for (size_t i = 1; i < n; i++) {
		node_t lhs = id(as->multi_vars[i]->name.c_str(), origin);
		node_t rhs = node2(N_IND, id(nm, origin),
				   integer((int64_t)i, origin), origin);
		m_pending_stmts.push_back(node2(N_EXPR, list(),
					  node2(N_ASSIGN, lhs, rhs, origin), origin));
	}

	// multi_vars[0]'s initializer = __mret_K[0]
	return node2(N_IND, id(nm, origin), integer((int64_t)0, origin), origin);
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
		if (is_std_string(ptype)
		    || pdt == DataType::dtARRAY || pdt == DataType::dtARRAYref
		    || pdt == DataType::dtSSTREAM || pdt == DataType::dtSSTREAMref) {
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
	// Mangler-generated (never hardcoded): std::cout → _ZSt4cout, etc.
	static const std::string cout_s = itanium_mangle_std_var("cout");
	static const std::string cerr_s = itanium_mangle_std_var("cerr");
	static const std::string clog_s = itanium_mangle_std_var("clog");
	static const std::string cin_s  = itanium_mangle_std_var("cin");
	switch (k) {
	case SK_COUT: return cout_s.c_str();
	case SK_CERR: return cerr_s.c_str();
	case SK_CLOG: return clog_s.c_str();
	case SK_CIN:  return cin_s.c_str();
	default:      return cout_s.c_str();
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
	// All symbols mangler-generated (never hardcoded). OSTREAM = the concrete
	// libstdc++ char ostream specialization. The const char*/char overloads are
	// non-member std function templates (signature in the function's template
	// params); the rest are member operators on the complete ostream.
	static const std::string OSTREAM = "std::basic_ostream<char,std::char_traits<char>>";
	static const std::string sym_pkc = itanium_mangle_std_free_template("<<",
		{"std::char_traits<char>"}, "std::basic_ostream<char,$T0>&",
		{"std::basic_ostream<char,$T0>&", "const char*"});
	static const std::string sym_c = itanium_mangle_std_free_template("<<",
		{"std::char_traits<char>"}, "std::basic_ostream<char,$T0>&",
		{"std::basic_ostream<char,$T0>&", "char"});
	static const std::string sym_pkv = itanium_mangle_operator_sub(OSTREAM, "<<", {"const void*"}, false);
	static const std::string sym_d   = itanium_mangle_operator_sub(OSTREAM, "<<", {"double"}, false);
	static const std::string sym_i   = itanium_mangle_operator_sub(OSTREAM, "<<", {"int"}, false);
	static const std::string sym_j   = itanium_mangle_operator_sub(OSTREAM, "<<", {"unsigned int"}, false);
	static const std::string sym_l   = itanium_mangle_operator_sub(OSTREAM, "<<", {"long"}, false);

	bool is_ptr = dd && dd->is_pointer();
	DataType dt = dd ? dd->rawtype() : DataType::dtINT64;
	// NOTE: a string OBJECT value (operator<<(ostream&, const string&)) is
	// handled directly in translate_stream_chain, which has the token to tell
	// an object apart from a literal. Here `is_string()` means a const char*
	// (string literal / char* value).
	if (dd && dd->is_string()) { p_out = {{N_CHAR}, true}; return sym_pkc.c_str(); }
	if (is_ptr && dt == DataType::dtCHAR) { p_out = {{N_CHAR}, true}; return sym_pkc.c_str(); }
	if (is_ptr) { p_out = {{N_VOID}, true}; return sym_pkv.c_str(); }
	switch (dt) {
	case DataType::dtCHAR:   p_out = {{N_CHAR}, false};  return sym_c.c_str();
	case DataType::dtFLOAT:
	case DataType::dtDOUBLE: p_out = {{N_DOUBLE}, false}; return sym_d.c_str();
	case DataType::dtINT16:
	case DataType::dtINT32:  p_out = {{N_INT}, false};    return sym_i.c_str();
	case DataType::dtUINT8:
	case DataType::dtUINT16:
	case DataType::dtUINT32: p_out = {{N_UNSIGNED, N_INT}, false}; return sym_j.c_str();
	case DataType::dtINT64:
	case DataType::dtUINT64:
	default:                 p_out = {{N_LONG}, false};   return sym_l.c_str();
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
			// Mangler-generated (never hardcoded): the member manipulator
			// operator<<(ostream& (*)(ostream&)) and the std::endl<char,traits> fn.
			static const std::string OSTREAM = "std::basic_ostream<char,std::char_traits<char>>";
			static const std::string manip_s = itanium_mangle_operator_sub(
				OSTREAM, "<<", { OSTREAM + "& (*)(" + OSTREAM + "&)" }, false);
			static const std::string endlf_s = itanium_mangle_std_free_template("endl",
				{ "char", "std::char_traits<char>" }, "std::basic_ostream<$T0,$T1>&",
				{ "std::basic_ostream<$T0,$T1>&" });
			const char *MANIP = manip_s.c_str();
			const char *ENDLF = endlf_s.c_str();
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
		if (is_string_object_value(vals[i]) || is_string_returning_call(vals[i])) {
			// operator<<(ostream&, const std::string&) — takes the object
			// pointer, returns ostream& (chains). Literals are NOT objects and
			// fall through to the char* overload below. A by-value
			// string-RETURNING call (direct `make()` or a fn-ptr/lambda `f()`)
			// is a string-object rvalue: string_obj_arg materializes it into a
			// __retbuf temp and yields the temp's address.
			// Mangler-generated (never hardcoded): non-member std::operator<<
			// (ostream&, const string&), the cxx11 template form.
			static const std::string ssym_s = itanium_mangle_std_free_template("<<",
				{ "char", "std::char_traits<char>", "std::allocator<char>" },
				"std::basic_ostream<$T0,$T1>&",
				{ "std::basic_ostream<$T0,$T1>&",
				  "const std::__cxx11::basic_string<$T0,$T1,$T2>&" });
			const char *SSYM = ssym_s.c_str();
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

// `ss << a << b ...` on a std::stringstream OBJECT. Unlike cout (a global
// std::ostream addressed directly), a stringstream's ostream base is at a
// non-zero offset, so we route through the generic streamout_* wrappers on
// sstream_ostream((void*)&ss) (which applies the base adjustment). Each `<< v`
// becomes one streamout_TYPE(os, v) call; the calls are chained with N_COMMA so
// the whole chain is a single expression (its value is unused as a statement).
node_t CirBuilder::translate_sstream_chain(TokenOperator *top, const char *ssname)
{
	need_output_extern("sstream_ostream", true, { { {N_VOID}, true } });

	// Collect chained values along the right spine (left-to-right), exactly
	// as translate_stream_chain does.
	std::vector<TokenBase *> vals;
	TokenBase *n = top->right;
	while (TokenOperator *o = dynamic_cast<TokenOperator *>(n)) {
		if (o->id() != top->id() || o->is_bracketed()) break;
		vals.push_back(o->left);
		n = o->right;
	}
	if (n) vals.push_back(n);

	// A fresh `sstream_ostream((void*)ss)` ostream* for each value.
	auto os_arg = [&]() -> node_t {
		node_t a = list();
		append(a, string_obj_addr(ssname, top));
		node_t call = node2(N_CALL, id("sstream_ostream", top), a, top);
		CIR_NODE(call)->synth_from_origin = true;
		return call;
	};

	node_t result = NULL;
	for (size_t i = 0; i < vals.size(); i++) {
		std::string vn;
		if (TokenVar *tv = dynamic_cast<TokenVar *>(vals[i])) vn = tv->var.name;
		else if (TokenIdent *ti = dynamic_cast<TokenIdent *>(vals[i])) vn = ti->str;

		node_t call;
		if (vn == "endl" || vn == "__std_endl") {
			need_output_extern("streamout_endl", false, { { {N_VOID}, true } });
			node_t a = list();
			append(a, os_arg());
			call = node2(N_CALL, id("streamout_endl", top), a, top);
		} else if (is_string_object_value(vals[i])) {
			// streamout_string(os, (void*)&strobj)
			need_output_extern("streamout_string", false,
					   { { {N_VOID}, true }, { {N_VOID}, true } });
			node_t a = list();
			append(a, os_arg());
			append(a, string_obj_arg(vals[i]));
			call = node2(N_CALL, id("streamout_string", top), a, top);
		} else {
			// Pick the streamout_* wrapper by value type (mirrors
			// ostream_insert_symbol's classification).
			DataDef *vdd = vals[i]->datadef();
			bool is_ptr = vdd && vdd->is_pointer();
			DataType dt = vdd ? vdd->rawtype() : DataType::dtINT64;
			const char *sym;
			ExternParam vp;
			if ((vdd && vdd->is_string()) || (is_ptr && dt == DataType::dtCHAR)) {
				sym = "streamout_cstr"; vp = { {N_CHAR}, true };
			} else if (dt == DataType::dtCHAR) {
				sym = "streamout_char"; vp = { {N_INT}, false };
			} else if (dt == DataType::dtFLOAT || dt == DataType::dtDOUBLE) {
				sym = "streamout_double"; vp = { {N_DOUBLE}, false };
			} else if (dt == DataType::dtUINT8 || dt == DataType::dtUINT16
				   || dt == DataType::dtUINT32 || dt == DataType::dtUINT64) {
				sym = "streamout_uint64"; vp = { {N_UNSIGNED, N_LONG}, false };
			} else {
				sym = "streamout_int64"; vp = { {N_LONG}, false };
			}
			need_output_extern(sym, false, { { {N_VOID}, true }, vp });
			node_t a = list();
			append(a, os_arg());
			append(a, translate_expr(vals[i]));
			call = node2(N_CALL, id(sym, top), a, top);
		}
		CIR_NODE(call)->synth_from_origin = true;
		result = result ? node2(N_COMMA, result, call, top) : call;
	}
	// Empty chain (no values) is a no-op; emit a harmless 0.
	return result ? result : integer(0, top);
}

bool CirBuilder::init_slot_is_aggregate(DataDef *dd, size_t idx)
{
	if (!dd)
		return false;
	// Array type `T x[N] = {...}`: every positional slot is an element of
	// type T; it's an aggregate iff T is a fixed array or struct/union.
	if (DataDefCArray *ca = dynamic_cast<DataDefCArray *>(dd)) {
		DataDef *et = ca->element_type;
		return et && (dynamic_cast<DataDefCArray *>(et)
			      || (et->is_struct() && !et->is_complex()));
	}
	// Struct/union `struct S x = {...}`: slot `idx` targets member `idx`.
	// A member is an aggregate when it is a fixed array (member_counts != 1)
	// or its own type is a struct/union.
	if (DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd)) {
		if (sdd->is_complex())
			return false;
		if (idx >= sdd->members.size())
			return false;
		if (idx < sdd->member_counts.size() && sdd->member_counts[idx] != 1)
			return true;
		DataDef *mt = sdd->members[idx].second;
		return mt && (dynamic_cast<DataDefCArray *>(mt)
			      || (mt->is_struct() && !mt->is_complex()));
	}
	return false;
}

node_t CirBuilder::init_value(TokenBase *elem, bool target_is_aggregate)
{
	// A NULL element is a designated-initializer GAP: the parser normalizes
	// `.field`/`[index]` designators into positional slots at parse time
	// (parser.cpp: assign_initializer_range / field_index resolution),
	// NULL-filling the slots between explicit values. C semantics zero-fill
	// those gaps.
	//
	// The emitted gap value MUST match the target slot's shape so c2mir
	// consumes exactly one slot and advances. For a scalar member a dense
	// `I 0` is correct (N_IGNORE crashes c2mir_compile_tree). For an
	// AGGREGATE member (a fixed-array or nested-struct slot, e.g. the `int
	// a[3]` skipped by `struct s = { c: {1,2,3} }`) a scalar `0` is WRONG:
	// c2mir reads it as `a[0]=0` and keeps consuming the next INIT for
	// `a[1]`, swallowing the following member's value ("excess elements in
	// scalar initializer"). The faithful gap for an aggregate is an empty
	// brace `INIT(LIST(), LIST())`, which c2mir zero-fills and treats as one
	// slot — exactly the C99 `= { [n] = ... }` zero-init of skipped members.
	if (!elem) return target_is_aggregate ? list() : integer(0);
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

// C99 compound literal `(T){ field... }` -> an unnamed object of type T,
// initialized in place. c2mir models this natively as
// N_COMPOUND_LITERAL(N_TYPE(spec, N_DECL(IGNORE, decl)), N_LIST(N_INIT...)),
// exactly the cast's type-name node plus the brace initializer (see c2mir.c
// unary_expr / N_COMPOUND_LITERAL). The element list reuses init_value, the
// same lowering an ordinary `T x = {...}` declaration uses.
// Build the type-specifier list for a compound-literal element/object type.
// Mirrors a cast / var_decl spec: a typedef alias becomes ID("alias"); a tagged
// struct/union becomes `struct Tag`/`union Tag`; an anonymous aggregate inlines
// its members; anything else (scalar/builtin) defers to append_type_specs.
void CirBuilder::append_lit_type_spec(node_t spec, DataDef *dd,
				      const std::string &typedef_name)
{
	DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd);
	if (!typedef_name.empty()) {
		// Named via a typedef: emit ID("alias") so c2mir resolves the
		// (possibly anonymous struct/union) layout from the typedef.
		append(spec, id(typedef_name.c_str()));
	} else if (sdd && !dd->is_complex() && !sdd->is_anonymous) {
		// A tagged struct/union: `struct Tag` / `union Tag`.
		append(spec, node2(sdd->union_layout ? N_UNION : N_STRUCT,
				   id(sdd->name.c_str()), ignore()));
	} else if (sdd && !dd->is_complex()) {
		// Anonymous struct/union with no typedef alias: inline the full
		// member definition, matching var_decl's anon_sdd path.
		node_t ml = list();
		for (size_t i = 0; i < sdd->members.size(); i++)
			append(ml, member_node(sdd->members[i], sdd));
		append(spec, node2(sdd->union_layout ? N_UNION : N_STRUCT,
				   ignore(), ml));
	} else {
		// Scalar / builtin element type.
		append_type_specs(spec, dd);
	}
}

node_t CirBuilder::translate_struct_lit(TokenStructLit *slit)
{
	DataDef *dd = slit->datadef();
	if (!dd)
		return error_node("compound literal has no type", slit);

	// C99 ARRAY compound literal `(T[]){...}` / `(T[N]){...}`. The parser
	// models the literal's storage as a synthetic `__compound_array` struct
	// (so the brace-init reuses struct-init machinery), but that struct is a
	// forward-ref tag c2mir sees as an INCOMPLETE type — and a struct cannot
	// be subscripted, which is why `&(int[]){...}[i]` failed with both
	// "compound literal of incomplete type" and "subscripted value is neither
	// array nor pointer". Emit the faithful C99 type-name instead: element
	// type T with an UNSIZED array declarator `T[]`, so c2mir sizes the array
	// from the initializer count (N_ARR size = N_IGNORE). The init list reuses
	// the same INIT(LIST(), value) lowering as any aggregate.
	if (slit->array_elem_dd) {
		node_t aspec = list();
		// The element type may be a struct/union/typedef (e.g.
		// `(S[]){{.b=3}}`, pr98366), not just a scalar — build the spec
		// the same way the object path below does. slit->typedef_name
		// carries the element's typedef alias for the array case.
		append_lit_type_spec(aspec, slit->array_elem_dd,
				     slit->typedef_name);
		node_t adecl_list = list();
		append(adecl_list, node3(N_ARR, ignore(), list(), ignore()));
		node_t atype = node2(N_TYPE, aspec,
				     node2(N_DECL, ignore(), adecl_list));
		node_t ainits = list();
		for (size_t i = 0; i < slit->inits.size(); i++)
			append(ainits,
			       node2(N_INIT, list(), init_value(slit->inits[i])));
		return node2(N_COMPOUND_LITERAL, atype, ainits, slit);
	}

	// ---- Build the N_TYPE type-name node (mirrors a cast / var_decl spec). ----
	node_t spec = list();
	append_lit_type_spec(spec, dd, slit->typedef_name);
	node_t type_node = node2(N_TYPE, spec, node2(N_DECL, ignore(), list()));

	// ---- Build the initializer list: LIST( INIT(LIST(), value), ... ). ----
	node_t inits = list();
	for (size_t i = 0; i < slit->inits.size(); i++)
		append(inits, node2(N_INIT, list(), init_value(slit->inits[i])));

	node_t cl = node2(N_COMPOUND_LITERAL, type_node, inits, slit);
	return cl;
}

node_t CirBuilder::var_decl(Variable *v, TokenBase *origin)
{
	// std::string object: a runtime-object class. It flows through the general
	// class-instance path below — emitted as `struct string name` (opaque storage
	// via class_struct_def) with a cleanup attribute naming the mangled libstdc++
	// dtor. Construction is injected as a separate class_ctor_call by
	// translate_block (the 1->N C++ lowering).

	// MadArray object (`array a;`): same opaque-storage model as std::string.
	// madarray_construct is emitted as a separate statement by translate_block;
	// the cleanup attribute on the storage handles scope-exit destruction.
	if (is_array_object(v->type))
		return array_storage_decl(v->name.c_str(), origin);

	// std::stringstream object: opaque storage + cleanup(sstream_destruct).
	if (is_sstream_object(v->type))
		return sstream_storage_decl(v->name.c_str(), origin);

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
	// Pointer-to-array `T (*p)[N]`: the parser builds the type as
	// DataDefPTR(DataDefCArray(T, N)). After peeling the pointer level(s) above,
	// base_dd is the CArray — peel its fixed dims so the spec renders the element
	// type T (append_type_specs has no CArray case), and emit the dims as N_ARR
	// suffixes AFTER the pointer(s) below (declarator order [POINTER, ARR],
	// which c2m reads as "pointer to array", vs [ARR, POINTER] = array of ptr).
	std::vector<uint32_t> ptr_array_dims;
	if (is_ptr)
		base_dd = peel_carray_dims(base_dd, ptr_array_dims);

	// A variable whose type is an anonymous aggregate (`struct { ... } x;`)
	// has no tag to forward-reference, so the body must be emitted inline in
	// the variable's own spec: LIST(STRUCT(IGNORE, members)). Without this the
	// builder emits `struct anonymous` (a forward ref to a never-defined tag),
	// leaving the variable with an incomplete type. member_node carries the
	// member array dims via the owning struct.
	// An anonymous aggregate has no tag, so EVERY declarator that uses it
	// (value, pointer, static, extern) must inline its body — a forward
	// `struct anonymous` reference is never defined. The pointer case
	// (`struct { int i; } *sp;`) and the static/extern overrides below all
	// route through anon_inline_spec(), so drop the old `!is_ptr` guard.
	DataDefSTRUCT *anon_sdd = NULL;
	if (v->typedef_name.empty() && base_dd && base_dd->is_struct()
	    && !base_dd->is_complex()) {
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(base_dd);
		if (sdd && sdd->is_anonymous && !sdd->members.empty())
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
		// An array-of-fn-ptr variable (`int (*ops[N])(args)`) keeps a bare
		// DataDefFPTR type with the array shape carried in v->dims. The C
		// "spiral rule" declarator is `ARR(N) POINTER FUNC(params)` (array of
		// pointer to function): the array dimension binds tighter than the
		// fn-ptr `*`, so it must lead the declarator suffixes. Hand the dims to
		// fnptr_decl_pieces as lead_dims — it emits the N_ARR nodes first.
		std::vector<uint32_t> fnptr_dims;
		if (v->is_fixed_array())
			fnptr_dims = v->dims;
		fnptr_decl_pieces(fnptr->target, true, tl, fnptr_decl_list, fnptr_dims);
	} else if (anon_sdd) {
		tl = anon_inline_spec(anon_sdd);
	} else {
		tl = !v->typedef_name.empty()
				? type_list(v->type, v->typedef_name)
				: type_list(base_dd);
	}

	// Storage class qualifiers (fn-ptr vars handle storage class above).
	if (!fnptr && (v->flags & vfSTATIC)) {
		node_t new_list = list();
		append(new_list, simple(N_STATIC));
		if (!v->typedef_name.empty()) {
			// A typedef'd type keeps its alias spec (`static io *gp`), so
			// the pointee resolves through the typedef's own (complete)
			// definition. Dropping the alias to a `struct anonymous` tag left
			// the pointee incomplete ("struct has no member" through `gp->`).
			append(new_list, id(v->typedef_name.c_str()));
		} else if (anon_sdd) {
			// Anonymous aggregate: inline the body (no tag to reference).
			append(new_list, node2(anon_sdd->union_layout ? N_UNION : N_STRUCT,
					       ignore(), anon_members_list(anon_sdd)));
		} else if (base_dd && base_dd->is_struct() && !base_dd->is_complex()) {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(base_dd);
			if (sdd)
				append(new_list, node2(sdd->union_layout ? N_UNION : N_STRUCT, id(sdd->name.c_str()), ignore()));
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
		} else if (anon_sdd) {
			append(new_list, node2(anon_sdd->union_layout ? N_UNION : N_STRUCT,
					       ignore(), anon_members_list(anon_sdd)));
		} else if (base_dd && base_dd->is_struct() && !base_dd->is_complex()) {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(base_dd);
			if (sdd)
				append(new_list, node2(sdd->union_layout ? N_UNION : N_STRUCT, id(sdd->name.c_str()), ignore()));
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
		// NOTE: this flattening happens ONLY in the parser's non-pointer path.
		// When the declarator has a pointer prefix (`A3_28 *paa[]`, is_ptr), the
		// parser leaves the typedef's array dims in the pointee type (already
		// peeled into ptr_array_dims above and emitted as pointer-to-array
		// suffixes), so v->dims holds only the variable's OWN dims and nothing
		// must be skipped. Gating on !is_ptr keeps the outer `[]` of an
		// array-of-pointer-to-typedef'd-array (pr98366/strlen-4 family).
		size_t skip_tail = 0;
		if (!is_ptr && !v->typedef_name.empty() && m_prog) {
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
		// Pointer-to-array: array dims follow the pointer(s) — `T (*p)[N]`.
		for (size_t d = 0; d < ptr_array_dims.size(); d++)
			append(decl_list, node3(N_ARR, ignore(), list(),
						 integer(ptr_array_dims[d])));
	}

	node_t var_decl_node = node2(N_DECL, var_id, decl_list);
	node_t init_node = ignore();
	TokenDecl *tdecl = dynamic_cast<TokenDecl *>(origin);

	// C99 variable-length array local (`int a[n]`, runtime `n`). The parser
	// recorded the bound in v->vla_size_expr and emitted `a` as a plain
	// pointer (above). Back that pointer with heap storage:
	//   a = (T *) malloc(n * sizeof(T))
	// freed by a cleanup attribute at scope exit (see attr_node below), the
	// same RAII mechanism the class destructors use -- so a VLA in a loop /
	// reached by a backward goto is reclaimed, not leaked onto the stack
	// (__builtin_alloca would grow the frame per iteration -> the goto-loop
	// torture test 20040811-1 stack-overflows). c2mir never sees an array of
	// runtime size, so VLA is a Tier-1 lowering, NOT a c2mir/MIR floor gap.
	// Static/extern VLAs are not valid C; VLA *parameters* are plain pointers
	// (no init needed).
	bool is_vla_local = v->is_vla() && v->vla_size_expr && is_ptr
			    && (v->flags & vfLOCAL)
			    && !(v->flags & (vfSTATIC | vfEXTERN));
	node_t vla_init = NULL;
	if (is_vla_local) {
		referenced_funcs.insert("malloc");
		// Declare `void *malloc(unsigned long)` — without a prototype c2mir
		// assumes an `int` return and TRUNCATES the 64-bit pointer -> SIGSEGV.
		need_output_extern("malloc", true, { { {N_UNSIGNED, N_LONG}, false } });
		node_t szof = node1(N_SIZEOF,
				    node2(N_TYPE, type_list(base_dd),
					  node2(N_DECL, ignore(), list())),
				    origin);
		// Element count = product of ALL VLA dims. The outer dim is
		// vla_size_expr; the inner dims (multidim `int a[m][n]`) live in
		// v->type's pointee DataDefCArray chain (peel_carray_dims already
		// flattened base_dd to the scalar, so multiply each inner dim in here
		// to malloc the whole block, not just the first row).
		node_t count = translate_expr(v->vla_size_expr);
		{
			DataDef *w = v->type;
			if (DataDefPTR *wp = dynamic_cast<DataDefPTR *>(w))
				w = wp->base_type;
			for (DataDefCArray *c = dynamic_cast<DataDefCArray *>(w);
			     c; c = dynamic_cast<DataDefCArray *>(c->element_type)) {
				node_t d = c->has_runtime_size()
					   ? translate_expr(c->count_expr)
					   : integer((int64_t)c->count);
				count = node2(N_MUL, count, d, origin);
			}
		}
		node_t nbytes = node2(N_MUL, count, szof, origin);
		node_t aargs = list();
		append(aargs, nbytes);
		node_t acall = node2(N_CALL, id("malloc", origin),
				     aargs, origin);
		node_t cast_decl_list = list();
		append(cast_decl_list, pointer());
		node_t cast_type = node2(N_TYPE, type_list(base_dd),
					 node2(N_DECL, ignore(), cast_decl_list));
		vla_init = node2(N_CAST, cast_type, acall, origin);
	}
	// A class instance (user class or std::string) is initialized by a
	// constructor call emitted separately (translate_block), never by a C
	// initializer on the declaration — `struct string s = "x"` would be an
	// incompatible aggregate init. Leave init_node empty for such instances.
	bool class_instance = (!is_ptr) && as_class_instance(base_dd) != NULL;
	// A static/global fixed array whose constant initializer the parser baked
	// into v->data and then cleared init_list (initialize_static_fixed_array_data
	// in parser.cpp). For a brace `{...}` init has_brace_init stays set so the
	// branch below still fires; but for the string-literal char-array form
	// (`static char s[]="hi"`) has_brace_init is FALSE and init_list is now
	// empty, so without this the whole branch is skipped and the initializer is
	// dropped (s reads as zeroes). Detect the baked array directly so both forms
	// reconstruct from v->data.
	//
	// EXCLUDE extern declarations: an `extern const T arr[N];` placeholder
	// shares the same Variable (and its already-allocated, zero-filled v->data)
	// as the later defining `const T arr[N] = {...};`. Reconstructing an
	// initializer on the extern decl would turn it into a SECOND definition —
	// MIR then rejects the pair with "Repeated item declaration". Only the
	// actual definition (which carried a real source initializer) may
	// reconstruct: require that THIS TokenDecl originally had one
	// (has_brace_init for `{...}`, or `initialize` for the string-literal form).
	bool baked_static_array = tdecl && (!is_ptr)
				  && !(v->flags & vfEXTERN)
				  && tdecl->baked_static_init
				  && v->is_fixed_array()
				  && v->data && tdecl->init_list.empty()
				  && v->type && v->type->is_integer();
	if (is_vla_local) {
		init_node = vla_init;
	} else if (class_instance) {
		// fall through with init_node = ignore()
	} else if (tdecl && (tdecl->has_brace_init || !tdecl->init_list.empty()
			     || baked_static_array)) {
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
				append(lst, node2(N_INIT, list(),
						  init_value(tdecl->init_list[i],
							     init_slot_is_aggregate(base_dd, i))));
		}
		init_node = lst;
	} else if (tdecl && tdecl->initialize) {
		// Scalar init: the parser stores `initialize` as a full
		// assignment AST (TokenAssign: left=var, right=value), but a
		// SPEC_DECL initializer is the bare value, not an ASSIGN. Unwrap
		// to the RHS so the 5th operand matches c2m (e.g. `I 7`).
		TokenBase *init_expr = tdecl->initialize;
		TokenAssign *as = dynamic_cast<TokenAssign *>(init_expr);
		if (as && as->multi_vars.size() > 1) {
			// Multi-return unpack `a, b, ... := f(args)`: this decl is for
			// multi_vars[0]; its initializer becomes __mret[0]. The buffer,
			// the call, and the assigns for multi_vars[1..] go to m_pending_stmts.
			init_node = multi_return_unpack(as, origin);
		} else {
		if (as && as->right)
			init_expr = as->right;
		init_node = translate_expr(init_expr);
		// Derived->base pointer initializer (`A *p = new B();`, `A *p = bptr;`):
		// emit an explicit `(A*)` cast so the conversion is explicit and c2mir
		// does not warn. Single inheritance, base subobject at offset 0 — value
		// unchanged.
		init_node = upcast_class_ptr(init_node, v->type,
					     init_expr, origin);
		}
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
		DataDefCLASS *cdd = (!is_ptr) ? as_class_instance(base_dd) : NULL;
		if (cdd && class_needs_dtor(cdd)) {
			std::string dtor_sym = class_complete_dtor_symbol(cdd);
			referenced_funcs.insert(dtor_sym);
			node_t attr_args = list();
			append(attr_args, id(dtor_sym.c_str(), origin));
			node_t attrs = list();
			append(attrs, node2(N_ATTR, id("cleanup", origin),
					    attr_args, origin));
			attr_node = attrs;
		} else if (is_vla_local) {
			// Free the VLA's malloc'd storage at scope exit. The
			// cleanup fn receives &a (a `T **`); __madc_vla_free does
			// free(*pp). Same mechanism as the class-dtor cleanup above.
			referenced_funcs.insert("__madc_vla_free");
			need_output_extern("__madc_vla_free", false,
					   { { {N_VOID}, true } });
			node_t attr_args = list();
			append(attr_args, id("__madc_vla_free", origin));
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

node_t CirBuilder::anon_members_list(DataDefSTRUCT *anon)
{
	node_t ml = list();
	for (size_t i = 0; i < anon->members.size(); i++)
		append(ml, member_node(anon->members[i], anon));
	return ml;
}

node_t CirBuilder::anon_inline_spec(DataDefSTRUCT *anon)
{
	return node1(N_LIST, node2(anon->union_layout ? N_UNION : N_STRUCT,
				   ignore(), anon_members_list(anon)));
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
	// A trailing flexible array member (`T x[]` / `T x[0]`) gets an array
	// declarator with an UNSPECIFIED size (N_ARR slot = N_IGNORE) so c2mir
	// treats it as flexible and sizes the file-scope object from its
	// initializer. Emitting a literal `[0]` instead makes c2mir reject the
	// brace initializer ("zero array size" / "excess elements").
	bool m_flexible = false;
	if (owner) {
		std::string mname = m.first;
		m_flexible = owner->m_is_flexible_array(mname);
		if (!m_flexible && owner->m_is_array_decl(mname)
		    && !owner->m_count_expr(mname)) {
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

	// A std::string OBJECT member (not a pointer) embeds the std::string object
	// in the struct as a real `struct string` (the same class-struct tag that
	// class_struct_def / type_list emit — sized by object_class_words from
	// sizeof(std::string), never hard-coded). Routing through the class layout
	// (rather than the legacy `long name[W];`) keeps members, pointers, and
	// elements all at the correct ABI stride — needed for vector<string> etc.
	// Construction / destruction of an embedded string member is driven by the
	// enclosing class's ctor/dtor, not the cleanup attribute (members have no
	// independent scope); those operate on &obj.name and are unaffected by the
	// member's emitted type. String member access reads through
	// string_cstr((void*)&obj.name) — handled at the use site.
	if (is_string_object(mtype)) {
		node_t mspec = type_list(mtype);   // -> struct string
		node_t mmember = simple(N_MEMBER, m.origin);
		append(mmember, node1(N_SHARE, mspec));
		append(mmember, node2(N_DECL, id(m.first.c_str(), m.origin), list()));
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
		// Peel ALL pointer levels to the innermost base, so the type spec
		// renders the real base (`struct A`, not the pointer's `int` rawtype)
		// and the declarator carries one star per level. Peeling only ONE level
		// left a pointer-to-pointer member (`A** data` — e.g. vector<A*>'s
		// `T* data` with T=A*) rendering as `int *data` (base collapsed to int,
		// and only one star — see the declarator below), so element stride was
		// sizeof(int)=4 not sizeof(A*)=8 and reading element[1] crashed.
		DataDef *mbase = mtype;
		while (mbase && mbase->is_pointer()) {
			DataDefPTR *mptr = dynamic_cast<DataDefPTR *>(mbase);
			if (!mptr || !mptr->base_type) break;
			mbase = mptr->base_type;
		}
		// An anonymous nested struct/union member (`struct { ... } f;` or
		// `union { ... } u;` inside the enclosing aggregate) has no tag to
		// forward-reference, so type_list would emit `struct anonymous` — an
		// undefined tag, leaving the member incomplete. Inline the full member
		// definition instead, matching var_decl's anon_sdd path. (member_node
		// recurses, so anonymous aggregates may nest.)
		DataDefSTRUCT *anon = dynamic_cast<DataDefSTRUCT *>(mbase);
		if (anon && anon->is_anonymous && !mbase->is_complex()) {
			mspec = anon_inline_spec(anon);
		} else {
			mspec = type_list(mbase);
		}
	}

	// Bit-field signedness reconciliation. A bit-field's signedness is the
	// field's resolved signedness (DataDefSTRUCT::BitFieldInfo::is_unsigned),
	// not necessarily the base type's default. The common case is an
	// enum-typed bit-field (`enum E e : 2;`): the enum lowers to a plain
	// signed `int` spec, but with all-non-negative enumerators its bit-field
	// is unsigned (GCC/Clang), so a 2-bit field holding 3 must read back 3,
	// not -1. If the field is unsigned but the rendered spec has no explicit
	// `unsigned`, prepend N_UNSIGNED so c2mir zero-extends on load. (Already
	// unsigned base types — `unsigned`, `unsigned long long` — render
	// N_UNSIGNED themselves and are skipped.)
	{
		const DataDefSTRUCT::BitFieldInfo *bfsign =
			owner ? owner->m_bitfield(m.first) : NULL;
		if (bfsign && bfsign->is_bitfield && bfsign->is_unsigned) {
			bool has_sign_spec = false;
			for (int i = 0; ; i++) {
				node_t sp = c2mir_node_op(mspec, i);
				if (!sp) break;
				if (sp->code == N_UNSIGNED || sp->code == N_SIGNED)
					has_sign_spec = true;
			}
			if (!has_sign_spec) {
				// Rebuild as [N_UNSIGNED, <existing specs...>].
				node_t fixed = list();
				append(fixed, simple(N_UNSIGNED));
				for (int i = 0; ; i++) {
					node_t sp = c2mir_node_op(mspec, i);
					if (!sp) break;
					append(fixed, sp);
				}
				mspec = fixed;
			}
		}
	}

	// __attribute__((aligned(N))) on this member -> _Alignas(N) in its spec.
	// c2mir lays the field out at the requested alignment (raising the struct's
	// own alignment via its max-member-align rule), matching GCC and madc's own
	// recorded offset/size. The alignment lives keyed by member index on the
	// owning struct; `&m` is an element of owner->members, so recover the index.
	// The struct TAG's own __attribute__((aligned(N))) is folded onto the first
	// member here too (c2mir derives the aggregate's alignment from its
	// strictest member): the tag attribute over-aligns the type without moving
	// any member offset, matching GCC.
	if (owner && !owner->members.empty() && &m >= &owner->members[0]
	    && &m <= &owner->members[owner->members.size() - 1]) {
		size_t midx = (size_t)(&m - &owner->members[0]);
		size_t want_align = 0;
		auto ai = owner->member_explicit_align.find(midx);
		if (ai != owner->member_explicit_align.end())
			want_align = ai->second;
		if (midx == 0 && owner->tag_explicit_align > want_align)
			want_align = owner->tag_explicit_align;
		if (want_align > 1) {
			node_t aligned_spec = list();
			append(aligned_spec, node1(N_ALIGNAS,
				integer((long)want_align, m.origin)));
			for (int i = 0; ; i++) {
				node_t sp = c2mir_node_op(mspec, i);
				if (!sp) break;
				append(aligned_spec, sp);
			}
			mspec = aligned_spec;
		}
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
	// Flexible array member: ARR with N_IGNORE size (sized by initializer).
	if (m_flexible)
		append(mdecl_list, node3(N_ARR, ignore(), list(), ignore()));
	for (size_t d = 0; d < mdims.size(); d++)
		append(mdecl_list, node3(N_ARR, ignore(), list(), integer(mdims[d])));
	if (!mtypedef.empty()) {
		for (int s = 0; s < stars; s++)
			append(mdecl_list, pointer());
	} else if (mtype && mtype->is_pointer()) {
		// One star per pointer level: `A** data` is depth 2. Emitting a single
		// star (the old behaviour) under-declared a pointer-to-pointer member,
		// halving its element stride.
		int depth = dd_ptr_depth(mtype);
		for (int s = 0; s < (depth > 0 ? depth : 1); s++)
			append(mdecl_list, pointer());
	}
	node_t mdecl = node2(N_DECL, mid, mdecl_list);

	node_t member = simple(N_MEMBER, m.origin);
	append(member, mshare);
	append(member, mdecl);
	append(member, ignore());	// attrs
	// Bit-field width (c2mir N_MEMBER slot 3 = const_expr, or N_IGNORE for a
	// plain member). The member's `:width` was parsed and recorded on the
	// owning struct; carry it into the emitted node_t so c2mir lays the field
	// out as a real bit-field and performs the standard mask-on-store /
	// sign-or-zero-extend-on-load itself (matching GCC). Without this, madc
	// emitted a full-width member and bit-field values were wrong.
	const DataDefSTRUCT::BitFieldInfo *bf =
		owner ? owner->m_bitfield(m.first) : NULL;
	if (bf && bf->is_bitfield)
		append(member, integer((long)bf->bit_width, m.origin));
	else
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

	// Emit N_UNION for a union so c2mir overlaps all members at offset 0
	// (shared storage / type-punning); N_STRUCT otherwise. The reference and
	// anonymous-inline paths already branch on union_layout — the DEFINITION
	// emitter must too, else `union U {..}` is laid out as a struct (distinct
	// offsets) and member aliasing silently breaks.
	node_t struct_node = node2(sdd->union_layout ? N_UNION : N_STRUCT,
				   struct_id, member_list);
	node_t tl = node1(N_LIST, struct_node);

	node_t spec_decl = simple(N_SPEC_DECL);
	append(spec_decl, tl);
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	append(spec_decl, ignore());
	return spec_decl;
}

static DataDefCLASS *class_behind(DataDef *dd); // defined below; used by the thunk path

// Emit the Itanium type_info object(s) for a polymorphic user class (S5b):
//   _ZTS<cls> : the bare mangled name as a char[] (explicit byte list — the
//               c2mir-friendly form; see .claude/rules/c11-transpiler.md)
//   _ZTI<cls> : a void*[] whose [0] is the real libsupc++ type_info-class vtable
//               (+16 address point) so libstdc++'s __dynamic_cast/typeid accept it.
// std:: classes are NOT emitted here (they reference libstdc++'s own _ZTI*).
// Returns an N_LIST of file-scope definitions (extern decls + _ZTS + _ZTI), or
// NULL for a non-polymorphic class.
node_t CirBuilder::class_typeinfo_def(DataDefCLASS *cdd)
{
	if (!cdd || !cdd->has_vtable)
		return NULL;

	std::string ti = itanium_typeinfo_sym(cdd->name);          // _ZTI<cls>
	std::string ts = itanium_typeinfo_name_sym(cdd->name);     // _ZTS<cls>
	std::string nm = itanium_typeinfo_name_string(cdd->name);  // "<len><name>"

	auto vptr_t = [&]() {                            // void* type node
		return node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
			     node2(N_DECL, ignore(), node1(N_LIST, pointer())));
	};
	auto void_ptr_to = [&](node_t e) { return node2(N_CAST, vptr_t(), e); };
	auto void_ptr_int = [&](long v) { return node2(N_CAST, vptr_t(), integer(v)); };

	node_t out = list();

	// extern <ckind> SYM[];  (data extern, deduped globally). ckind: N_CHAR for the
	// libsupc++ vtable bytes (never defined here, indexed +16), N_VOID* for a base
	// _ZTI object (matches its void*[] definition type to avoid a type clash).
	auto emit_data_extern = [&](const std::string &sym, bool void_ptr_elem) {
		if (m_rtti_data_externs.count(sym)) return;
		m_rtti_data_externs.insert(sym);
		node_t spec = list();
		append(spec, simple(N_EXTERN));
		append(spec, simple(void_ptr_elem ? N_VOID : N_CHAR));
		node_t dl = list();
		append(dl, node3(N_ARR, ignore(), list(), ignore())); // []
		if (void_ptr_elem) append(dl, pointer());             // void* element
		node_t decl = node2(N_DECL, id(sym.c_str()), dl);
		node_t sd = simple(N_SPEC_DECL);
		append(sd, node1(N_SHARE, spec));
		append(sd, decl);
		append(sd, ignore());
		append(sd, ignore());
		append(sd, ignore());
		append(out, sd);
	};

	// --- _ZTS<cls>: static char _ZTS...[] = { bytes... };  (explicit byte list) ---
	{
		node_t spec = list();
		append(spec, simple(N_CHAR));
		node_t dl = list();
		append(dl, node3(N_ARR, ignore(), list(), ignore())); // [] (sized by init)
		node_t decl = node2(N_DECL, id(ts.c_str()), dl);
		node_t bytes = list();
		for (size_t i = 0; i < nm.size(); i++)
			append(bytes, node2(N_INIT, list(), integer((long)(unsigned char)nm[i])));
		append(bytes, node2(N_INIT, list(), integer(0))); // NUL terminator
		node_t sd = simple(N_SPEC_DECL);
		append(sd, node1(N_SHARE, spec));
		append(sd, decl);
		append(sd, ignore());
		append(sd, ignore());
		append(sd, bytes);
		append(out, sd);
	}

	// --- which libsupc++ type_info-class vtable symbol backs this flavor ---
	const char *abi_vt;
	switch (cdd->typeinfo_flavor()) {
	case DataDefCLASS::TI_CLASS: abi_vt = "_ZTVN10__cxxabiv117__class_type_infoE"; break;
	case DataDefCLASS::TI_SI:    abi_vt = "_ZTVN10__cxxabiv120__si_class_type_infoE"; break;
	default:                     abi_vt = "_ZTVN10__cxxabiv121__vmi_class_type_infoE"; break;
	}
	emit_data_extern(abi_vt, /*void_ptr_elem=*/false); // extern char abi_vt[];
	referenced_funcs.insert(abi_vt);

	// --- _ZTI<cls>: void *_ZTI...[] = { ... }; ---
	node_t inits = list();
	// [0] = (void*)(abi_vt + 16)  -- the type_info-class vtable address point
	append(inits, node2(N_INIT, list(),
		void_ptr_to(node2(N_ADD, id(abi_vt), integer(16)))));
	// [1] = (void*)_ZTS<cls>  -- the name string
	append(inits, node2(N_INIT, list(), void_ptr_to(id(ts.c_str()))));

	auto base_ti_ref = [&](DataDefCLASS *b) -> node_t {
		std::string bti = itanium_typeinfo_sym(b->name);
		emit_data_extern(bti, /*void_ptr_elem=*/true); // extern void* _ZTI<base>[];
		referenced_funcs.insert(bti);
		return void_ptr_to(id(bti.c_str()));            // (void*)_ZTI<base> (array decays)
	};

	if (cdd->typeinfo_flavor() == DataDefCLASS::TI_SI) {
		// [2] = (void*)_ZTI<base>
		append(inits, node2(N_INIT, list(), base_ti_ref(cdd->bases[0].base)));
	} else if (cdd->typeinfo_flavor() == DataDefCLASS::TI_VMI) {
		// [2] = (void*)( flags | (base_count << 32) )  (.long flags; .long count;)
		unsigned long flags = 0;   // 0 is always runtime-safe
		unsigned long count = cdd->bases.size();
		append(inits, node2(N_INIT, list(),
			void_ptr_int((long)(flags | (count << 32)))));
		for (const BaseSpec &bs : cdd->bases) {
			append(inits, node2(N_INIT, list(), base_ti_ref(bs.base)));
			long offflags = ((long)bs.offset << 8)
				| (bs.access == 0 ? 0x2L : 0L)   // __public_mask
				| (bs.is_virtual ? 0x1L : 0L);   // __virtual_mask
			append(inits, node2(N_INIT, list(), void_ptr_int(offflags)));
		}
	}

	{
		node_t spec = list();
		append(spec, simple(N_VOID));
		node_t dl = list();
		append(dl, node3(N_ARR, ignore(), list(), ignore())); // []
		append(dl, pointer());                                 // void* element
		node_t decl = node2(N_DECL, id(ti.c_str()), dl);
		node_t sd = simple(N_SPEC_DECL);
		append(sd, node1(N_SHARE, spec));
		append(sd, decl);
		append(sd, ignore());
		append(sd, ignore());
		append(sd, inits);
		append(out, sd);
	}

	return out;
}

node_t CirBuilder::class_vtable_def(DataDefCLASS *cdd, std::vector<node_t> &thunks)
{
	if (!cdd || !cdd->has_vtable || cdd->vtable_slots.empty())
		return NULL;

	// Build a this-adjusting thunk: a function with the override's signature whose
	// body re-adjusts `this` from the secondary-base subobject back to the override's
	// class, then tail-calls the override. Returns the thunk's symbol name.
	//   RET Cls__thunk_<off>_<m>(__self, p1...) {
	//       return Override((MOwner*)((char*)__self - off), p1...);
	//   }
	auto make_thunk = [&](FuncDef *ov, const std::string &target_sym,
			      DataDefCLASS *mowner, size_t off,
			      const std::string &mname) -> std::string {
		std::string tname = cdd->name + "__thunk_" + std::to_string(off) + "_" + mname;
		node_t ret_spec = list();
		node_t throwaway = list();
		fnptr_decl_pieces(ov, true, ret_spec, throwaway, std::vector<uint32_t>());
		node_t plist = list();
		for (size_t i = 0; i < ov->parameters.size(); i++) {
			std::string pn = (i == 0) ? "__self" : ("p" + std::to_string(i));
			append(plist, param_decl(ov->parameters[i], pn.c_str(), std::string()));
		}
		node_t tdecl = node2(N_DECL, id(tname.c_str()),
				     node1(N_LIST, node1(N_FUNC, plist)));
		// (char*)__self - off
		node_t charp = node2(N_CAST,
			node2(N_TYPE, node1(N_LIST, simple(N_CHAR)),
			      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
			id("__self"));
		node_t adj = node2(N_SUB, charp, integer((long)off));
		// (MOwner*)adj
		node_t ownerp = node2(N_CAST,
			node2(N_TYPE, node1(N_LIST, node2(N_STRUCT,
				id(mowner->name.c_str()), ignore())),
			      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
			adj);
		node_t a = list();
		append(a, ownerp);
		for (size_t i = 1; i < ov->parameters.size(); i++)
			append(a, id(("p" + std::to_string(i)).c_str()));
		node_t call = node2(N_CALL, id(target_sym.c_str()), a);
		referenced_funcs.insert(target_sym);
		node_t body = node2(N_BLOCK, list(),
				    node1(N_LIST, node2(N_RETURN, list(), call)));
		thunks.push_back(node4(N_FUNC_DEF, ret_spec, tdecl, list(), body));
		return tname;
	};

	// This-adjusting thunk for a destructor slot reached through a secondary-base
	// vptr: void Cls__dthunk_<off>_<tag>(char *__self){ target((struct Cls*)(__self - off)); }
	// target = the most-derived complete (D1) or deleting (D0) dtor; for D0 it then
	// frees the adjusted COMPLETE-object pointer. Mirrors make_thunk's adjustment.
	auto make_dtor_thunk = [&](const std::string &target_sym, size_t off,
				   const char *tag) -> std::string {
		std::string tname = cdd->name + "__dthunk_" + std::to_string(off) + "_" + tag;
		node_t ret_spec = node1(N_LIST, simple(N_VOID));
		node_t pspec = simple(N_SPEC_DECL);
		append(pspec, node1(N_LIST, simple(N_CHAR)));
		append(pspec, node2(N_DECL, id("__self"), node1(N_LIST, pointer())));
		append(pspec, ignore()); append(pspec, ignore()); append(pspec, ignore());
		node_t plist = list();
		append(plist, pspec);
		node_t tdecl = node2(N_DECL, id(tname.c_str()),
				     node1(N_LIST, node1(N_FUNC, plist)));
		// (struct Cls *)(__self - off)
		node_t adj = node2(N_SUB, id("__self"), integer((long)off));
		node_t cls_spec = node2(N_TYPE,
			node1(N_LIST, node2(N_STRUCT, id(cdd->name.c_str()), ignore())),
			node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		node_t adj_cast = node2(N_CAST, cls_spec, adj);
		node_t a = list();
		append(a, adj_cast);
		node_t call = node2(N_CALL, id(target_sym.c_str()), a);
		referenced_funcs.insert(target_sym);
		node_t body = node2(N_BLOCK, list(),
				    node1(N_LIST, node2(N_EXPR, list(), call)));
		thunks.push_back(node4(N_FUNC_DEF, ret_spec, tdecl, list(), body));
		return tname;
	};

	// Initializer: grouped sub-tables back-to-back — primary group, then each
	// secondary polymorphic base's group (address points recorded in
	// build_vtable_groups). Each slot resolves by NAME to the most-derived
	// findMethod (so overrides win); a pure/abstract slot emits a null pointer.
	// A slot in a SECONDARY group whose resolved method is an OVERRIDE (its __this
	// owner != the group's base) gets a this-adjusting thunk: the slot is reached
	// via the secondary-base vptr with a base-subobject `this`, so the thunk
	// re-adjusts before entering the override.
	node_t inits = list();
	for (size_t g = 0; g < cdd->vtable_groups.size(); g++) {
		const DataDefCLASS::VtableGroup &G = cdd->vtable_groups[g];
		// Itanium prologue for this address point: [offset_to_top, &type_info].
		// offset_to_top = -(this_offset): how far back to the most-derived object
		// (so __dynamic_cast / typeid can recover the complete object from any
		// subobject vptr). build_vtable_groups reserved 2 slots per group. (S5a)
		node_t vtype = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
				     node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		node_t otop = node2(N_CAST, vtype, integer(-(long)G.this_offset));
		append(inits, node2(N_INIT, list(), otop));
		// RTTI slot: &_ZTI<cls> — the most-derived class's type_info (the SAME
		// object for every group; offset_to_top tells the runtime how far back). (S5b)
		std::string ti_sym = itanium_typeinfo_sym(cdd->name);
		referenced_funcs.insert(ti_sym);
		node_t vtype2 = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
				      node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		node_t rtti = node2(N_CAST, vtype2, id(ti_sym.c_str()));
		append(inits, node2(N_INIT, list(), rtti));
		for (const std::string &slot : G.slots) {
			std::string sname = slot;
			if (sname == "~" || sname == "~$deleting") {
				std::string dsym = (sname == "~")
					? class_complete_dtor_symbol(cdd)
					: (cdd->name + "___dtor_deleting");
				if (G.this_offset != 0)
					dsym = make_dtor_thunk(dsym, G.this_offset,
						(sname == "~") ? "D1" : "D0");
				else
					referenced_funcs.insert(dsym);
				node_t vptr_type = node2(N_TYPE,
					node1(N_LIST, simple(N_VOID)),
					node2(N_DECL, ignore(), node1(N_LIST, pointer())));
				node_t fnref = node2(N_CAST, vptr_type, id(dsym.c_str()));
				append(inits, node2(N_INIT, list(), fnref));
				continue;
			}
			Variable *mv = cdd->findMethod(sname);
			if (!mv) {
				append(inits, node2(N_INIT, list(), integer(0)));
				continue;
			}
			FuncDef *mfd = dynamic_cast<FuncDef *>(mv->type);
			DataDefCLASS *mowner = (mfd && !mfd->parameters.empty())
				? class_behind(mfd->parameters[0]) : NULL;
			std::string symname = mv->name;
			if (G.this_offset != 0 && mowner && mowner != G.owner && mfd)
				symname = make_thunk(mfd, mv->name, mowner, G.this_offset, sname);
			else
				referenced_funcs.insert(mv->name);
			node_t vptr_type = node2(N_TYPE,
				node1(N_LIST, simple(N_VOID)),
				node2(N_DECL, ignore(), node1(N_LIST, pointer())));
			node_t fnref = node2(N_CAST, vptr_type, id(symname.c_str()));
			append(inits, node2(N_INIT, list(), fnref));
		}
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
	// Emit the struct fields in ASCENDING OFFSET order, inserting explicit char
	// padding and named vptr fields, so c2mir's natural field layout reproduces the
	// Itanium offsets computed by DataDefCLASS::compute_layout. Member access is by
	// NAME (N_DEREF_FIELD), so member nodes keep their real names; pad/vptr names are
	// synthetic and never referenced by user code. For a single-inheritance or plain
	// class this yields the same struct as before (vptr@0, ascending members, no pads).
	size_t objwords = object_class_words(cdd);
	bool opaque = cdd->members.empty() && !cdd->has_vptr_slot;
	if (opaque && objwords > 0) {
		// Opaque runtime-object class (std::string and friends): `long _w[words];`
		// filler, a complete type of the right size — size COMPUTED, never hard-coded.
		node_t mspec = list();
		append(mspec, simple(N_LONG));
		node_t mdecl_list = list();
		append(mdecl_list, node3(N_ARR, ignore(), list(),
					 integer((long)objwords)));
		node_t member = simple(N_MEMBER);
		append(member, node1(N_SHARE, mspec));
		append(member, node2(N_DECL, id("_w"), mdecl_list));
		append(member, ignore());
		append(member, ignore());
		append(member_list, member);
	} else {
		struct Field { size_t off; size_t sz; int kind; size_t midx; }; // kind 0=vptr,1=member
		std::vector<Field> fields;
		if (cdd->has_vptr_slot)
			fields.push_back(Field{0, 8, 0, 0}); // primary vptr @0
		for (DataDefCLASS *o : cdd->secondary_vptr_owners)
			for (size_t b = 0; b < cdd->bases.size(); b++)
				if (cdd->bases[b].base == o) {
					fields.push_back(Field{cdd->bases[b].offset, 8, 0, 0});
					break;
				}
		for (size_t i = 0; i < cdd->members.size(); i++) {
			size_t msz = cdd->members[i].second->size
				   * (i < cdd->member_counts.size() ? cdd->member_counts[i] : 1);
			fields.push_back(Field{cdd->member_offsets[i], msz, 1, i});
		}
		std::sort(fields.begin(), fields.end(),
			  [](const Field &a, const Field &b){ return a.off < b.off; });
		size_t cursor = 0; int synth = 0;
		for (const Field &f : fields) {
			if (f.off > cursor) { // fill the gap with a char pad so the next field lands at f.off
				std::string pn = "__pad" + std::to_string(synth++);
				node_t pspec = list(); append(pspec, simple(N_CHAR));
				node_t pdl = list();
				append(pdl, node3(N_ARR, ignore(), list(), integer((long)(f.off - cursor))));
				node_t pm = simple(N_MEMBER);
				append(pm, node1(N_SHARE, pspec));
				append(pm, node2(N_DECL, id(pn.c_str()), pdl));
				append(pm, ignore()); append(pm, ignore());
				append(member_list, pm);
				cursor = f.off;
			}
			if (f.kind == 0) { // vptr (primary keeps __vptr; secondaries __vptr_<offset>)
				std::string vn = (f.off == 0) ? "__vptr" : ("__vptr_" + std::to_string(f.off));
				node_t vspec = list(); append(vspec, simple(N_VOID));
				node_t vdl = list(); append(vdl, pointer());
				node_t vm = simple(N_MEMBER);
				append(vm, node1(N_SHARE, vspec));
				append(vm, node2(N_DECL, id(vn.c_str()), vdl));
				append(vm, ignore()); append(vm, ignore());
				append(member_list, vm);
				cursor += 8;
			} else {
				append(member_list, member_node(cdd->members[f.midx], cdd));
				cursor += f.sz;
			}
		}
		if (cdd->size > cursor) { // tail pad to the full computed size
			std::string pn = "__pad" + std::to_string(synth++);
			node_t pspec = list(); append(pspec, simple(N_CHAR));
			node_t pdl = list();
			append(pdl, node3(N_ARR, ignore(), list(), integer((long)(cdd->size - cursor))));
			node_t pm = simple(N_MEMBER);
			append(pm, node1(N_SHARE, pspec));
			append(pm, node2(N_DECL, id(pn.c_str()), pdl));
			append(pm, ignore()); append(pm, ignore());
			append(member_list, pm);
		}
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

// Resolve the class behind a (possibly pointer-wrapped) DataDef. Recognizes
// ordinary user classes AND runtime-object classes (std::string) so a method
// call on a `string` receiver routes through the class path.
static DataDefCLASS *class_behind(DataDef *dd)
{
	if (!dd) return NULL;
	if (DataDefCLASS *c = as_class_instance(dd)) return c;
	if (DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd))
		return as_class_instance(p->base_type);
	return NULL;
}

// True when a NAMED variable holds the OBJECT'S ADDRESS rather than the object
// itself — a by-value `string` parameter and a `string&` reference are lowered
// to a `void*` (param_decl), and an explicit pointer type (`string*`) is a
// pointer. For these the object address is the variable's value (`name`); for a
// value object lvalue it is `&name`. This is the single addressing rule shared
// by every object-class receiver (methods, operators, stream, args).
static bool var_is_pointer_stored(const Variable &v)
{
	if (v.type && v.type->is_pointer()) return true;
	if ((v.flags & vfPARAM) || (v.flags & vfREFERENCE)) return true;
	return is_std_string_ref(v.type);
}

// The raw object address of a NAMED variable (NOT cast to void*): the pointer
// itself when pointer-stored, else `&name`. The single source of truth for
// object-instance addressing; string_var_addr wraps this in a void* cast.
node_t CirBuilder::object_var_addr(const Variable &v, TokenBase *origin)
{
	node_t base = id(v.name.c_str(), origin);
	return var_is_pointer_stored(v) ? base : node1(N_ADDR, base, origin);
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
	bool from_var = false;
	if (tm->parent_expr) {
		recv_type = tm->parent_expr->datadef();
		recv_node = translate_expr(tm->parent_expr);
	} else {
		recv_type = tm->object.type;
		recv_node = id(tm->object.name.c_str(), origin);
		from_var = true;
	}
	recv_class = class_behind(recv_type);
	if (!recv_class) return NULL;
	// A NAMED object variable uses the unified addressing rule (a by-value /
	// by-ref `string` param is stored AS the object address, so its `this` is
	// the variable itself, NOT `&variable` — taking `&` would yield a double
	// pointer and segfault). A chained sub-expression (parent_expr) is an
	// rvalue/lvalue computed by translate_expr; address it by pointer-ness.
	if (from_var)
		return object_var_addr(tm->object, origin);
	bool recv_is_ptr = recv_type && recv_type->is_pointer();
	// Value receiver -> &obj; pointer receiver -> obj.
	return recv_is_ptr ? recv_node : node1(N_ADDR, recv_node, origin);
}

// The extern return-spec for an emit_symbol-bound method/operator: a pointer
// return (c_str()) is declared via ret_ptr; a `long` return (size()/length())
// needs an explicit {N_LONG} base so the value reads correctly in arithmetic;
// everything else (void / string& treated as void) defaults to a void base.
// `ret_ptr` is set out-param.
static std::vector<c2mir_node_code_t> emit_symbol_ret_specs(FuncDef *fd, bool &ret_ptr)
{
	ret_ptr = fd && fd->returns.is_pointer();
	if (ret_ptr) return {};   // void* / char* — base is void, ret_ptr carries the star
	DataType rt = fd ? fd->returns.rawtype() : DataType::dtVOID;
	if (rt == DataType::dtINT64 || rt == DataType::dtINT32)
		return { N_LONG };
	return {};   // void (clear, assign/append return string& we ignore)
}

// Lower a call to a class method bound to an external symbol (emit_symbol — a
// mangled libstdc++ std::string member). Declares the extern from the FuncDef's
// real signature, passes the receiver address as void*, and coerces explicit
// args (string params -> string_obj_arg). `empty()` is lowered to `size()==0`.
node_t CirBuilder::emit_symbol_method_call(TokenMember *tm, FuncDef *callee,
					   const std::string &sym, node_t this_arg,
					   TokenBase *origin)
{
	const std::string &method = tm->var.name;

	// empty() -> (size() == 0). The real size() member returns a clean size_t;
	// the bool-returning empty() returns a 1-byte value (garbage upper bits as
	// a 64-bit int). Same observable result, robust read. (g++ canon.)
	if (method == "empty") {
		need_output_extern(STR_SIZE, false, { { {N_VOID}, true } }, { N_LONG });
		node_t a = list();
		append(a, node2(N_CAST, void_ptr_type(), this_arg, origin));
		node_t call = node2(N_CALL, id(STR_SIZE, origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		node_t eq = node2(N_EQ, call, integer(0, origin), origin);
		CIR_NODE(eq)->synth_from_origin = true;
		return eq;
	}

	// Build the parameter shapes for the extern declaration and the args.
	// Param 0 is the hidden __this (a void* object pointer). Each subsequent
	// param mirrors the method's declared parameter type.
	std::vector<ExternParam> eparams;
	eparams.push_back({ {N_VOID}, true });   // this (void*)
	node_t args = list();
	append(args, node2(N_CAST, void_ptr_type(), this_arg, origin));   // this (void*)
	for (size_t i = 0; i < tm->parameters.size(); i++) {
		TokenBase *arg = tm->parameters[i];
		size_t pi = i + 1;   // +1 to skip __this
		DataDef *pt = (pi < callee->parameters.size())
				? callee->parameters[pi] : NULL;
		if (is_std_string(pt)) {
			// const string& -> object pointer (void*).
			eparams.push_back({ {N_VOID}, true });
			append(args, string_obj_arg(arg));
		} else if (pt && pt->is_pointer()) {
			// const char* etc.
			eparams.push_back({ {N_CHAR}, true });
			append(args, translate_expr(arg));
		} else {
			eparams.push_back({ {N_LONG}, false });
			append(args, translate_expr(arg));
		}
	}

	bool ret_ptr = false;
	std::vector<c2mir_node_code_t> ret_specs = emit_symbol_ret_specs(callee, ret_ptr);
	need_output_extern(sym.c_str(), ret_ptr, eparams, ret_specs);
	node_t call = node2(N_CALL, id(sym.c_str(), origin), args, origin);
	CIR_NODE(call)->synth_from_origin = true;
	// A T&-returning method (assign/append return string&) — we declared the
	// return as void and ignore the result, so no deref.
	return call;
}

node_t CirBuilder::class_method_call(TokenMember *tm, TokenBase *origin)
{
	DataDefCLASS *recv_class = NULL;
	node_t this_arg = class_this_arg(tm, recv_class, origin);
	if (!recv_class) return NULL;

	// The method's call symbol (`ClassName__method`) and signature. When the
	// FuncDef carries an explicit emit_symbol (e.g. a mangled libstdc++
	// std::string member), call through that instead of the default scheme.
	FuncDef *callee = dynamic_cast<FuncDef *>(tm->var.type);
	const std::string sym = (callee && !callee->emit_symbol.empty())
				? callee->emit_symbol : tm->var.name;

	// A method bound to an external symbol (emit_symbol — e.g. a mangled
	// libstdc++ std::string member) has no madc-emitted body and is not in
	// funcdef_map under its mangled name, so the referenced-funcs prototype
	// pass won't declare it. Declare it here from its real signature (return
	// type matters: length()/size() return `long`, c_str() returns char* —
	// an implicit/void return would misread the value), and route the call
	// directly. `empty()` is lowered to `size() == 0` (the bool-returning
	// libstdc++ empty() returns a 1-byte value that reads as garbage upper
	// bits when taken as a 64-bit int), matching g++'s observable result.
	if (callee && !callee->emit_symbol.empty())
		return emit_symbol_method_call(tm, callee, sym, this_arg, origin);

	// An inherited method's __this is typed `Base *`, but the receiver is a
	// `Derived *`. The base subobject sits at base_offset_of(owner) within the
	// derived object (0 for a primary/single-inheritance base, non-zero for a
	// secondary or virtual base). Adjust the byte address before casting to the
	// owner-class pointer type, so the method reads/writes its own subobject.
	DataDefCLASS *owner = (callee && !callee->parameters.empty())
				? class_behind(callee->parameters[0]) : NULL;
	if (owner && owner != recv_class) {
		node_t adj = this_arg;
		size_t boff = recv_class ? recv_class->base_offset_of(owner) : 0;
		if (boff != 0 && boff != (size_t)-1) {
			// (char*)this_arg + boff
			node_t charp = node2(N_CAST,
				node2(N_TYPE, node1(N_LIST, simple(N_CHAR)),
				      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
				this_arg, origin);
			adj = node2(N_ADD, charp, integer((long)boff), origin);
		}
		this_arg = node2(N_CAST,
			node2(N_TYPE,
			      node1(N_LIST, node2(N_STRUCT, id(owner->name.c_str()),
						  ignore())),
			      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
			adj, origin);
	}

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
		bool is_ref_param = callee && pi < callee->ref_params.size()
				    && callee->ref_params[pi];
		if (is_std_string(pt))
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
	// Grouped dispatch: find which vtable group (subobject) owns the method, load
	// THAT group's vptr field from the most-derived receiver, index by the in-group
	// slot (the vptr already points at the group's address point). Single
	// inheritance = group 0 / "__vptr" / flat slot, reproducing the old lowering.
	size_t grp; int slot;
	if (recv_class->find_vslot(mname, grp, slot) && callee) {
		DataDefCLASS *vowner = owner ? owner : recv_class;
		const DataDefCLASS::VtableGroup &G = recv_class->vtable_groups[grp];
		std::string vfld = (G.this_offset == 0)
			? "__vptr" : ("__vptr_" + std::to_string(G.this_offset));
		DataDefCLASS *dummy = NULL;
		node_t recv_for_vptr = class_this_arg(tm, dummy, origin);
		// (void**)recv->__vptr[_<off>] — load the owning group's vptr field by
		// name from the most-derived receiver (no vowner cast: the field already
		// lives in recv_class's struct at the right offset).
		node_t vptr = node2(N_DEREF_FIELD, recv_for_vptr,
				    id(vfld.c_str(), origin));
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
	node_t mcall = node2(N_CALL, id(sym.c_str(), origin), args, origin);
	// A T&-returning method returns the address of its result; deref so the
	// call expression is the referenced lvalue (read: *p; write: *p = rhs).
	if (callee && callee->returns_ref)
		return node1(N_DEREF, mcall, origin);
	return mcall;
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
static const char *MEMBER_COPY_SYM = "string_construct_copy";

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

bool CirBuilder::class_has_own_user_dtor(DataDefCLASS *cdd)
{
	if (!cdd) return false;
	return cdd->method_map.find("~" + cdd->name) != cdd->method_map.end();
}

// The user class that `dd` denotes IF returning it by value must use the
// struct-return (__retbuf) ABI — i.e. a NON-TRIVIAL class: one that needs a
// destructor (object members / user dtor / non-trivial base). A bit-copy return
// of such a class would double-free its members' resources at the two scope
// exits, so the callee must deep-copy (member copy-construct) into *__retbuf.
// std::string is handled by its own (mangled copy-ctor) string path, NOT here;
// a TRIVIAL struct keeps c2mir's native struct return. Returns NULL otherwise.
DataDefCLASS *CirBuilder::class_return_via_retbuf(DataDef *dd)
{
	if (!dd || dd->is_pointer()) return NULL;
	if (is_std_string(dd)) return NULL;   // string path owns it
	DataDefCLASS *cdd = as_class_instance(dd);
	if (!cdd) return NULL;
	return class_needs_dtor(cdd) ? cdd : NULL;
}

// Member-wise copy-construct each object member of `cdd` from `src` into the
// __retbuf return slot, and bit-copy the whole object first for the scalar
// members. Emits into `out`. `src` is a TokenBase whose translate_expr yields
// the source object lvalue (the `return obj;` operand). Mirrors g++'s implicit
// copy-ctor for a class with object members: copy each member, deep-copying the
// object ones (string -> string_construct_copy) so no buffer is shared.
void CirBuilder::class_copy_construct_into_retbuf(DataDefCLASS *cdd,
						  TokenBase *src,
						  std::vector<node_t> &out,
						  TokenBase *origin)
{
	// 1) Bit-copy the source object into *__retbuf (covers scalar members and
	//    establishes the layout). `*__retbuf = src;` — c2mir struct assignment.
	node_t retderef = node1(N_DEREF, id(RETBUF_NAME, origin), origin);
	node_t bitcopy = node2(N_ASSIGN, retderef, translate_expr(src), origin);
	out.push_back(node2(N_EXPR, list(), bitcopy, origin));
	// 2) Re-construct each object member from the source's member, so the slot
	//    owns its own buffers (the bit-copy aliased them). string member:
	//    string_construct_copy((void*)&__retbuf->m, (void*)&src.m).
	for (const auto &m : cdd->members) {
		if (!is_string_object(m.second)) continue;
		need_output_extern(MEMBER_COPY_SYM, false,
				   { { {N_VOID}, true }, { {N_VOID}, true } });
		node_t dstfld = node2(N_DEREF_FIELD, id(RETBUF_NAME, origin),
				      id(m.first.c_str(), origin));
		node_t dst = node2(N_CAST, void_ptr_type(),
				   node1(N_ADDR, dstfld, origin), origin);
		node_t srcfld = node2(N_FIELD, translate_expr(src),
				      id(m.first.c_str(), origin));
		node_t srcp = node2(N_CAST, void_ptr_type(),
				    node1(N_ADDR, srcfld, origin), origin);
		node_t a = list();
		append(a, dst);
		append(a, srcp);
		node_t call = node2(N_CALL, id(MEMBER_COPY_SYM, origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		out.push_back(node2(N_EXPR, list(), call, origin));
	}
}

// Memberwise copy-ASSIGNMENT of a NON-TRIVIAL class (one needing a dtor) into an
// EXISTING object: `lhs = rhs`. Unlike copy-CONSTRUCTION (into raw storage), the
// members are already constructed, so each member is COPY-ASSIGNED (free-old +
// deep-copy), NOT placement-copy-constructed — matching g++'s implicit
// operator=. A bit-copy `*lhs = *rhs` would alias the object members' heap
// buffers and double-free at cleanup; per-member assignment avoids that and is
// self-assignment-safe (std::string::operator= / string_assign handle x=x). The
// scalar members are assigned individually (NO whole-struct bit-copy).
//   string member: string_assign((void*)&lhs.m, (void*)&rhs.m)  (frees old, copies)
//   scalar member: lhs.m = rhs.m
// `lhs`/`rhs` are object lvalues; the builder re-translates them per member.
// Per-member copy-assignment core: given two already-bound `struct Cls *` local
// names (`lname` = &lhs, `rname` = &rhs), emit `lname->m`-from-`rname->m` for
// each member (string -> string_assign free-old+copy, scalar -> plain =).
void CirBuilder::class_copy_assign_members(DataDefCLASS *cdd, const char *lname,
					   const char *rname,
					   std::vector<node_t> &out,
					   TokenBase *origin)
{
	for (const auto &m : cdd->members) {
		node_t lfld = node2(N_DEREF_FIELD, id(lname, origin),
				    id(m.first.c_str(), origin));
		node_t rfld = node2(N_DEREF_FIELD, id(rname, origin),
				    id(m.first.c_str(), origin));
		if (is_string_object(m.second)) {
			// string_assign(&lhs.m, &rhs.m) — frees the old buffer and
			// deep-copies; self-assignment-safe.
			need_output_extern("string_assign", false,
					   { { {N_VOID}, true }, { {N_VOID}, true } });
			node_t ld = node2(N_CAST, void_ptr_type(),
					  node1(N_ADDR, lfld, origin), origin);
			node_t rd = node2(N_CAST, void_ptr_type(),
					  node1(N_ADDR, rfld, origin), origin);
			node_t a = list();
			append(a, ld);
			append(a, rd);
			node_t call = node2(N_CALL, id("string_assign", origin), a, origin);
			CIR_NODE(call)->synth_from_origin = true;
			out.push_back(node2(N_EXPR, list(), call, origin));
		} else {
			// Scalar (or trivial sub-object) member: plain assignment.
			node_t asn = node2(N_ASSIGN, lfld, rfld, origin);
			CIR_NODE(asn)->synth_from_origin = true;
			out.push_back(node2(N_EXPR, list(), asn, origin));
		}
	}
}

// A `struct Cls *<nm> = <init>;` pointer-binding decl (synthetic).
node_t CirBuilder::class_ptr_bind(DataDefCLASS *cdd, const char *nm,
				  node_t init, TokenBase *origin)
{
	node_t sd = simple(N_SPEC_DECL, origin);
	append(sd, node1(N_SHARE, node1(N_LIST,
		node2(N_STRUCT, id(cdd->name.c_str(), origin), ignore()))));
	append(sd, node2(N_DECL, id(nm, origin), node1(N_LIST, pointer())));
	append(sd, ignore());
	append(sd, ignore());
	append(sd, init);
	CIR_NODE(sd)->synth_from_origin = true;
	return sd;
}

void CirBuilder::class_copy_assign(DataDefCLASS *cdd, TokenBase *lhs,
				   TokenBase *rhs, std::vector<node_t> &out,
				   TokenBase *origin)
{
	// Bind lhs and rhs to `struct Cls *` locals ONCE (the rhs is evaluated a
	// single time) and assign member-by-member through the stable pointers. The
	// rhs is an object LVALUE (variable / member / by-value param), addressed by
	// &rhs. (An object-returning-CALL rhs goes through class_copy_assign_from_addr.)
	char lname[40], rname[40];
	snprintf(lname, sizeof(lname), "__ca_l_%d", m_strtmp_counter++);
	snprintf(rname, sizeof(rname), "__ca_r_%d", m_strtmp_counter++);
	out.push_back(class_ptr_bind(cdd, lname,
		node1(N_ADDR, translate_expr(lhs), origin), origin));
	out.push_back(class_ptr_bind(cdd, rname,
		node1(N_ADDR, translate_expr(rhs), origin), origin));
	class_copy_assign_members(cdd, lname, rname, out, origin);
}

// Copy-assign `lhs = *(Cls*)rhs_addr` where rhs_addr is a pre-materialized
// (void*) address of the source object (e.g. a by-value-return call temp). Used
// when the rhs is NOT a simple lvalue, so it must be evaluated exactly once into
// a stable address before the memberwise assignment reads from it.
void CirBuilder::class_copy_assign_from_addr(DataDefCLASS *cdd, TokenBase *lhs,
					     node_t rhs_addr,
					     std::vector<node_t> &out,
					     TokenBase *origin)
{
	char lname[40], rname[40];
	snprintf(lname, sizeof(lname), "__ca_l_%d", m_strtmp_counter++);
	snprintf(rname, sizeof(rname), "__ca_r_%d", m_strtmp_counter++);
	node_t rcast = node2(N_CAST,
		node2(N_TYPE, node1(N_LIST, node2(N_STRUCT,
			id(cdd->name.c_str(), origin), ignore())),
			node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
		rhs_addr, origin);
	out.push_back(class_ptr_bind(cdd, lname,
		node1(N_ADDR, translate_expr(lhs), origin), origin));
	out.push_back(class_ptr_bind(cdd, rname, rcast, origin));
	class_copy_assign_members(cdd, lname, rname, out, origin);
}

std::string CirBuilder::class_dtor_symbol(DataDefCLASS *cdd)
{
	if (!cdd) return std::string();
	// A class whose destructor is bound to an external symbol (emit_symbol set
	// — e.g. std::string's libstdc++ ~basic_string()) uses that symbol directly
	// as the cleanup function; madc synthesizes no Cls___dtor body for it
	// (synth_dtor_def early-returns on has_user_dtor). The libstdc++ dtor takes
	// only `this`, matching the cleanup attribute's `void (*)(T*)` shape.
	auto it = cdd->method_map.find("~" + cdd->name);
	if (it != cdd->method_map.end() && it->second) {
		FuncDef *dt = dynamic_cast<FuncDef *>(it->second->type);
		if (dt && !dt->emit_symbol.empty())
			return dt->emit_symbol;
	}
	return cdd->name + "___dtor";
}

// The COMPLETE-object dtor symbol: a class with virtual bases gets a synthesized
// `Cls___dtor_complete` (base dtor + vbase dtors, once); otherwise the plain dtor IS
// the complete dtor. Complete-object destruction sites (stack-var cleanup, delete,
// exception unwind) use this; base-subobject dtor calls use class_dtor_symbol.
std::string CirBuilder::class_complete_dtor_symbol(DataDefCLASS *cdd)
{
	if (!cdd) return std::string();
	std::vector<DataDefCLASS *> vbs; std::set<DataDefCLASS *> seen;
	cdd->collect_vbases(vbs, seen);
	if (vbs.empty()) return class_dtor_symbol(cdd);
	return cdd->name + "___dtor_complete";
}

// Append a dtor call for each transitive, deduped virtual base of `cdd`, in REVERSE
// (most-base last), offset-adjusted to vbase_offset[vb]. Mirror of vbase_ctor_stmts.
void CirBuilder::vbase_dtor_stmts(const std::string &objname, bool addr_of,
				  DataDefCLASS *cdd,
				  std::vector<node_t> &out, TokenBase *o)
{
	std::vector<DataDefCLASS *> vbs; std::set<DataDefCLASS *> seen;
	cdd->collect_vbases(vbs, seen);
	for (size_t i = vbs.size(); i-- > 0; ) {
		DataDefCLASS *vb = vbs[i];
		if (!class_needs_dtor(vb)) continue;
		size_t off = cdd->vbase_offset.count(vb) ? cdd->vbase_offset[vb] : 0;
		node_t obj_addr = addr_of
			? node1(N_ADDR, id(objname.c_str(), o), o)
			: id(objname.c_str(), o);
		node_t adj = obj_addr;
		if (off != 0) {
			node_t charp = node2(N_CAST,
				node2(N_TYPE, node1(N_LIST, simple(N_CHAR)),
				      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
				obj_addr, o);
			adj = node2(N_ADD, charp, integer((long)off, o), o);
		}
		node_t vt = node2(N_TYPE,
			node1(N_LIST, node2(N_STRUCT, id(vb->name.c_str()), ignore())),
			node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		std::string dsym = class_dtor_symbol(vb);
		referenced_funcs.insert(dsym);
		node_t a = list();
		append(a, node2(N_CAST, vt, adj, o));
		out.push_back(node2(N_EXPR, list(),
			node2(N_CALL, id(dsym.c_str(), o), a, o), o));
	}
}

// void Cls___dtor_complete(struct Cls *__this) { Cls___dtor(__this); <vbase dtors>; }
node_t CirBuilder::synth_complete_dtor_def(DataDefCLASS *cdd)
{
	node_t ret_type = node1(N_LIST, simple(N_VOID));
	node_t pspec = node1(N_LIST, node2(N_STRUCT, id(cdd->name.c_str()), ignore()));
	node_t param = simple(N_SPEC_DECL);
	append(param, pspec);
	append(param, node2(N_DECL, id("__this"), node1(N_LIST, pointer())));
	append(param, ignore());
	append(param, ignore());
	append(param, ignore());
	node_t param_list = list();
	append(param_list, param);
	node_t decl = node2(N_DECL, id((cdd->name + "___dtor_complete").c_str()),
			    node1(N_LIST, node1(N_FUNC, param_list)));
	std::vector<node_t> stmts;
	// base-object dtor (members + non-virtual bases)
	std::string bsym = class_dtor_symbol(cdd);
	referenced_funcs.insert(bsym);
	node_t ba = list();
	append(ba, id("__this"));
	stmts.push_back(node2(N_EXPR, list(), node2(N_CALL, id(bsym.c_str()), ba)));
	// then the virtual bases, once, in reverse
	vbase_dtor_stmts("__this", /*addr_of=*/false, cdd, stmts, NULL);
	node_t items = list();
	for (node_t s : stmts) append(items, s);
	node_t body = node2(N_BLOCK, list(), items);
	return node4(N_FUNC_DEF, ret_type, decl, list(), body);
}

// void Cls___dtor_deleting(struct Cls *__this) { <complete-dtor>(__this); free(__this); }
// Itanium D0 (deleting) destructor: complete-object destruction, then operator
// delete (free, for user classes). Referenced from the D0 vtable slot.
node_t CirBuilder::synth_deleting_dtor_def(DataDefCLASS *cdd)
{
	node_t ret_type = node1(N_LIST, simple(N_VOID));
	node_t pspec = node1(N_LIST, node2(N_STRUCT, id(cdd->name.c_str()), ignore()));
	node_t param = simple(N_SPEC_DECL);
	append(param, pspec);
	append(param, node2(N_DECL, id("__this"), node1(N_LIST, pointer())));
	append(param, ignore());
	append(param, ignore());
	append(param, ignore());
	node_t param_list = list();
	append(param_list, param);
	node_t decl = node2(N_DECL, id((cdd->name + "___dtor_deleting").c_str()),
			    node1(N_LIST, node1(N_FUNC, param_list)));
	std::vector<node_t> stmts;
	std::string csym = class_complete_dtor_symbol(cdd);
	referenced_funcs.insert(csym);
	node_t ca = list();
	append(ca, id("__this"));
	stmts.push_back(node2(N_EXPR, list(), node2(N_CALL, id(csym.c_str()), ca)));
	need_output_extern("free", false, { { {N_VOID}, true } });
	node_t fa = list();
	append(fa, id("__this"));
	stmts.push_back(node2(N_EXPR, list(), node2(N_CALL, id("free"), fa)));
	node_t items = list();
	for (node_t s : stmts) append(items, s);
	node_t body = node2(N_BLOCK, list(), items);
	return node4(N_FUNC_DEF, ret_type, decl, list(), body);
}

// Forward prototype `void <sym>(struct Cls *__this);` for a synthesized dtor
// symbol (base / complete / deleting). The vtable initializer (Pass 1.5) takes
// these function addresses as global-init constants, which c2mir requires to be
// declared first; the bodies are emitted later (Passes 1.6/1.7/1.8).
node_t CirBuilder::synth_dtor_proto(const std::string &sym, DataDefCLASS *cdd)
{
	node_t ret_type = node1(N_LIST, simple(N_VOID));
	node_t pspec = node1(N_LIST, node2(N_STRUCT, id(cdd->name.c_str()), ignore()));
	node_t param = simple(N_SPEC_DECL);
	append(param, pspec);
	append(param, node2(N_DECL, id("__this"), node1(N_LIST, pointer())));
	append(param, ignore());
	append(param, ignore());
	append(param, ignore());
	node_t param_list = list();
	append(param_list, param);
	node_t decl = node2(N_DECL, id(sym.c_str()),
			    node1(N_LIST, node1(N_FUNC, param_list)));
	node_t proto = simple(N_SPEC_DECL);
	append(proto, node1(N_SHARE, ret_type));
	append(proto, decl);
	append(proto, ignore());
	append(proto, ignore());
	append(proto, ignore());
	return proto;
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
	if (!cdd || class_has_own_user_dtor(cdd) || !class_needs_dtor(cdd))
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

	// Body: destruct object members (reverse order), then base dtors in REVERSE
	// declaration order (MI), each with `this` adjusted to that base's subobject.
	std::vector<node_t> stmts;
	class_member_destruct(cdd, stmts, NULL);
	for (size_t bi = cdd->bases.size(); bi-- > 0; ) {
		if (cdd->bases[bi].is_virtual) continue; // vbases: complete-object dtor
		DataDefCLASS *b = cdd->bases[bi].base;
		if (!class_needs_dtor(b)) continue;
		std::string bsym = class_dtor_symbol(b);
		referenced_funcs.insert(bsym);
		size_t off = cdd->base_offset_of(b);
		node_t self = id("__this");
		node_t adj = self;
		if (off != 0) {
			node_t charp = node2(N_CAST,
				node2(N_TYPE, node1(N_LIST, simple(N_CHAR)),
				      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
				self);
			adj = node2(N_ADD, charp, integer((long)off));
		}
		node_t bt = node2(N_TYPE,
			node1(N_LIST, node2(N_STRUCT, id(b->name.c_str()), ignore())),
			node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		node_t a = list();
		append(a, node2(N_CAST, bt, adj));
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

// Generic overload-resolution ranking (declared in madc.h). No type is
// special-cased; std::string falls out of the class-object + converting-ctor
// rules like any other class.
int score_arg_to_param(const DataDef *adc, const DataDef *pdc,
		       bool param_is_ref, bool allow_udc)
{
	if (!pdc || !adc)
		return 0;   // unknown shape: neutral, don't reject
	// A reference parameter (`T&`) is represented as a pointer-to-T; bind it as
	// the referenced type T (object/value semantics), NOT as a raw pointer — so a
	// `T` argument binds a `const T&` parameter (e.g. the copy ctor) like C++.
	if (param_is_ref && pdc->is_pointer()) {
		const DataDefPTR *pp = dynamic_cast<const DataDefPTR *>(pdc);
		if (pp && pp->base_type)
			pdc = pp->base_type;
	}
	// A class-object parameter binds: an argument of the SAME class (identity
	// / copy), or — via one user-defined conversion — an argument that one of
	// the class's single-argument constructors accepts.
	if (pdc->is_object()) {
		if (adc->is_object() && adc->rawtype() == pdc->rawtype())
			return 5;   // exact class match (copy / same object)
		if (!allow_udc)
			return -1;  // no further user-defined conversion permitted
		const DataDefCLASS *pc = dynamic_cast<const DataDefCLASS *>(pdc);
		if (!pc)
			return -1;
		int best = -1;
		for (Variable *cv : pc->ctors) {
			FuncDef *fd = cv ? dynamic_cast<FuncDef *>(cv->type) : NULL;
			if (!fd || fd->parameters.size() != 2)   // __this + exactly one
				continue;
			bool cref = fd->ref_params.size() > 1 && fd->ref_params[1];
			int s = score_arg_to_param(adc, fd->parameters[1], cref, false);
			if (s > best)
				best = s;
		}
		return best >= 0 ? 2 : -1;   // a viable converting ctor => conversion
	}
	// A class-object ARGUMENT only binds the same-class parameter (handled
	// above); there is no implicit object->scalar/pointer conversion here.
	if (adc->is_object())
		return -1;
	bool p_ptr = pdc->is_pointer(), a_ptr = adc->is_pointer();
	bool p_num = pdc->is_numeric(), a_num = adc->is_numeric();
	if (p_ptr && a_ptr)
		return adc->rawtype() == pdc->rawtype() ? 5 : 4;
	if (p_num && a_num)
		return adc->rawtype() == pdc->rawtype() ? 5 : 4;
	if ((p_ptr && a_num) || (p_num && a_ptr))
		return -1;   // cross-category (numeric <-> pointer): cannot bind
	return 0;            // unrecognized pairing: neutral
}

// The call symbol for a constructor of `cdd`. Precedence: an external ABI
// symbol (emit_symbol — e.g. a libstdc++-bound std::string ctor) > a
// disambiguated-overload symbol (class_emit_name — the 2nd+ same-arity ctor,
// which mangles to its own ClassName__ClassName__oN) > the default
// ClassName__ClassName scheme (the first / only ctor).
static std::string ctor_call_symbol(DataDefCLASS *cdd, FuncDef *ctor)
{
	if (ctor && !ctor->emit_symbol.empty())
		return ctor->emit_symbol;
	if (ctor && !ctor->class_emit_name.empty())
		return ctor->class_emit_name;
	return cdd->name + "__" + cdd->name;
}

// Select the constructor overload of `cdd` matching the initializer arguments by
// ARGUMENT TYPE (general overload resolution, not arity alone), via the shared
// generic score_arg_to_param ranking. The candidate whose parameter types best
// match the argument types wins; a candidate that cannot bind any argument is
// rejected. No type is special-cased — a same-class argument selects the copy
// ctor, a convertible argument selects a converting ctor, for std::string and
// every user class identically. Relies on operator-result initializers being
// correctly TYPED (Program::resolve_object_operator_type), so a `T c = a + b`
// copy-init sees the operator's class return type, not a pointer/arithmetic type.
// `cdd->ctors` lists the overloads. Returns NULL when no overload set is recorded
// (caller falls back to the single ctor keyed under the class name).
FuncDef *CirBuilder::select_ctor_overload(DataDefCLASS *cdd,
					  const std::vector<TokenBase *> &ctor_args)
{
	if (!cdd || cdd->ctors.empty()) return NULL;
	FuncDef *best = NULL;
	int best_score = -1;
	for (Variable *cv : cdd->ctors) {
		FuncDef *fd = cv ? dynamic_cast<FuncDef *>(cv->type) : NULL;
		if (!fd) continue;
		// Parameter count excluding the hidden __this (param 0). With default
		// arguments a ctor is callable with [required..total] user args; the
		// omitted trailing ones are filled from param_defaults by class_ctor_call.
		size_t pn = fd->parameters.empty() ? 0 : fd->parameters.size() - 1;
		size_t req = fd->required_param_count();
		size_t req_user = req > 0 ? req - 1 : 0;   // exclude hidden __this
		if (ctor_args.size() < req_user || ctor_args.size() > pn) continue;
		int total = 0;
		bool ok = true;
		for (size_t i = 0; i < ctor_args.size(); i++) {
			size_t pi = i + 1;   // skip __this
			DataDef *pt = (pi < fd->parameters.size())
				    ? fd->parameters[pi] : NULL;
			bool refp = pi < fd->ref_params.size() && fd->ref_params[pi];
			DataDef *adc = ctor_args[i] ? ctor_args[i]->datadef() : NULL;
			int s = score_arg_to_param(adc, pt, refp);
			if (s < 0) { ok = false; break; }
			total += s;
		}
		if (ok && total > best_score) { best_score = total; best = fd; }
	}
	return best;
}

node_t CirBuilder::class_ctor_call(Variable *v, DataDefCLASS *cdd,
				   const std::vector<TokenBase *> &ctor_args,
				   TokenBase *origin)
{
	if (!v || !cdd) return NULL;

	// No user ctor, but a polymorphic class still needs its vptr(s) initialized so
	// a virtual call through a base pointer to this (stack) object works — C++'s
	// implicit default ctor sets the vptr. Emit the grouped vptr-init (same as the
	// `new` path / user-ctor prologue). Non-polymorphic ctorless classes need
	// nothing. (A vbase-only class with no virtual methods has no emitted vtable —
	// its vbase-offset table is S4 — so gate on has_vtable, not has_vptr_slot.)
	if (!cdd->has_user_ctor) {
		if (!cdd->has_vtable) return NULL;
		std::string vname = cdd->name + "__vtable";
		node_t blk = list();
		for (size_t g = 0; g < cdd->vtable_groups.size(); g++) {
			const DataDefCLASS::VtableGroup &G = cdd->vtable_groups[g];
			std::string fld = (G.this_offset == 0)
				? "__vptr" : ("__vptr_" + std::to_string(G.this_offset));
			node_t lhs = node2(N_DEREF_FIELD,
				node1(N_ADDR, id(v->name.c_str(), origin), origin),
				id(fld.c_str(), origin));
			node_t vptr_type = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
				node2(N_DECL, ignore(), node1(N_LIST, pointer())));
			node_t tab = id(vname.c_str(), origin);
			node_t ap = (G.addr_point == 0) ? tab
				: node2(N_ADD, tab, integer((long)G.addr_point, origin), origin);
			node_t vtab = node2(N_CAST, vptr_type, ap, origin);
			append(blk, node2(N_EXPR, list(),
				node2(N_ASSIGN, lhs, vtab, origin), origin));
		}
		return node2(N_BLOCK, list(), blk, origin);
	}

	// Resolve the ctor: prefer the overload matching the initializer (the
	// general path; a user class has one ctor so this is a no-op). Fall back
	// to the single ctor keyed under the class name.
	FuncDef *ctor = select_ctor_overload(cdd, ctor_args);
	if (!ctor) {
		// IMPLICIT COPY CONSTRUCTOR: `T c = <T value>` with no matching ctor.
		// C++ synthesizes a copy ctor when the class declares none; for a TRIVIAL
		// class (no user dtor / object members / non-trivial base) that is a
		// member-wise bit copy, expressed here as a struct assignment from the
		// source object. So a user class without an explicit copy ctor still
		// copy-initializes (e.g. from an operator+ result) instead of mis-binding
		// an unrelated ctor. (Non-trivial classes need member-wise
		// copy-construction; deferred — they typically declare a copy ctor.)
		if (ctor_args.size() == 1 && ctor_args[0]
		    && as_class_instance(ctor_args[0]->datadef()) == cdd
		    && !class_needs_dtor(cdd)) {
			node_t src = translate_expr(ctor_args[0]);
			node_t asgn = node2(N_ASSIGN, id(v->name.c_str(), origin),
					    src, origin);
			return node2(N_EXPR, list(), asgn, origin);
		}
		auto it = cdd->method_map.find(cdd->name);
		if (it != cdd->method_map.end() && it->second)
			ctor = dynamic_cast<FuncDef *>(it->second->type);
	}

	std::string sym = ctor_call_symbol(cdd, ctor);

	node_t args = list();
	append(args, node1(N_ADDR, id(v->name.c_str(), origin), origin)); // &v
	for (size_t i = 0; i < ctor_args.size(); i++) {
		TokenBase *arg = ctor_args[i];
		size_t pi = i + 1;   // skip __this
		DataDef *pt = (ctor && pi < ctor->parameters.size())
				? ctor->parameters[pi] : NULL;
		bool is_ref_param = ctor && pi < ctor->ref_params.size()
				    && ctor->ref_params[pi];
		if (is_std_string(pt))
			append(args, string_obj_arg(arg));
		else if (is_ref_param)
			append(args, node1(N_ADDR, translate_expr(arg), arg));
		else
			append(args, translate_expr(arg));
	}
	// C++ default arguments: fill omitted trailing parameters from their default
	// expressions (param_defaults is index-aligned with parameters). Same arg
	// shaping as the user args above.
	for (size_t pi = ctor_args.size() + 1;
	     ctor && pi < ctor->parameters.size() && pi < ctor->param_defaults.size()
	     && ctor->param_defaults[pi]; pi++) {
		TokenBase *darg = ctor->param_defaults[pi];
		DataDef *pt = ctor->parameters[pi];
		bool is_ref_param = pi < ctor->ref_params.size() && ctor->ref_params[pi];
		if (is_std_string(pt))
			append(args, string_obj_arg(darg));
		else if (is_ref_param)
			append(args, node1(N_ADDR, translate_expr(darg), darg));
		else
			append(args, translate_expr(darg));
	}
	// libstdc++ basic_string(const char*, const allocator&): the real ABI takes
	// a trailing allocator& madc has no value for — pass &this (a throwaway
	// pointer the ctor treats as an empty allocator). See FuncDef::ctor_trailing_self.
	if (ctor && ctor->ctor_trailing_self)
		append(args, node1(N_ADDR, id(v->name.c_str(), origin), origin));
	referenced_funcs.insert(sym);
	node_t call = node2(N_CALL, id(sym.c_str(), origin), args, origin);
	node_t ctor_stmt = node2(N_EXPR, list(), call, origin);

	// Complete-object construction: build the (transitive, deduped) virtual bases
	// FIRST (base-most order), then run the ctor. The ctor itself constructs only
	// non-virtual bases (it skips vbases), so this is the single place each shared
	// vbase is constructed. No vbases -> just the ctor call (byte-identical).
	std::vector<DataDefCLASS *> vbs; std::set<DataDefCLASS *> vseen;
	cdd->collect_vbases(vbs, vseen);
	if (vbs.empty())
		return ctor_stmt;
	std::vector<node_t> stmts;
	vbase_ctor_stmts(v->name, /*addr_of=*/true, cdd, stmts, origin);
	node_t blk = list();
	for (node_t s : stmts) append(blk, s);
	append(blk, ctor_stmt);
	return node2(N_BLOCK, list(), blk, origin);
}

// Append a ctor call for each transitive, deduped virtual base of `cdd`, base-most
// first, with `this` adjusted to vbase_offset[vb]. The object is named `objname`;
// `addr_of` true means take its address (a stack var), false means it is already a
// pointer (a `new`'d temp). A fresh address node is built per vbase (no node reuse).
// Trivial (ctorless) vbases contribute nothing.
void CirBuilder::vbase_ctor_stmts(const std::string &objname, bool addr_of,
				  DataDefCLASS *cdd,
				  std::vector<node_t> &out, TokenBase *o)
{
	std::vector<DataDefCLASS *> vbs; std::set<DataDefCLASS *> seen;
	cdd->collect_vbases(vbs, seen);
	for (DataDefCLASS *vb : vbs) {
		if (!vb->has_user_ctor) continue;
		size_t off = cdd->vbase_offset.count(vb) ? cdd->vbase_offset[vb] : 0;
		node_t obj_addr = addr_of
			? node1(N_ADDR, id(objname.c_str(), o), o)
			: id(objname.c_str(), o);
		node_t adj = obj_addr;
		if (off != 0) {
			node_t charp = node2(N_CAST,
				node2(N_TYPE, node1(N_LIST, simple(N_CHAR)),
				      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
				obj_addr, o);
			adj = node2(N_ADD, charp, integer((long)off, o), o);
		}
		node_t vt = node2(N_TYPE,
			node1(N_LIST, node2(N_STRUCT, id(vb->name.c_str()), ignore())),
			node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		std::string vsym = vb->name + "__" + vb->name;
		referenced_funcs.insert(vsym);
		node_t a = list();
		append(a, node2(N_CAST, vt, adj, o));
		out.push_back(node2(N_EXPR, list(),
			node2(N_CALL, id(vsym.c_str(), o), a, o), o));
	}
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
	case TokenID::tkAssign: return "=";
	case TokenID::tkAddEq:  return "+=";
	// Compound assignments.
	case TokenID::tkSubEq:  return "-=";
	case TokenID::tkMulEq:  return "*=";
	case TokenID::tkDivEq:  return "/=";
	case TokenID::tkModEq:  return "%=";
	case TokenID::tkBandEq: return "&=";
	case TokenID::tkBorEq:  return "|=";
	case TokenID::tkXorEq:  return "^=";
	case TokenID::tkBSLEq:  return "<<=";
	case TokenID::tkBSREq:  return ">>=";
	// Bitwise.
	case TokenID::tkBand:   return "&";
	case TokenID::tkBor:    return "|";
	case TokenID::tkXor:    return "^";
	// Shifts. The stream-chain `cout << x` / `cin >> x` path is intercepted
	// BEFORE class_operator_call (see translate_expr), so these only reach a
	// class operator<<()/operator>>() declared on a non-stream class object.
	case TokenID::tkBSL:    return "<<";
	case TokenID::tkBSR:    return ">>";
	// Logical.
	case TokenID::tkLand:   return "&&";
	case TokenID::tkLor:    return "||";
	default:                return "";
	}
}

// Does this operator WRITE its left operand? True for plain assignment and the
// compound assignments (+= -= *= /= %= &= |= ^= <<= >>=). Used by the const-LHS
// enforcement: writing through a const lvalue is an error. (The inc/dec write
// path is handled separately in translate_expr's inc/dec branch.)
static bool is_assign_op(TokenID id)
{
	switch (id) {
	case TokenID::tkAssign:
	case TokenID::tkAddEq: case TokenID::tkSubEq:
	case TokenID::tkMulEq: case TokenID::tkDivEq: case TokenID::tkModEq:
	case TokenID::tkBandEq: case TokenID::tkBorEq: case TokenID::tkXorEq:
	case TokenID::tkBSLEq: case TokenID::tkBSREq:
		return true;
	default:
		return false;
	}
}

// Among a class's operator overloads of name `mname`, select the one whose
// single explicit parameter (param 1; param 0 = __this) matches the RHS: a
// string-OBJECT RHS picks the (const string&) overload, a const char* value
// picks the (const char*) overload. Falls back to the first by-name match for
// a class with a single (non-overloaded) operator. NULL when none is found.
FuncDef *CirBuilder::select_operator_overload(DataDefCLASS *cls,
					      const std::string &mname,
					      TokenBase *rhs)
{
	bool rhs_is_string = CirBuilder::is_string_object_value(rhs);
	// Scan the methods vector for same-name overloads (std::string registers
	// operator=/operator+= here under the unmangled name, two overloads each).
	FuncDef *first = NULL;
	for (Variable *mv : cls->methods) {
		if (!mv || mv->name != mname) continue;
		FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
		if (!fd) continue;
		if (!first) first = fd;
		DataDef *p1dd = (fd->parameters.size() > 1)
			      ? fd->parameters[1] : NULL;
		bool p1_is_string = is_std_string(p1dd);
		if (rhs_is_string == p1_is_string) return fd;
	}
	if (first) return first;
	// User-class operators carry the MANGLED name (ClassName__operatorX) in the
	// methods vector, so the unmangled-name scan above misses them. Scan the
	// mangled family and prefer the BINARY (parameterized, params > 1) overload
	// — this is the binary dispatch. A same-name unary peer (params == 1, the
	// `_un`-suffixed nullary) is skipped here; select_unary_operator_overload
	// owns it. (P2.1b gaps 3 & 4.)
	std::string mangled_canon = cls->name + "__" + mname;
	std::string mangled_un = mangled_canon + "_un";
	FuncDef *any = NULL;
	for (Variable *mv : cls->methods) {
		if (!mv || (mv->name != mangled_canon && mv->name != mangled_un))
			continue;
		FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
		if (!fd) continue;
		if (!any) any = fd;
		if (fd->parameters.size() > 1) return fd;   // binary
	}
	if (any) return any;
	// Fall back to the keyed lookup (method_map under the unmangled name).
	std::string key = mname;
	Variable *mv = cls->findMethod(key);
	return mv ? dynamic_cast<FuncDef *>(mv->type) : NULL;
}

node_t CirBuilder::class_operator_call(TokenOperator *top, TokenBase *origin)
{
	if (!top || !top->left || !top->right) return NULL;
	// The lhs is either an ordinary user class or a runtime-object class
	// (std::string, via the object-class bridge), both of which use the class
	// operator path. NULL lhs class -> not an overloadable operator here.
	// A string-OBJECT subscript element (`v[i]` whose operator[] returns a
	// string&) is itself a std::string object; its class is ddSTRING even
	// though TokenSubscript::datadef() reports the element as a scalar. Resolve
	// the element's class from the operator[] result so `v[i] = rhs` /
	// `v[i] += rhs` route through std::string's operator=/operator+=.
	//
	// The operator LHS must be a class OBJECT lvalue, not a pointer to one:
	// `string s; s = "x"` is operator=, but `string *p; p = malloc(...)` (a
	// `T* data` member of a Vec<string>) is a plain pointer assignment.
	// as_class_instance resolves an object class without unwrapping pointers;
	// class_behind would treat `string*` as a string object and mis-route the
	// pointer assignment through operator=.
	DataDefCLASS *lcls = as_class_instance(top->left->datadef());
	if (!lcls && is_string_subscript(top->left)) {
		TokenSubscript *lsub = dynamic_cast<TokenSubscript *>(top->left);
		DataDefCLASS *ccls = lsub ? class_behind(lsub->object.type) : NULL;
		std::string opname = "operator[]";
		Variable *omv = ccls ? ccls->findMethod(opname) : NULL;
		FuncDef *ofd = omv ? dynamic_cast<FuncDef *>(omv->type) : NULL;
		if (ofd) lcls = class_behind(&ofd->returns);
	}
	// A chained `(a + b) + c`: the LHS is itself a string operator+ RVALUE whose
	// datadef() is the default scalar (TokenOperator carries no string type), so
	// as_class_instance misses it. It is a std::string object — resolve its class
	// directly so the outer + binds to std::string's operator+.
	if (!lcls && is_string_operator_plus(top->left))
		lcls = class_behind(&ddSTRING);
	// A std::string REFERENCE lhs that as_class_instance missed: a `for (string&
	// s : c)` loop var is a `string*`+vfREFERENCE pointer-to-object (P2.7), so its
	// datadef() is pointer-typed and as_class_instance never unwraps it. Match
	// ONLY a vfREFERENCE var whose pointee is a std::string — NOT a plain `string*`
	// member/local (e.g. a vector<string>'s `data` member, where `data = malloc()`
	// must stay a pointer assignment, never std::string::operator=). string_obj_arg
	// addresses the ref via its pointer value, so the operator dispatches like a
	// string value (P2.8b).
	if (!lcls) {
		TokenVar *rtv = dynamic_cast<TokenVar *>(top->left);
		if (rtv && (rtv->var.flags & vfREFERENCE) && is_std_string(rtv->var.type))
			lcls = class_behind(&ddSTRING);
	}
	if (!lcls) return NULL;
	// A `string&` receiver is already canonicalized to the one populated ddSTRING
	// by as_object_class (it returns ddSTRING for any std::string identity), so
	// lcls owns std::string's operators here — no separate canonicalize step.
	const char *opsym = binop_overload_symbol(top->id());
	if (!opsym[0]) return NULL;

	// operator+ on std::string is CONCATENATION, which differs from =/+=/==:
	// its LHS may be a const char* used in POINTER ARITHMETIC (`"lit" + n`,
	// `label + i`) — every char* literal/expr is std::string-typed, so the
	// as_class_instance(datadef) trigger above matches it spuriously. Defer to
	// is_string_operator_plus (genuine string-OBJECT LHS + string-like RHS); a
	// spurious match falls through to ordinary pointer/arithmetic +.
	// This char*-arithmetic ambiguity is SPECIFIC to the string class; a real
	// user class `operator+` (`Counter + Counter`) is an unambiguous object
	// operand and proceeds to overload selection (which returns NULL — and thus
	// falls through to generic + — when the class declares no operator+).
	if (top->id() == TokenID::tkAdd && is_string_object(lcls)
	    && !is_string_operator_plus(top))
		return NULL;

	std::string mname = std::string("operator") + opsym;
	// Pick the overload matching the RHS (string& vs const char*). A class
	// without this operator yields NULL -> fall through to generic handling.
	FuncDef *callee = select_operator_overload(lcls, mname, top->right);
	if (!callee) return NULL;

	// A class-bound external operator (std::string) names its real symbol via
	// emit_symbol and is declared from its signature; otherwise the default
	// ClassName__operator<op> scheme + the referenced-funcs prototype pass.
	if (!callee->emit_symbol.empty()) {
		// Int-returning comparison operators (== != < > <= >=) bind to an
		// extern-C runtime wrapper (string_equals / string_lt/gt/le/ge) that
		// RETURNS INT and takes BOTH operands as string-object addresses (two
		// void*). This differs from =/+= (which return basic_string& / void* and
		// take one rhs) and from + (by-value string return). Each wrapper
		// computes its own comparison, so NO negation — EXCEPT !=, which reuses
		// string_equals and negates the int result (no separate wrapper needed).
		TokenID oid = top->id();
		if (oid == TokenID::tkEquals || oid == TokenID::tkNotEq
		    || oid == TokenID::tkLT || oid == TokenID::tkGT
		    || oid == TokenID::tkLE || oid == TokenID::tkGE) {
			node_t cmpargs = list();
			append(cmpargs, string_obj_arg(top->left));
			append(cmpargs, string_obj_arg(top->right));
			// int string_XX(void*, void*)
			need_output_extern(callee->emit_symbol.c_str(), false,
					   { { {N_VOID}, true }, { {N_VOID}, true } },
					   { N_INT });
			node_t cmpcall = node2(N_CALL,
					       id(callee->emit_symbol.c_str(), origin),
					       cmpargs, origin);
			CIR_NODE(cmpcall)->synth_from_origin = true;
			if (oid == TokenID::tkNotEq) {
				node_t neg = node1(N_NOT, cmpcall, origin);
				CIR_NODE(neg)->synth_from_origin = true;
				return neg;
			}
			return cmpcall;
		}

		// operator+ binds to the extern-C wrapper string_concat, which RETURNS
		// A NEW string BY VALUE. Like a by-value string-returning call, allocate
		// a cleanup-tagged `struct string` temp and have the wrapper construct
		// the result into it: `string_concat((void*)&__t, &lhs, &rhs)` (three
		// void* args — out-slot + both operand object addresses). The expression
		// VALUE is the temp's (void*) address — a string-OBJECT lvalue, so
		// `string c = a + b` copy-constructs from it and `cout << (a + b)` prints
		// it. The const char* RHS overload still passes a string object (string_obj_arg
		// materializes a literal into a temp), matching string_concat's `void*` b.
		if (oid == TokenID::tkAdd) {
			char name[32];
			string_temp_decl(name, sizeof(name), origin);
			node_t cargs = list();
			append(cargs, string_obj_addr(name, origin));   // out-slot = &__t
			append(cargs, string_obj_arg(top->left));        // const string& a
			append(cargs, string_obj_arg(top->right));       // const string& b
			// void string_concat(void*, void*, void*)
			need_output_extern(callee->emit_symbol.c_str(), false,
					   { { {N_VOID}, true }, { {N_VOID}, true },
					     { {N_VOID}, true } });
			node_t ccall = node2(N_CALL,
					     id(callee->emit_symbol.c_str(), origin),
					     cargs, origin);
			CIR_NODE(ccall)->synth_from_origin = true;
			m_pending_stmts.push_back(node2(N_EXPR, list(), ccall, origin));
			return string_obj_addr(name, origin);   // (void*)&__t
		}

		DataDef *pt = (callee->parameters.size() > 1)
				? callee->parameters[1] : NULL;
		std::vector<ExternParam> eparams = { { {N_VOID}, true } };
		node_t args = list();
		// The lhs `this` is a std::string OBJECT — string_obj_arg yields its
		// (void*) address whether it is a named variable (honouring pointer-
		// stored params via string_var_addr), a string MEMBER (`&obj.field`),
		// or a string-object subscript element.
		append(args, string_obj_arg(top->left));
		if (is_std_string(pt)) {
			eparams.push_back({ {N_VOID}, true });
			append(args, string_obj_arg(top->right));   // const string& -> object ptr
		} else {
			eparams.push_back({ {N_CHAR}, true });
			append(args, translate_expr(top->right));   // const char*
		}
		// The libstdc++ operators return basic_string& (a void* we ignore
		// when the result is unused). Declared returning void* so a chained
		// use still type-checks; the common statement use discards it.
		need_output_extern(callee->emit_symbol.c_str(), true, eparams);
		node_t ocall = node2(N_CALL, id(callee->emit_symbol.c_str(), origin),
				     args, origin);
		CIR_NODE(ocall)->synth_from_origin = true;
		return ocall;
	}

	// The lhs `this` address for a user-class operator: a NAMED variable
	// honours the unified pointer-stored rule; any other lhs expression is
	// addressed by &expr (a value lvalue).
	node_t this_arg;
	if (TokenVar *ltv = dynamic_cast<TokenVar *>(top->left))
		this_arg = object_var_addr(ltv->var, origin);
	else
		this_arg = node1(N_ADDR, translate_expr(top->left), origin);

	// ClassName__operator== by default; an arity-disambiguated same-name
	// overload (P2.1b gaps 3 & 4) carries its real symbol in class_emit_name.
	std::string sym = !callee->class_emit_name.empty()
			  ? callee->class_emit_name : (lcls->name + "__" + mname);

	// Part B: an operator returning a class object BY VALUE (e.g. V operator+(V&))
	// yields an rvalue; materialize it into an addressable cleanup temp so the
	// result can be copy-constructed from, passed by reference, or have members
	// called — generic for any class (the std::string operator+ path above does
	// the same with its own wrapper). A NON-TRIVIAL return was compiled with the
	// __retbuf ABI (hidden return-slot first param) -> pass &__t as that slot and
	// the void call writes *__retbuf; a TRIVIAL (native struct) return is assigned
	// into __t. The expression value is then the temp's object lvalue.
	DataDefCLASS *retc = as_class_instance(&callee->returns);
	bool by_value_object = retc && !callee->returns_ref
			       && !callee->returns.is_pointer();
	bool via_retbuf = by_value_object
			  && class_return_via_retbuf(&callee->returns) != NULL;
	char objtmp[40] = { 0 };
	if (by_value_object) {
		snprintf(objtmp, sizeof(objtmp), "__madc_objtmp_%d", m_strtmp_counter++);
		Variable *tmp = new Variable(objtmp, *retc, 1, NULL, false);
		tmp->flags |= vfLOCAL;
		m_pending_stmts.push_back(var_decl(tmp, origin));
	}

	node_t args = list();
	if (via_retbuf)
		append(args, node1(N_ADDR, id(objtmp, origin), origin));   // __retbuf slot
	append(args, this_arg);
	// Single explicit RHS argument (operator parameter 1; param 0 = __this).
	{
		DataDef *pt = (callee->parameters.size() > 1)
				? callee->parameters[1] : NULL;
		bool refp = callee->ref_params.size() > 1 && callee->ref_params[1];
		if (is_std_string(pt))
			append(args, string_obj_arg(top->right));
		else if (refp)
			append(args, node1(N_ADDR, translate_expr(top->right), top->right));
		else
			append(args, translate_expr(top->right));
	}
	referenced_funcs.insert(sym);
	node_t ocall = node2(N_CALL, id(sym.c_str(), origin), args, origin);

	if (by_value_object) {
		if (via_retbuf)
			m_pending_stmts.push_back(node2(N_EXPR, list(), ocall, origin));
		else
			m_pending_stmts.push_back(node2(N_EXPR, list(),
				node2(N_ASSIGN, id(objtmp, origin), ocall, origin), origin));
		return id(objtmp, origin);   // materialized object lvalue
	}
	// T&-returning operator returns an address; deref to the lvalue.
	if (callee && callee->returns_ref)
		return node1(N_DEREF, ocall, origin);
	return ocall;
}

// Among a class's operator overloads of name `mname`, select the UNARY one:
// the overload with NO explicit parameter (only param 0 = __this). This is how
// a unary `operator-()` is told apart from the binary `operator-(const C&)`
// (which shares the same method name). NULL when the class declares no nullary
// overload of that operator.
static FuncDef *select_unary_operator_overload(DataDefCLASS *cls,
					       const std::string &mname)
{
	if (!cls) return NULL;
	// Scan the methods vector for a same-name overload taking only __this.
	for (Variable *mv : cls->methods) {
		if (!mv || mv->name != mname) continue;
		FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
		if (!fd) continue;
		if (fd->parameters.size() <= 1) return fd;   // __this only -> unary
	}
	// User-class operators carry the MANGLED name in the methods vector. Scan
	// the mangled family (ClassName__operatorX and the `_un`-suffixed nullary
	// variant) and return the NULLARY (params <= 1 == __this only) overload —
	// the unary dispatch. This is how a unary `operator-()` is told apart from a
	// same-name binary `operator-(const C&)`. (P2.1b gaps 3 & 4.)
	std::string mangled_canon = cls->name + "__" + mname;
	std::string mangled_un = mangled_canon + "_un";
	for (Variable *mv : cls->methods) {
		if (!mv || (mv->name != mangled_canon && mv->name != mangled_un))
			continue;
		FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
		if (!fd) continue;
		if (fd->parameters.size() <= 1) return fd;   // nullary -> unary
	}
	// Fall back to the keyed lookup (method_map under the unmangled name).
	std::string key = mname;
	Variable *mv = cls->findMethod(key);
	FuncDef *fd = mv ? dynamic_cast<FuncDef *>(mv->type) : NULL;
	if (fd && fd->parameters.size() <= 1) return fd;
	return NULL;
}

node_t CirBuilder::class_unary_operator_call(const char *opsym,
					     TokenBase *operand, TokenBase *origin)
{
	if (!opsym || !opsym[0] || !operand) return NULL;
	// The operand must be a class OBJECT lvalue. as_class_instance resolves an
	// object class without unwrapping pointers (so `C* p; -p` stays a built-in
	// pointer op and never mis-routes). NULL -> not an overloadable unary op.
	DataDefCLASS *cls = as_class_instance(operand->datadef());
	if (!cls) return NULL;

	std::string mname = std::string("operator") + opsym;
	FuncDef *callee = select_unary_operator_overload(cls, mname);
	if (!callee) return NULL;   // class declares no unary operator<sym> -> built-in

	// The `this` address: a NAMED object variable honours the unified
	// pointer-stored rule; any other operand expression is addressed by &expr.
	node_t this_arg;
	if (TokenVar *otv = dynamic_cast<TokenVar *>(operand))
		this_arg = object_var_addr(otv->var, origin);
	else
		this_arg = node1(N_ADDR, translate_expr(operand), origin);

	// A class-bound external operator names its real symbol via emit_symbol;
	// otherwise the default ClassName__operator<sym> scheme + the
	// referenced-funcs prototype pass (a madc-emitted method body). An
	// arity-disambiguated same-name overload (P2.1b gaps 3 & 4 — the unary peer
	// of a binary of the same name) carries its real symbol in class_emit_name.
	std::string sym = !callee->emit_symbol.empty()
			  ? callee->emit_symbol
			  : (!callee->class_emit_name.empty()
			     ? callee->class_emit_name : (cls->name + "__" + mname));
	std::vector<ExternParam> eparams = { { {N_VOID}, true } };
	node_t args = list();
	if (!callee->emit_symbol.empty()) {
		append(args, node2(N_CAST, void_ptr_type(), this_arg, origin));
		bool ret_ptr = false;
		std::vector<c2mir_node_code_t> ret_specs =
			emit_symbol_ret_specs(callee, ret_ptr);
		need_output_extern(sym.c_str(), ret_ptr, eparams, ret_specs);
	} else {
		append(args, this_arg);
		referenced_funcs.insert(sym);
	}
	node_t ocall = node2(N_CALL, id(sym.c_str(), origin), args, origin);
	CIR_NODE(ocall)->synth_from_origin = true;
	// A T&-returning unary operator returns an address; deref to the lvalue.
	if (callee->returns_ref)
		return node1(N_DEREF, ocall, origin);
	return ocall;
}

// Build the bare `ClassName__operator[](&obj, i)` call — the raw method result.
// For a T&-returning operator[] this is the element ADDRESS (a T*); callers that
// want the element lvalue deref it (class_subscript_call). NULL when the object
// is not a class with an operator[].
node_t CirBuilder::class_subscript_addr(TokenSubscript *tsub, TokenBase *origin)
{
	if (!tsub) return NULL;
	DataDefCLASS *cls = class_behind(tsub->object.type);
	if (!cls) return NULL;
	std::string opname = "operator[]";
	Variable *mv = cls->findMethod(opname);
	if (!mv) return NULL;
	FuncDef *callee = dynamic_cast<FuncDef *>(mv->type);

	// A class-bound external operator[] (std::string's char& operator[](size_t))
	// names its real libstdc++ symbol via emit_symbol and has no madc-emitted
	// body. Emit the mangled symbol and declare it as an extern from its
	// signature — mirroring class_operator_call's emit_symbol branch — instead
	// of the default ClassName__operator[] + referenced-funcs scheme (which only
	// works for a user-class method body). The libstdc++ operator returns char&
	// (a char* pointer return); class_subscript_call derefs it (returns_ref) so
	// `s[i]` is a proper char lvalue.
	if (callee && !callee->emit_symbol.empty()) {
		// __this: the std::string object's (void*) address. string_var_addr
		// honours the unified pointer-stored-param rule (a string& param holds
		// the address; a value var is addressed by &var).
		node_t this_arg = string_var_addr(tsub->object, origin);
		// Param 0 = this (void*); param 1 = size_t index (a 64-bit scalar).
		std::vector<ExternParam> eparams = { { {N_VOID}, true },
						     { {N_LONG}, false } };
		node_t args = list();
		append(args, this_arg);
		append(args, translate_expr(tsub->index));
		// char& -> a char* pointer return; class_subscript_call derefs it to
		// the char lvalue (so `s[i]` reads and `s[i] = c` writes).
		need_output_extern(callee->emit_symbol.c_str(), true, eparams,
				   { N_CHAR });
		node_t call = node2(N_CALL, id(callee->emit_symbol.c_str(), origin),
				    args, origin);
		CIR_NODE(call)->synth_from_origin = true;
		return call;
	}

	std::string sym = cls->name + "__operator[]";   // ClassName__operator[]
	// __this: value receiver -> &obj, pointer receiver -> obj.
	bool recv_is_ptr = tsub->object.type && tsub->object.type->is_pointer();
	node_t recv = id(tsub->object.name.c_str(), origin);
	node_t this_arg = recv_is_ptr ? recv : node1(N_ADDR, recv, origin);

	node_t args = list();
	append(args, this_arg);
	// Index argument (operator[] parameter 1; parameter 0 = __this).
	DataDef *idx_pt = (callee && callee->parameters.size() > 1)
			  ? callee->parameters[1] : NULL;
	bool refp = callee && callee->ref_params.size() > 1 && callee->ref_params[1];
	if (is_std_string(idx_pt))
		// A string-keyed operator[] (`map<string,V>`): the key parameter is a
		// std::string object, passed by address (matching build_call_args).
		append(args, string_obj_arg(tsub->index));
	else if (refp)
		append(args, node1(N_ADDR, translate_expr(tsub->index), tsub->index));
	else
		append(args, translate_expr(tsub->index));

	referenced_funcs.insert(sym);
	return node2(N_CALL, id(sym.c_str(), origin), args, origin);
}

node_t CirBuilder::class_subscript_call(TokenSubscript *tsub, TokenBase *origin)
{
	node_t call = class_subscript_addr(tsub, origin);
	if (!call) return NULL;
	DataDefCLASS *cls = class_behind(tsub->object.type);
	std::string opname = "operator[]";
	Variable *mv = cls ? cls->findMethod(opname) : NULL;
	FuncDef *callee = mv ? dynamic_cast<FuncDef *>(mv->type) : NULL;
	// operator[] conventionally returns T& -> deref to the lvalue so the
	// result is usable as both an rvalue (read) and an lvalue (`v[i] = x`).
	if (callee && callee->returns_ref)
		return node1(N_DEREF, call, origin);
	return call;
}

// Is `tsub` a subscript yielding a std::string-OBJECT element — i.e. an
// operator[] on a class whose element type is a string object? Such an element
// is a real string object reached by address (the bare operator[] call).
/*static*/ bool CirBuilder::is_string_subscript(TokenBase *arg)
{
	TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(arg);
	if (!tsub) return false;
	DataDefCLASS *cls = class_behind(tsub->object.type);
	if (!cls) return false;
	std::string opname = "operator[]";
	Variable *mv = cls->findMethod(opname);
	if (!mv) return false;
	FuncDef *callee = dynamic_cast<FuncDef *>(mv->type);
	return callee && is_string_object(&callee->returns);
}

// A raw string-array element `base[i]` where base is a `string*` pointer or a
// `string[]` fixed array. Unlike is_string_subscript (an operator[] container),
// here the subscript is a plain pointer/array index; its element datadef is a
// string object reached by `&base[i]`. Used so `string cur = keys[i]` (copy
// from a string element of a member array inside a template class) selects the
// std::string copy ctor, not the const char* ctor.
/*static*/ bool CirBuilder::is_string_array_subscript(TokenBase *arg)
{
	// Exclude the operator[]-container case (handled by is_string_subscript):
	// there the base CLASS itself owns an operator[] returning string&.
	if (is_string_subscript(arg)) return false;
	// Form 1 — `var[i]` where var is a `string*`/`string[]` local/param.
	if (TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(arg)) {
		if (!is_string_object(tsub->datadef())) return false;
		DataDef *bt = tsub->object.type;
		if (!bt) return false;
		if (tsub->object.is_fixed_array())
			return bt->is_string();
		DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(bt);
		return pdd && pdd->base_type && pdd->base_type->is_string();
	}
	// Form 2 — `expr[i]` where expr is a `string*`-valued expression (e.g. a
	// member array `__this->keys` inside a class method). The element type the
	// subscript computes must be a string object, and the base expression's
	// type must be a pointer-to-string.
	if (TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(arg)) {
		if (!is_string_object(tse->datadef())) return false;
		DataDef *bt = tse->base_expr ? tse->base_expr->datadef() : NULL;
		DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(bt);
		return pdd && pdd->base_type && pdd->base_type->is_string();
	}
	return false;
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
			// Tag kind must match the aggregate's real kind (union vs
			// struct) here too — `typedef union {..} U` else lowers to a
			// struct and union aliasing/by-value passing breaks.
			c2mir_node_code_t agg = sdd->union_layout ? N_UNION : N_STRUCT;
			if (force_incomplete_struct || emitted_structs.count(sdd->name) || !sdd->is_complete) {
				// Forward reference / already-emitted / incomplete:
				// STRUCT/UNION(tag, IGNORE). The full body is emitted at the
				// aggregate's own definition point.
				append(tl, node2(agg, id(sdd->name.c_str()), ignore()));
			} else {
				// Emit definition inline (typedef struct/union { ... } NAME)
				append(tl, node2(agg, id(sdd->name.c_str()),
						 anon_members_list(sdd)));
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
	bool ret_is_ref = fd->returns_ref;   // T& -> returned by address (one more *)
	DataDefPTR *ret_ptr = ret_is_ptr ? dynamic_cast<DataDefPTR *>(ret_dd) : NULL;
	if (ret_ptr && ret_ptr->base_type) ret_dd = ret_ptr->base_type;

	// A by-value std::string OR non-trivial class return uses the __retbuf ABI
	// (void return + hidden `struct <T> *__retbuf` first param); keep the
	// prototype in lock-step with func_def's lowering. A trivial struct keeps
	// c2mir's native struct return.
	bool ret_is_string = !ret_is_ptr && !ret_is_ref && is_std_string(ret_dd)
			     && !fd->is_multi_return();
	DataDefCLASS *ret_obj = (!ret_is_ptr && !ret_is_ref && !fd->is_multi_return())
				? class_return_via_retbuf(ret_dd) : NULL;
	bool ret_via_retbuf = ret_is_string || ret_obj;
	DataDef *retbuf_dd = ret_obj ? (DataDef *)ret_obj : (DataDef *)&ddSTRING;
	bool ret_is_multi = fd->is_multi_return();   // void ret + `long *__retbuf` first param

	node_t ret_type = (ret_via_retbuf || ret_is_multi) ? type_list(&ddVOID)
							   : type_list(ret_dd);
	node_t share = node1(N_SHARE, ret_type);

	// Parameters. A variadic function carries a trailing synthetic param
	// (the parser pushes a ddINT64 placeholder when it sees `...`); drop it
	// and emit N_DOTS instead so the prototype is truly variadic.
	node_t param_list = list();
	if (ret_via_retbuf)
		append(param_list, retbuf_param(retbuf_dd, tf));
	else if (ret_is_multi)
		append(param_list, retbuf_param(&ddINT64, tf));   // long *__retbuf
	size_t nparam = fd->parameters.size();
	if (fd->is_varargs && nparam > 0) nparam--;
	// A capturing nested fn / [&] lambda gains hidden `T *name` capture params
	// (FuncDef::captured_vars, populated by func_def in Pass 2 — which runs
	// BEFORE this proto pass). A zero-user-param capturing fn that actually
	// captured at least one var is therefore NOT `(void)`. Keep the prototype's
	// parameter list in lock-step with func_def's.
	bool has_capture_params = fd->has_captures && !fd->captured_vars.empty();
	// A zero-param function emits `(void)` ONLY when it was declared `(void)`
	// (is_void_params). A bare K&R `()` is unprototyped: leave the param list
	// empty so c2mir imposes no arg-count check at call sites, matching gcc's
	// gnu89 behavior. Kept in lock-step with func_def and fnptr_func_node.
	if (nparam == 0 && !fd->is_varargs && !ret_via_retbuf && !ret_is_multi
	    && !has_capture_params) {
		if (fd->is_void_params) {
			node_t void_spec = node1(N_LIST, simple(N_VOID));
			node_t void_decl = node2(N_DECL, ignore(), list());
			node_t void_param = node2(N_TYPE, void_spec, void_decl);
			append(param_list, void_param);
		}
		// else: bare () — unprototyped (K&R) parameter list
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
	if (fd->has_captures)
		for (Variable *cv : fd->captured_vars) {
			if (!cv) continue;
			DataDef *capt_ptr = new DataDefPTR(*cv->type);
			append(param_list, param_decl(capt_ptr, cv->name.c_str(),
						      std::string()));
		}
	if (fd->is_varargs)
		append(param_list, simple(N_DOTS));

	node_t func_inner = node1(N_FUNC, param_list);

	node_t func_id = id(tf->var.name.c_str(), tf);
	node_t decl_list = list();
	append(decl_list, func_inner);
	if (ret_is_ptr)
		append(decl_list, pointer());
	// T&-returning method: returned by address (one extra pointer level), so
	// the prototype matches the definition and call sites can deref.
	if (ret_is_ref)
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

// __builtin_{add,sub,mul}_overflow[_p] dispatch.
//
// The lexer text-maps every type-generic overflow builtin to a single
// long-width helper (__madc_add_overflow(long,long,long*)). That is wrong:
// the overflow predicate AND the truncated store both depend on the
// DESTINATION type's width and signedness, which the lexer can't see. GCC
// computes in infinite precision and tests the fit against the destination
// type. So the correct layer to choose the helper is here in the CIR builder,
// where the argument types are resolved:
//
//   _overflow   : 3rd arg is `&dst` — use dst's pointee width + signedness.
//   _overflow_p : 3rd arg is a typed zero — use that operand's width + signedness.
//
// va_helpers.cpp already defines the width/signedness-specific helpers
// (__madc_add_overflow_s16/_u16/.../_s64/_u64 and the _p_* variants); they were
// previously dead because nothing selected them. This routes to them.
// c2mir's NATIVE overflow builtins are not usable here: they reject any
// destination narrower than int, which these GCC-torture tests exercise.
static std::string overflow_helper_name(const std::string &generic,
					 TokenBase *third_arg,
					 bool operands_all_unsigned)
{
	// generic is one of __madc_{add,sub,mul}_overflow[_p].
	bool is_p = generic.size() > 2 && generic.compare(generic.size() - 2, 2, "_p") == 0;

	// Resolve the destination integer type.
	DataDef *dst = NULL;
	if (third_arg) {
		DataDef *dd = third_arg->datadef();
		if (!is_p) {
			// `&dst` — a pointer; the destination is its pointee.
			if (DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(dd))
				dst = pdd->base_type;
		} else {
			// typed-zero — its own type is the destination type.
			dst = dd;
		}
	}
	if (!dst || !dst->is_integer())
		return generic;   // unknown — keep the long-width generic helper.

	size_t bytes = dst->size;
	bool uns = dst->is_unsigned();

	// Map byte width to the helper suffix. The helpers cover 8/16/32/64-bit.
	int bits = bytes >= 8 ? 64 : bytes >= 4 ? 32 : bytes >= 2 ? 16 : 8;

	// The _overflow_p helpers only provide 16/32/64-bit variants; an 8-bit
	// destination uses the 16-bit signed/unsigned helper safely (the predicate
	// tests the same infinite-precision fit — but to stay exact, widen 8->16
	// only for _p, where no _p_*8 helper exists).
	if (is_p && bits == 8)
		bits = 16;

	// 64-bit op with BOTH operands unsigned: the operands reach the helper as
	// `long long`, which sign-extends a large unsigned value (2^64-18 -> -18).
	// The generic `_u64` helper signed-widens those -> wrong overflow flag and
	// result (pr85095). The `_uu64` helper reinterprets the inputs as unsigned
	// first. Key on the OPERANDS' signedness, NOT the destination: with a
	// uint64 destination but SIGNED operands (`__builtin_mul_overflow(int,int,
	// &u64)`), the signed widening is correct and `_uu64` would be wrong
	// (pr91450-1). u8/u16/u32 unsigned operands fit positively in long long, so
	// only the 64-bit width needs this. (No `_p_uu64`: the _p form takes a
	// typed-zero, not the operands, so it isn't affected.)
	if (operands_all_unsigned && bits == 64 && !is_p)
		return generic + "_uu64";

	char suffix[16];
	snprintf(suffix, sizeof(suffix), "_%c%d", uns ? 'u' : 's', bits);
	return generic + suffix;
}

// -----------------------------------------------------------------------
// Expression translation
// -----------------------------------------------------------------------

node_t CirBuilder::translate_expr(TokenBase *tb)
{
	if (!tb) return ignore();

	// Imaginary literal (`1.0i`, `3i`, `2.0iF`) — the lexer types the constant
	// as a DataDefCOMPLEX. It must lower to c2mir's imaginary-constant node
	// (N_CF/N_CD/N_CLD), NOT a bare real/integer, or the imaginary part is lost
	// (`5.0 + 7.0i` folds to the real `12.0`). Covers both integer- and
	// real-spelled imaginary literals.
	if ((tb->type() == TokenType::ttInteger || tb->type() == TokenType::ttReal)
	    && tb->datadef() && tb->datadef()->is_complex())
		return complex_literal(tb->dval(), tb->datadef(), tb);

	// Integer literal — preserve the literal's declared type (the lexer
	// records signedness/width from the u/l/ll suffix, e.g. `0xffffffffull`
	// is unsigned long long). Emitting it as a bare signed N_I/N_L would
	// drop that and break usual-arithmetic-conversion signedness downstream.
	if (tb->type() == TokenType::ttInteger)
		return integer_typed(tb->ival(), tb->datadef(), tb);

	// Real literal — emit single-precision (N_F) when the literal carries an
	// `f`/`F` suffix (datadef == float), so its width drives c2mir's usual-
	// arithmetic conversions; otherwise double (N_D).
	if (tb->type() == TokenType::ttReal) {
		DataDef *rdd = tb->datadef();
		if (rdd && rdd->rawtype() == DataType::dtFLOAT && !rdd->is_complex())
			return real_float((float)tb->dval(), tb);
		return real(tb->dval(), tb);
	}

	// Char literal (C char constants emit N_CH; value via ival())
	if (tb->type() == TokenType::ttChar)
		return ch(tb->ival(), tb);

	// dynamic_cast<Tgt*>(e)  (S5c) ->
	//   (struct Tgt*)__dynamic_cast((void*)e, (void*)_ZTI<src>, (void*)_ZTI<dst>, hint)
	// libstdc++'s __dynamic_cast reads e's vptr[-1] (the RTTI slot wired in S5b) to
	// recover the most-derived type, then walks the type_info base graph. The _ZTI
	// objects are emitted file-scope in Pass 1.5 (before this body), so referencing
	// them by name resolves. hint = offset of src within dst when src is a unique
	// public non-virtual base of dst (g++'s optimization), else -1 (general search).
	if (TokenDynamicCast *dc = dynamic_cast<TokenDynamicCast *>(tb)) {
		DataDefCLASS *dstC = class_behind(dc->target_type);
		DataDefCLASS *srcC = class_behind(dc->operand ? dc->operand->datadef() : NULL);
		if (!dstC || !srcC || !dstC->has_vtable || !srcC->has_vtable)
			return error_node("dynamic_cast requires polymorphic class pointers", tb);
		std::string src_ti = itanium_typeinfo_sym(srcC->name);
		std::string dst_ti = itanium_typeinfo_sym(dstC->name);
		referenced_funcs.insert(src_ti);
		referenced_funcs.insert(dst_ti);
		long hint = -1; size_t off = 0;
		if (dstC->is_unique_public_nonvirtual_base(srcC, &off)) hint = (long)off;
		need_output_extern("__dynamic_cast", /*ret_ptr=*/true,
			{ { {N_VOID}, true }, { {N_VOID}, true },
			  { {N_VOID}, true }, { {N_LONG}, false } });
		node_t args = list();
		append(args, node2(N_CAST, void_ptr_type(), translate_expr(dc->operand), tb));
		append(args, node2(N_CAST, void_ptr_type(), id(src_ti.c_str()), tb));
		append(args, node2(N_CAST, void_ptr_type(), id(dst_ti.c_str()), tb));
		append(args, integer(hint, tb));
		node_t call = node2(N_CALL, id("__dynamic_cast"), args, tb);
		node_t tgt_t = node2(N_TYPE,
			node1(N_LIST, node2(N_STRUCT, id(dstC->name.c_str()), ignore())),
			node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		return node2(N_CAST, tgt_t, call, tb);
	}

	// typeid(T) / typeid(e)  (S5d) -> a std::type_info* (the result is modeled as
	// a pointer; type_info::name()/== operate on it). The TYPE form and a
	// non-polymorphic expression yield the static &_ZTI<class>; a POLYMORPHIC
	// expression reads the runtime type from the object's vptr[-1] (the RTTI slot
	// wired in S5b), so typeid(*base_ptr) reports the most-derived type.
	if (TokenTypeid *ti = dynamic_cast<TokenTypeid *>(tb)) {
		DataDefCLASS *tinfo = class_behind(tb->datadef()); // std::type_info
		auto tinfo_ptr = [&]() -> node_t {
			return node2(N_TYPE,
				node1(N_LIST, node2(N_STRUCT,
					id(tinfo ? tinfo->name.c_str() : "type_info"), ignore())),
				node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		};
		if (ti->static_type) {
			DataDefCLASS *c = class_behind(ti->static_type);
			if (!c) return error_node("typeid type operand is not a class type", tb);
			std::string sym = itanium_typeinfo_sym(c->name);
			referenced_funcs.insert(sym);
			return node2(N_CAST, tinfo_ptr(), id(sym.c_str()), tb);
		}
		DataDefCLASS *ec = class_behind(ti->operand ? ti->operand->datadef() : NULL);
		if (ec && ec->is_polymorphic()) {
			// (type_info*)( ((void**)(&obj)->__vptr)[-1] )
			node_t objptr = node1(N_ADDR, translate_expr(ti->operand), tb);
			node_t vptr = node2(N_DEREF_FIELD, objptr, id("__vptr", tb));
			node_t vpp_dl = list();
			append(vpp_dl, pointer());
			append(vpp_dl, pointer());
			node_t vpp_type = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
						node2(N_DECL, ignore(), vpp_dl));
			node_t vtab = node2(N_CAST, vpp_type, vptr, tb);
			node_t rtti = node2(N_IND, vtab, integer(-1, tb), tb);
			return node2(N_CAST, tinfo_ptr(), rtti, tb);
		}
		if (!ec) return error_node("typeid operand has no RTTI class type", tb);
		std::string sym = itanium_typeinfo_sym(ec->name);
		referenced_funcs.insert(sym);
		return node2(N_CAST, tinfo_ptr(), id(sym.c_str()), tb);
	}

	// GNU statement expression `({ stmt...; expr; })` used as a VALUE. c2mir
	// models it natively as N_STMTEXPR(compound_stmt); the block's last item
	// must be an expression-statement (c2mir enforces this). A bare TokenCpnd
	// only reaches translate_expr in value position — in statement position
	// translate_stmt routes it to translate_block — so this is unambiguous.
	if (TokenCpnd *tc = dynamic_cast<TokenCpnd *>(tb))
		return node1(N_STMTEXPR, translate_block(tc), tb);

	// new ClassName(args) -> a statement expression that allocates zeroed
	// storage, constructs in place, and yields the typed pointer:
	//   ({ struct C *__newN = (struct C*)calloc(1, sizeof(struct C));
	//      C__C(__newN, args); __newN; })
	// (No ctor call when the class has no user constructor — calloc already
	// zero-initializes, matching value-init of a POD.)
	if (TokenNEW *tn = dynamic_cast<TokenNEW *>(tb)) {
		// -------- Placement new: `new (addr) T(args)` --------
		// Construct at the given address, no allocation. The placement
		// address is already T* (e.g. &data[len] where data is T*), so the
		// element lvalue is *addr and the class __this / string-this is addr
		// directly. Yields the address (a statement expression), like g++.
		if (tn->placement) {
			// A node must not appear twice in the tree; the placement
			// address is a pure expression, so re-translate per use.
			auto addr = [&]() -> node_t { return translate_expr(tn->placement); };
			node_t construct = NULL;
			// std::string element: `new (slot) string(v)` in a
			// template-instantiated container (T == string). T resolves to
			// the std::string object class, so it lands in `alloc_class`
			// (DataDefSTRING IS-A DataDefCLASS) rather than `alloc_type`.
			// Route both through the string ctor path so the placement
			// copy/cstr/default ctor is the mangled libstdc++ symbol with a
			// declared extern — the alloc_class path below would emit the
			// copy ctor via referenced_funcs (no extern -> implicit decl ->
			// "incompatible argument type" check error).
			bool placement_string =
				(tn->alloc_type && tn->alloc_type->is_string())
				|| (tn->alloc_class && is_string_object(tn->alloc_class));
			if (placement_string) {
				// Reuse the string ctor selection: copy / cstr / default.
				node_t a = list();
				append(a, node2(N_CAST, void_ptr_type(), addr(), tb));
				const char *sym;
				if (!tn->ctor_args.empty()
				    && is_string_object_value(tn->ctor_args[0])) {
					append(a, string_obj_arg(tn->ctor_args[0]));
					sym = STR_CTOR_CP;
					need_output_extern(sym, false,
						{ { {N_VOID}, true }, { {N_VOID}, true } });
				} else if (!tn->ctor_args.empty()) {
					append(a, translate_expr(tn->ctor_args[0]));
					append(a, node2(N_CAST, void_ptr_type(), addr(), tb)); // dummy alloc&
					sym = STR_CTOR_S;
					need_output_extern(sym, false,
						{ { {N_VOID}, true }, { {N_CHAR}, true }, { {N_VOID}, true } });
				} else {
					sym = STR_CTOR0;
					need_output_extern(sym, false, { { {N_VOID}, true } });
				}
				construct = node2(N_CALL, id(sym, tb), a, tb);
			} else if (tn->alloc_class) {
				DataDefCLASS *pc = tn->alloc_class;
				if (pc->has_user_ctor) {
					FuncDef *ctor = select_ctor_overload(pc, tn->ctor_args);
					if (!ctor) {
						auto it = pc->method_map.find(pc->name);
						if (it != pc->method_map.end() && it->second)
							ctor = dynamic_cast<FuncDef *>(it->second->type);
					}
					std::string csym = ctor_call_symbol(pc, ctor);
					referenced_funcs.insert(csym);
					node_t a = list();
					append(a, addr());   // __this (already Class*)
					for (size_t i = 0; i < tn->ctor_args.size(); i++) {
						TokenBase *arg = tn->ctor_args[i];
						size_t pi = i + 1;
						DataDef *pt = (ctor && pi < ctor->parameters.size())
								? ctor->parameters[pi] : NULL;
						bool refp = ctor && pi < ctor->ref_params.size()
							    && ctor->ref_params[pi];
						if (is_std_string(pt))
							append(a, string_obj_arg(arg));
						else if (refp)
							append(a, node1(N_ADDR, translate_expr(arg), arg));
						else
							append(a, translate_expr(arg));
					}
					if (ctor && ctor->ctor_trailing_self)
						append(a, addr());   // trailing allocator& == &this
					construct = node2(N_CALL, id(csym.c_str(), tb), a, tb);
				}
			} else {
				// Scalar T: *addr = arg (or zero-init when no args).
				node_t rhs = tn->ctor_args.empty()
						? integer(0, tb) : translate_expr(tn->ctor_args[0]);
				construct = node2(N_ASSIGN, node1(N_DEREF, addr(), tb), rhs, tb);
			}
			// ({ <construct>; addr; }) — yields the placement address (T*).
			node_t items = list();
			if (construct)
				append(items, node2(N_EXPR, list(), construct, tb));
			append(items, node2(N_EXPR, list(), addr(), tb));
			return node1(N_STMTEXPR, node2(N_BLOCK, list(), items, tb), tb);
		}

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
			FuncDef *ctor = select_ctor_overload(cdd, tn->ctor_args);
			if (!ctor) {
				auto it = cdd->method_map.find(cdd->name);
				if (it != cdd->method_map.end() && it->second)
					ctor = dynamic_cast<FuncDef *>(it->second->type);
			}
			std::string csym = ctor_call_symbol(cdd, ctor);
			referenced_funcs.insert(csym);
			node_t cargs = list();
			append(cargs, id(tmp, tb));
			for (size_t i = 0; i < tn->ctor_args.size(); i++) {
				TokenBase *arg = tn->ctor_args[i];
				size_t pi = i + 1;
				DataDef *pt = (ctor && pi < ctor->parameters.size())
						? ctor->parameters[pi] : NULL;
				bool refp = ctor && pi < ctor->ref_params.size()
					    && ctor->ref_params[pi];
				if (is_std_string(pt))
					append(cargs, string_obj_arg(arg));
				else if (refp)
					append(cargs, node1(N_ADDR, translate_expr(arg), arg));
				else
					append(cargs, translate_expr(arg));
			}
			if (ctor && ctor->ctor_trailing_self)
				append(cargs, id(tmp, tb));   // trailing allocator& == this
			node_t ccall = node2(N_CALL, id(csym.c_str(), tb), cargs, tb);
			append(items, node2(N_EXPR, list(), ccall, tb));
		} else if (cdd->has_vtable) {
			// No user constructor, but a virtual (polymorphic) class: calloc
			// zeroes the storage, leaving __vptr NULL — a virtual call would
			// deref NULL and crash. A user ctor's body sets __vptr (func_def
			// prologue); with no ctor we must set it here so `new B()` yields a
			// usable polymorphic object. `__newN->__vptr = (void*)B__vtable`.
			std::string vname = cdd->name + "__vtable";
			for (size_t g = 0; g < cdd->vtable_groups.size(); g++) {
				const auto &G = cdd->vtable_groups[g];
				std::string fld = (G.this_offset == 0)
					? "__vptr" : ("__vptr_" + std::to_string(G.this_offset));
				node_t vptr_lhs = node2(N_DEREF_FIELD, id(tmp, tb),
							id(fld.c_str(), tb));
				node_t vptr_type = node2(N_TYPE,
					node1(N_LIST, simple(N_VOID)),
					node2(N_DECL, ignore(), node1(N_LIST, pointer())));
				node_t tab = id(vname.c_str(), tb);
				node_t ap = (G.addr_point == 0) ? tab
					: node2(N_ADD, tab, integer((long)G.addr_point), tb);
				node_t vtab = node2(N_CAST, vptr_type, ap, tb);
				node_t asn = node2(N_ASSIGN, vptr_lhs, vtab, tb);
				CIR_NODE(asn)->synth_from_origin = true;
				append(items, node2(N_EXPR, list(), asn, tb));
			}
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
		DataDefCLASS *cdd = tdl->del_class;
		// Virtual destructor: dispatch through the vtable D0 (deleting) slot,
		// which runs the most-derived complete destructor AND frees. No
		// separate free() — the D0 dtor owns the free.
		if (cdd && cdd->vtable_slot("~$deleting") >= 0) {
			size_t grp; int slot;
			cdd->find_vslot("~$deleting", grp, slot);
			const DataDefCLASS::VtableGroup &G = cdd->vtable_groups[grp];
			std::string vfld = (G.this_offset == 0)
				? "__vptr" : ("__vptr_" + std::to_string(G.this_offset));
			// ptr->__vptr — load the owning group's vptr field by name from
			// the static-type pointer (already `struct Cls *`, no recast).
			node_t vptr = node2(N_DEREF_FIELD, translate_expr(tdl->expr),
					    id(vfld.c_str(), tb));
			// (void**)vptr[slot]
			node_t vpp_dl = list();
			append(vpp_dl, pointer());
			append(vpp_dl, pointer());
			node_t vpp_type = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
						node2(N_DECL, ignore(), vpp_dl));
			node_t vtab = node2(N_CAST, vpp_type, vptr, tb);
			node_t slotref = node2(N_IND, vtab, integer(slot, tb), tb);
			// Cast the slot to void(*)(void*) — the deleting-dtor signature.
			// Suffix order matches fnptr_decl_pieces: POINTER first (the
			// `(*)`), then FUNC (the `(void*)` params) — i.e. pointer-to-
			// function, not function-returning-pointer.
			node_t fp_dl = list();
			append(fp_dl, pointer());
			append(fp_dl, node1(N_FUNC,
				node1(N_LIST,
				      node2(N_TYPE,
					    node1(N_LIST, simple(N_VOID)),
					    node2(N_DECL, ignore(),
						  node1(N_LIST, pointer()))))));
			node_t fp_type = node2(N_TYPE, node1(N_LIST, simple(N_VOID)),
					       node2(N_DECL, ignore(), fp_dl));
			node_t fn = node2(N_CAST, fp_type, slotref, tb);
			node_t darg = list();
			append(darg, translate_expr(tdl->expr));
			node_t dcall = node2(N_CALL, fn, darg, tb);
			node_t items = list();
			append(items, node2(N_EXPR, list(), dcall, tb));
			append(items, node2(N_EXPR, list(), integer(0, tb), tb));
			node_t block = node2(N_BLOCK, list(), items, tb);
			return node1(N_STMTEXPR, block, tb);
		}
		node_t ptr = translate_expr(tdl->expr);
		node_t items = list();
		if (cdd && cdd->has_user_dtor) {
			std::string dsym = class_complete_dtor_symbol(cdd);
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
			// Constant-fold a read of an integer constant to its value —
			// but ONLY for a constant whose value was genuinely STORED into
			// its own data buffer (enum values, set via set()). A plain
			// `const`-DECLARED variable carries vfCONSTDECL: it has either no
			// data (a local) or an allocated-but-uninitialized buffer (a
			// global, whose initializer runs as an ordinary store), so folding
			// it would read 0 — it must read as the variable instead. (P2.4
			// added vfCONSTANT to const-declared scalars for WRITE enforcement;
			// excluding vfCONSTDECL here keeps the READ fold limited to the
			// set()-valued constants it always handled correctly.)
			if (tv->var.is_constant() && tv->var.data
			    && !(tv->var.flags & vfCONSTDECL) && tv->var.type
			    && tv->var.type->is_integer() && !tv->var.type->is_pointer())
				return integer(tv->var.get<int64_t>(), tb);
			if (tv->var.name.compare(0, 11, "__literal__") == 0) {
				const std::string &content = tv->var.name.substr(11);
				return str(content.c_str(), content.size() + 1, tb);
			}
			// Value-use of a GNU nested function's in-scope alias (taking its
			// address / passing it as a callback) names the HOISTED symbol. Only
			// for a capture-free nested fn: a capturing one cannot be a plain
			// function pointer (no slot for the captures — GCC uses a trampoline),
			// so leaving the short name there yields a clean c2mir error rather
			// than a callback that reads garbage captures.
			if (FuncDef *nfd = dynamic_cast<FuncDef *>(tv->var.type)) {
				if (!nfd->nested_emit_name.empty()
				    && nfd->captured_vars.empty()) {
					referenced_funcs.insert(nfd->nested_emit_name);
					return id(nfd->nested_emit_name.c_str(), tb);
				}
				// A top-level function used as a VALUE (address-taken /
				// function-pointer decay: `&abort`, `f = exit`, `{abort}`).
				// Record it in referenced_funcs so translate_module emits its
				// prototype — without it c2mir sees an "undeclared identifier"
				// (call sites already record callees; a bare value-use must too).
				if (nfd->nested_emit_name.empty()) {
					std::string sym = var_emit_name(tv->var);
					referenced_funcs.insert(sym);
					return id(sym.c_str(), tb);
				}
			}
			// Record file-scope (non-local, non-param) references so
			// translate_module can emit extern decls for libc globals
			// (stderr/stdout/stdin) that were registered lazily and never
			// reached top_decls. Locals/params already have in-scope decls.
			if (!(tv->var.flags & vfLOCAL) && !(tv->var.flags & vfPARAM)) {
				// An aliased global (`b __attribute__((alias("a")))`) emits
				// the TARGET's symbol, so record the target under its own name
				// — recording `b` would re-introduce the undefined import.
				std::string en = var_emit_name(tv->var);
				if (en == tv->var.name)
					referenced_globals[tv->var.name] = &tv->var;
			}
			// A numeric reference parameter (`int &x`) is lowered to a
			// pointer parameter (`int *x`) by the parser, with vfREFERENCE
			// set for auto-deref. Every value use of the reference reads
			// through the pointer: `x` -> `(*x)`. (String references are a
			// separate object-pointer path handled elsewhere.)
			if ((tv->var.flags & vfREFERENCE) && tv->var.type
			    && tv->var.type->is_pointer())
				return node1(N_DEREF, id(tv->var.name.c_str(), tb), tb);
			// GNU nested-function / [&]-lambda capture: an enclosing local/param
			// the body reads is captured by reference, lowered to a hidden
			// pointer parameter `T *name`. The value read is `(*name)`.
			if (note_capture(&tv->var))
				return node1(N_DEREF, id(tv->var.name.c_str(), tb), tb);
			return id(var_emit_name(tv->var).c_str(), tb);
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
		if (ta) {
			// `&capturedvar` inside a nested fn IS the capture pointer param.
			if (note_capture(&ta->var))
				return id(ta->var.name.c_str(), tb);
			// `&ref` where ref is a T& parameter (lowered to a T* with
			// vfREFERENCE): the variable's stored VALUE is already the
			// referent's address, so &ref is just that value — NOT
			// N_ADDR(slot) (which would yield a T**). (C++ reference semantics.)
			if (ta->var.flags & vfREFERENCE)
				return id(var_emit_name(ta->var).c_str(), tb);
			return node1(N_ADDR, id(var_emit_name(ta->var).c_str(), tb), tb);
		}
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
		if (td) {
			// `*p` where p is a captured pointer var: the capture param is
			// `T **p` (&enclosing-p), so the enclosing p's value is `(*p)` and
			// the user deref is `(*(*p))`.
			if (note_capture(&td->var))
				return node1(N_DEREF,
					node1(N_DEREF, id(td->var.name.c_str(), tb), tb), tb);
			return node1(N_DEREF, id(td->var.name.c_str(), tb), tb);
		}
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
			// User-defined `operator[]` on a class object (`v[i]`): dispatch
			// to the method. It returns T& (a deref'd address), so the result
			// is a lvalue usable for both reads and `v[i] = x` (the generic
			// N_ASSIGN path stores through the deref). Checked before the STL
			// container and raw-array paths.
			node_t ocall = class_subscript_call(tsub, tb);
			if (ocall) return ocall;
			// A subscript on a lifted string literal (`"X"[0]`) keeps the
			// synthetic `__literal__X` variable as its object. Emit the string
			// literal itself, not a reference to an undefined symbol.
			node_t base;
			if (tsub->object.name.compare(0, 11, "__literal__") == 0) {
				const std::string &content = tsub->object.name.substr(11);
				base = str(content.c_str(), content.size() + 1, tb);
			} else if (note_capture(&tsub->object)) {
				// Captured container: subscript through the deref of the
				// capture pointer param (`(*name)[i]`).
				base = node1(N_DEREF, id(tsub->object.name.c_str(), tb), tb);
			} else {
				base = id(var_emit_name(tsub->object).c_str(), tb);
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
		if (tse) {
			// Multi-dim VLA `M1[i0][i1]...[ik]` parses as a NESTED
			// TokenSubscriptExpr(...(TokenSubscript(M1,i0))...,ik) because M1 is
			// a flat malloc'd pointer (c2mir has no VLA types), so the nested
			// IND(IND(M1,i0),i1) fails (M1[i0] is a scalar). Unwind the chain to
			// (root M1, [i0..ik]) and emit ONE linearized subscript
			// M1[i0*d1*..*dk + i1*d2*..*dk + ... + ik] (row-major), with the
			// inner dim sizes d1..dk from M1's pointee DataDefCArray chain.
			std::vector<TokenBase *> idxs; // outermost-first: [ik .. i1]
			TokenBase *cur = tse;
			Variable *root = NULL;
			while (TokenSubscriptExpr *e =
				       dynamic_cast<TokenSubscriptExpr *>(cur)) {
				idxs.push_back(e->index);
				cur = e->base_expr;
			}
			if (TokenSubscript *ts0 = dynamic_cast<TokenSubscript *>(cur)) {
				if (ts0->extra_indices.empty()) {
					idxs.push_back(ts0->index); // i0 (innermost root index)
					root = &ts0->object;
				}
			}
			DataDefPTR *rp = root ? dynamic_cast<DataDefPTR *>(root->type) : NULL;
			DataDefCArray *rc = rp ? dynamic_cast<DataDefCArray *>(rp->base_type)
					       : NULL;
			if (root && rc && rc->has_runtime_size()) {
				std::vector<node_t> dims; // d1..dk (inner dims)
				for (DataDefCArray *c = rc; c;
				     c = dynamic_cast<DataDefCArray *>(c->element_type))
					dims.push_back(c->has_runtime_size()
						       ? translate_expr(c->count_expr)
						       : integer((int64_t)c->count));
				size_t ni = idxs.size();
				// Only flatten a FULL access (one index per dimension =
				// 1 outer + k inner). idxs is outermost-first, so idxs[ni-1]
				// is the root index i0.
				if (ni == dims.size() + 1) {
					node_t flat = translate_expr(idxs[ni - 1]); // i0
					for (size_t m = 1; m < ni; m++)
						flat = node2(N_ADD,
							     node2(N_MUL, flat, dims[m - 1], tb),
							     translate_expr(idxs[ni - 1 - m]), tb);
					node_t rbase = id(var_emit_name(*root).c_str(), tb);
					return node2(N_IND, rbase, flat, tb);
				}
			}
			return node2(N_IND,
				translate_expr(tse->base_expr),
				translate_expr(tse->index), tb);
		}
	}

	// Method call on a class-object receiver. A TokenCallMethod IS-A
	// TokenMember, so this must run before the generic member-access handler
	// below — otherwise `s.c_str()` mis-lowers to a bare field access
	// `s.c_str`. std::string methods route through class_method_call (their
	// FuncDef carries the mangled libstdc++ emit_symbol); each lowering helper
	// returns NULL for an unrecognized receiver so the others fall through.
	if (tb->type() == TokenType::ttCallMethod) {
		TokenMember *tcm = dynamic_cast<TokenMember *>(tb);
		if (tcm) {
			// Class method call (user class OR std::string via the
			// object-class bridge): c.method(args) -> the emit_symbol /
			// ClassName__method call. Returns NULL when the receiver is
			// not a class instance.
			node_t lowered = class_method_call(tcm, tb);
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
			else if (note_capture(&tm->object))
				// Captured struct object: `(*name).field` (the `.`/`->`
				// selection below is unchanged — `(*name)` is the struct lvalue).
				obj = node1(N_DEREF, id(tm->object.name.c_str(), tb), tb);
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
			// sizeof a VLA type (`typedef int c[i+2]; sizeof(c)`) is a
			// RUNTIME value -- C11 6.5.3.4p2 evaluates the operand. c2mir has
			// no VLA types, so compute it explicitly as
			// (dim0 * dim1 * ... ) * sizeof(scalar element). alignof stays the
			// element's compile-time alignment (constant), so only sizeof is
			// lowered this way.
			DataDefCArray *vca = dynamic_cast<DataDefCArray *>(ttq->query_type);
			if (vca && vca->has_runtime_size() && !ttq->want_alignof) {
				node_t total = translate_expr(vca->count_expr);
				DataDef *elem = vca->element_type;
				while (DataDefCArray *inner =
					       dynamic_cast<DataDefCArray *>(elem)) {
					node_t d = inner->has_runtime_size()
						   ? translate_expr(inner->count_expr)
						   : integer((int64_t)inner->count);
					total = node2(N_MUL, total, d, tb);
					elem = inner->element_type;
				}
				node_t elem_szof = node1(N_SIZEOF,
					node2(N_TYPE, type_list(elem),
					      node2(N_DECL, ignore(), list())), tb);
				return node2(N_MUL, total, elem_szof, tb);
			}
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

	// C99 compound literal: `(T){ init... }` as a value expression.
	if (TokenStructLit *slit = dynamic_cast<TokenStructLit *>(tb))
		return translate_struct_lit(slit);

	// Operators
	if (tb->is_operator()) {
		TokenOperator *top = dynamic_cast<TokenOperator *>(tb);

		// Inc/Dec
		if (tb->id() == TokenID::tkInc || tb->id() == TokenID::tkDec) {
			bool is_post = (top->left != NULL);
			TokenBase *operand_tb = is_post ? top->left : top->right;
			// const enforcement (P2.4): ++/-- writes its operand. A direct
			// const variable operand is an error (g++: "increment of read-only
			// variable 'x'"). Checked before the class-operator dispatch so a
			// class operator++ on a const object is rejected too. Scoped to the
			// direct const-var case (see the binary const check).
			if (TokenVar *itv = dynamic_cast<TokenVar *>(operand_tb)) {
				if (itv->var.is_constant()) {
					std::string msg = std::string(
						tb->id() == TokenID::tkInc ? "increment"
									   : "decrement")
						+ " of read-only variable '"
						+ itv->var.name + "'";
					return error_node(msg.c_str(), tb);
				}
			}
			// Class operator++/operator-- dispatch (declared-only; falls
			// through to the built-in inc/dec when the class has no such
			// operator). C++ distinguishes prefix `operator++()` from postfix
			// `operator++(int)` (P2.1b gap 3): a postfix use (`c++`) prefers the
			// PARAMETERIZED overload, called with a dummy `0`, whose body returns
			// the OLD value. A prefix use (`++c`) routes to the nullary overload.
			// When only one form is declared, fall back to it.
			const char *uop = (tb->id() == TokenID::tkInc) ? "++" : "--";
			std::string opmname = std::string("operator") + uop;
			DataDefCLASS *icls = as_class_instance(operand_tb->datadef());
			if (is_post && icls) {
				// Prefer the parameterized (postfix) overload: scan the mangled
				// family for a params>1 (int-taking) operator++/--.
				std::string mc = icls->name + "__" + opmname;
				std::string mu = mc + "_un";
				FuncDef *post = NULL;
				for (Variable *mv : icls->methods) {
					if (!mv || (mv->name != mc && mv->name != mu)) continue;
					FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
					if (fd && fd->parameters.size() > 1) { post = fd; break; }
				}
				if (post) {
					node_t this_arg;
					if (TokenVar *otv = dynamic_cast<TokenVar *>(operand_tb))
						this_arg = object_var_addr(otv->var, tb);
					else
						this_arg = node1(N_ADDR, translate_expr(operand_tb), tb);
					std::string sym = !post->class_emit_name.empty()
							  ? post->class_emit_name
							  : (icls->name + "__" + opmname);
					referenced_funcs.insert(sym);
					node_t pargs = list();
					append(pargs, this_arg);
					append(pargs, integer(0, tb));   // dummy int (postfix marker)
					node_t pcall = node2(N_CALL, id(sym.c_str(), tb), pargs, tb);
					CIR_NODE(pcall)->synth_from_origin = true;
					if (post->returns_ref)
						return node1(N_DEREF, pcall, tb);
					return pcall;
				}
				// No postfix overload: fall through to the nullary form below.
			}
			if (node_t ov = class_unary_operator_call(uop, operand_tb, tb))
				return ov;
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
			// Class unary-operator dispatch (declared-only): `-c`/`!c`/`~c`
			// on a class object whose class declares the matching nullary
			// operator. NULL -> fall through to the built-in lowering, so a
			// built-in `-x`/`!x`/`~x` and any class lacking the operator keep
			// normal semantics (the invariant: overload only when declared).
			const char *uop = NULL;
			switch (tb->id()) {
			case TokenID::tkNeg:  uop = "-"; break;
			case TokenID::tkLnot: uop = "!"; break;
			case TokenID::tkBnot: uop = "~"; break;
			default: break;
			}
			if (uop) {
				if (node_t ov = class_unary_operator_call(uop, top->right, tb))
					return ov;
			}
			node_t operand = translate_expr(top->right);
			if (tb->id() == TokenID::tkNeg)
				// Unary negation: c2mir represents `-x` as a SINGLE-operand
				// N_SUB (grammar: `N_SUB (expr)`), lowered with float UNOP
				// semantics. Emitting `0 - x` instead computes `+0.0` for
				// `-0.0` — the IEEE sign bit is lost (signbit/copysign break).
				return node1(N_SUB, operand, tb);
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
			// const enforcement (P2.4): writing through a const lvalue is an
			// error in every C/C++ dialect. Scoped to the DIRECT case — the LHS
			// is a `const`-declared variable (TokenVar with is_constant()) and
			// the operator writes it (= or a compound assign). Deep/transitive
			// const (members, `const T*`/`T* const`, const params/methods) is a
			// larger effort and intentionally NOT enforced here. gcc canon:
			// "assignment of read-only variable 'x'".
			if (is_assign_op(tb->id())) {
				if (TokenVar *ltv = dynamic_cast<TokenVar *>(top->left)) {
					if (ltv->var.is_constant()) {
						std::string msg = "assignment of read-only variable '"
							+ ltv->var.name + "'";
						return error_node(msg.c_str(), tb);
					}
				}
			}

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
				// `ss << a << b` on a stringstream OBJECT: lower to streamout_*
				// calls on the adjusted ostream*. (cin/>> into a stringstream is
				// not yet handled — falls through.)
				if (is_out) {
					TokenVar *sv = dynamic_cast<TokenVar *>(leaf);
					if (sv && is_sstream_object(sv->var.type))
						return translate_sstream_chain(top, sv->var.name.c_str());
				}
			}

			// Operator overloading on a class lvalue (user class OR
			// std::string via the object-class bridge): `c <op> rhs` where
			// c's class defines `operator<op>` lowers to the operator call.
			// For std::string this covers `s = "x"`/`s = t`/`s += ...` ->
			// the mangled libstdc++ basic_string::operator=/operator+=
			// (c2mir cannot assign through the long[] buffer, and g++ emits
			// these operator calls). The overload is selected by RHS type.
			// The legacy std::string assign interception was deleted in A4b
			// Surface 3 — class_operator_call now owns it.
			{
				node_t ov = class_operator_call(top, tb);
				if (ov) return ov;
			}

			// Implicit memberwise copy-ASSIGNMENT for a NON-TRIVIAL class
			// (`lhs = rhs`, both the same class needing a dtor, declaring no
			// user operator= — class_operator_call returned NULL above, so a
			// user-declared operator= still wins). A bit-copy N_ASSIGN would
			// alias the object members' heap buffers and double-free at
			// cleanup; assign each member instead (string -> string_assign
			// free-old+copy, scalar -> plain =). Trivial classes fall through
			// to the native bit-copy. The statement-expr yields the lhs object.
			if (tb->id() == TokenID::tkAssign) {
				DataDefCLASS *lc = as_class_instance(top->left->datadef());
				DataDefCLASS *rc = as_class_instance(top->right->datadef());
				if (lc && lc == rc
				    && class_return_via_retbuf(top->left->datadef())) {
					DataDefCLASS *callc =
						object_returning_call_class(top->right);
					if (callc == lc) {
						// rhs is a by-value-returning CALL: materialize its
						// result into a function-scope cleanup-tagged temp
						// (object_call_temp_addr -> m_pending_stmts, where
						// cleanup scoping is correct — NOT inside a statement-
						// expression), then memberwise-assign lhs from the temp.
						// All emitted as pending statements; the expression value
						// is the lhs object. (A stmt-expr-local cleanup temp
						// mis-scopes its dtor in c2mir, so keep it block-scoped.)
						node_t taddr = object_call_temp_addr(top->right, lc, tb);
						std::vector<node_t> stmts;
						class_copy_assign_from_addr(lc, top->left, taddr,
									    stmts, tb);
						for (node_t s : stmts)
							m_pending_stmts.push_back(s);
						return translate_expr(top->left);
					}
					// rhs is an object LVALUE: memberwise copy-assign inline.
					std::vector<node_t> stmts;
					class_copy_assign(lc, top->left, top->right, stmts, tb);
					node_t items = list();
					for (node_t p : m_pending_stmts)
						append(items, p);
					m_pending_stmts.clear();
					for (node_t s : stmts)
						append(items, s);
					append(items, node2(N_EXPR, list(),
							    translate_expr(top->left), tb));
					node_t blk = node2(N_BLOCK, list(), items, tb);
					return node1(N_STMTEXPR, blk, tb);
				}
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
			// Derived->base pointer reassignment (`A *a; B *b; a = b;`):
			// make the implicit upcast explicit so c2mir does not warn.
			if (code == N_ASSIGN)
				right = upcast_class_ptr(right, top->left->datadef(),
							 top->right, tb);
			// GNU void*/func-ptr/incomplete-ptr arithmetic (element size 1).
			// c2mir rejects it, so cast the size-1 pointer operand(s) to
			// `char *` — the exact size-1 semantics GCC uses — before the op.
			if (code == N_ADD || code == N_SUB) {
				DataDef *ldd = top->left  ? top->left->datadef()  : NULL;
				DataDef *rdd = top->right ? top->right->datadef() : NULL;
				if (is_size1_pointer(ldd))
					left = node2(N_CAST, char_ptr_type(), left, tb);
				if (is_size1_pointer(rdd))
					right = node2(N_CAST, char_ptr_type(), right, tb);
			}
			// Compound `vp += n` / `vp -= n` on a size-1 pointer lvalue:
			// the lvalue cannot be cast in place, so lower to
			// `vp = (T*)((char*)vp <op> n)` — same size-1 semantics, and an
			// assignment c2mir accepts.
			if (code == N_ADD_ASSIGN || code == N_SUB_ASSIGN) {
				DataDef *ldd = top->left ? top->left->datadef() : NULL;
				if (is_size1_pointer(ldd)) {
					c2mir_node_code_t bin =
						(code == N_ADD_ASSIGN) ? N_ADD : N_SUB;
					node_t lhs2 = translate_expr(top->left);
					node_t arith = node2(bin,
						node2(N_CAST, char_ptr_type(), lhs2, tb),
						right, tb);
					node_t back = node2(N_CAST,
						ptr_type_node(ldd), arith, tb);
					return node2(N_ASSIGN, left, back, tb);
				}
			}
			return node2(code, left, right, tb);
		}
	}

	// Function call
	if (tb->type() == TokenType::ttCallFunc) {
		TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(tb);
		if (tcf) {
			// __madc_{add,sub,mul}_overflow[_p]: choose the width/signedness-
			// specific helper from the destination operand's type (the lexer
			// only emitted the long-width generic, which mis-detects overflow
			// and mis-truncates for narrow / signedness-differing destinations).
			if (tcf->var.name.compare(0, 7, "__madc_") == 0
			    && tcf->var.name.find("_overflow") != std::string::npos
			    && tcf->parameters.size() == 3) {
				// Both operands unsigned? (selects the _uu64 helper
				// for 64-bit unsigned overflow — see overflow_helper_name).
				DataDef *o0 = tcf->parameters[0] ? tcf->parameters[0]->datadef() : NULL;
				DataDef *o1 = tcf->parameters[1] ? tcf->parameters[1]->datadef() : NULL;
				bool ops_unsigned = o0 && o1
					&& o0->is_unsigned() && o1->is_unsigned();
				std::string sel = overflow_helper_name(
					tcf->var.name, tcf->parameters[2], ops_unsigned);
				if (sel != tcf->var.name) {
					referenced_funcs.insert(sel);
					node_t args = list();
					build_call_args(tcf, args);
					return node2(N_CALL, id(sel.c_str(), tb), args, tb);
				}
			}
			// __builtin_conj{,f,l}(z): complex conjugate. c2mir lowers `~z`
			// (N_BITWISE_NOT on a complex operand) to (re, -im) and that is the
			// builtin's entire meaning. Lower here (Tier-1 — madc owns the front
			// end; lowering-vs-raising.md) rather than raising the fork: stock
			// c2mir has no __builtin_conj* and would fall through to a nonexistent
			// dlsym symbol ("can not load symbol __builtin_conjf"). The operand is
			// a complex value by the builtin's contract, so c2mir conjugates it.
			if ((tcf->var.name == "__builtin_conjf"
			     || tcf->var.name == "__builtin_conj"
			     || tcf->var.name == "__builtin_conjl")
			    && tcf->parameters.size() == 1) {
				return node1(N_BITWISE_NOT,
					     translate_expr(tcf->parameters[0]), tb);
			}
			// __destroy(ptr): compiler intrinsic — destruct the pointed-to
			// object element. Lowers to the ELEMENT TYPE's class destructor
			// (mangled ~basic_string for std::string, Cls___dtor for a user
			// class with a dtor), or to NOTHING for a scalar/pointer element
			// type (no dtor). Generic and element-type-driven — never string-
			// special-cased — so the std:: container headers (<vector>/<map>/
			// <set>) can destruct live elements before free() for ANY T while
			// the same template body is a no-op for vector<int>. The argument
			// keeps its original (uncoerced) type, so `data + i` reports the
			// real `T*`; the pointed-to T is base_type of that DataDefPTR.
			if (tcf->var.name == "__destroy" && tcf->parameters.size() == 1) {
				TokenBase *parg = tcf->parameters[0];
				DataDef *argdd = parg ? parg->datadef() : NULL;
				DataDef *elem = NULL;
				if (DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(argdd))
					elem = pdd->base_type;
				DataDefCLASS *ecls = as_class_instance(elem);
				// Scalar / pointer / dtor-less element: emit nothing.
				// A bare `0` keeps the expression-statement valid and
				// is discarded (the call is evaluated for effect only).
				if (!ecls || !class_needs_dtor(ecls))
					return integer(0, tb);
				// dtor_sym((void*)(ptr)) — the dtor takes only `this`,
				// matching its `void (*)(T*)` shape (delete uses the same).
				std::string dsym = class_complete_dtor_symbol(ecls);
				referenced_funcs.insert(dsym);
				need_output_extern(dsym.c_str(), false,
						   { { {N_VOID}, true } });
				node_t a = list();
				append(a, node2(N_CAST, void_ptr_type(),
						translate_expr(parg), tb));
				return node2(N_CALL, id(dsym.c_str(), tb), a, tb);
			}
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
				// A call to a GNU nested function resolves the in-scope
				// source-named alias, but the function is HOISTED to a unique
				// top-level symbol — emit that hoisted name (FuncDef::
				// nested_emit_name), not the source name.
				FuncDef *cdf = call_target_funcdef(tcf);
				const char *callee_name = (cdf && !cdf->nested_emit_name.empty())
					? cdf->nested_emit_name.c_str()
					: tcf->var.name.c_str();
				// c2mir intrinsics (e.g. __builtin_va_start) are
				// recognized by name and lowered in-place; emitting an
				// extern prototype for one shadows the intrinsic and
				// turns it into an undefined external symbol at MIR-link.
				// So skip the proto for them. (__builtin_va_arg has its
				// own translation path above.)
				if (tcf->var.name.compare(0, 13, "__builtin_va_") != 0)
					referenced_funcs.insert(callee_name);
				func_id = id(callee_name, tb);
			}
			// A by-value string-returning call uses the __retbuf ABI: it is
			// lowered to a void call writing into a caller temp, NOT an N_CALL
			// expression. Materialize the temp here and yield its dereffed
			// object value (`*(struct string*)&temp`) as the call's result.
			if (is_string_returning_call(tcf)) {
				node_t addr = string_call_temp_addr(tcf, tb);
				// The temp's value as a `struct string` lvalue: cast (void*)addr
				// back to `struct string *` and deref. (Most contexts then take
				// its address again — string_obj_arg etc. — but a few read the
				// object value directly.)
				node_t sp_type = node2(N_TYPE,
					node1(N_LIST, node2(N_STRUCT, id("string", tb), ignore())),
					node2(N_DECL, ignore(), node1(N_LIST, pointer())));
				node_t as_sp = node2(N_CAST, sp_type, addr, tb);
				return node1(N_DEREF, as_sp, tb);
			}
			// A by-value NON-TRIVIAL class-returning call uses the same __retbuf
			// ABI: materialize a cleanup-tagged temp of the class, emit the void
			// call writing into it, and yield the temp as a `struct Cls` lvalue
			// (`*(struct Cls*)&temp`) — so member access / further use reads it.
			if (DataDefCLASS *ocls = object_returning_call_class(tcf)) {
				node_t addr = object_call_temp_addr(tcf, ocls, tb);
				node_t cp_type = node2(N_TYPE,
					node1(N_LIST, node2(N_STRUCT,
						id(ocls->name.c_str(), tb), ignore())),
					node2(N_DECL, ignore(), node1(N_LIST, pointer())));
				node_t as_cp = node2(N_CAST, cp_type, addr, tb);
				return node1(N_DEREF, as_cp, tb);
			}
			node_t args = list();
			build_call_args(tcf, args);
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

	// __real__ / __imag__ <expr>  ->  c2mir N_REALPART / N_IMAGPART (native
	// complex support in the madc MIR fork). Yields the scalar component and is
	// an lvalue when the operand is, so `&(__real dc)` and `__real__ x = v` work.
	if (TokenComplexPart *tcp = dynamic_cast<TokenComplexPart *>(tb))
		return node1(tcp->imag_part ? N_IMAGPART : N_REALPART,
			     translate_expr(tcp->expr), tb);

	// GNU label address `&&label` -> N_LABEL_ADDR(N_ID) — a void* usable as a
	// computed-goto target (see the N_INDIRECT_GOTO statement form).
	if (TokenLabelAddr *tla = dynamic_cast<TokenLabelAddr *>(tb))
		return node1(N_LABEL_ADDR, id(tla->name.c_str(), tb), tb);

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
	// Multi-return (`return a, b, ...;`): store each value into __retbuf[i]
	// (the hidden `long *__retbuf` first param), then `return;` (void). The
	// function was lowered to the multi-return __retbuf ABI by func_def/func_proto.
	if (m_cur_func_multi_return && !tr->return_exprs.empty()) {
		node_t items = list();
		for (size_t i = 0; i < tr->return_exprs.size(); i++) {
			node_t val = translate_expr(tr->return_exprs[i]);
			// flush any temps a value expression materialized
			for (node_t p : m_pending_stmts) append(items, p);
			m_pending_stmts.clear();
			node_t slot = node2(N_IND, id(RETBUF_NAME, tr),
					    integer((int64_t)i, tr), tr);
			append(items, node2(N_EXPR, list(),
					    node2(N_ASSIGN, slot, val, tr), tr));
		}
		append(items, node2(N_RETURN, list(), ignore(), tr));
		return node2(N_BLOCK, list(), items, tr);
	}
	// A by-value std::string return uses the __retbuf ABI: copy-construct the
	// returned object into *__retbuf, then `return;`. The returned local keeps
	// its own cleanup dtor (its lifetime ends at scope exit as usual); because we
	// COPY into __retbuf rather than bit-copying, destructing the local is safe
	// (no double-free of a shared heap buffer). Matches g++'s class-return ABI.
	if (m_cur_func_returns_string && tr->returns) {
		node_t items = list();
		// basic_string(const string&): C1ERKS4_(__retbuf, &returned)
		need_output_extern(STR_CTOR_CP, false,
				   { { {N_VOID}, true }, { {N_VOID}, true } });
		node_t args = list();
		append(args, id(RETBUF_NAME, tr));            // struct string *__retbuf
		append(args, string_obj_arg(tr->returns));    // const string& (by address)
		node_t cc = node2(N_CALL, id(STR_CTOR_CP, tr), args, tr);
		CIR_NODE(cc)->synth_from_origin = true;
		// A ctor arg may have materialized a temp (string-returning sub-call):
		// emit those before the copy-construct that references them.
		for (node_t p : m_pending_stmts)
			append(items, p);
		m_pending_stmts.clear();
		append(items, node2(N_EXPR, list(), cc, tr));
		append(items, node2(N_RETURN, list(), ignore(), tr));
		return node2(N_BLOCK, list(), items, tr);
	}
	// A by-value NON-TRIVIAL class return uses the same __retbuf ABI: deep-copy
	// the returned object into *__retbuf (bit-copy scalars + member copy-construct
	// object members), then `return;`. The returned local keeps its own cleanup
	// dtor; because object members are copy-constructed (not bit-aliased), the
	// caller's slot owns its own buffers and there is no double-free.
	if (m_cur_func_returns_object && tr->returns) {
		// Build the deep-copy statements into a scratch vector first (this calls
		// translate_expr on the return operand, which may queue sub-call temps in
		// m_pending_stmts). Then flush those temps, then the copy statements.
		std::vector<node_t> copy_stmts;
		class_copy_construct_into_retbuf(m_cur_func_returns_object,
						 tr->returns, copy_stmts, tr);
		node_t items = list();
		for (node_t p : m_pending_stmts)
			append(items, p);
		m_pending_stmts.clear();
		for (node_t s : copy_stmts)
			append(items, s);
		append(items, node2(N_RETURN, list(), ignore(), tr));
		return node2(N_BLOCK, list(), items, tr);
	}
	node_t expr = tr->returns ? translate_expr(tr->returns) : ignore();
	// A bare `return;` in a non-void function: gcc (gnu89/c11) warns but accepts
	// it, returning an indeterminate value. c2mir requires a value, so emit a
	// typed zero of the function's scalar C return type — a conformant lowering
	// (the value is indeterminate anyway). This matches gcc's lenient handling
	// of implicit-int K&R functions and `-Wreturn-type` non-void fall-through.
	if (!tr->returns && m_cur_func_scalar_ret) {
		DataDef *rdd = m_cur_func_scalar_ret;
		if (rdd->is_pointer())
			expr = node2(N_CAST, type_list(rdd), integer(0), tr);
		else
			// integer 0 implicitly converts to the scalar return type
			// (int/long/float/double) — the value is indeterminate anyway.
			expr = integer(0);
	}
	// A T&-returning function returns the ADDRESS of its (lvalue) result —
	// `return x;` becomes `return &x;`. g++ does exactly this; the call site
	// derefs. The expression must be an lvalue (the parser/g++ enforce that).
	if (m_cur_func_returns_ref && tr->returns)
		expr = node1(N_ADDR, expr, tr);
	// Derived->base pointer return (`A *f() { return bptr; }`): make the
	// implicit upcast explicit so c2mir does not warn.
	else if (m_cur_func_returns_class_ptr && tr->returns) {
		DataDefCLASS *base = m_cur_func_returns_class_ptr;
		DataDefCLASS *derived = expr_pointee_class(tr->returns);
		if (base && derived && base != derived
		    && derived->is_or_derives_from(base))
			expr = node2(N_CAST, class_ptr_type(base), expr, tr);
	}
	return node2(N_RETURN, list(), expr, tr);
}

node_t CirBuilder::translate_if(TokenIF *ti)
{
	// Compile-time-constant condition: fold and prune the dead branch, like
	// GCC. This is what makes `if (__builtin_constant_p(var)) link_error();`
	// work: constant_p folds to a literal 0, so the dead `then` branch — and
	// any reference to an undefined symbol inside it (link_error) — is never
	// translated. Neither c2mir nor MIR eliminates a dead `if (0)` branch, so
	// the undefined extern would otherwise fail at MIR-link. Restricted to
	// literal integer/char conditions so no side-effecting expression is
	// dropped; the taken branch is still emitted in full.
	if (ti->condition &&
	    (ti->condition->type() == TokenType::ttInteger ||
	     ti->condition->type() == TokenType::ttChar)) {
		bool taken = ti->condition->ival() != 0;
		TokenBase *branch = taken ? ti->statement : ti->elsestmt;
		return branch ? translate_stmt_required(branch) : ignore();
	}
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

// throw lowering: `throw <expr>` -> __madc_throw_int/double/cstr(expr) by the
// operand type; bare `throw;` -> __madc_rethrow(). The runtime sets the
// exception, unwinds the cleanup stack to the active try's mark, and
// longjmp's to the try's setjmp (or aborts if no try is active).
node_t CirBuilder::translate_throw(TokenTHROW *th)
{
	if (!th->throw_expr) {
		// bare `throw;` — rethrow the in-flight exception.
		need_output_extern("__madc_rethrow", false, {});
		node_t call = node2(N_CALL, id("__madc_rethrow", th), list(), th);
		CIR_NODE(call)->synth_from_origin = true;
		return node2(N_EXPR, list(), call, th);
	}
	DataDef *edd = th->throw_expr->datadef();
	DataType dt = edd ? edd->rawtype() : DataType::dtINT64;
	const char *sym;
	ExternParam ep;
	if (dt == DataType::dtDOUBLE || dt == DataType::dtFLOAT) {
		sym = "__madc_throw_double";
		ep = { {N_DOUBLE}, false };
	} else if ((edd && edd->is_pointer()) || is_std_string(edd)) {
		// const char* / string literal -> cstr throw. A std::string object
		// throws its c_str() (string_obj_arg materializes literals; a real
		// string object would need string_cstr — left as a follow-up, scalar/
		// literal cstr is the common case).
		sym = "__madc_throw_cstr";
		ep = { {N_CHAR}, true };
	} else {
		sym = "__madc_throw_int";
		ep = { {N_LONG}, false };
	}
	need_output_extern(sym, false, { ep });
	node_t args = list();
	append(args, translate_expr(th->throw_expr));
	node_t call = node2(N_CALL, id(sym, th), args, th);
	CIR_NODE(call)->synth_from_origin = true;
	return node2(N_EXPR, list(), call, th);
}

// Register a try-body object's destructor on the RUNTIME cleanup stack so it
// runs when an exception (longjmp) unwinds out of the try body — c2mir cleanup
// attributes do NOT fire on longjmp. The object keeps its cleanup attribute too;
// that handles the NORMAL scope-exit path (the try lowering discards these
// runtime entries on normal exit), so each dtor runs exactly once per path.
//
// Emits a SINGLE call with only immediate arguments — NO new stack locals in the
// try body:
//   __madc_cleanup_push_dtor((void*)Cls___dtor, (void*)&varname);
// The runtime heap-allocates the entry. Adding stack arrays/locals inside a try
// body perturbs the MIR JIT frame allocation and was observed to make the
// try-context overlap an object (2026-05-31) — passing only immediates avoids it.
// P1.1c.
void CirBuilder::emit_try_body_cleanup_push(const char *varname,
					    DataDefCLASS *cdd,
					    node_t items, TokenBase *origin)
{
	if (m_try_body_depth <= 0 || !cdd || !class_needs_dtor(cdd))
		return;
	std::string dtor_sym = class_complete_dtor_symbol(cdd);
	// A user-class dtor is a madc-EMITTED function `void Cls___dtor(struct Cls*)`;
	// referencing it (referenced_funcs) drives its emission and declaration with
	// the real signature — re-declaring it here as `void f(void*)` would conflict.
	// An externally-bound dtor (std::string's mangled libstdc++ ~basic_string,
	// emit_symbol set) has NO madc body, so it must be declared extern here so
	// `(void*)dtor_sym` is a well-typed function address (not "undeclared").
	bool dtor_is_external = (dtor_sym != cdd->name + "___dtor");
	if (dtor_is_external)
		need_output_extern(dtor_sym.c_str(), false, { { {N_VOID}, true } });
	referenced_funcs.insert(dtor_sym);

	// __madc_cleanup_push_dtor((void*)dtor_sym, (void*)&varname);
	need_output_extern("__madc_cleanup_push_dtor", false,
			   { { {N_VOID}, true }, { {N_VOID}, true } });
	node_t args = list();
	append(args, node2(N_CAST, void_ptr_type(),
			   id(dtor_sym.c_str(), origin), origin));
	append(args, node2(N_CAST, void_ptr_type(),
			   node1(N_ADDR, id(varname, origin), origin), origin));
	node_t call = node2(N_CALL, id("__madc_cleanup_push_dtor", origin), args, origin);
	CIR_NODE(call)->synth_from_origin = true;
	append(items, node2(N_EXPR, list(), call, origin));
}

// SJLJ try/catch lowering. Emits, as a block of statements (NOT a statement-
// expression — a cleanup-attribute temp inside `({...})` mis-scopes its dtor in
// c2mir, per the P0.5 finding):
//   long __try_ctx_N[W];   // opaque MadcTryContext storage, W from sizeof
//   if (setjmp(*(jmp_buf*)__madc_try_push(&__try_ctx_N)) == 0) {
//       <try body>; __madc_try_pop();
//   } else {
//       int/double/cstr __t dispatch: bind the catch var, run the handler,
//       __madc_exception_clear(); no matching catch -> __madc_rethrow();
//   }
node_t CirBuilder::translate_try(TokenTRY *tt)
{
	int n = m_try_ctx_counter++;
	char ctxname[40];
	snprintf(ctxname, sizeof(ctxname), "__try_ctx_%d", n);

	// Opaque, 8-aligned storage sized to MadcTryContext (jmp_buf + 2 ptrs). The
	// builder is C++ so it knows the real ABI size; round up to a long[] count.
	size_t ctx_words = (sizeof(jmp_buf) + 2 * sizeof(void *) + sizeof(long) - 1)
			   / sizeof(long);
	node_t ctx_spec = list();
	append(ctx_spec, simple(N_LONG, tt));
	node_t ctx_decl_list = list();
	append(ctx_decl_list, node3(N_ARR, ignore(), list(),
				    integer((long)ctx_words, tt)));
	node_t ctx_sd = simple(N_SPEC_DECL, tt);
	append(ctx_sd, node1(N_SHARE, ctx_spec));
	append(ctx_sd, node2(N_DECL, id(ctxname, tt), ctx_decl_list));
	append(ctx_sd, ignore());
	append(ctx_sd, ignore());
	append(ctx_sd, ignore());
	CIR_NODE(ctx_sd)->synth_from_origin = true;

	// __madc_try_push((void*)&__try_ctx_N) -> (void*)jbuf
	need_output_extern("__madc_try_push", true, { { {N_VOID}, true } });
	node_t push_args = list();
	append(push_args, node2(N_CAST, void_ptr_type(),
				node1(N_ADDR, id(ctxname, tt), tt), tt));
	node_t push_call = node2(N_CALL, id("__madc_try_push", tt), push_args, tt);
	// setjmp(*(jmp_buf*)push_call) — declare setjmp returning int; the arg is the
	// jbuf the runtime returned. c2mir takes the (void*) and treats it as the
	// jmp_buf the libc setjmp expects (jmp_buf is an array -> a pointer in C).
	need_output_extern("setjmp", false, { { {N_VOID}, true } }, { N_INT });
	node_t sj_args = list();
	append(sj_args, push_call);
	node_t sj_call = node2(N_CALL, id("setjmp", tt), sj_args, tt);
	node_t cond = node2(N_EQ, sj_call, integer(0, tt), tt);

	// THEN arm: capture the cleanup-stack mark (== ctx->cleanup_mark, set by
	// __madc_try_push), lower the try body (object ctors inside it push runtime
	// cleanup entries — see emit_try_body_cleanup_push), then on normal exit
	// DISCARD those entries (their dtors already ran via the cleanup attribute at
	// the body block's C scope exit) and __madc_try_pop(). On the exception path
	// the body block is abandoned by longjmp (cleanup attribute does NOT fire),
	// and __madc_throw_* has already unwound the runtime entries to this same mark
	// — so each try-body dtor runs exactly once on either path. P1.1c.
	char markname[40];
	snprintf(markname, sizeof(markname), "__cleanup_mark_%d", n);
	need_output_extern("__madc_cleanup_top", true, {});
	node_t mark_spec = list();
	append(mark_spec, simple(N_VOID, tt));
	node_t mark_decl_list = list();
	append(mark_decl_list, pointer());
	node_t mark_init = node2(N_CALL, id("__madc_cleanup_top", tt), list(), tt);
	node_t mark_sd = simple(N_SPEC_DECL, tt);
	append(mark_sd, node1(N_SHARE, mark_spec));
	append(mark_sd, node2(N_DECL, id(markname, tt), mark_decl_list));
	append(mark_sd, ignore());
	append(mark_sd, ignore());
	append(mark_sd, mark_init);
	CIR_NODE(mark_sd)->synth_from_origin = true;

	node_t then_items = list();
	append(then_items, mark_sd);
	m_try_body_depth++;
	node_t body = translate_stmt(tt->try_body);
	m_try_body_depth--;
	if (body) append(then_items, body);
	// __madc_cleanup_discard_to(__cleanup_mark_N) — pop the try-body runtime
	// entries WITHOUT calling dtors (the cleanup attribute ran them at scope exit).
	need_output_extern("__madc_cleanup_discard_to", false, { { {N_VOID}, true } });
	node_t disc_args = list();
	append(disc_args, id(markname, tt));
	node_t disc_call = node2(N_CALL, id("__madc_cleanup_discard_to", tt),
				 disc_args, tt);
	CIR_NODE(disc_call)->synth_from_origin = true;
	append(then_items, node2(N_EXPR, list(), disc_call, tt));
	need_output_extern("__madc_try_pop", false, {});
	node_t pop_call = node2(N_CALL, id("__madc_try_pop", tt), list(), tt);
	CIR_NODE(pop_call)->synth_from_origin = true;
	append(then_items, node2(N_EXPR, list(), pop_call, tt));
	node_t then_blk = node2(N_BLOCK, list(), then_items, tt);

	// ELSE arm: catch dispatch. __t = __madc_exception_type(); then a chain of
	// if/else by type tag, falling through to __madc_rethrow() if no catch
	// matches. Built inside-out so the rethrow is the innermost else.
	need_output_extern("__madc_exception_type", false, {}, { N_INT });
	need_output_extern("__madc_exception_clear", false, {});
	need_output_extern("__madc_rethrow", false, {});

	char tname[40];
	snprintf(tname, sizeof(tname), "__exc_t_%d", n);
	// int __exc_t_N = __madc_exception_type();
	node_t t_spec = list();
	append(t_spec, simple(N_INT, tt));
	node_t t_init = node2(N_CALL, id("__madc_exception_type", tt), list(), tt);
	node_t t_sd = simple(N_SPEC_DECL, tt);
	append(t_sd, node1(N_SHARE, t_spec));
	append(t_sd, node2(N_DECL, id(tname, tt), list()));
	append(t_sd, ignore());
	append(t_sd, ignore());
	append(t_sd, t_init);
	CIR_NODE(t_sd)->synth_from_origin = true;

	// No-match tail: __madc_rethrow();
	node_t rethrow_call = node2(N_CALL, id("__madc_rethrow", tt), list(), tt);
	CIR_NODE(rethrow_call)->synth_from_origin = true;
	node_t chain = node2(N_EXPR, list(), rethrow_call, tt);

	// Walk catch clauses in REVERSE, wrapping each as an if whose else is the
	// chain so far (so earlier clauses are tested first).
	for (size_t ci = tt->catch_bodies.size(); ci-- > 0; ) {
		int tag = tt->catch_types[ci];
		const std::string &vname = tt->catch_varnames[ci];

		node_t handler_items = list();
		// Bind the catch variable from the exception value (when named + typed).
		// The parser registered the catch var in the catch BODY's scope, so the
		// body block would emit a bare `T e;`. Declare it WITH the exception
		// value as initializer in the handler instead, and remove it from the
		// body's variables so it isn't double-declared.
		if (!vname.empty() && tag != 99) {
			const char *valsym; std::vector<c2mir_node_code_t> rs;
			if (tag == 2) { valsym = "__madc_exception_double"; rs = { N_DOUBLE }; }
			else if (tag == 3) { valsym = "__madc_exception_cstr"; rs = {}; }
			else { valsym = "__madc_exception_int"; rs = { N_LONG }; }
			bool ret_ptr = (tag == 3);
			need_output_extern(valsym, ret_ptr, {}, rs);
			node_t vcall = node2(N_CALL, id(valsym, tt), list(), tt);
			// Resolve the catch var's declared type from the body scope, then
			// remove it so translate_block won't re-declare it.
			TokenCpnd *cbody = dynamic_cast<TokenCpnd *>(tt->catch_bodies[ci]);
			DataDef *cvtype = &ddINT64;
			if (cbody) {
				for (size_t vi = 0; vi < cbody->variables.size(); vi++) {
					if (cbody->variables[vi]->name == vname) {
						cvtype = cbody->variables[vi]->type;
						cbody->variables.erase(cbody->variables.begin() + vi);
						break;
					}
				}
			}
			// `T e = <exc value>;` — spec from the var's type, with initializer.
			node_t cv_spec = list();
			if (tag == 2) append(cv_spec, simple(N_DOUBLE, tt));
			else if (tag == 3) { append(cv_spec, simple(N_CHAR, tt)); }
			else append(cv_spec, simple(N_LONG, tt));
			node_t cv_decl_list = list();
			if (tag == 3) append(cv_decl_list, pointer());   // const char*
			node_t cv_sd = simple(N_SPEC_DECL, tt);
			append(cv_sd, node1(N_SHARE, cv_spec));
			append(cv_sd, node2(N_DECL, id(vname.c_str(), tt), cv_decl_list));
			append(cv_sd, ignore());
			append(cv_sd, ignore());
			append(cv_sd, vcall);
			CIR_NODE(cv_sd)->synth_from_origin = true;
			append(handler_items, cv_sd);
			(void)cvtype;
		}
		node_t hbody = translate_stmt(tt->catch_bodies[ci]);
		if (hbody) append(handler_items, hbody);
		node_t clear_call = node2(N_CALL, id("__madc_exception_clear", tt),
					  list(), tt);
		CIR_NODE(clear_call)->synth_from_origin = true;
		append(handler_items, node2(N_EXPR, list(), clear_call, tt));
		node_t handler = node2(N_BLOCK, list(), handler_items, tt);

		// catch(...) matches any type -> no guard, just the handler (but still
		// wrapped so an earlier-tested clause's else reaches it).
		node_t guard;
		if (tag == 99)
			guard = integer(1, tt);   // always true
		else
			guard = node2(N_EQ, id(tname, tt), integer(tag, tt), tt);
		chain = node4(N_IF, list(), guard, handler, chain, tt);
	}

	node_t else_items = list();
	append(else_items, t_sd);
	append(else_items, chain);
	node_t else_blk = node2(N_BLOCK, list(), else_items, tt);

	node_t ifnode = node4(N_IF, list(), cond, then_blk, else_blk, tt);

	// Whole try lowering: { ctx_decl; if (setjmp...) {body} else {dispatch} }
	node_t items = list();
	append(items, ctx_sd);
	append(items, ifnode);
	return node2(N_BLOCK, list(), items, tt);
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

	DataDef *cdd = fe->container->datadef();

	// A user-defined class / template-instantiated container (e.g.
	// std::vector<int> from the header template) iterates by index using
	// its size() and operator[] methods.
	if (DataDefCLASS *ccls = class_behind(cdd)) {
		std::string szname = "size", opname = "operator[]";
		Variable *szmv = ccls->findMethod(szname);
		Variable *opmv = ccls->findMethod(opname);
		if (szmv && opmv)
			return translate_foreach_class(fe, ccls, szmv, opmv);
	}

	// A raw fixed-size C array (`int a[N]`): compile-time element count + direct
	// subscript — no MadArray runtime helper. Emit an ordinary indexed for-loop
	// `for (long i = 0; i < N; i += 1) { T x = a[i]; <body> }`, exactly what g++
	// lowers a range-for over an array to. (A VLA / runtime-sized array has no
	// compile-time bound and is NOT handled here — it falls through.) Without
	// this a plain array hit the MadArray fallback below, reading its raw bytes
	// as a MadArray header -> garbage length -> out-of-bounds get -> SIGSEGV.
	if (TokenVar *ctv = dynamic_cast<TokenVar *>(fe->container)) {
		if (ctv->var.is_fixed_array() && !ctv->var.is_vla()
		    && ctv->var.total_elements() > 0)
			return translate_foreach_carray(fe, ctv);
	}

	// Reference loop var over a MadArray (php/perl dynamic array) is not
	// supported: MadArray elements are tagged-union values fetched by
	// __php_array_get_int (a value copy), with no stable element address to
	// alias — so a `T&` can't faithfully mutate the source. Reject rather than
	// silently copy (the reference contract would be violated). The aliasing
	// `T&` forms ARE handled above for raw arrays and size()/operator[]
	// containers (translate_foreach_carray / translate_foreach_class).
	if (fe->elem_is_ref)
		return error_node("range-for by reference (T&) is not supported over "
				  "this container (only raw arrays and "
				  "size()/operator[] containers)", fe);

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
	if (is_std_string(fe->elemtype)) {
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

node_t CirBuilder::translate_foreach_class(TokenFOREACH *fe, DataDefCLASS *cls,
					   Variable *szmv, Variable *opmv)
{
	FuncDef *opfd = dynamic_cast<FuncDef *>(opmv->type);
	bool recv_is_ptr = fe->container->datadef()
			   && fe->container->datadef()->is_pointer();
	// __this for each method call: value receiver -> &c, pointer -> c.
	auto recv_addr = [&]() -> node_t {
		node_t c = translate_expr(fe->container);
		return recv_is_ptr ? c : node1(N_ADDR, c, fe);
	};
	std::string szsym = cls->name + "__size";
	std::string opsym = cls->name + "__operator[]";

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

	// __fe_i < c.size()
	referenced_funcs.insert(szsym);
	node_t sz_args = list();
	append(sz_args, recv_addr());
	node_t sz_call = node2(N_CALL, id(szsym.c_str(), fe), sz_args, fe);
	node_t cond = node2(N_LT, id(idx, fe), sz_call, fe);

	// __fe_i += 1
	node_t incr = node2(N_ADD_ASSIGN, id(idx, fe), integer(1, fe), fe);

	// The element accessor `c[__fe_i]`. operator[] returns T& == a T*; the
	// bare call is the element ADDRESS, the deref is the element lvalue.
	referenced_funcs.insert(opsym);
	auto op_addr = [&]() -> node_t {
		node_t a = list();
		append(a, recv_addr());
		append(a, id(idx, fe));
		return node2(N_CALL, id(opsym.c_str(), fe), a, fe);
	};

	node_t body_items = list();
	if (fe->elem_is_ref) {
		// Reference loop var (`for (T& v : c)`): bind `v` (a vfREFERENCE
		// pointer) to the element ADDRESS returned by operator[] (which
		// yields T& == a T*). op_addr() IS that address, so writes through
		// `v` mutate the container element. No deref here (the reference
		// holds the address; reads auto-deref via vfREFERENCE).
		node_t assign = node2(N_ASSIGN, id(fe->elemname.c_str(), fe),
				      op_addr(), fe);
		append(body_items, node2(N_EXPR, list(), assign, fe));
	} else if (is_std_string(fe->elemtype)) {
		// String element: copy-assign the loop var (an enclosing-scope string
		// object, already constructed) from the element reference:
		// string_assign((void*)&x, (void*)&c[__fe_i]).
		need_output_extern("string_assign", false,
				   { { {N_VOID}, true }, { {N_VOID}, true } });
		node_t a = list();
		append(a, string_obj_addr(fe->elemname.c_str(), fe));
		append(a, node2(N_CAST, void_ptr_type(), op_addr(), fe));
		node_t fill = node2(N_CALL, id("string_assign", fe), a, fe);
		append(body_items, node2(N_EXPR, list(), fill, fe));
	} else {
		// Scalar element: x = *(c[__fe_i])  (load through the reference).
		node_t elem = opfd && opfd->returns_ref
				? node1(N_DEREF, op_addr(), fe) : op_addr();
		node_t assign = node2(N_ASSIGN, id(fe->elemname.c_str(), fe), elem, fe);
		append(body_items, node2(N_EXPR, list(), assign, fe));
	}

	node_t user_body = translate_stmt_required(fe->statement);
	if (user_body) append(body_items, user_body);
	node_t body = node2(N_BLOCK, list(), body_items, fe);

	return node5(N_FOR, list(), init, cond, incr, body, fe);
}

// Range-for over a raw fixed-size C array: `for (T x : a)` ->
//   for (long i = 0; i < N; i += 1) { x = a[i]; <body> }
// N is the array's compile-time element count; `a[i]` is a direct subscript
// (N_IND), element stride sizeof(T) — c2mir handles it natively, exactly like
// g++'s array range-for lowering. No MadArray runtime helper.
node_t CirBuilder::translate_foreach_carray(TokenFOREACH *fe, TokenVar *ctv)
{
	const char *arrname = ctv->var.name.c_str();
	long count = (long)ctv->var.total_elements();

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

	// __fe_i < N   (compile-time element count)
	node_t cond = node2(N_LT, id(idx, fe), integer(count, fe), fe);
	// __fe_i += 1
	node_t incr = node2(N_ADD_ASSIGN, id(idx, fe), integer(1, fe), fe);

	// Element accessor: a[__fe_i] (direct subscript).
	auto elem_lvalue = [&]() -> node_t {
		return node2(N_IND, id(arrname, fe), id(idx, fe), fe);
	};

	node_t body_items = list();
	if (fe->elem_is_ref) {
		// Reference loop var (`for (T& v : a)`): bind `v` (a vfREFERENCE
		// pointer) to the element ADDRESS — v = &a[__fe_i] — so reads
		// auto-deref and writes mutate the source array. Works for any
		// element type (string& aliases the object too).
		node_t assign = node2(N_ASSIGN, id(fe->elemname.c_str(), fe),
				      node1(N_ADDR, elem_lvalue(), fe), fe);
		append(body_items, node2(N_EXPR, list(), assign, fe));
	} else if (is_std_string(fe->elemtype)) {
		// String element: copy-assign the loop var (an enclosing-scope string
		// object) from the element: string_assign((void*)&x, (void*)&a[i]).
		need_output_extern("string_assign", false,
				   { { {N_VOID}, true }, { {N_VOID}, true } });
		node_t a = list();
		append(a, string_obj_addr(fe->elemname.c_str(), fe));
		append(a, node2(N_CAST, void_ptr_type(),
				node1(N_ADDR, elem_lvalue(), fe), fe));
		node_t fill = node2(N_CALL, id("string_assign", fe), a, fe);
		append(body_items, node2(N_EXPR, list(), fill, fe));
	} else {
		// Scalar element: x = a[__fe_i].
		node_t assign = node2(N_ASSIGN, id(fe->elemname.c_str(), fe),
				      elem_lvalue(), fe);
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

	// Exceptions: try/catch (SJLJ) and throw.
	{ TokenTRY *tt = dynamic_cast<TokenTRY *>(tb);
	  if (tt) return translate_try(tt); }
	{ TokenTHROW *th = dynamic_cast<TokenTHROW *>(tb);
	  if (th) return translate_throw(th); }

	// Goto — label form `goto L` (N_GOTO) or GNU computed form `goto *expr`
	// (N_INDIRECT_GOTO, the jump target is a void* label address).
	{ TokenGOTO *tg = dynamic_cast<TokenGOTO *>(tb);
	  if (tg) {
		if (tg->indirect_target)
			return node2(N_INDIRECT_GOTO, list(),
				     translate_expr(tg->indirect_target), tb);
		return node2(N_GOTO, list(), id(tg->target.c_str(), tb), tb);
	  } }

	// Label: `L: stmt`. A label names the statement it prefixes (carried in
	// tl->labeled by the parser). Emit the label as its own marker statement
	// `L: 0;` (an N_EXPR whose label list holds the N_LABEL — the same shape
	// translate_block uses) followed by the prefixed statement, wrapped in a
	// compound block so the pair is one node. This makes the label survive in
	// EVERY statement context — an if/while/for body or a switch case body,
	// not only the compound-block path. C labels are function-scoped, so the
	// extra block introduces no scope issue.
	{ TokenLabel *tl = dynamic_cast<TokenLabel *>(tb);
	  if (tl) {
		node_t items = list();
		node_t ll = list();
		append(ll, node1(N_LABEL, id(tl->name.c_str(), tb)));
		append(items, node2(N_EXPR, ll, integer(0), tb));
		if (tl->labeled) {
			node_t s = translate_stmt(tl->labeled);
			if (s) append(items, s);
		}
		return node2(N_BLOCK, list(), items, tb);
	  } }

	// Declaration
	{ TokenDecl *td = dynamic_cast<TokenDecl *>((TokenBase *)tb);
	  if (td) {
		// A function-block-scope `extern T name;` that refers to a file-scope
		// global: emit a REAL in-block `extern T name;` so c2mir's scope
		// resolution rebinds `name` to the file-scope object (an enclosing local
		// of the same name must not shadow it — C scope rules). Gated to a PLAIN
		// SCALAR extern (no array shape, no typedef alias, not a struct/union,
		// not a pointer):
		//   - Array / typedef-aliased / aggregate / pointer externs were the
		//     source of "incompatible types of <name> declarations" c2mir
		//     conflicts in SMAUG — the in-block extern's computed type
		//     (array-decay, typedef-alias divergence) did not match the
		//     file-scope global's. Those fall through to the file-scope-global
		//     path (return NULL), correct for SMAUG (the global is already
		//     emitted + in scope; the local-shadow case is not exercised there).
		//   - vfALLOC is cleared and data NULLed on the local view so ~Variable
		//     does NOT free the global's shared `data` — an owning copy with
		//     vfALLOC + shared pointer double-freed SMAUG's storage on boot
		//     ("free(): double free detected" SIGABRT).
		if (td->block_extern_redecl
		    && td->var.dims.empty()
		    && !(td->var.flags & vfFIXEDARRAY)
		    && td->var.typedef_name.empty()
		    && td->var.type
		    && !td->var.type->is_struct()
		    && !td->var.type->is_pointer()) {
			Variable ev = td->var;
			ev.flags |= vfEXTERN;
			ev.flags &= ~(vfLOCAL | vfSTATIC | vfALLOC); // non-owning view
			ev.data = NULL;
			return var_decl(&ev, td);
		}
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

	// Block-scope typedef (`typedef int (*BAZFN)(int);` inside a function). The
	// parser records file-scope typedefs in Program::top_decls (emitted once at
	// file scope by build()); a block-scope typedef instead reaches the
	// statement stream as this node and is emitted in-place so c2mir registers
	// the alias in this block's scope. Reuse the file-scope emitter (typedef_decl)
	// for a single declaration shape.
	{ TokenTypedefDecl *ttd = dynamic_cast<TokenTypedefDecl *>(tb);
	  if (ttd) {
		std::set<std::string> no_emitted_structs;
		node_t n = typedef_decl(ttd->alias, ttd->target_type,
					no_emitted_structs, false);
		if (n) { CIR_NODE(n)->origin = ttd; set_pos(CIR_NODE(n), ttd); }
		return n;
	  } }

	// Break
	if (tb->id() == TokenID::tkBREAK)
		return node1(N_BREAK, list(), tb);

	// Continue
	if (tb->id() == TokenID::tkCONT)
		return node1(N_CONTINUE, list(), tb);

	// Compound statement (block)
	TokenCpnd *tc = dynamic_cast<TokenCpnd *>(tb);
	if (tc) {
		// A GNU statement-expression `({...})` appearing in STATEMENT position
		// is an expression-statement whose value is the block's last
		// expression — emit N_EXPR(N_STMTEXPR(block)), not a plain N_BLOCK.
		// This makes its value flow when it is the last item of an enclosing
		// stmt-expr (the nested `({ ({...}); })` form); c2mir otherwise rejects
		// the outer with "last statement in statement expression is not an
		// expression". A plain `{...}` block has is_stmt_expr==false -> N_BLOCK.
		if (tc->is_stmt_expr) {
			node_t blk = translate_block(tc);
			// Wrap as a value-producing expression-statement ONLY when the
			// block's last item is itself an expression-statement (the
			// stmt-expr has a value). A VOID stmt-expr (last item is an
			// if/loop/label/etc., e.g. `({ ...; goto L; });`) used in
			// statement position stays a plain block — its value is
			// discarded, and N_STMTEXPR would wrongly demand an expression
			// last-item (regressing 930406-1).
			node_t items = c2mir_node_op(blk, 1);
			node_t last = NULL;
			for (int i = 0; ; i++) {
				node_t e = c2mir_node_op(items, i);
				if (!e) break;
				last = e;
			}
			if (last && last->code == N_EXPR)
				return node2(N_EXPR, list(),
					     node1(N_STMTEXPR, blk, tb), tb);
			return blk;
		}
		return translate_block(tc);
	}

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
		// A GNU nested function's in-scope alias is a Variable whose type is a
		// FuncDef — it names the hoisted function, it is not storage. Emit no
		// local declaration for it (calls resolve to the hoisted symbol). Without
		// this it rendered as `void Foo;`, which c2mir tolerates but gcc rejects,
		// breaking the portable `--emit=c11` output.
		if (dynamic_cast<FuncDef *>(v->type)) continue;
		append(items, var_decl(v));
		if (is_array_object(v->type)) {
			// `array a;` — default-construct the MadArray object. Scope-exit
			// destruction is handled by the cleanup attribute (array_storage_decl).
			append(items, array_ctor_call(v->name.c_str(), tc));
		} else if (is_sstream_object(v->type)) {
			// `stringstream ss;` — default-construct; cleanup attribute destructs.
			append(items, sstream_ctor_call(v->name.c_str(), tc));
		} else if (DataDefCLASS *cdd = as_class_instance(v->type)) {
			// `Foo f;` / `string s;` — a class instance declared without an
			// explicit constructor-call (no ctor args). Default-construct it;
			// scope-exit destruction is via the cleanup attribute (var_decl). The
			// argful form `Foo f(a,b)` parses to a TokenDecl statement handled below.
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
		// A label names the statement it prefixes (`L: stmt`). The label
		// carries that statement (parser), so peel labels off here — record
		// the names in pending_labels (the marker `<labels>: 0;` is emitted
		// before the prefixed statement below) and descend to the prefixed
		// statement, which is then translated as if it were the list entry.
		// Handles label chains (`L1: L2: stmt`) and a label as the final item
		// of a block (`L:` with no statement -> just the marker). This keeps
		// a label-before-declaration in the SAME block scope (correct RAII),
		// rather than wrapping it in a nested block.
		while (TokenLabel *tl = dynamic_cast<TokenLabel *>(stb)) {
			pending_labels.push_back(tl->name);
			stb = tl->labeled;
		}
		if (!stb) {
			// Trailing label(s) with no statement (block-end label).
			if (!pending_labels.empty()) {
				node_t ll = list();
				for (auto &ln : pending_labels)
					append(ll, node1(N_LABEL, id(ln.c_str())));
				pending_labels.clear();
				append(items, node2(N_EXPR, ll, integer(0)));
			}
			continue;
		}

		// std::string object declaration WITH initializer (`string s = "x"`,
		// `string b = a`) is handled by the generic class-instance path below:
		// the initializer becomes the single ctor argument and select_ctor_overload
		// picks the const-char*/copy/default ctor by its type.
		{
			TokenDecl *sdcl = dynamic_cast<TokenDecl *>(stb);
			bool file_global = sdcl && !(sdcl->var.flags & vfLOCAL)
					&& !(sdcl->var.flags & vfSTATIC);
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
			// std::stringstream object declaration.
			if (sdcl && is_sstream_object(sdcl->var.type) && !file_global) {
				if (!pending_labels.empty()) {
					node_t ll = list();
					for (auto &ln : pending_labels)
						append(ll, node1(N_LABEL, id(ln.c_str())));
					pending_labels.clear();
					append(items, node2(N_EXPR, ll, integer(0)));
				}
				append(items, var_decl(&sdcl->var, sdcl));
				append(items, sstream_ctor_call(sdcl->var.name.c_str(), sdcl));
				continue;
			}
			// Class instance declared with constructor args `Foo f(a,b)` or an
			// initializer `string s = "x"`: emit the struct storage (+ cleanup
			// attr) then the ctor call. The no-arg form `Foo f;` / `string s;` is
			// handled in the variables loop above.
			DataDefCLASS *cdcl = sdcl ? as_class_instance(sdcl->var.type) : NULL;
			if (cdcl && !file_global) {
				if (!pending_labels.empty()) {
					node_t ll = list();
					for (auto &ln : pending_labels)
						append(ll, node1(N_LABEL, id(ln.c_str())));
					pending_labels.clear();
					append(items, node2(N_EXPR, ll, integer(0)));
				}
				append(items, var_decl(&sdcl->var, sdcl));
				// An `=`-style initializer (`string s = "x"`) is not in ctor_args;
				// thread it as the single ctor argument so select_ctor_overload
				// chooses const-char*/copy by its type. (An explicit `Foo f(a,b)`
				// already populated ctor_args.)
				std::vector<TokenBase *> ctor_args = sdcl->ctor_args;
				if (ctor_args.empty()) {
					TokenBase *initexpr = sdcl->initialize;
					if (TokenAssign *as =
					    dynamic_cast<TokenAssign *>(initexpr))
						initexpr = as->right;
					if (!initexpr && !sdcl->init_list.empty())
						initexpr = sdcl->init_list[0];
					if (initexpr) ctor_args.push_back(initexpr);
				}
				// `B z = makeB();` where makeB returns a NON-TRIVIAL class by
				// value (the __retbuf ABI): construct directly into z by passing
				// &z as the call's __retbuf (copy-elision / NRVO into z). z's
				// storage decl already carries the cleanup dtor, so z is
				// destructed once; the callee placement-copy-constructs z's
				// object members — no default member ctors here, no bit-copy
				// double-free.
				if (ctor_args.size() == 1
				    && object_returning_call_class(ctor_args[0]) == cdcl) {
					TokenCallFunc *itcf =
						dynamic_cast<TokenCallFunc *>(ctor_args[0]);
					referenced_funcs.insert(itcf->var.name);
					node_t cargs = list();
					append(cargs, node1(N_ADDR,
						id(sdcl->var.name.c_str(), sdcl), sdcl));
					build_call_args(itcf, cargs);
					node_t icall = node2(N_CALL,
						id(itcf->var.name.c_str(), sdcl), cargs, sdcl);
					CIR_NODE(icall)->synth_from_origin = true;
					for (node_t p : m_pending_stmts)
						append(items, p);
					m_pending_stmts.clear();
					append(items, node2(N_EXPR, list(), icall, sdcl));
					continue;
				}
				node_t cc = class_ctor_call(&sdcl->var, cdcl,
							    ctor_args, sdcl);
				// A ctor arg may have materialized a temporary (e.g. a
				// string-returning call -> a `struct string` temp): emit those
				// pending decls BEFORE the ctor call that references them. The
				// normal statement loop flushes m_pending_stmts itself; this
				// var-decl branch builds cc outside that loop, so flush here.
				for (node_t p : m_pending_stmts)
					append(items, p);
				m_pending_stmts.clear();
				if (cc) append(items, cc);
				// No user ctor but embedded object members: construct each
				// member in place (string_construct(&inst.member)). With a
				// user ctor, the ctor body's prologue constructs them.
				else if (class_has_object_members(cdcl))
					class_instance_member_ctors(sdcl->var.name.c_str(),
								    cdcl, items, sdcl);
				// No ctor, no object members — a TRIVIAL class/struct with an
				// initializer (`A z = makeA(9)`, `Point m = mid(a,b)`): emit the
				// initialization `var = <init>` so the value is actually copied
				// in. Without this the initializer was dropped and the object
				// read garbage. A trivial struct has no dtor, so c2mir's native
				// struct copy is correct (no double-free). (A class WITH object
				// members needs copy-construction via the __retbuf ABI — handled
				// by the object-returning-call path, not here.)
				else if (!ctor_args.empty() && ctor_args[0]) {
					DataDef *idd = ctor_args[0]->datadef();
					if (idd && (idd->is_struct() || idd->is_object())
					    && !idd->is_pointer()) {
						node_t lhs = id(sdcl->var.name.c_str(), sdcl);
						node_t rhs = translate_expr(ctor_args[0]);
						// A sub-call may have queued pending temps; flush first.
						for (node_t p : m_pending_stmts)
							append(items, p);
						m_pending_stmts.clear();
						node_t asg = node2(N_ASSIGN, lhs, rhs, sdcl);
						append(items, node2(N_EXPR, list(), asg, sdcl));
					}
				}
				// In a try body, also register this object's dtor on the runtime
				// cleanup stack so it runs on the exception (longjmp) unwind path
				// (the cleanup attribute alone does not — P1.1c). No-op otherwise.
				emit_try_body_cleanup_push(sdcl->var.name.c_str(),
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

// GNU nested-function / [&]-lambda capture detection. While translating a
// capturing function's body, every variable reference funnels through
// translate_expr; this records an enclosing variable the body uses (one of the
// FuncDef's potential_captures, by pointer identity) as a real capture, in
// first-reference order. Returns true when `v` is a captured variable of the
// current nested function (so the caller emits a deref of the same-named
// pointer parameter instead of a bare identifier).
bool CirBuilder::note_capture(Variable *v)
{
	if (!m_cur_captured_fd || !v) return false;
	if (!m_cur_capture_set.count(v)) return false;
	std::vector<Variable *> &cv = m_cur_captured_fd->captured_vars;
	if (std::find(cv.begin(), cv.end(), v) == cv.end())
		cv.push_back(v);
	return true;
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
	bool ret_is_ref = fd->returns_ref;   // T& -> returned by address (one more *)
	DataDefPTR *ret_ptr = ret_is_ptr ? dynamic_cast<DataDefPTR *>(ret_dd) : NULL;
	if (ret_ptr && ret_ptr->base_type) ret_dd = ret_ptr->base_type;

	// A by-value std::string OR non-trivial class return uses the struct-return
	// (__retbuf) ABI: the C return type is `void`, a hidden `struct <T> *__retbuf`
	// is the first parameter, and `return obj;` copy-constructs *__retbuf (see
	// translate_return). A trivial struct keeps c2mir's native struct return.
	bool ret_is_string = !ret_is_ptr && !ret_is_ref && is_std_string(ret_dd)
			     && !fd->is_multi_return();
	m_cur_func_returns_string = ret_is_string;
	DataDefCLASS *ret_obj = (!ret_is_ptr && !ret_is_ref && !fd->is_multi_return())
				? class_return_via_retbuf(ret_dd) : NULL;
	m_cur_func_returns_object = ret_obj;
	bool ret_via_retbuf = ret_is_string || ret_obj;
	DataDef *retbuf_dd = ret_obj ? (DataDef *)ret_obj : (DataDef *)&ddSTRING;
	// Multi-return (`return a, b;`): C return type void + a hidden `long *__retbuf`
	// first param; translate_return stores each value to __retbuf[i].
	bool ret_is_multi = fd->is_multi_return();
	m_cur_func_multi_return = ret_is_multi;

	// Track whether this function returns void, so translate_return can lower
	// a gcc-accepted `return <expr>;` (void function) to `<expr>; return;`.
	// A retbuf-returning fn also has a `void` C return type but goes through the
	// __retbuf path, so keep it out of the plain-void lowering.
	m_cur_func_returns_void = !ret_is_ptr && !ret_is_ref && !ret_via_retbuf && ret_dd
				  && ret_dd->rawtype() == DataType::dtVOID
				  && !fd->is_multi_return();
	// Track reference return so translate_return emits `return &<expr>`.
	m_cur_func_returns_ref = ret_is_ref;
	// Track a `Cls *` return so translate_return can emit a derived->base upcast.
	m_cur_func_returns_class_ptr = ret_is_ptr ? pointee_user_class(&fd->returns)
						  : NULL;
	// Track the scalar C return type so a gcc-accepted bare `return;` in a
	// non-void function gets a typed zero (the returned value is indeterminate
	// per gnu89/c11, so zero is a conformant lowering). Skip ref (returns by
	// address) — a bare return there is already nonsensical and rare.
	m_cur_func_scalar_ret = (!ret_via_retbuf && !ret_is_ref && !m_cur_func_returns_void
				 && !ret_is_multi)
				? &fd->returns : NULL;

	// Retbuf-returning OR multi-return fn: C return type is `void`.
	node_t ret_type = (ret_via_retbuf || ret_is_multi) ? type_list(&ddVOID)
							   : type_list(ret_dd);

	// GNU nested-function / [&]-lambda capture context. A capturing function
	// (has_captures) implicitly captures by reference whatever enclosing
	// vars/params its body uses (its potential_captures). We translate the body
	// FIRST, recording the actually-used captures (FuncDef::captured_vars) via
	// note_capture at every variable-reference chokepoint, then synthesize one
	// hidden `T *name` pointer parameter per used capture. So the body is
	// translated before the parameter list is finalized below. Reentrancy:
	// save/restore the enclosing context (a nested fn can itself be nested).
	FuncDef *saved_captured_fd = m_cur_captured_fd;
	std::set<Variable *> saved_capture_set;
	saved_capture_set.swap(m_cur_capture_set);
	if (fd->has_captures) {
		m_cur_captured_fd = fd;
		fd->captured_vars.clear();
		for (Variable *pv : fd->potential_captures)
			if (pv) m_cur_capture_set.insert(pv);
	} else {
		m_cur_captured_fd = nullptr;
	}

	// Parameters. A variadic function carries a trailing synthetic param
	// (the parser pushes a ddINT64 placeholder when it sees `...`); drop it
	// and emit N_DOTS instead so the definition is truly variadic.
	node_t param_list = list();
	// Hidden return-slot parameter `struct <T> *__retbuf` first. Named param
	// => N_SPEC_DECL (specs, DECL(id, [POINTER]), asm, attr, init), matching
	// param_decl's wrap() for a named pointer parameter.
	if (ret_via_retbuf)
		append(param_list, retbuf_param(retbuf_dd, tf));
	else if (ret_is_multi)
		append(param_list, retbuf_param(&ddINT64, tf));   // long *__retbuf
	size_t nparam = fd->parameters.size();
	if (fd->is_varargs && nparam > 0) nparam--;
	// A capturing function with zero user params must NOT emit `(void)`: its
	// capture params (appended after the body translates) make it non-void.
	// Defer the empty-param `(void)` to after body translation in that case.
	bool defer_void_params = (nparam == 0 && !fd->is_varargs && !ret_via_retbuf
				  && !ret_is_multi && fd->has_captures);
	// `(void)` ONLY for an explicit `(void)` declaration (is_void_params); a
	// bare K&R `()` stays unprototyped (empty param list) so c2mir accepts any
	// call-site arg count, matching gcc's gnu89 behavior. Kept in lock-step
	// with func_proto and fnptr_func_node.
	if (nparam == 0 && !fd->is_varargs && !ret_via_retbuf && !ret_is_multi
	    && !defer_void_params) {
		if (fd->is_void_params) {
			node_t void_spec = node1(N_LIST, simple(N_VOID));
			node_t void_decl = node2(N_DECL, ignore(), list());
			append(param_list, node2(N_TYPE, void_spec, void_decl));
		}
		// else: bare () — unprototyped (K&R) parameter list
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
	// A T&-returning method returns by address: one extra pointer level (so
	// `int&`->`int*`, `char*&`->`char**`). Matches g++'s reference ABI.
	if (ret_is_ref)
		append(decl_list, pointer());

	node_t decl = node2(N_DECL, func_id, decl_list);
	node_t body = translate_block((TokenCpnd *)tf);

	// C99 VLA-parameter bound side effects: a parameter array bound such as
	// `int a[i++]` must have its bound expression evaluated on function entry
	// (C11 6.9.1p10) — the `i++` actually runs. The parser recorded each such
	// expr in Variable::param_vla_side_effect_expr; emit them at the top of the
	// body in parameter order, before any user statement.
	{
		// The param Variables (carrying param_vla_side_effect_expr) live in
		// tf->method->parameters (the parser pushes them there).
		std::vector<node_t> vla_side_fx;
		if (tf->method) {
			for (Variable *pv : tf->method->parameters) {
				if (pv && pv->param_vla_side_effect_expr)
					vla_side_fx.push_back(
						node2(N_EXPR, list(),
						      translate_expr(pv->param_vla_side_effect_expr),
						      tf));
			}
		}
		if (!vla_side_fx.empty()) {
			node_t outer = list();
			for (node_t s : vla_side_fx) append(outer, s);
			append(outer, body);
			body = node2(N_BLOCK, list(), outer, tf);
		}
	}

	// Capture lowering (post-body): the body translation recorded which
	// enclosing variables this nested fn/[&]-lambda actually uses
	// (fd->captured_vars, in first-reference order). Append one hidden pointer
	// parameter `T *name` per used capture (param_list is the live N_FUNC
	// operand, so appending here extends the emitted signature). Call sites
	// forward `&var` for each (see the TokenCallFunc path / build_call_args).
	if (fd->has_captures) {
		for (Variable *cv : fd->captured_vars) {
			if (!cv) continue;
			DataDef *capt_ptr = new DataDefPTR(*cv->type);
			append(param_list, param_decl(capt_ptr, cv->name.c_str(),
						      std::string()));
		}
		// Deferred `(void)` for a zero-user-param fn that turned out to
		// capture nothing: emit it now so the signature is `(void)`.
		if (defer_void_params && fd->captured_vars.empty()) {
			node_t void_spec = node1(N_LIST, simple(N_VOID));
			node_t void_decl = node2(N_DECL, ignore(), list());
			append(param_list, node2(N_TYPE, void_spec, void_decl));
		}
	}
	// Restore the enclosing capture context.
	m_cur_captured_fd = saved_captured_fd;
	m_cur_capture_set.swap(saved_capture_set);

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
		// Cast __this to `Base *` (offset-adjusted to the base subobject) for a
		// chained base ctor/dtor call. Offset 0 (primary/single base) = a plain
		// cast, byte-identical to the previous single-base path.
		auto base_call_at = [&](DataDefCLASS *b, size_t off, const std::string &bsym) -> node_t {
			referenced_funcs.insert(bsym);
			node_t self = id("__this", tf);
			node_t adj = self;
			if (off != 0) {
				node_t charp = node2(N_CAST,
					node2(N_TYPE, node1(N_LIST, simple(N_CHAR)),
					      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
					self, tf);
				adj = node2(N_ADD, charp, integer((long)off), tf);
			}
			node_t t = node2(N_TYPE,
				node1(N_LIST, node2(N_STRUCT, id(b->name.c_str()), ignore())),
				node2(N_DECL, ignore(), node1(N_LIST, pointer())));
			node_t a = list();
			append(a, node2(N_CAST, t, adj, tf));
			node_t call = node2(N_CALL, id(bsym.c_str(), tf), a, tf);
			return node2(N_EXPR, list(), call, tf);
		};
		// Prologue statements (run before the body), in order:
		//   1. base ctor call (ctor of a derived class with a base ctor)
		//   2. __this->__vptr = (void*)ClassName__vtable (ctor of a virtual
		//      class) — set after base construction, matching C++.
		// Epilogue (after the body): base dtor call (dtor with a base dtor).
		std::vector<node_t> prologue, epilogue;
		// Construct each base in declaration order, with `this` adjusted to that
		// base's subobject (MI). Single inheritance = one base @ offset 0.
		if (is_ctor)
			for (size_t bi = 0; bi < ocls->bases.size(); bi++) {
				if (ocls->bases[bi].is_virtual) continue; // vbases: complete-object site
				DataDefCLASS *b = ocls->bases[bi].base;
				if (b->has_user_ctor)
					prologue.push_back(base_call_at(b, ocls->base_offset_of(b),
						b->name + "__" + b->name));
			}
		// Construct embedded object members at ctor entry (after base ctor),
		// destruct them at dtor exit (before base dtor) — C++ member lifetime.
		if (is_ctor)
			class_member_construct(ocls, prologue, tf);
		if (is_dtor)
			class_member_destruct(ocls, epilogue, tf);
		if (is_ctor && ocls->has_vtable) {
			std::string vname = ocls->name + "__vtable";
			// Set each subobject's vptr to its group's address point in the flat
			// table: __this->__vptr[_<off>] = (void*)(Cls__vtable + addr_point).
			// Primary group (offset 0, addr_point 0) reproduces the old single
			// assignment byte-for-byte.
			for (size_t g = 0; g < ocls->vtable_groups.size(); g++) {
				const auto &G = ocls->vtable_groups[g];
				std::string fld = (G.this_offset == 0)
					? "__vptr" : ("__vptr_" + std::to_string(G.this_offset));
				node_t vptr_lhs = node2(N_DEREF_FIELD, id("__this", tf),
							id(fld.c_str(), tf));
				node_t vptr_type = node2(N_TYPE,
					node1(N_LIST, simple(N_VOID)),
					node2(N_DECL, ignore(), node1(N_LIST, pointer())));
				node_t tab = id(vname.c_str(), tf);
				node_t ap = (G.addr_point == 0) ? tab
					: node2(N_ADD, tab, integer((long)G.addr_point), tf);
				node_t vtab = node2(N_CAST, vptr_type, ap, tf);
				prologue.push_back(node2(N_EXPR, list(),
					node2(N_ASSIGN, vptr_lhs, vtab, tf), tf));
			}
		}
		// Destroy NON-VIRTUAL bases in REVERSE declaration order (MI), offset-adjusted.
		// Virtual bases are destroyed once by the complete-object dtor (_dtor_complete).
		if (is_dtor)
			for (size_t bi = ocls->bases.size(); bi-- > 0; ) {
				if (ocls->bases[bi].is_virtual) continue;
				DataDefCLASS *b = ocls->bases[bi].base;
				if (b->has_user_dtor)
					epilogue.push_back(base_call_at(b, ocls->base_offset_of(b),
						b->name + "___dtor"));
			}

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

	// File-scope class globals (std::string etc.) construct before any user
	// code runs (C++ static init). madc realizes this by prepending their ctor
	// calls — collected in source/declaration order — to main's body. (Their
	// scope-exit destruction is deferred: the cleanup attribute on a file-scope
	// object never fires; program teardown reclaims the memory.)
	if (tf->var.name == "main" && !m_global_ctor_stmts.empty()) {
		node_t outer = list();
		for (node_t s : m_global_ctor_stmts)
			append(outer, s);
		append(outer, body);
		body = node2(N_BLOCK, list(), outer, tf);
		// Re-emit with the wrapped body; the prologue runs once at main entry.
		return node4(N_FUNC_DEF, ret_type, decl, list(), body, tf);
	}

	return node4(N_FUNC_DEF, ret_type, decl, list(), body, tf);
}

// Build the constructor-call statement for a file-scope class global `v` of
// class `cdd`. The initializer comes from the linked TokenDecl (user source:
// `string g = "hi";`) when present, else from the runtime backing object
// (built-ins like `version`, registered via addGlobal with a const char*
// initializer that Variable's ctor stored as a host std::string). A class with
// no constructor (no has_user_ctor) yields NULL — nothing to run.
node_t CirBuilder::global_ctor_call(Variable *v, DataDefCLASS *cdd, TokenDecl *decl)
{
	if (!v || !cdd) return NULL;

	// Recover the initializer expression for a source-declared global. Mirrors
	// translate_block's class-instance path: an `=`-style init is wrapped in a
	// TokenAssign; a braced/list init rides in init_list/ctor_args.
	std::vector<TokenBase *> ctor_args;
	if (decl) {
		ctor_args = decl->ctor_args;
		if (ctor_args.empty()) {
			TokenBase *initexpr = decl->initialize;
			if (TokenAssign *as = dynamic_cast<TokenAssign *>(initexpr))
				initexpr = as->right;
			if (!initexpr && !decl->init_list.empty())
				initexpr = decl->init_list[0];
			if (initexpr) ctor_args.push_back(initexpr);
		}
		return class_ctor_call(v, cdd, ctor_args, decl);
	}

	// No source TokenDecl (built-in global): the initializer value lives in the
	// runtime backing object. For a std::string that is `*(std::string*)v->data`;
	// construct via the const-char* ctor with a literal synthesized from it. A
	// class without a const-char* initializer falls back to the default ctor.
	if (!is_std_string(v->type)) {
		// Other built-in object classes: default-construct (no stored literal).
		std::vector<TokenBase *> none;
		return class_ctor_call(v, cdd, none, NULL);
	}
	const std::string init = (v->data ? *(std::string *)v->data : std::string());
	node_t args = list();
	append(args, node1(N_ADDR, id(v->name.c_str(), NULL), NULL)); // &v (this)
	append(args, str(init.c_str(), init.size() + 1, NULL));       // const char*
	append(args, node1(N_ADDR, id(v->name.c_str(), NULL), NULL)); // dummy alloc&
	need_output_extern(STR_CTOR_S, false,
			   { { {N_VOID}, true }, { {N_CHAR}, true }, { {N_VOID}, true } });
	referenced_funcs.insert(STR_CTOR_S);
	node_t call = node2(N_CALL, id(STR_CTOR_S, NULL), args, NULL);
	CIR_NODE(call)->synth_from_origin = true;
	return node2(N_EXPR, list(), call, NULL);
}

// Emit storage for, and queue construction of, every file-scope class-instance
// global (std::string and friends). Source-declared globals already had their
// struct storage emitted by the dkGlobalVar pass (recorded in emitted_globals);
// built-ins registered via addGlobal (e.g. `version`) are NOT in top_decls, so
// emit their storage here. Either way, queue the ctor into m_global_ctor_stmts
// for func_def to run at main entry, in declaration order.
void CirBuilder::collect_global_ctors(Program *prog,
				      std::vector<node_t> &deferred_globals,
				      std::set<std::string> &emitted_globals)
{
	m_global_ctor_stmts.clear();
	if (!prog || !prog->tkProgram) return;
	for (Variable *v : prog->tkProgram->variables) {
		if (!v) continue;
		// File-scope only (a global is non-LOCAL or a file-scope static).
		if ((v->flags & vfLOCAL) && !(v->flags & vfSTATIC)) continue;
		// An extern is a reference to a definition elsewhere — no storage, no ctor.
		if (v->flags & vfEXTERN) continue;
		DataDefCLASS *cdd = as_class_instance(v->type);
		if (!cdd) continue;
		bool already_emitted = emitted_globals.count(v->name) != 0;
		TokenDecl *decl = NULL;
		if (!already_emitted) {
			// Built-in (not source-declared): emit its struct storage now —
			// reuse var_decl (the same `struct string name __attribute__((cleanup
			// (dtor)))` shape the deferred-global pass emits for source globals).
			// Deferred with the source globals so it follows the function
			// prototypes (definition-before-use; see deferred_globals).
			node_t gd = var_decl(v, NULL);
			if (gd) deferred_globals.push_back(gd);
			emitted_globals.insert(v->name);
		} else {
			// Source-declared: locate the TokenDecl carrying the initializer.
			for (auto &td : prog->top_decls)
				if (td.kind == Program::DeclKind::dkGlobalVar
				    && td.var == v) { decl = td.decl; break; }
		}
		node_t cc = global_ctor_call(v, cdd, decl);
		if (cc) m_global_ctor_stmts.push_back(cc);
	}
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

	// std::string lowers to a real `struct string` (opaque object class). Emit
	// its definition once, ahead of every use, whenever the string machinery is
	// active (ddSTRING populated by add_string_methods). A program that never
	// declares a string carries one harmless complete `struct string` type.
	if (!ddSTRING.methods.empty()) {
		node_t sd = class_struct_def(&ddSTRING);
		if (sd) append(top_list, sd);
	}

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
			// A complete definition (`struct X { ... };`) is a def point even
			// when it has no members (`struct X {};` is a valid empty struct);
			// a bare forward declaration (`struct X;`) is not.
			if (sdd && sdd->is_complete)
				struct_def_points.insert(sdd->name);
		} else if (td.kind == Program::DeclKind::dkTypedef && td.struct_body) {
			// A combined `typedef struct Tag {...} Tag;` is ALSO a body
			// definition point: the tag's full body rides inline in this
			// typedef's SPEC_DECL. Recording it lets an EARLIER typedef that
			// only references the tag (`typedef struct Tag *p;`, written before
			// the body) emit STRUCT(tag, IGNORE) instead of hoisting the body
			// above types its members name (self-referential typedef cluster).
			DataDefSTRUCT *sdd = struct_behind(td.dd);
			if (sdd && sdd->is_complete)
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

	// Global variable declarations are DEFERRED out of the Pass 0 loop into
	// this list (kept in source order) and emitted after the function
	// prototypes (Pass 1). A function used as an address constant in a global
	// initializer (`void (*tbl[])(void) = { f };`, SMAUG command tables) must
	// be DECLARED before that global — C rejects a forward reference in a
	// non-auto initializer (verified: both gcc and c2m error on
	// `void *(*p)(void)=one;` when `one` is defined later). The original
	// source defines the function first; madc emits all globals before all
	// function bodies (Pass 2), so the prototype is the declaration that
	// restores definition-before-use. Globals follow the prototypes (and the
	// struct/class defs below) so a by-value struct param in a prototype sees
	// the complete type and the global's fn-name initializer sees the proto.
	std::vector<node_t> deferred_globals;

	for (auto &td : prog->top_decls) {
		switch (td.kind) {
		case Program::DeclKind::dkTypedef: {
			DataDefSTRUCT *sdd = struct_behind(td.dd);
			// This typedef IS the tag's body-definition point when it carries
			// the inline body (`typedef struct Tag {...} Tag;`, td.struct_body).
			bool is_def_point = td.struct_body && sdd;
			// Forward ref iff the struct's body is defined elsewhere (a separate
			// bare dkStruct OR a later combined typedef) and hasn't been emitted
			// yet -> STRUCT(tag, IGNORE). The def-point typedef itself never
			// forward-refs; it emits the body.
			bool forward = sdd && struct_def_points.count(sdd->name)
					   && !emitted_structs.count(sdd->name)
					   && !is_def_point;
			node_t n = typedef_decl(td.name, td.dd, emitted_structs, forward);
			if (n) { stamp(n, td); append(top_list, n); }
			// Mark the tag emitted once its body actually went out here: either
			// this is its recorded def point, or a combined `typedef struct X
			// {...} Y;` with no separate def point (anonymous-tag / pure-pointer
			// alias) that renders its body inline.
			if (sdd && sdd->is_complete &&
			    (is_def_point || !struct_def_points.count(sdd->name)))
				emitted_structs.insert(sdd->name);
			break;
		}
		case Program::DeclKind::dkStruct:
		case Program::DeclKind::dkUnion: {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(td.dd);
			if (sdd && sdd->is_complete && !emitted_structs.count(sdd->name)) {
				emitted_structs.insert(sdd->name);
				node_t sd = struct_def(sdd);
				if (sd) { stamp(sd, td); append(top_list, sd); }
			}
			break;
		}
		case Program::DeclKind::dkGlobalVar: {
			// An aliased global (`b __attribute__((alias("a")))`) emits NO
			// declaration of its own — references to it name the target symbol
			// (var_emit_name). Emitting `extern int b` here would re-introduce
			// the undefined import at MIR-link.
			if (td.var && var_emit_name(*td.var) != td.var->name)
				break;
			if (td.var && !dynamic_cast<FuncDef *>(td.var->type)) {
				// Pass the linked TokenDecl as origin so var_decl's
				// existing initializer logic (scalar + brace) emits the
				// global's init in its SPEC_DECL — the single source for
				// the CIR backend (tkProgram->statements is not walked here).
				// Deferred until after the function prototypes (see above).
				node_t gd = var_decl(td.var, td.decl);
				if (gd) { stamp(gd, td); deferred_globals.push_back(gd); }
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

	// Collect user function names. Stored as a member too, so the body
	// translation (below) can tell a madc-COMPILED function (whose by-value
	// string return madc lowers via the __retbuf ABI) from an external / native
	// function that merely has a std::string return type but its own ABI.
	std::set<std::string> user_func_names;
	for (TokenFunc *tf : funcs)
		user_func_names.insert(tf->var.name);
	m_user_func_names = &user_func_names;

	// File-scope class-instance globals (std::string and friends): emit storage
	// for any not already covered by the dkGlobalVar pass (built-ins like
	// `version`) and queue their ctor calls for main's prologue. Done before the
	// body loop so func_def(main) sees m_global_ctor_stmts; the storage rides in
	// deferred_globals (emitted after the prototypes, Pass 1a). referenced_funcs
	// it populates (the ctor symbol) is captured by the later proto pass.
	collect_global_ctors(prog, deferred_globals, emitted_globals);

	// Translate function bodies first, into a temp list, so referenced_funcs
	// (populated as N_CALL nodes are built) is complete before the prototype
	// pass — letting us emit extern protos for ONLY referenced functions.
	std::vector<node_t> func_def_nodes;
	for (TokenFunc *tf : funcs) {
		node_t fd = func_def(tf);
		if (fd) func_def_nodes.push_back(fd);
	}
	m_user_func_names = NULL;   // the backing set is a local; don't dangle

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
				append(ext_list, node2(sdd->union_layout ? N_UNION : N_STRUCT, id(sdd->name.c_str()), ignore()));
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
				// 'p' + up to 20 digits (size_t max) + NUL.
				char pname[24];
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

	// Pass 1: forward prototypes for every user function. Emitted AFTER the
	// struct/class definitions (Pass 0 / 0.5) — so a by-value struct param or
	// return in a prototype sees the complete type — and BEFORE the deferred
	// globals below, so a function used as an address constant in a global
	// initializer is declared first (C requires definition-before-use in a
	// non-auto initializer; see deferred_globals).
	for (TokenFunc *tf : funcs) {
		if (tf->var.name == "main") continue;
		node_t proto = func_proto(tf);
		if (proto) append(top_list, proto);
	}

	// Pass 1a: the deferred global variable declarations (collected in source
	// order during Pass 0), now that every user function is prototyped above.
	for (node_t gd : deferred_globals)
		append(top_list, gd);

	// Pass 1.45: forward prototypes for synthesized destructor symbols a vtable
	// will reference (the "~"/"~$deleting" slots resolve to class_complete_dtor_symbol
	// and Cls___dtor_deleting — both defined later in Passes 1.6/1.7/1.8). c2mir
	// requires a function used as a global-init address constant to be declared
	// first, so prototype them before Pass 1.5. A user dtor's base symbol is already
	// prototyped in Pass 1; only the synthesized base (no user dtor), the complete
	// dtor, and the deleting dtor need protos here. Deduped by class pointer.
	std::set<DataDefCLASS *> emitted_dtor_protos;
	for (auto &kv : prog->struct_map) {
		DataDefCLASS *cdd = as_user_class(kv.second);
		if (!cdd || cdd->vtable_slot("~$deleting") < 0) continue;
		if (emitted_dtor_protos.count(cdd)) continue;
		emitted_dtor_protos.insert(cdd);
		std::string csym = class_complete_dtor_symbol(cdd);
		if (csym != cdd->name + "___dtor" || !class_has_own_user_dtor(cdd)) {
			node_t cp = synth_dtor_proto(csym, cdd);
			if (cp) append(top_list, cp);
		}
		node_t dp = synth_dtor_proto(cdd->name + "___dtor_deleting", cdd);
		if (dp) append(top_list, dp);
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
		// type_info first — the vtable's RTTI slot references _ZTI<cls>. (S5b)
		node_t ti = class_typeinfo_def(cdd);
		if (ti) append(top_list, ti);
		std::vector<node_t> thunks;
		node_t vt = class_vtable_def(cdd, thunks);
		// Thunks first — the vtable initializer references their symbols.
		for (node_t th : thunks) append(top_list, th);
		if (vt) append(top_list, vt);
	}

	// Pass 1.6: synthesized destructors for classes that need a dtor (object
	// members and/or a base dtor) but have no user-written one. Emitted here
	// so the symbol is in scope for the cleanup attribute in function bodies
	// (Pass 2). Deduped by class pointer.
	std::set<DataDefCLASS *> emitted_synth_dtors;
	for (auto &kv : prog->struct_map) {
		DataDefCLASS *cdd = as_user_class(kv.second);
		if (!cdd || class_has_own_user_dtor(cdd) || !class_needs_dtor(cdd)) continue;
		if (emitted_synth_dtors.count(cdd)) continue;
		emitted_synth_dtors.insert(cdd);
		node_t dd = synth_dtor_def(cdd);
		if (dd) append(top_list, dd);
	}

	// Pass 1.7: complete-object destructors for classes with virtual bases. The
	// complete dtor (Cls___dtor_complete) calls the base dtor (Cls___dtor — user or
	// synth, prototyped in Pass 1 / emitted above) then destroys the shared virtual
	// bases once. Complete-object destruction sites (cleanup attribute, delete,
	// unwind) target it via class_complete_dtor_symbol. Deduped.
	std::set<DataDefCLASS *> emitted_complete_dtors;
	for (auto &kv : prog->struct_map) {
		DataDefCLASS *cdd = as_user_class(kv.second);
		if (!cdd || !class_needs_dtor(cdd)) continue;
		std::vector<DataDefCLASS *> vbs; std::set<DataDefCLASS *> seen;
		cdd->collect_vbases(vbs, seen);
		if (vbs.empty()) continue;
		if (emitted_complete_dtors.count(cdd)) continue;
		emitted_complete_dtors.insert(cdd);
		node_t cd = synth_complete_dtor_def(cdd);
		if (cd) append(top_list, cd);
	}

	// Pass 1.8: deleting (D0) destructors for every polymorphic class with a
	// virtual destructor (a "~$deleting" vtable slot). NOT gated on virtual bases
	// (unlike Pass 1.7) — most virtual-dtor classes have none. Deduped.
	std::set<DataDefCLASS *> emitted_deleting_dtors;
	for (auto &kv : prog->struct_map) {
		DataDefCLASS *cdd = as_user_class(kv.second);
		if (!cdd || cdd->vtable_slot("~$deleting") < 0) continue;
		if (emitted_deleting_dtors.count(cdd)) continue;
		emitted_deleting_dtors.insert(cdd);
		node_t dd0 = synth_deleting_dtor_def(cdd);
		if (dd0) append(top_list, dd0);
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
	case N_CF:           fprintf(f, " %gi", (double)n->u.f); break;
	case N_CD:           fprintf(f, " %gi", n->u.d); break;
	case N_CLD:          fprintf(f, " %gi", (double)n->u.ld); break;
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
