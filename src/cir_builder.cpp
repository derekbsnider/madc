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
#include <functional>
#include <stdint.h>
#include <setjmp.h>
#include <dlfcn.h>
#include <cstdlib>
#include <typeinfo>
#include <cxxabi.h>


#define DBG(x) do { if(madc_verbose){x;} } while(0)

// Compile-time-gated diagnostics (toggle with -DMADC_DBG_FREEFN); avoids the
// thread_local madc_verbose that silences DBG on worker threads, and avoids
// add/remove churn. Off in normal builds.
#ifdef MADC_DBG_FREEFN
#define FFDBG(x) do { x; } while(0)
#else
#define FFDBG(x) do {} while(0)
#endif

#include "datadef.h"
#include "tokens.h"
#include "token_arena.h"
#include "datatokens.h"
#include "madc.h"
#include "cir_builder.h"
#include "madc_mangle.h"

extern "C" {
#include "c2mir/c2mir_api.h"
}

extern thread_local bool madc_verbose;

namespace {
class CirNullStreambuf : public std::streambuf
{
protected:
	int overflow(int c) override { return c; }
};

CirNullStreambuf g_cir_tsubst_null_streambuf;

class TsubstSpeculativeDiagnostics
{
	Program *m_prog;
	size_t m_diag_count;
	Program::ErrorInfo m_error;
	std::streambuf *m_cerr_buf;
	std::ios::iostate m_cerr_state;
public:
	explicit TsubstSpeculativeDiagnostics(Program *prog)
		: m_prog(prog),
		  m_diag_count(prog ? prog->diagnostics.size() : 0),
		  m_error(prog ? prog->last_error : Program::ErrorInfo()),
		  m_cerr_buf(std::cerr.rdbuf()),
		  m_cerr_state(std::cerr.rdstate())
	{
		std::cerr.rdbuf(&g_cir_tsubst_null_streambuf);
	}

	~TsubstSpeculativeDiagnostics()
	{
		std::cerr.rdbuf(m_cerr_buf);
		std::cerr.clear(m_cerr_state);
	}

	void restore_public_state()
	{
		if (!m_prog)
			return;
		m_prog->diagnostics.resize(m_diag_count);
		m_prog->last_error = m_error;
	}
};
}

// Derived source position: a node's position IS its origin token's position
// (the single source of truth). No absolute offset is stored on the node, so a
// future switch to relative token positions changes only the token layer.
const char *cir_node::src_file()   const { TokenBase *o = madc_token_for_slot(origin_id); return o ? o->file   : NULL; }
int         cir_node::src_line()   const { TokenBase *o = madc_token_for_slot(origin_id); return o ? o->line   : 0; }
int         cir_node::src_column() const { TokenBase *o = madc_token_for_slot(origin_id); return o ? o->column : 0; }

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

	cn->origin_id = madc_slot_id_for(origin);   // stable arena slot-id (0 if synthetic)
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
		if (!idt->spelling_empty()) desc += std::string(" '") + idt->spelling() + "'";
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
	// DATA alias names another DEFINED file-scope variable, while a function
	// asm-label (`void f() asm("g")`) also lands here. Data aliases resolve to
	// their backing storage; function labels are the emitted symbol contract.
	if (v.type && v.type->is_function())
		return v.storage_alias_name;
	Variable *target =
		m_prog->resolve_global_storage_variable(const_cast<Variable *>(&v));
	if (target && target != &v && !target->name.empty()
	    && !(target->type && target->type->is_function()))
		return target->name;
	return v.storage_alias_name;
}

// THE single source of truth for the C symbol a call references. Precedence:
//   1. emit_symbol     — bound to an EXTERNAL ABI symbol; madc emits no body.
//   2. local_emit_name — a madc-emitted body's non-default symbol (a hoisted
//                        nested fn / lambda, or an arity-disambiguated
//                        method/operator).
//   3. var_emit_name   — the variable's own emit name (default scheme; also
//                        resolves data/asm-label aliases).
// emit_symbol and local_emit_name are mutually exclusive by construction. The
// parser maintains the invariant that whenever local_emit_name is set the
// Variable's name is set equal to it, so historical call sites that fell back
// to var.name already produced this value — routing them here is behavior-
// preserving. New call sites MUST use this (enforced by
// scripts/check-call-emit-symbol.sh) so the precedence cannot drift.
std::string CirBuilder::call_emit_symbol(FuncDef *fd, const std::string &default_sym)
{
	if (fd && !fd->emit_symbol.empty())
		return fd->emit_symbol;
	if (fd && !fd->local_emit_name.empty())
		return fd->local_emit_name;
	return default_sym;
}

std::string CirBuilder::call_emit_symbol(const Variable &v, FuncDef *fd) const
{
	return call_emit_symbol(fd, var_emit_name(v));
}

std::string CirBuilder::func_emit_name(const Variable &v, FuncDef *fd) const
{
	return call_emit_symbol(v, fd);
}

static bool external_symbol_available(const std::string &sym)
{
	return !sym.empty() && dlsym(RTLD_DEFAULT, sym.c_str()) != NULL;
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
// Tree copy (two-tree / materialize-from-AST, Phase 1)
// -----------------------------------------------------------------------
// Substitute template-parameter placeholders in a datadef (two-tree Phase 3,
// the `tsubst` TYPE half). A DataDefTemplateParam that appears directly, or
// under pointer / reference / const layers, is replaced by its concrete type
// from `subst`; derived layers are rebuilt through the canonical builders
// (getPointerType / getReferenceType / getConstType) so type identity and
// caching are preserved. Returns `dd` unchanged when no placeholder is involved
// (the common case). DataDefREF IS-A DataDefPTR, so it is tested first.
// `packs`/`pack_params` (the caller's active type-arg-pack window) enable the
// structural dependent template-id case: a shell with a DependentShellOrigin
// record rebuilds its CONCRETE instantiation by substituting the recorded arg
// runs (pack name -> elements, sizeof...(P) -> arity) and replaying them
// through the parser's instantiation seam.
static DataDef *subst_datadef(Program *prog, DataDef *dd,
			      const std::map<DataDef *, DataDef *> &subst,
			      const std::vector<std::vector<DataDef *> > *packs = NULL,
			      const std::map<unsigned, DataDef *> *pack_params = NULL);

static DataDefTemplateParam *template_param_under_type_layers(DataDef *dd)
{
	for (int guard = 0; dd && guard < 8; ++guard) {
		if (DataDefTemplateParam *tp =
			    dynamic_cast<DataDefTemplateParam *>(dd))
			return tp;
		if (DataDefCONST *cd = dynamic_cast<DataDefCONST *>(dd))
			{ dd = cd->base_type; continue; }
		if (DataDefREF *rd = dynamic_cast<DataDefREF *>(dd))
			{ dd = rd->base_type; continue; }
		if (DataDefPTR *pd = dynamic_cast<DataDefPTR *>(dd))
			{ dd = pd->base_type; continue; }
		if (DataDefCArray *ad = dynamic_cast<DataDefCArray *>(dd))
			{ dd = ad->element_type; continue; }
		break;
	}
	return NULL;
}

// A dependent placeholder CLASS (directly or under ptr/ref/const/array
// layers), origin-recorded or not — a type that cannot ground concrete
// object materialization. NULL otherwise.
static DataDefCLASS *dependent_placeholder_under_type_layers(DataDef *dd)
{
	for (int guard = 0; dd && guard < 8; ++guard) {
		if (DataDefCONST *cd = dynamic_cast<DataDefCONST *>(dd))
			{ dd = cd->base_type; continue; }
		if (DataDefREF *rd = dynamic_cast<DataDefREF *>(dd))
			{ dd = rd->base_type; continue; }
		if (DataDefPTR *pd = dynamic_cast<DataDefPTR *>(dd))
			{ dd = pd->base_type; continue; }
		if (DataDefCArray *ad = dynamic_cast<DataDefCArray *>(dd))
			{ dd = ad->element_type; continue; }
		DataDefCLASS *cls = dynamic_cast<DataDefCLASS *>(dd);
		if (cls && cls->is_dependent_placeholder)
			return cls;
		break;
	}
	return NULL;
}

// A dependent template-id SHELL class (directly or under ptr/ref/const/array
// layers) whose structural origin was recorded at creation — i.e. one
// subst_datadef's rebuild case can concretize. NULL otherwise.
static DataDefCLASS *dependent_shell_under_type_layers(Program *prog,
							DataDef *dd)
{
	if (!prog)
		return NULL;
	DataDefCLASS *cls = dependent_placeholder_under_type_layers(dd);
	return (cls && prog->dependent_shell_origin.count(cls)) ? cls : NULL;
}

// A class with a member whose type is (or wraps) a template parameter — i.e. a
// LOCAL class defined in a dependent member-template body (`struct Guard { U v; };`,
// _M_construct's _Guard, _Rb_tree's _Auto_node). It is a Tree-1 pattern artifact:
// emitting it globally would lower its placeholder member and hit the
// "unsubstituted template parameter" error. Per instantiation a CONCRETE clone is
// materialized (subst_datadef) and emitted instead. (g++ treats a local class in a
// template as dependent — instantiated WITH the enclosing template.)
static bool struct_has_dependent_member(DataDefSTRUCT *sdd)
{
	if (!sdd)
		return false;
	for (auto &m : sdd->members)
		if (template_param_under_type_layers(m.second))
			return true;
	return false;
}

static std::string tsubst_datadef_key(DataDef *dd,
				      const std::map<DataDef *, DataDef *> &subst,
				      std::set<DataDef *> &seen)
{
	if (!dd)
		return "<null>";
	std::map<DataDef *, DataDef *>::const_iterator it = subst.find(dd);
	if (it != subst.end())
		return tsubst_datadef_key(it->second, subst, seen);
	if (DataDefREF *rd = dynamic_cast<DataDefREF *>(dd))
		return "ref(" + tsubst_datadef_key(rd->base_type, subst, seen) + ")";
	if (DataDefPTR *pd = dynamic_cast<DataDefPTR *>(dd))
		return "ptr(" + tsubst_datadef_key(pd->base_type, subst, seen) + ")";
	if (DataDefCONST *cd = dynamic_cast<DataDefCONST *>(dd))
		return "const(" + tsubst_datadef_key(cd->base_type, subst, seen) + ")";
	if (DataDefCArray *ad = dynamic_cast<DataDefCArray *>(dd)) {
		std::ostringstream os;
		os << "arr[" << ad->count << "]("
		   << tsubst_datadef_key(ad->element_type, subst, seen) << ")";
		return os.str();
	}
	if (DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd)) {
		if (struct_has_dependent_member(sdd) && seen.insert(dd).second) {
			std::ostringstream os;
			os << "agg@" << dd << ":" << sdd->name << "{";
			for (size_t i = 0; i < sdd->members.size(); ++i) {
				if (i) os << ",";
				os << sdd->members[i].first << ":"
				   << tsubst_datadef_key(sdd->members[i].second,
							 subst, seen);
			}
			os << "}";
			seen.erase(dd);
			return os.str();
		}
	}
	return dd->name;
}

static std::string tsubst_local_aggregate_key(
	DataDefSTRUCT *sdd, const std::map<DataDef *, DataDef *> &subst)
{
	std::set<DataDef *> seen;
	return tsubst_datadef_key(sdd, subst, seen);
}

static std::string sanitize_c_identifier(const std::string &s)
{
	std::string out;
	for (char ch : s) {
		if ((ch >= 'a' && ch <= 'z') || (ch >= 'A' && ch <= 'Z')
		    || (ch >= '0' && ch <= '9') || ch == '_')
			out.push_back(ch);
		else
			out.push_back('_');
	}
	if (out.empty() || !((out[0] >= 'a' && out[0] <= 'z')
			 || (out[0] >= 'A' && out[0] <= 'Z')
			 || out[0] == '_'))
		out.insert(out.begin(), '_');
	return out;
}

static std::string tsubst_local_aggregate_name(
	Program *prog, DataDefSTRUCT *sdd, const std::string &key)
{
	unsigned long h = 5381;
	for (char ch : key)
		h = ((h << 5) + h) ^ (unsigned char)ch;
	std::ostringstream os;
	os << sanitize_c_identifier(sdd && !sdd->name.empty()
				    ? sdd->name : std::string("__local"))
	   << "__tsubst_" << std::hex << h;
	std::string base = os.str();
	std::string name = base;
	for (unsigned n = 2; prog && prog->struct_map.count(name); ++n) {
		std::ostringstream alt;
		alt << base << "_" << n;
		name = alt.str();
	}
	return name;
}

static Variable *tsubst_local_class_own_dtor(DataDefCLASS *cdd)
{
	if (!cdd)
		return NULL;
	for (auto &kv : cdd->method_map) {
		if (kv.first.empty() || kv.first[0] != '~' || !kv.second)
			continue;
		if (std::find(cdd->methods.begin(), cdd->methods.end(), kv.second)
		    != cdd->methods.end())
			return kv.second;
	}
	return NULL;
}

static bool tsubst_local_function_body_empty(Program *prog, Variable *var)
{
	if (!prog || !var)
		return false;
	FuncDef *fd = dynamic_cast<FuncDef *>(var->type);
	if (!fd || fd->declaration_only || fd->defaulted_or_deleted)
		return false;
	auto matches_empty_body = [&](TokenFunc *tf) {
		if (!tf || tf->is_overridden)
			return false;
		FuncDef *tfd = dynamic_cast<FuncDef *>(tf->var.type);
		return tfd == fd && tf->statements.empty()
		    && tf->deferred.empty();
	};
	for (TokenBase *pb : prog->pending_funcs) {
		if (matches_empty_body(dynamic_cast<TokenFunc *>(pb)))
			return true;
	}
	std::queue<TokenBase *> q = prog->ast;
	while (!q.empty()) {
		TokenBase *tb = q.front();
		q.pop();
		if (matches_empty_body(dynamic_cast<TokenFunc *>(tb)))
			return true;
	}
	return false;
}

static bool tsubst_local_class_has_only_empty_dtor(Program *prog,
						  DataDefCLASS *cdd)
{
	if (!prog || !cdd || !cdd->has_user_dtor)
		return false;
	if (cdd->methods.size() != 1 || cdd->method_map.size() != 1)
		return false;
	Variable *dtor = tsubst_local_class_own_dtor(cdd);
	if (!dtor || cdd->methods[0] != dtor)
		return false;
	return tsubst_local_function_body_empty(prog, dtor);
}

static bool tsubst_local_class_clone_supported(Program *prog, DataDefCLASS *cdd)
{
	if (!cdd)
		return true;
	bool only_empty_dtor =
		tsubst_local_class_has_only_empty_dtor(prog, cdd);
	if ((!cdd->methods.empty() && !only_empty_dtor) || !cdd->ctors.empty()
	    || !cdd->staticconst.empty()
	    || (!cdd->method_map.empty() && !only_empty_dtor)
	    || !cdd->bases.empty() || cdd->base_class
	    || cdd->has_user_ctor
	    || (cdd->has_user_dtor && !only_empty_dtor)
	    || cdd->has_vtable || cdd->has_vptr_slot
	    || cdd->extern_ctor || cdd->extern_dtor)
		return false;
	return true;
}

static bool clone_local_aggregate_members(
	Program *prog, DataDefSTRUCT *src, DataDefSTRUCT *dst,
	const std::map<DataDef *, DataDef *> &subst)
{
	if (!src || !dst)
		return false;
	if (src->has_anon_aggregate || src->has_runtime_size())
		return false;

	dst->canonical_cpp_spelling = dst->name;
	dst->runtime_size_expr = NULL;
	dst->pack = src->pack;
	dst->tag_explicit_align = src->tag_explicit_align;
	dst->union_layout = src->union_layout;
	dst->is_complete = true;
	dst->is_anonymous = false;
	dst->reverse_scalar_storage = src->reverse_scalar_storage;

	bool substituted_object_member = false;
	for (size_t i = 0; i < src->members.size(); ++i) {
		const memberpair_t &m = src->members[i];
		DataDef *mt = subst_datadef(prog, m.second, subst);
		if (!mt || template_param_under_type_layers(mt))
			return false;
		if (DataDefSTRUCT *ms = dynamic_cast<DataDefSTRUCT *>(mt))
			if (struct_has_dependent_member(ms))
				return false;
		if (mt != m.second && mt->is_object() && !mt->is_pointer())
			substituted_object_member = true;

		size_t cnt = i < src->member_counts.size()
			   ? src->member_counts[i] : 1;
		TokenBase *count_expr = i < src->member_count_exprs.size()
				      ? src->member_count_exprs[i] : NULL;
		if (count_expr)
			return false;
		bool is_array = i < src->member_array_flags.size()
			      ? src->member_array_flags[i] : false;
		const std::vector<carray_dim_t> *dims = NULL;
		if (i < src->member_dims.size() && !src->member_dims[i].empty())
			dims = &src->member_dims[i];

		const DataDefSTRUCT::BitFieldInfo *bf =
			i < src->member_bitfields.size()
			? &src->member_bitfields[i] : NULL;
		if (bf && bf->is_bitfield)
			dst->addBitField(m.first, *mt, bf->bit_width);
		else
			dst->addMember(m.first, *mt, cnt, NULL, is_array, dims);
		if (dst->members.empty())
			return false;
		dst->members.back().typedef_name =
			(mt == m.second) ? m.typedef_name : std::string();
		dst->members.back().origin = m.origin;
		size_t di = dst->members.size() - 1;
		if (i < src->member_access.size()) {
			if (dst->member_access.size() <= di)
				dst->member_access.resize(di + 1, 0);
			dst->member_access[di] = src->member_access[i];
		}
		if (i < src->member_origin.size()) {
			if (dst->member_origin.size() <= di)
				dst->member_origin.resize(di + 1, -1);
			dst->member_origin[di] = src->member_origin[i];
		}
		std::map<size_t, size_t>::const_iterator ai =
			src->member_explicit_align.find(i);
		if (ai != src->member_explicit_align.end())
			dst->apply_member_alignment(ai->second);
		std::map<std::string, TokenBase *>::const_iterator dii =
			src->member_default_inits.find(m.first);
		if (dii != src->member_default_inits.end())
			dst->member_default_inits[m.first] = dii->second;
	}

	if (!dynamic_cast<DataDefCLASS *>(src) && substituted_object_member)
		return false;
	if (src->tag_explicit_align > dst->max_align)
		dst->max_align = src->tag_explicit_align;
	if (src->reverse_scalar_storage)
		dst->setReverseScalarStorage(true);
	return true;
}

static DataDefSTRUCT *materialize_local_aggregate_datadef(
	Program *prog, DataDefSTRUCT *sdd,
	const std::map<DataDef *, DataDef *> &subst)
{
	if (!prog || !sdd || !struct_has_dependent_member(sdd))
		return sdd;
	std::string key = tsubst_local_aggregate_key(sdd, subst);
	std::map<std::string, DataDefSTRUCT *>::iterator mi =
		prog->tsubst_local_aggregate_map.find(key);
	if (mi != prog->tsubst_local_aggregate_map.end())
		return mi->second;

	DataDefCLASS *src_class = dynamic_cast<DataDefCLASS *>(sdd);
	if (!tsubst_local_class_clone_supported(prog, src_class))
		return NULL;

	std::string name = tsubst_local_aggregate_name(prog, sdd, key);
	DataDefSTRUCT *clone = src_class
		? (DataDefSTRUCT *)new DataDefCLASS(name, 0, src_class->rawtype())
		: new DataDefSTRUCT(name, 0, sdd->rawtype());
	if (!clone_local_aggregate_members(prog, sdd, clone, subst))
		return NULL;

	if (DataDefCLASS *clone_class = dynamic_cast<DataDefCLASS *>(clone)) {
		clone_class->member_origin.resize(clone_class->members.size(), -1);
		clone_class->compute_layout();
		clone_class->apply_member_layout();
		clone_class->build_vtable_groups();
		clone_class->is_complete = true;
	} else {
		clone->finalize();
	}

	prog->struct_map[clone->name] = clone;
	prog->datatype_map[clone->name] =
		new TokenDataType(clone->name.c_str(), *clone);
	prog->tsubst_local_aggregate_map[key] = clone;
	return clone;
}

// Collect every distinct DataDef referenced as a `node->datadef` anywhere in a
// cir subtree. Used to discover the Tree-1 pattern's LOCAL classes (their type
// appears on the local-var decl node) so they can be mapped to their concrete
// per-instantiation counterparts before the tsubst copy runs.
static void collect_cir_node_datadefs(node_t n, std::set<DataDef *> &out)
{
	if (!n)
		return;
	cir_node *cn = CIR_NODE(n);
	if (cn->datadef)
		out.insert(cn->datadef);
	if (n->code > N_ID)
		for (int i = 0; ; i++) {
			node_t op = c2mir_node_op(n, i);
			if (!op)
				break;
			collect_cir_node_datadefs(op, out);
		}
}

// Pack-window name lookups for the shell-origin rebuild: the recorded arg
// tokens name template parameters by SPELLING (`_Args1`), while the binding /
// pack window key by placeholder DataDef — bridge by the placeholder's name.
static const std::vector<DataDef *> *tsubst_pack_for_name(
	const std::vector<std::vector<DataDef *> > *packs,
	const std::map<unsigned, DataDef *> *pack_params,
	const char *name)
{
	if (!packs || !pack_params || !name)
		return NULL;
	for (std::map<unsigned, DataDef *>::const_iterator it =
		     pack_params->begin(); it != pack_params->end(); ++it)
		if (it->second && it->second->name == name)
			return it->first < packs->size()
				   ? &(*packs)[it->first] : NULL;
	return NULL;
}

static DataDef *tsubst_scalar_for_name(
	const std::map<DataDef *, DataDef *> &subst, const char *name)
{
	if (!name)
		return NULL;
	for (std::map<DataDef *, DataDef *>::const_iterator it = subst.begin();
	     it != subst.end(); ++it)
		if (it->first && it->first->is_template_param()
		    && it->first->name == name)
			return it->second;
	return NULL;
}

// Decompose a concrete type into the structural tokens the template-argument
// grammar consumes (`const` qualifier + core type token + `*`/`&` declarator
// suffixes), so the replay resolves through the SAME qualifier/fold path as
// source text and lands on the SAME instantiation key. A composite
// TokenDataType wrapping the derived type would spell through the wrapper's
// (empty-canonical) name and FORK the key — one logical type under two
// datatype_map entries (the empty-pack-key lesson). False = a qualifier nest
// this decomposition doesn't model; the caller falls back to the composite.
static bool tsubst_decompose_elem_tokens(DataDef *elem,
					 std::vector<TokenBase *> &run)
{
	if (!elem)
		return false;
	bool is_ref = false;
	int ptr_depth = 0;
	DataDef *core = elem;
	if (DataDefREF *r = dynamic_cast<DataDefREF *>(core)) {
		is_ref = true;
		core = r->base_type;
	}
	while (core && !dynamic_cast<DataDefREF *>(core)) {
		DataDefPTR *p = dynamic_cast<DataDefPTR *>(core);
		if (!p)
			break;
		++ptr_depth;
		core = p->base_type;
	}
	bool is_const = false;
	if (DataDefCONST *c = dynamic_cast<DataDefCONST *>(core)) {
		is_const = true;
		core = c->base_type;
	}
	if (!core || dynamic_cast<DataDefCONST *>(core)
	    || dynamic_cast<DataDefPTR *>(core)
	    || dynamic_cast<DataDefTemplateParam *>(core))
		return false;
	if (is_const)
		run.push_back(new TokenCONST());
	const std::string &cs = core->canonical_cpp_spelling;
	run.push_back(new TokenDataType(
		(cs.empty() ? core->name : cs).c_str(), *core));
	for (int i = 0; i < ptr_depth; ++i)
		run.push_back(new TokenMul());
	if (is_ref)
		run.push_back(new TokenBand());
	return true;
}

// Substitute ONE recorded shell-origin arg-token run under the binding/pack
// window into concrete replay runs (a pack-name run fans out to one run per
// element). Appends to `runs_out`; sets `any_subst` when a substitution
// actually happened. Returns false when a template-parameter name in the run
// cannot be resolved (the rebuild must then bail — an unresolved name would
// replay as a stray identifier). Failure paths do NOT delete built tokens:
// cloned keywords are shared prototypes (TokenKeyword::clone returns this) —
// leak-tolerant like every parser replay.
static bool tsubst_subst_origin_arg_run(
	const std::vector<TokenBase *> &raw,
	const std::map<DataDef *, DataDef *> &subst,
	const std::vector<std::vector<DataDef *> > *packs,
	const std::map<unsigned, DataDef *> *pack_params,
	std::vector<std::vector<TokenBase *> > &runs_out,
	bool &any_subst)
{
	// Whole-run bare pack (`_Args1` — a trailing `...` may or may not have
	// been recorded with it): fan out to one concrete type run per element.
	{
		TokenBase *first = NULL;
		bool rest_dots = true;
		for (TokenBase *t : raw) {
			if (!t)
				continue;
			if (!first)
				first = t;
			else if (t->id() != TokenID::tkDot)
				rest_dots = false;
		}
		TokenIdent *fid = dynamic_cast<TokenIdent *>(first);
		if (fid && rest_dots && first->type() == TokenType::ttIdentifier) {
			if (const std::vector<DataDef *> *pk =
				    tsubst_pack_for_name(packs, pack_params,
							 fid->spelling())) {
				for (DataDef *elem : *pk) {
					if (!elem)
						return false;
					std::vector<TokenBase *> erun;
					if (!tsubst_decompose_elem_tokens(elem,
									  erun))
						erun.assign(1,
							new TokenDataType(
								elem->name.c_str(),
								*elem));
					runs_out.push_back(erun);
				}
				any_subst = true;
				return true;
			}
		}
	}
	std::vector<TokenBase *> out;
	bool ok = true;
	for (size_t k = 0; k < raw.size() && ok; ++k) {
		TokenBase *t = raw[k];
		if (!t)
			continue;
		// Idents AND keywords (`sizeof` is a TokenKeyword, type()
		// ttKeyword — still a TokenIdent by inheritance).
		TokenIdent *anyid = dynamic_cast<TokenIdent *>(t);
		TokenIdent *tid = (anyid
				   && t->type() == TokenType::ttIdentifier)
				      ? anyid : NULL;
		// `sizeof ... ( PACK )` -> the pack's arity as a literal int
		// (so instantiate_template_use's own __integer_pack expander
		// folds the enclosing `__integer_pack ( N ) ...`).
		if (anyid && anyid->spelling_is("sizeof") && k + 6 < raw.size()
		    && raw[k+1] && raw[k+1]->id() == TokenID::tkDot
		    && raw[k+2] && raw[k+2]->id() == TokenID::tkDot
		    && raw[k+3] && raw[k+3]->id() == TokenID::tkDot
		    && raw[k+4] && raw[k+4]->id() == TokenID::tkOpBrk
		    && raw[k+5] && raw[k+5]->type() == TokenType::ttIdentifier
		    && raw[k+6] && raw[k+6]->id() == TokenID::tkClBrk) {
			TokenIdent *pid = dynamic_cast<TokenIdent *>(raw[k+5]);
			const std::vector<DataDef *> *pk = pid
				? tsubst_pack_for_name(packs, pack_params,
						       pid->spelling())
				: NULL;
			if (!pk) {
				ok = false;
				break;
			}
			out.push_back(new TokenInt((int64_t)pk->size()));
			any_subst = true;
			k += 6;
			continue;
		}
		if (tid) {
			if (DataDef *sc = tsubst_scalar_for_name(
					subst, tid->spelling())) {
				std::vector<TokenBase *> srun;
				if (!tsubst_decompose_elem_tokens(sc, srun))
					srun.assign(1, new TokenDataType(
						sc->name.c_str(), *sc));
				out.insert(out.end(), srun.begin(),
					   srun.end());
				any_subst = true;
				continue;
			}
			// A pack name anywhere but the whole-run / sizeof...
			// shapes above is a form this walker cannot expand.
			if (tsubst_pack_for_name(packs, pack_params,
						 tid->spelling())) {
				ok = false;
				break;
			}
		}
		out.push_back(t->clone());
	}
	if (!ok)
		return false;
	runs_out.push_back(out);
	return true;
}

// The structural dependent template-id case (road (i)): a shell whose
// DependentShellOrigin was recorded at creation rebuilds its CONCRETE
// instantiation by substituting the recorded arg runs and replaying them
// through the parser's instantiation seam. NULL = not rebuildable here
// (no record, unresolvable run, nothing substituted, or replay failure) —
// the caller keeps the shell and its own bail semantics.
static DataDef *rebuild_dependent_shell(Program *prog, DataDefCLASS *shell,
	const std::map<DataDef *, DataDef *> &subst,
	const std::vector<std::vector<DataDef *> > *packs,
	const std::map<unsigned, DataDef *> *pack_params)
{
	std::map<DataDef *, Program::DependentShellOrigin>::const_iterator oit =
		prog->dependent_shell_origin.find(shell);
	if (oit == prog->dependent_shell_origin.end())
		return NULL;
	const Program::DependentShellOrigin &org = oit->second;
	std::vector<std::vector<TokenBase *> > runs;
	bool any_subst = false;
	bool ok = true;
	for (const std::vector<TokenBase *> &raw : org.raw_arg_tokens)
		if (!tsubst_subst_origin_arg_run(raw, subst, packs, pack_params,
						 runs, any_subst)) {
			ok = false;
			break;
		}
	if (!ok || !any_subst)
		return NULL;
	TokenDataType *real = prog->instantiate_shell_origin_replay(org, runs);
	if (getenv("MADC_XTEST_PAT_MEMINIT_DEBUG"))
		fprintf(stderr, "[SHELL-REBUILD] %s (%zu runs) -> %s\n",
			shell->name.c_str(), runs.size(),
			real ? real->definition.name.c_str() : "(failed)");
	if (!real || &real->definition == (DataDef *)shell)
		return NULL;
	return &real->definition;
}

static DataDef *subst_datadef(Program *prog, DataDef *dd,
			      const std::map<DataDef *, DataDef *> &subst,
			      const std::vector<std::vector<DataDef *> > *packs,
			      const std::map<unsigned, DataDef *> *pack_params)
{
	if (!dd)
		return dd;
	std::map<DataDef *, DataDef *>::const_iterator it = subst.find(dd);
	if (it != subst.end())
		return it->second;
	if (DataDefREF *rd = dynamic_cast<DataDefREF *>(dd)) {
		DataDef *nb = subst_datadef(prog, rd->base_type, subst,
					    packs, pack_params);
		return (prog && nb != rd->base_type)
			   ? (DataDef *)prog->getReferenceType(nb) : dd;
	}
	if (DataDefPTR *pd = dynamic_cast<DataDefPTR *>(dd)) {
		DataDef *nb = subst_datadef(prog, pd->base_type, subst,
					    packs, pack_params);
		return (prog && nb != pd->base_type)
			   ? (DataDef *)prog->getPointerType(nb) : dd;
	}
	if (DataDefCONST *cd = dynamic_cast<DataDefCONST *>(dd)) {
		DataDef *nb = subst_datadef(prog, cd->base_type, subst,
					    packs, pack_params);
		return (prog && nb != cd->base_type)
			   ? (DataDef *)prog->getConstType(nb) : dd;
	}
	if (DataDefCArray *ad = dynamic_cast<DataDefCArray *>(dd)) {
		DataDef *nb = subst_datadef(prog, ad->element_type, subst,
					    packs, pack_params);
		if (nb != ad->element_type)
			return new DataDefCArray(*nb, nb->name, ad->count,
						 ad->count_expr);
		return dd;
	}
	if (DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd)) {
		if (struct_has_dependent_member(sdd)) {
			DataDefSTRUCT *clone =
				materialize_local_aggregate_datadef(prog, sdd, subst);
			return clone ? (DataDef *)clone : dd;
		}
	}
	if (prog) {
		DataDefCLASS *shell = dynamic_cast<DataDefCLASS *>(dd);
		if (shell && shell->is_dependent_placeholder) {
			DataDef *conc = rebuild_dependent_shell(
				prog, shell, subst, packs, pack_params);
			if (conc)
				return conc;
		}
	}
	return dd;
}

DataDef *CirBuilder::subst_datadef_active(DataDef *dd,
					  const std::map<DataDef *, DataDef *> &subst)
{
	return subst_datadef(m_prog, dd, subst,
			     m_tsubst_active_type_arg_packs,
			     &m_tsubst_active_pack_params);
}

static void collect_pack_params_in_pattern(
	TokenBase *tb, std::vector<DataDefTemplateParam *> &out);

// Lockstep pack binding ([temp.variadic]): every pack mentioned in one
// expansion pattern expands in lockstep, so element `elem` binds EVERY such
// pack's param to that pack's elem-th window entry — including packs that
// appear only in a nested call's explicit template args (`std::get<_Indexes1>`,
// a NON-TYPE index pack the primary-pack walker never sees). Returns false on
// a lockstep arity violation (a mentioned pack with no elem-th element) —
// the relower must clean-fail, not mis-lower.
bool CirBuilder::tsubst_bind_lockstep_packs(
	TokenBase *pattern, size_t elem,
	std::map<DataDef *, DataDef *> &elem_subst)
{
	if (!m_tsubst_active_type_arg_packs)
		return true;
	std::vector<DataDefTemplateParam *> tps;
	collect_pack_params_in_pattern(pattern, tps);
	for (DataDefTemplateParam *tp : tps) {
		if (tp->param_index >= m_tsubst_active_type_arg_packs->size())
			continue;
		std::map<unsigned, DataDef *>::iterator pmi =
			m_tsubst_active_pack_params.find(tp->param_index);
		if (pmi == m_tsubst_active_pack_params.end() || !pmi->second)
			continue;	// scalar param — bound via the base subst
		const std::vector<DataDef *> &elems =
			(*m_tsubst_active_type_arg_packs)[tp->param_index];
		if (elem >= elems.size() || !elems[elem])
			return false;
		elem_subst[pmi->second] = elems[elem];
	}
	return true;
}

static DataDefTemplateParam *template_param_in_pack_pattern(TokenBase *tb)
{
	if (!tb)
		return NULL;
	if (DataDefTemplateParam *tp =
		    template_param_under_type_layers(tb->datadef()))
		return tp;
	if (TokenPackExpansion *pe = dynamic_cast<TokenPackExpansion *>(tb))
		return template_param_in_pack_pattern(pe->pattern);
	if (TokenCallFunc *tc = dynamic_cast<TokenCallFunc *>(tb)) {
		for (TokenBase *p : tc->parameters)
			if (DataDefTemplateParam *tp =
				    template_param_in_pack_pattern(p))
				return tp;
		if (DataDefTemplateParam *tp =
			    template_param_in_pack_pattern(tc->src_node))
			return tp;
		if (TokenMember *tm = dynamic_cast<TokenMember *>(tc))
			return template_param_in_pack_pattern(tm->parent_expr);
		return NULL;
	}
	if (TokenOperator *op = dynamic_cast<TokenOperator *>(tb)) {
		if (DataDefTemplateParam *tp =
			    template_param_in_pack_pattern(op->left))
			return tp;
		if (DataDefTemplateParam *tp =
			    template_param_in_pack_pattern(op->right))
			return tp;
		if (TokenTerQ *tq = dynamic_cast<TokenTerQ *>(op)) {
			if (DataDefTemplateParam *tp =
				    template_param_in_pack_pattern(tq->condition))
				return tp;
			if (DataDefTemplateParam *tp =
				    template_param_in_pack_pattern(tq->true_expr))
				return tp;
			return template_param_in_pack_pattern(tq->false_expr);
		}
		return NULL;
	}
	if (TokenCast *tc = dynamic_cast<TokenCast *>(tb))
		return template_param_in_pack_pattern(tc->expr);
	if (TokenAddrExpr *ta = dynamic_cast<TokenAddrExpr *>(tb))
		return template_param_in_pack_pattern(ta->expr);
	if (TokenDerefExpr *td = dynamic_cast<TokenDerefExpr *>(tb))
		return template_param_in_pack_pattern(td->expr);
	if (TokenSubscriptExpr *ts = dynamic_cast<TokenSubscriptExpr *>(tb)) {
		if (DataDefTemplateParam *tp =
			    template_param_in_pack_pattern(ts->base_expr))
			return tp;
		return template_param_in_pack_pattern(ts->index);
	}
	if (TokenSubscript *ts = dynamic_cast<TokenSubscript *>(tb)) {
		if (DataDefTemplateParam *tp =
			    template_param_in_pack_pattern(ts->index))
			return tp;
		for (TokenBase *e : ts->extra_indices)
			if (DataDefTemplateParam *tp =
				    template_param_in_pack_pattern(e))
				return tp;
	}
	return NULL;
}

static void collect_pack_param_dd(DataDef *dd,
				  std::vector<DataDefTemplateParam *> &out)
{
	DataDefTemplateParam *tp = template_param_under_type_layers(dd);
	if (!tp)
		return;
	for (DataDefTemplateParam *have : out)
		if (have == tp)
			return;
	out.push_back(tp);
}

// Collect EVERY template param mentioned in an expansion pattern — unlike
// template_param_in_pack_pattern (first-match, legacy traversal order, no
// explicit-template-arg descent), this also walks each nested call's
// explicit_template_args, where a NON-TYPE index pack (`std::get<_Indexes1>`)
// appears without ever being a token datadef. Lockstep expansion
// ([temp.variadic]) needs all of them bound per element.
static void collect_pack_params_in_pattern(TokenBase *tb,
					   std::vector<DataDefTemplateParam *> &out)
{
	if (!tb)
		return;
	collect_pack_param_dd(tb->datadef(), out);
	if (TokenPackExpansion *pe = dynamic_cast<TokenPackExpansion *>(tb)) {
		collect_pack_params_in_pattern(pe->pattern, out);
		return;
	}
	if (TokenCallFunc *tc = dynamic_cast<TokenCallFunc *>(tb)) {
		for (DataDef *ea : tc->explicit_template_args)
			collect_pack_param_dd(ea, out);
		for (TokenBase *p : tc->parameters)
			collect_pack_params_in_pattern(p, out);
		collect_pack_params_in_pattern(tc->src_node, out);
		if (TokenMember *tm = dynamic_cast<TokenMember *>(tc))
			collect_pack_params_in_pattern(tm->parent_expr, out);
		return;
	}
	if (TokenOperator *op = dynamic_cast<TokenOperator *>(tb)) {
		collect_pack_params_in_pattern(op->left, out);
		collect_pack_params_in_pattern(op->right, out);
		if (TokenTerQ *tq = dynamic_cast<TokenTerQ *>(op)) {
			collect_pack_params_in_pattern(tq->condition, out);
			collect_pack_params_in_pattern(tq->true_expr, out);
			collect_pack_params_in_pattern(tq->false_expr, out);
		}
		return;
	}
	if (TokenCast *tcst = dynamic_cast<TokenCast *>(tb)) {
		collect_pack_params_in_pattern(tcst->expr, out);
		return;
	}
	if (TokenAddrExpr *ta = dynamic_cast<TokenAddrExpr *>(tb)) {
		collect_pack_params_in_pattern(ta->expr, out);
		return;
	}
	if (TokenDerefExpr *td = dynamic_cast<TokenDerefExpr *>(tb)) {
		collect_pack_params_in_pattern(td->expr, out);
		return;
	}
	if (TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(tb)) {
		collect_pack_params_in_pattern(tse->base_expr, out);
		collect_pack_params_in_pattern(tse->index, out);
		return;
	}
	if (TokenSubscript *tss = dynamic_cast<TokenSubscript *>(tb)) {
		collect_pack_params_in_pattern(tss->index, out);
		for (TokenBase *e : tss->extra_indices)
			collect_pack_params_in_pattern(e, out);
	}
}

static bool tsubst_destroy_call_has_template_pointee(TokenCallFunc *tc)
{
	if (!tc || tc->var.name != "__destroy" || tc->parameters.size() != 1)
		return false;
	TokenBase *arg = tc->parameters[0];
	DataDef *argdd = arg ? arg->datadef() : NULL;
	DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(argdd);
	return pdd && template_param_under_type_layers(pdd->base_type);
}

static bool tsubst_inline_destroy_call(TokenCallFunc *tc)
{
	if (!tc || tc->parameters.size() != 1)
		return false;
	FuncDef *fd = dynamic_cast<FuncDef *>(tc->var.type);
	return fd && fd->inline_builtin_kind == "destroy";
}

static bool tsubst_destroy_marker_call(TokenCallFunc *tc)
{
	return tsubst_destroy_call_has_template_pointee(tc)
	    || tsubst_inline_destroy_call(tc);
}

static DataDef *tsubst_destroy_marker_datadef(TokenBase *arg, DataDef *argdd)
{
	DataDef *marker_dd = argdd;
	if (!template_param_under_type_layers(marker_dd)) {
		if (DataDefTemplateParam *tp = template_param_in_pack_pattern(arg))
			marker_dd = tp;
	}
	return template_param_under_type_layers(marker_dd) ? marker_dd : NULL;
}

static DataDef *tsubst_explicit_dtor_marker_datadef(TokenExplicitDtor *td)
{
	if (!td || td->dtor_class || !td->obj)
		return NULL;
	DataDef *objdd = td->obj->datadef();
	return template_param_under_type_layers(objdd) ? objdd : NULL;
}

static std::string pack_value_name_in_pattern(TokenBase *tb,
					      unsigned pack_index)
{
	if (!tb)
		return std::string();
	if (TokenPackExpansion *pe = dynamic_cast<TokenPackExpansion *>(tb))
		return pack_value_name_in_pattern(pe->pattern, pack_index);
	if (TokenCallFunc *tc = dynamic_cast<TokenCallFunc *>(tb)) {
		for (TokenBase *p : tc->parameters) {
			std::string n = pack_value_name_in_pattern(p, pack_index);
			if (!n.empty())
				return n;
		}
		std::string n = pack_value_name_in_pattern(tc->src_node,
							   pack_index);
		if (!n.empty())
			return n;
		if (TokenMember *tm = dynamic_cast<TokenMember *>(tc))
			return pack_value_name_in_pattern(tm->parent_expr,
							  pack_index);
		return std::string();
	}
	if (tb->type() == TokenType::ttVariable) {
		DataDefTemplateParam *tp =
			template_param_under_type_layers(tb->datadef());
		if (tp && tp->param_index == pack_index) {
			if (TokenVar *tv = dynamic_cast<TokenVar *>(tb))
				return tv->var.name;
			if (TokenIdent *ti = dynamic_cast<TokenIdent *>(tb))
				return ti->spelling();
		}
	}
	if (TokenOperator *op = dynamic_cast<TokenOperator *>(tb)) {
		std::string n = pack_value_name_in_pattern(op->left,
							   pack_index);
		if (!n.empty())
			return n;
		n = pack_value_name_in_pattern(op->right, pack_index);
		if (!n.empty())
			return n;
		if (TokenTerQ *tq = dynamic_cast<TokenTerQ *>(op)) {
			n = pack_value_name_in_pattern(tq->condition, pack_index);
			if (!n.empty())
				return n;
			n = pack_value_name_in_pattern(tq->true_expr, pack_index);
			if (!n.empty())
				return n;
			return pack_value_name_in_pattern(tq->false_expr,
							  pack_index);
		}
		return std::string();
	}
	if (TokenCast *tc = dynamic_cast<TokenCast *>(tb))
		return pack_value_name_in_pattern(tc->expr, pack_index);
	if (TokenAddrExpr *ta = dynamic_cast<TokenAddrExpr *>(tb))
		return pack_value_name_in_pattern(ta->expr, pack_index);
	if (TokenDerefExpr *td = dynamic_cast<TokenDerefExpr *>(tb))
		return pack_value_name_in_pattern(td->expr, pack_index);
	if (TokenSubscriptExpr *ts = dynamic_cast<TokenSubscriptExpr *>(tb)) {
		std::string n = pack_value_name_in_pattern(ts->base_expr,
							   pack_index);
		if (!n.empty())
			return n;
		return pack_value_name_in_pattern(ts->index, pack_index);
	}
	if (TokenSubscript *ts = dynamic_cast<TokenSubscript *>(tb)) {
		std::string n = pack_value_name_in_pattern(ts->index,
							   pack_index);
		if (!n.empty())
			return n;
		for (TokenBase *e : ts->extra_indices) {
			n = pack_value_name_in_pattern(e, pack_index);
			if (!n.empty())
				return n;
		}
	}
	return std::string();
}

static bool tsubst_args_have_pack_expansion(
	const std::vector<TokenBase *> &args)
{
	for (TokenBase *arg : args)
		if (dynamic_cast<TokenPackExpansion *>(arg)
		    && template_param_in_pack_pattern(arg))
			return true;
	return false;
}

static bool cir_id_spells(cir_node *n, const char *name)
{
	return n && name && n->base.code == N_ID && n->base.u.s.s
	    && strcmp(n->base.u.s.s, name) == 0;
}

static bool attr_list_has_cleanup(node_t attrs)
{
	if (!attrs || attrs->code != N_LIST)
		return false;
	for (node_t a = c2mir_node_first_op(attrs); a;
	     a = c2mir_node_next_op(a)) {
		if (!a || a->code != N_ATTR)
			continue;
		node_t name = c2mir_node_op(a, 0);
		if (cir_id_spells(CIR_NODE(name), "cleanup"))
			return true;
	}
	return false;
}

static bool node_is_deref_of_id(node_t n, const char *name)
{
	if (!n || n->code != N_DEREF)
		return false;
	node_t op = c2mir_node_first_op(n);
	return cir_id_spells(CIR_NODE(op), name);
}

static DataDefCLASS *as_class_instance(DataDef *dd);
static DataDefCLASS *class_behind(DataDef *dd);
static DataDefCLASS *param_object_class(DataDef *dd, bool refp);
static DataDef *ref_param_referent(DataDef *pt);
static bool tsubst_is_class_object_arg(DataDef *dd);
static bool is_c2mir_builtin_call_name(const std::string &name);

std::string CirBuilder::copied_pack_value_name(const char *name) const
{
	if (!name)
		return std::string();
	if (m_tsubst_copy_pack_index < 0 || !m_tsubst_copy_pack_value_name)
		return std::string(name);
	if (strcmp(name, m_tsubst_copy_pack_value_name) != 0)
		return std::string(name);
	bool multi = true;
	if (m_tsubst_active_type_arg_packs
	    && (size_t)m_tsubst_copy_pack_index
	       < m_tsubst_active_type_arg_packs->size())
		multi = (*m_tsubst_active_type_arg_packs)
			[(size_t)m_tsubst_copy_pack_index].size() > 1;
	std::string nm = std::string(name);
	if (multi)
		nm += "__" + std::to_string(m_tsubst_copy_pack_elem);
	return nm;
}

void CirBuilder::rename_copied_pack_value_id(cir_node *src, cir_node *dst)
{
	if (!src || !dst || src->base.code != N_ID || !src->base.u.s.s)
		return;
	std::string nm = copied_pack_value_name(src->base.u.s.s);
	if (nm == src->base.u.s.s)
		return;
	const char *interned = c2mir_uniq_str(c2m, nm.c_str(), nm.size() + 1);
	dst->base.u.s.s = interned;
	dst->base.u.s.len = nm.size() + 1;
}

static bool tsubst_call_can_rewrite_after_subst(TokenCallFunc *tcf)
{
	if (!tcf)
		return false;
	FuncDef *fd = dynamic_cast<FuncDef *>(tcf->var.type);
	return fd && (!fd->function_display_name.empty()
		      || !fd->method_display_name.empty());
}

static bool tsubst_call_has_pack_expansion_arg(TokenCallFunc *tcf)
{
	if (!tcf)
		return false;
	for (TokenBase *arg : tcf->parameters)
		if (dynamic_cast<TokenPackExpansion *>(arg))
			return true;
	return false;
}

static const DataDefCLASS *class_pointer_pointee(const DataDef *dd);

node_t CirBuilder::copied_reference_slot_arg(TokenBase *arg, node_t src_arg,
					     DataDef *formal, bool refp)
{
	TokenVar *tv = dynamic_cast<TokenVar *>(arg);
	if (!tv || !tv->var.is_reference() || !tv->var.type || !refp)
		return NULL;
	if (!template_param_under_type_layers(tv->var.type))
		return NULL;
	if (!node_is_deref_of_id(src_arg, tv->var.name.c_str()))
		return NULL;
	std::string nm = copied_pack_value_name(tv->var.name.c_str());
	node_t slot = id(nm.c_str(), arg);
	if (formal && formal->is_pointer())
		return node2(N_CAST, ptr_type_node(formal), slot, arg);
	return slot;
}

node_t CirBuilder::copied_call_arg_for_formal(TokenBase *arg, node_t src_arg,
					     DataDef *formal, bool refp,
					     const std::map<DataDef *, DataDef *> *subst)
{
	if (node_t rewritten = copied_reference_slot_arg(arg, src_arg, formal, refp))
		return rewritten;
	cir_node *copied = copy_cir_subtree(CIR_NODE(src_arg), subst);
	if (!copied)
		return NULL;
	node_t out = copied->as_node();
	// A Tree-1 member call may have lowered a reference receiver as `&a` while
	// its type was still dependent. After substitution the copied `a` can be the
	// stored object pointer itself (`Inner& a` lowers as `Inner *a`), so the
	// hidden `this` formal wants `a`, not `&a`.
	if (formal && formal->is_pointer()) {
		cir_node *on = CIR_NODE(out);
		node_t inner = (on && on->base.code == N_ADDR)
			     ? c2mir_node_first_op(out) : NULL;
		cir_node *in = inner ? CIR_NODE(inner) : NULL;
		DataDef *inner_dd = in ? in->datadef : NULL;
		if (in && in->origin_id)
			if (TokenVar *tv = dynamic_cast<TokenVar *>(
				    madc_token_for_slot(in->origin_id)))
				if (tv->var.is_reference() && tv->var.type)
					inner_dd = tv->var.type;
		const DataDefCLASS *fc = class_pointer_pointee(formal);
		const DataDefCLASS *ic = class_pointer_pointee(inner_dd);
		if (fc && ic && (ic == fc || ic->is_or_derives_from(fc)))
			return inner_dd == formal ? inner
				: node2(N_CAST, ptr_type_node(formal), inner, arg);
	}
	if (refp && formal && formal->is_pointer()) {
		// Symmetric to the &a -> a case above: the Tree-1 arg may have
		// lowered as the referenced object's VALUE (`*__t1`, the
		// auto-deref of a reference var) while the callee was still an
		// unresolved dependent template with no formals to coerce
		// against. The reference formal takes the object's ADDRESS: a
		// deref's operand IS that address; any other class-object
		// value re-takes it. Casting a struct VALUE to the pointer
		// type is never right (c2mir: "conversion of non-scalar value
		// requested").
		cir_node *on = CIR_NODE(out);
		if (on && on->base.code == N_DEREF) {
			node_t inner = c2mir_node_first_op(out);
			if (inner)
				out = inner;
		} else if (on && on->base.code != N_ADDR && on->datadef
			   && as_class_instance(on->datadef)) {
			out = node1(N_ADDR, out, arg);
		}
		return node2(N_CAST, ptr_type_node(formal), out, arg);
	}
	return out;
}

static bool pending_function_body_available(CirBuilder *cb, Program *prog,
					    const std::string &sym)
{
	if (!cb || !prog || sym.empty())
		return false;
	for (TokenBase *pb : prog->pending_funcs) {
		TokenFunc *tf = dynamic_cast<TokenFunc *>(pb);
		FuncDef *fd = tf ? dynamic_cast<FuncDef *>(tf->var.type) : NULL;
		if (tf && fd && !fd->declaration_only
		    && (tf->var.name == sym || cb->func_emit_name(tf->var, fd) == sym))
			return true;
	}
	return false;
}

static TokenFunc *find_ast_function_body(CirBuilder *cb, Program *prog,
					 const std::string &sym)
{
	if (!cb || !prog || sym.empty())
		return NULL;
	std::queue<TokenBase *> q = prog->ast;
	while (!q.empty()) {
		TokenBase *tb = q.front();
		q.pop();
		TokenFunc *tf = dynamic_cast<TokenFunc *>(tb);
		FuncDef *fd = tf ? dynamic_cast<FuncDef *>(tf->var.type) : NULL;
		if (tf && fd && !tf->is_overridden && !fd->declaration_only
		    && (tf->var.name == sym || cb->func_emit_name(tf->var, fd) == sym))
			return tf;
	}
	return NULL;
}

static bool requeue_tsubst_instance_body(CirBuilder *cb, Program *prog, Variable &var,
					 FuncDef *fd)
{
	if (!cb || !prog || !fd || !fd->tsubst_source)
		return false;
	std::string sym = cb->func_emit_name(var, fd);
	if (sym.empty())
		return false;
	if (pending_function_body_available(cb, prog, sym))
		return true;
	TokenFunc *tf = find_ast_function_body(cb, prog, sym);
	if (!tf)
		return false;
	prog->pending_funcs.push_back(tf);
	return true;
}

static DataDef *tsubst_overload_arg_type(DataDef *dd)
{
	if (dd && dd->is_reference())
		if (DataDefPTR *rp = dynamic_cast<DataDefPTR *>(dd))
			return rp->base_type;
	return dd;
}

static DataDefCLASS *class_arg_type(DataDef *dd)
{
	DataDefCLASS *c = as_class_instance(dd);
	if (!c)
		c = class_behind(dd);
	return c;
}

static FuncDef *single_arg_conversion_ctor(DataDefCLASS *target,
					   DataDef *source)
{
	if (!target || !source)
		return NULL;
	DataDefCLASS *source_class = class_arg_type(source);
	if (!source_class || source_class == target
	    || source_class->is_or_derives_from(target))
		return NULL;
	FuncDef *best = NULL;
	int best_score = -1;
	for (Variable *cv : target->ctors) {
		FuncDef *fd = cv ? dynamic_cast<FuncDef *>(cv->type) : NULL;
		if (!fd || fd->parameters.size() < 2
		    || fd->required_param_count() > 2)
			continue;
		DataDef *pt = fd->parameters[1];
		bool refp = fd->is_ref_param(1);
		int score = score_arg_to_param(source, pt, refp, false, false);
		if (score > best_score) {
			best_score = score;
			best = fd;
		}
	}
	return best_score >= 0 ? best : NULL;
}

static std::string method_candidate_display_name(Variable *v, FuncDef *fd)
{
	if (fd && !fd->method_display_name.empty())
		return fd->method_display_name;
	if (fd && !fd->function_display_name.empty())
		return fd->function_display_name;
	return v ? v->name : std::string();
}

static size_t method_hidden_param_count(Variable *v, FuncDef *fd,
					DataDefCLASS *receiver)
{
	if (!v || !fd || (v->flags & vfSTATIC))
		return 0;
	Method *md = static_cast<Method *>(v->data);
	if (md && md->owner_class)
		return 1;
	if (!receiver || fd->parameters.empty())
		return 0;
	DataDefCLASS *owner = class_behind(fd->parameters[0]);
	if (!owner)
		return 0;
	return receiver == owner || receiver->is_or_derives_from(owner)
		? 1 : 0;
}

static bool method_body_matches_args(Variable *v, FuncDef *fd,
				     DataDefCLASS *receiver,
				     const std::vector<const DataDef *> &argtypes)
{
	if (!v || !fd)
		return false;
	size_t hidden = method_hidden_param_count(v, fd, receiver);
	size_t pn = fd->parameters.size() >= hidden
		  ? fd->parameters.size() - hidden : 0;
	bool varargs = fd->is_varargs && pn > 0;
	size_t fixed = varargs ? pn - 1 : pn;
	if ((!varargs && pn != argtypes.size())
	    || (varargs && argtypes.size() < fixed))
		return false;
	for (size_t i = 0; i < fixed; ++i) {
		size_t pi = i + hidden;
		DataDef *pt = pi < fd->parameters.size()
			    ? fd->parameters[pi] : NULL;
		bool refp = fd->is_ref_param(pi);
		if (score_arg_to_param(argtypes[i], pt, refp) < 0)
			return false;
	}
	return true;
}

static bool ref_return_class_can_bind(DataDefCLASS *target,
				      DataDef *returned_type)
{
	DataDefCLASS *rc = class_arg_type(returned_type);
	if (!target || !rc)
		return false;
	return rc == target || rc->is_or_derives_from(target)
	    || single_arg_conversion_ctor(target, returned_type) != NULL;
}

static bool ref_return_class_binds_direct(DataDefCLASS *target,
					  DataDef *returned_type)
{
	DataDefCLASS *rc = class_arg_type(returned_type);
	return target && rc && (rc == target || rc->is_or_derives_from(target));
}

static TokenVar *tsubst_concrete_arg_token(DataDef *dd, size_t index,
					   TokenBase *origin)
{
	if (!dd)
		dd = &ddVOID;
	std::string name = "__madc_tsubst_arg" + std::to_string(index);
	Variable *v = new Variable(name, *dd, 1, NULL, false);
	TokenVar *tv = new TokenVar(*v);
	if (origin) {
		tv->file = origin->file;
		tv->line = origin->line;
		tv->column = origin->column;
	}
	return tv;
}

Variable *CirBuilder::resolve_copied_dependent_call(
	TokenCallFunc *tcf, const std::map<DataDef *, DataDef *> *subst,
	bool *changed_out, std::vector<DataDef *> *concrete_param_types_out,
	std::string *error_out)
{
	if (changed_out)
		*changed_out = false;
	if (concrete_param_types_out)
		concrete_param_types_out->clear();
	if (error_out)
		error_out->clear();
	if (!subst || !m_prog)
		return NULL;
	bool system_header_call = m_tsubst_copy_pack_index < 0 && tcf && tcf->file
			       && m_prog->is_system_header_path(tcf->file);
	if (!tsubst_call_can_rewrite_after_subst(tcf)) {
		FuncDef *dfd = tcf ? dynamic_cast<FuncDef *>(tcf->var.type) : NULL;
		if (system_header_call && dfd
		    && requeue_tsubst_instance_body(this, m_prog, tcf->var, dfd))
			referenced_funcs.insert(func_emit_name(tcf->var, dfd));
		return NULL;
	}
	FuncDef *fd = dynamic_cast<FuncDef *>(tcf->var.type);

	auto concrete_member_template_instance = [&](Variable *v,
						     FuncDef *vfd) -> Variable * {
		if (!v || !vfd || vfd->local_emit_name.empty() || !m_prog)
			return v;
		Variable *iv = m_prog->findVariable(vfd->local_emit_name);  // allowed-exception: lookup key, not symbol build
		FuncDef *ifd = iv ? dynamic_cast<FuncDef *>(iv->type) : NULL;
		return ifd ? iv : v;
	};
	auto body_available_for = [&](Variable *v) -> bool {
		FuncDef *wfd = v ? dynamic_cast<FuncDef *>(v->type) : NULL;
		std::string sym = v ? func_emit_name(*v, wfd) : std::string();
		if (sym.empty())
			return false;
		bool body_available = m_prog->has_deferred_lazy_body(sym);
		if (!body_available && wfd && !wfd->emit_symbol.empty())
			body_available = external_symbol_available(wfd->emit_symbol);
		if (!body_available)
			body_available = pending_function_body_available(this,
									  m_prog,
									  sym);
		if (!body_available && wfd && wfd->tsubst_source)
			body_available = requeue_tsubst_instance_body(this, m_prog,
								      *v, wfd);
		return body_available;
	};

	std::vector<DataDef *> explicit_args;
	explicit_args.reserve(tcf->explicit_template_args.size());
	bool changed = false;
	for (DataDef *arg : tcf->explicit_template_args) {
		DataDef *sarg = subst_datadef_active(arg, *subst);
		explicit_args.push_back(sarg);
		if (sarg != arg)
			changed = true;
	}

	std::vector<const DataDef *> at;
	std::vector<DataDef *> concrete_param_types;
	std::vector<TokenBase *> param_origins;
	std::vector<bool> zeros;
	at.reserve(tcf->parameters.size());
	concrete_param_types.reserve(tcf->parameters.size());
	param_origins.reserve(tcf->parameters.size());
	zeros.reserve(tcf->parameters.size());
	auto append_substituted_param = [&](TokenBase *origin, DataDef *pdd,
					    const std::map<DataDef *, DataDef *> &smap,
					    bool zero) {
		DataDef *sdd = subst_datadef_active(pdd, smap);
		at.push_back(tsubst_overload_arg_type(sdd));
		concrete_param_types.push_back(sdd);
		param_origins.push_back(origin);
		zeros.push_back(zero);
		if (sdd != pdd)
			changed = true;
	};
	for (TokenBase *p : tcf->parameters) {
		if (TokenPackExpansion *pe = dynamic_cast<TokenPackExpansion *>(p)) {
			DataDefTemplateParam *tp = pe->pattern
				? template_param_in_pack_pattern(pe->pattern)
				: NULL;
			std::map<unsigned, DataDef *>::iterator pmi =
				tp ? m_tsubst_active_pack_params.find(tp->param_index)
				   : m_tsubst_active_pack_params.end();
			if (tp && m_tsubst_active_type_arg_packs
			    && tp->param_index < m_tsubst_active_type_arg_packs->size()
			    && pmi != m_tsubst_active_pack_params.end()
			    && pmi->second) {
				const std::vector<DataDef *> &elems =
					(*m_tsubst_active_type_arg_packs)
						[tp->param_index];
				DataDef *pdd = pe->pattern
					? pe->pattern->datadef() : p->datadef();
				if (m_tsubst_copy_pack_index == (int)tp->param_index) {
					if (m_tsubst_copy_pack_elem < elems.size()
					    && elems[m_tsubst_copy_pack_elem]) {
						std::map<DataDef *, DataDef *> elem_subst =
							*subst;
						elem_subst[pmi->second] =
							elems[m_tsubst_copy_pack_elem];
						append_substituted_param(
							pe->pattern, pdd, elem_subst,
							is_zero_integer_literal(pe->pattern));
					}
					changed = true;
					continue;
				}
				for (size_t e = 0; e < elems.size(); ++e) {
					if (!elems[e])
						continue;
					std::map<DataDef *, DataDef *> elem_subst =
						*subst;
					elem_subst[pmi->second] = elems[e];
					append_substituted_param(
						pe->pattern, pdd, elem_subst,
						is_zero_integer_literal(pe->pattern));
				}
				changed = true;
				continue;
			}
		}
		append_substituted_param(p, p ? p->datadef() : NULL, *subst,
					 is_zero_integer_literal(p));
	}
	if (concrete_param_types_out)
		*concrete_param_types_out = concrete_param_types;
	if (!changed && !system_header_call)
		return NULL;

	if (TokenMember *tm = dynamic_cast<TokenMember *>(tcf)) {
		if (changed_out)
			*changed_out = changed;
		DataDef *recv_type = tm->parent_expr
				     ? tm->parent_expr->datadef()
				     : tm->object.type;
		recv_type = subst_datadef_active(recv_type, *subst);
		DataDefCLASS *recv_class = class_behind(recv_type);
		if (!recv_class) {
			if (error_out)
				*error_out = "tsubst: unresolved dependent member receiver";
			return NULL;
		}
		const std::string mname =
			(fd && !fd->method_display_name.empty())
			? fd->method_display_name
			: ((fd && !fd->function_display_name.empty())
			   ? fd->function_display_name : tm->var.name);
		std::vector<const DataDef *> mat;
		mat.reserve(concrete_param_types.size());
		for (DataDef *pt : concrete_param_types)
			mat.push_back(tsubst_overload_arg_type(pt));
		Variable *winner = recv_class->findMethodOverload(mname, mat);
		if (!winner) {
			for (Variable *mv : recv_class->methods) {
				FuncDef *mfd = mv
					? dynamic_cast<FuncDef *>(mv->type) : NULL;
				if (!mfd || !mfd->is_member_template
				    || !mfd->declaration_only
				    || method_candidate_display_name(mv, mfd) != mname)
					continue;
				Variable synth_recv("__madc_tsubst_recv",
						    *recv_type, 1,
						    NULL, false);
				TokenCallMethod synth(synth_recv, *mv);
				synth.explicit_template_args = explicit_args;
				synth.file = tcf->file;
				synth.line = tcf->line;
				synth.column = tcf->column;
				synth.parameters.reserve(concrete_param_types.size());
				for (size_t i = 0; i < concrete_param_types.size(); ++i)
					synth.parameters.push_back(
						tsubst_concrete_arg_token(
							concrete_param_types[i], i,
							param_origins[i]));
				m_prog->instantiate_member_fn_template_for_call(&synth);
				Variable *body =
					concrete_member_template_instance(mv, mfd);
				FuncDef *bfd = body
					? dynamic_cast<FuncDef *>(body->type) : NULL;
				if (body && body != mv && bfd
				    && method_body_matches_args(body, bfd,
								recv_class, mat)
				    && body_available_for(body)) {
					winner = mv;
					break;
				}
			}
		}
		if (!winner) {
			if (error_out)
				*error_out = "tsubst: unresolved dependent member call";
			return NULL;
		}
		FuncDef *wfd = dynamic_cast<FuncDef *>(winner->type);
		if (wfd && wfd->is_member_template && wfd->declaration_only) {
			Variable synth_recv("__madc_tsubst_recv", *recv_type, 1,
					    NULL, false);
			TokenCallMethod synth(synth_recv, *winner);
			synth.explicit_template_args = explicit_args;
			synth.file = tcf->file;
			synth.line = tcf->line;
			synth.column = tcf->column;
			synth.parameters.reserve(concrete_param_types.size());
			for (size_t i = 0; i < concrete_param_types.size(); ++i)
				synth.parameters.push_back(
					tsubst_concrete_arg_token(
						concrete_param_types[i], i,
						param_origins[i]));
			m_prog->instantiate_member_fn_template_for_call(&synth);
		}
		Variable *body_winner =
			concrete_member_template_instance(winner, wfd);
		FuncDef *body_fd = body_winner
			? dynamic_cast<FuncDef *>(body_winner->type) : NULL;
		if (!body_available_for(body_winner)) {
			if (error_out)
				*error_out = "tsubst: unresolved dependent member body";
			return NULL;
		}
		if (changed_out) {
			std::string old_sym = func_emit_name(tcf->var, fd);
			std::string new_sym = func_emit_name(*body_winner, body_fd);
			*changed_out = changed || old_sym != new_sym;
		}
		return body_winner;
	}

	// A system-header free-function dependent call re-resolves on the
	// substituted (concrete) argument types exactly like a local one — the
	// SAME find_namespace_function_overload + instantiate-on-miss path. The
	// post-resolve body-availability check below is the safety net: a call
	// that re-resolves but has no materializable body still falls back. (This
	// retired the earlier "simple scalar/pointer types only" pre-gate, which
	// needlessly rejected calls with class-pointer/reference args — e.g.
	// std::__do_uninit_copy(basic_string*, ...) — that resolve+instantiate fine.
	// g++ shape: tsubst re-runs finish_call_expr on substituted args.)
	auto instantiate_concrete_call = [&]() {
		TokenCallFunc synth(tcf->var);
		synth.explicit_template_args = explicit_args;
		synth.file = tcf->file;
		synth.line = tcf->line;
		synth.column = tcf->column;
		synth.parameters.reserve(concrete_param_types.size());
		for (size_t i = 0; i < concrete_param_types.size(); ++i)
			synth.parameters.push_back(tsubst_concrete_arg_token(
				concrete_param_types[i], i, param_origins[i]));
		m_prog->instantiate_namespace_fn_template_for_call(&synth);
	};
	auto instantiate_concrete_operator_call = [&]() -> Variable * {
		if (fd->function_display_name.compare(0, 8, "operator") != 0
		    || tcf->parameters.size() != 2)
			return NULL;
		DataDef *ld = m_prog->free_operator_arg_datadef(tcf->parameters[0]);
		DataDef *rd = m_prog->free_operator_arg_datadef(tcf->parameters[1]);
		ld = subst_datadef_active(ld, *subst);
		rd = subst_datadef_active(rd, *subst);
		if (!ld || !rd || template_param_under_type_layers(ld)
		    || template_param_under_type_layers(rd))
			return NULL;
		TokenVar *lhs = tsubst_concrete_arg_token(ld, 0,
							  tcf->parameters[0]);
		TokenVar *rhs = tsubst_concrete_arg_token(rd, 1,
							  tcf->parameters[1]);
		Variable *op_callee = NULL;
		if (!m_prog->instantiate_free_operator_template(
			    fd->function_display_name, lhs, rhs, &op_callee))
			return NULL;
		return op_callee;
	};

	Variable *winner = m_prog->find_namespace_function_overload(
		fd->namespace_name, fd->function_display_name, at, &zeros,
		&explicit_args);
	if (!winner) {
		instantiate_concrete_call();
		winner = m_prog->find_namespace_function_overload(
			fd->namespace_name, fd->function_display_name, at, &zeros,
			&explicit_args);
	} else {
		if (system_header_call && !body_available_for(winner)) {
			Variable *inst = instantiate_concrete_operator_call();
			if (!inst || !body_available_for(inst)) {
				instantiate_concrete_call();
				inst = m_prog->find_namespace_function_overload(
					fd->namespace_name,
					fd->function_display_name,
					at, &zeros, &explicit_args);
			}
			if (inst)
				winner = inst;
		}
	}
	if (!winner) {
		if (error_out)
			*error_out = "tsubst: unresolved dependent call";
		return NULL;
	}
	if (system_header_call) {
		if (!body_available_for(winner)) {
			if (error_out)
				*error_out = "tsubst: system-header dependent call";
			return NULL;
		}
	}
	if (changed_out) {
		FuncDef *wfd = dynamic_cast<FuncDef *>(winner->type);
		std::string old_sym = func_emit_name(tcf->var, fd);
		std::string new_sym = func_emit_name(*winner, wfd);
		bool system_operator_call = system_header_call
			&& fd->function_display_name.compare(0, 8, "operator") == 0;
		*changed_out = changed || old_sym != new_sym
			       || system_operator_call;
	}
	return winner;
}

bool CirBuilder::system_header_pack_element_call_resolves(
	TokenPackExpansion *pe, const std::map<DataDef *, DataDef *> &subst,
	DataDefTemplateParam *tp, DataDef *elem, size_t elem_index,
	DataDefCLASS *target)
{
	if (!pe || !pe->pattern || !tp || !elem || !target || !m_prog)
		return false;
	TokenCallFunc *call = dynamic_cast<TokenCallFunc *>(pe->pattern);
	if (!call)
		return false;
	std::map<unsigned, DataDef *>::iterator pmi =
		m_tsubst_active_pack_params.find(tp->param_index);
	if (pmi == m_tsubst_active_pack_params.end() || !pmi->second)
		return false;
	std::string value_name =
		pack_value_name_in_pattern(pe->pattern, tp->param_index);
	std::map<DataDef *, DataDef *> elem_subst = subst;
	elem_subst[pmi->second] = elem;
	if (!tsubst_bind_lockstep_packs(pe->pattern, elem_index, elem_subst))
		return false;
	int saved_index = m_tsubst_copy_pack_index;
	size_t saved_elem = m_tsubst_copy_pack_elem;
	const char *saved_name = m_tsubst_copy_pack_value_name;
	m_tsubst_copy_pack_index = (int)tp->param_index;
	m_tsubst_copy_pack_elem = elem_index;
	m_tsubst_copy_pack_value_name =
		value_name.empty() ? NULL : value_name.c_str();
	bool changed = false;
	std::string err;
	Variable *winner = resolve_copied_dependent_call(
		call, &elem_subst, &changed, NULL, &err);
	m_tsubst_copy_pack_index = saved_index;
	m_tsubst_copy_pack_elem = saved_elem;
	m_tsubst_copy_pack_value_name = saved_name;
	if (!changed || !winner)
		return false;
	FuncDef *wfd = dynamic_cast<FuncDef *>(winner->type);
	if (!wfd || !wfd->returns_reference())
		return false;
	DataDef *ret = &wfd->return_value_type();
	return ref_return_class_can_bind(target, ret);
}

void CirBuilder::rewrite_copied_dependent_call_id(cir_node *src, cir_node *dst,
						  const std::map<DataDef *, DataDef *> *subst)
{
	if (!src || !dst || src->base.code != N_ID || src->origin_id == 0)
		return;
	TokenCallFunc *tcf =
		dynamic_cast<TokenCallFunc *>(madc_token_for_slot(src->origin_id));
	FuncDef *oldfd = tcf ? dynamic_cast<FuncDef *>(tcf->var.type) : NULL;
	std::string old_sym = tcf ? func_emit_name(tcf->var, oldfd) : std::string();
	if (old_sym.empty() || !cir_id_spells(src, old_sym.c_str()))
		return;
	bool changed = false;
	std::string err;
	Variable *winner = resolve_copied_dependent_call(tcf, subst, &changed,
							 NULL, &err);
	if (!changed)
		return;
	if (!winner) {
		dst->error_msg = err.empty()
			       ? "tsubst: unresolved dependent call"
			       : arena.intern(err.c_str());
		return;
	}
	FuncDef *wfd = dynamic_cast<FuncDef *>(winner->type);
	std::string sym = func_emit_name(*winner, wfd);
	if (sym.empty()) {
		dst->error_msg = "tsubst: unresolved dependent call symbol";
		return;
	}
	if (!is_c2mir_builtin_call_name(tcf->var.name))
		referenced_funcs.insert(sym);
	const char *interned = c2mir_uniq_str(c2m, sym.c_str(), sym.size() + 1);
	dst->base.u.s.s = interned;
	dst->base.u.s.len = sym.size() + 1;
}

cir_node *CirBuilder::tsubst_relower_deferred_construction(
	const std::vector<TokenBase *> &ctor_args, TokenBase *origin,
	DataDefCLASS *concrete_class,
	const std::map<DataDef *, DataDef *> *subst,
	const std::function<node_t()> &this_addr, bool yield_this_addr,
	bool relax_class_args, bool require_overload_match)
{
	// relax_class_args (the decl-marker path) has no simple re-translate
	// fallback, so it always takes the manual assembly below.
	bool manual_class_pack_lowering = relax_class_args;
	bool unsupported_class_arg = false;
	FuncDef *placement_ctor = NULL;
	bool placement_ctor_checked = false;
	std::vector<TokenBase *> expanded_ctor_args;
	bool expanded_ctor_args_checked = false;
	auto placement_ctor_args = [&]() -> const std::vector<TokenBase *> & {
		if (expanded_ctor_args_checked)
			return expanded_ctor_args;
		expanded_ctor_args_checked = true;
		for (size_t ai = 0; ai < ctor_args.size(); ++ai) {
			TokenBase *arg = ctor_args[ai];
			if (TokenPackExpansion *pe =
				    dynamic_cast<TokenPackExpansion *>(arg)) {
				DataDefTemplateParam *tp = pe->pattern
					? template_param_in_pack_pattern(pe->pattern)
					: NULL;
				if (tp && m_tsubst_active_type_arg_packs
				    && tp->param_index
				       < m_tsubst_active_type_arg_packs->size()) {
					const std::vector<DataDef *> &elems =
						(*m_tsubst_active_type_arg_packs)
							[tp->param_index];
					for (DataDef *elem : elems)
						expanded_ctor_args.push_back(
							tsubst_concrete_arg_token(
								elem,
								expanded_ctor_args.size(),
								pe->pattern));
					continue;
				}
			}
			// A dependent (pattern-token) argument scores -1 against
			// every concrete parameter — overload selection needs the
			// SUBSTITUTED type. Stand in a concrete-typed token for
			// scoring only; the lowering loop below still consumes the
			// original token. Covers placeholder LAYERS and dependent
			// template-id SHELLS (rebuilt via their recorded origin
			// under the active pack window).
			DataDef *add = arg ? arg->datadef() : NULL;
			if (add && subst
			    && (template_param_under_type_layers(add)
				|| dependent_shell_under_type_layers(m_prog,
								     add))) {
				DataDef *sdd = subst_datadef_active(add, *subst);
				// `sdd != add` (not a shell re-check): a
				// CONCRETE-arg rebuild (`_Index_tuple<0>`) may
				// itself carry the opaque-path placeholder
				// flag + an origin record; the walker already
				// guarantees a rebuild only succeeds with
				// every param name resolved.
				if (sdd && sdd != add
				    && !template_param_under_type_layers(sdd)) {
					expanded_ctor_args.push_back(
						tsubst_concrete_arg_token(
							sdd,
							expanded_ctor_args.size(),
							arg));
					continue;
				}
			}
			expanded_ctor_args.push_back(arg);
		}
		return expanded_ctor_args;
	};
	auto selected_placement_ctor = [&]() -> FuncDef * {
		if (!placement_ctor_checked) {
			placement_ctor = select_ctor_overload(
				concrete_class, placement_ctor_args());
			if (getenv("MADC_XTEST_PAT_MEMINIT_DEBUG")) {
				fprintf(stderr, "[RELOWER-SELECT] class=%s nctors=%zu nargs=%zu -> %s\n",
					concrete_class->name.c_str(),
					concrete_class->ctors.size(),
					placement_ctor_args().size(),
					placement_ctor ? placement_ctor->name.c_str() : "(none)");
				for (TokenBase *a : placement_ctor_args())
					fprintf(stderr, "[RELOWER-SELECT]   arg dd=%s\n",
						(a && a->datadef()) ? a->datadef()->name.c_str() : "(null)");
			}
			if (!placement_ctor && !require_overload_match) {
				auto it = concrete_class->method_map.find(
					concrete_class->name);
				if (it != concrete_class->method_map.end()
				    && it->second)
					placement_ctor =
						dynamic_cast<FuncDef *>(
							it->second->type);
			}
			placement_ctor_checked = true;
		}
		return placement_ctor;
	};
	size_t concrete_arg_pos = 0;
	for (size_t ai = 0; ai < ctor_args.size(); ++ai) {
		TokenBase *arg = ctor_args[ai];
		if (TokenPackExpansion *pe =
			    dynamic_cast<TokenPackExpansion *>(arg)) {
			DataDefTemplateParam *tp = pe->pattern
				? template_param_in_pack_pattern(pe->pattern)
				: NULL;
			if (!tp || !m_tsubst_active_type_arg_packs
			    || tp->param_index
			       >= m_tsubst_active_type_arg_packs->size()) {
				unsupported_class_arg = true;
				break;
			}
			const std::vector<DataDef *> &elems =
				(*m_tsubst_active_type_arg_packs)
					[tp->param_index];
			for (size_t e = 0; e < elems.size(); ++e) {
				DataDef *elem = elems[e];
				size_t pi = concrete_arg_pos + 1;
				++concrete_arg_pos;
				if (!tsubst_is_class_object_arg(elem))
					continue;
				FuncDef *ctor = selected_placement_ctor();
				DataDef *pt = (ctor && pi < ctor->parameters.size())
						? ctor->parameters[pi] : NULL;
				bool refp = ctor && ctor->is_ref_param(pi);
				if (DataDefCLASS *pc =
					    param_object_class(pt, refp)) {
					if (expr_is_nonaddressable_rvalue(pe->pattern)) {
						if (!class_trivially_copyable(pc)) {
							unsupported_class_arg = true;
							break;
						}
						manual_class_pack_lowering = true;
						continue;
					}
					if (pe->pattern && pe->pattern->file && m_prog
					    && m_prog->is_system_header_path(
						    pe->pattern->file)) {
						if (!system_header_pack_element_call_resolves(
							    pe, *subst, tp, elem, e, pc)) {
							unsupported_class_arg = true;
							break;
						}
					}
					manual_class_pack_lowering = true;
					continue;
				}
				// By-value object params can reuse marked-expression
				// fan-out for simple local cases, but real
				// system-header constructor packs need the same
				// copied-argument path as reference-bound object
				// params so ordinary temp construction does not
				// recurse through the unresolved pack expression.
				if (!elem->is_reference()
				    && as_class_instance(pt)) {
					manual_class_pack_lowering = true;
					continue;
				}
				unsupported_class_arg = true;
				break;
			}
			if (unsupported_class_arg)
				break;
			continue;
		}
		++concrete_arg_pos;
		DataDef *arg_dd = arg
			? subst_datadef_active(arg->datadef(), *subst)
			: NULL;
		if (!relax_class_args && tsubst_is_class_object_arg(arg_dd)) {
			unsupported_class_arg = true;
			break;
		}
	}
	if (unsupported_class_arg)
		return CIR_NODE(error_node(
			"tsubst: class deferred-construction object argument pack"));
	if (manual_class_pack_lowering) {
		FuncDef *ctor = selected_placement_ctor();
		if (!ctor)
			return CIR_NODE(error_node(
				"tsubst: unresolved deferred-construction ctor"));

		std::vector<node_t> prefix_items;
		auto copy_node_under = [&](node_t raw,
					   const std::map<DataDef *, DataDef *> &smap,
					   int pack_index,
					   size_t pack_elem,
					   const char *pack_name) -> node_t {
			if (!raw)
				return error_node(
					"tsubst: missing deferred-construction pack expr");
			int saved_index = m_tsubst_copy_pack_index;
			size_t saved_elem = m_tsubst_copy_pack_elem;
			const char *saved_name =
				m_tsubst_copy_pack_value_name;
			m_tsubst_copy_pack_index = pack_index;
			m_tsubst_copy_pack_elem = pack_elem;
			m_tsubst_copy_pack_value_name = pack_name;
			cir_node *copied =
				copy_cir_subtree(CIR_NODE(raw), &smap);
			m_tsubst_copy_pack_index = saved_index;
			m_tsubst_copy_pack_elem = saved_elem;
			m_tsubst_copy_pack_value_name = saved_name;
			return copied ? copied->as_node()
				      : error_node(
					      "tsubst: failed deferred-construction pack copy");
		};
		auto copy_expr_under = [&](TokenBase *expr,
					   const std::map<DataDef *, DataDef *> &smap,
					   int pack_index,
					   size_t pack_elem,
					   const char *pack_name) -> node_t {
			std::vector<node_t> saved_pending =
				m_pending_stmts;
			std::set<std::string> saved_refs =
				referenced_funcs;
			m_pending_stmts.clear();
			node_t raw = translate_expr(expr);
			std::vector<node_t> pending =
				m_pending_stmts;
			m_pending_stmts = saved_pending;
			referenced_funcs = saved_refs;
			for (node_t p : pending)
				prefix_items.push_back(copy_node_under(
					p, smap, pack_index, pack_elem,
					pack_name));
			return copy_node_under(raw, smap, pack_index,
					       pack_elem, pack_name);
		};
		auto void_addr_of = [&](node_t value,
					TokenBase *origin) -> node_t {
			return node2(N_CAST, void_ptr_type(),
				     node1(N_ADDR, value, origin),
				     origin);
		};
		auto temp_addr_from_value = [&](DataDefCLASS *target,
						node_t value,
						TokenBase *origin) -> node_t {
			if (!target || !class_trivially_copyable(target))
				return error_node(
					"tsubst: non-trivial deferred-construction pack temp");
			char name[40];
			snprintf(name, sizeof(name), "__madc_objtmp_%d",
				 m_strtmp_counter++);
			Variable *tmp = new Variable(name, *target, 1,
						     NULL, false);
			tmp->flags |= vfLOCAL;
			prefix_items.push_back(var_decl(tmp, origin));
			node_t assign = node2(N_ASSIGN, id(name, origin),
					      value, origin);
			prefix_items.push_back(node2(N_EXPR, list(),
						     assign, origin));
			return object_addr(name, origin);
		};
		auto copied_pack_id_node = [&](const char *pack_name,
					       int pack_index,
					       size_t pack_elem,
					       TokenBase *origin,
					       DataDef *dd) -> node_t {
			int saved_index = m_tsubst_copy_pack_index;
			size_t saved_elem = m_tsubst_copy_pack_elem;
			const char *saved_name =
				m_tsubst_copy_pack_value_name;
			m_tsubst_copy_pack_index = pack_index;
			m_tsubst_copy_pack_elem = pack_elem;
			m_tsubst_copy_pack_value_name = pack_name;
			std::string nm = copied_pack_value_name(pack_name);
			m_tsubst_copy_pack_index = saved_index;
			m_tsubst_copy_pack_elem = saved_elem;
			m_tsubst_copy_pack_value_name = saved_name;
			node_t n = id(nm.c_str(), origin);
			CIR_NODE(n)->datadef = dd;
			return n;
		};
		std::function<node_t(
			TokenBase *, DataDef *, bool,
			const std::map<DataDef *, DataDef *> &,
			int, size_t, const char *)> explicit_arg_node;
		explicit_arg_node =
			[&](TokenBase *arg, DataDef *pt, bool refp,
			    const std::map<DataDef *, DataDef *> &smap,
			    int pack_index, size_t pack_elem,
			    const char *pack_name) -> node_t {
			if (DataDefCLASS *pc = param_object_class(pt, refp)) {
				DataDef *rt = ref_returning_call_type(arg);
				DataDef *srt = rt
					? subst_datadef_active(rt, smap) : NULL;
				if (rt && !ref_return_class_binds_direct(pc, srt)) {
					FuncDef *conv =
						single_arg_conversion_ctor(pc, srt);
					if (conv && conv->parameters.size() >= 2) {
						char name[40];
						snprintf(name, sizeof(name),
							 "__madc_objtmp_%d",
							 m_strtmp_counter++);
						Variable *tmp = new Variable(
							name, *pc, 1, NULL, false);
						tmp->flags |= vfLOCAL;
						prefix_items.push_back(var_decl(tmp, arg));
						std::vector<node_t> cnodes;
						DataDef *cpt = conv->parameters[1];
						bool crefp = conv->is_ref_param(1);
						cnodes.push_back(explicit_arg_node(
							arg, cpt, crefp, smap,
							pack_index, pack_elem, pack_name));
						node_t cc = ctor_call_assemble(
							node1(N_ADDR, id(name, arg), arg),
							pc, conv, cnodes, arg);
						if (cc)
							prefix_items.push_back(cc);
						return object_addr(name, arg);
					}
				}
				if (rt && pack_name)
					return node2(N_CAST, void_ptr_type(),
						copied_pack_id_node(pack_name,
							pack_index, pack_elem,
							arg, srt),
						arg);
				node_t value = copy_expr_under(arg, smap,
							      pack_index,
							      pack_elem,
							      pack_name);
				if (rt && value && value->code == N_CALL)
					value = node1(N_DEREF, value, arg);
				if (!rt && expr_is_nonaddressable_rvalue(arg))
					return temp_addr_from_value(pc, value,
									    arg);
				return void_addr_of(value, arg);
			}
			if (as_class_instance(pt)) {
				DataDef *rt = ref_returning_call_type(arg);
				if (rt && pack_name) {
					DataDef *srt =
						subst_datadef_active(rt, smap);
					node_t slot = copied_pack_id_node(
						pack_name, pack_index, pack_elem,
						arg, srt);
					node_t value = node1(N_DEREF,
						slot, arg);
					CIR_NODE(value)->datadef = srt;
					return value;
				}
				// A zero-arg VALUE-INIT construction of a
				// dependent shell (`typename _Build_index_tuple
				// <...>::__type()`): translate_expr would type
				// its temp with the SHELL, mismatching the
				// concrete by-value param — declare the temp as
				// the SUBSTITUTED class directly instead.
				DataDef *add = arg ? arg->datadef() : NULL;
				TokenObjTemp *aot =
					dynamic_cast<TokenObjTemp *>(arg);
				if (add && aot && aot->ctor_args.empty()
				    && dependent_shell_under_type_layers(m_prog,
									 add)) {
					DataDef *sdd =
						subst_datadef_active(add, smap);
					DataDefCLASS *sc = (sdd && sdd != add)
						? as_class_instance(sdd) : NULL;
					if (sc && class_trivially_copyable(sc)) {
						char name[40];
						snprintf(name, sizeof(name),
							 "__madc_objtmp_%d",
							 m_strtmp_counter++);
						Variable *tmp = new Variable(
							name, *sc, 1, NULL,
							false);
						tmp->flags |= vfLOCAL;
						prefix_items.push_back(
							var_decl(tmp, arg));
						return id(name, arg);
					}
				}
				return copy_expr_under(arg, smap, pack_index,
						       pack_elem, pack_name);
			}
			if (refp) {
				node_t value = copy_expr_under(arg, smap,
							      pack_index,
							      pack_elem,
							      pack_name);
				if (expr_is_nonaddressable_rvalue(arg)) {
					char name[40];
					snprintf(name, sizeof(name),
						 "__madc_objtmp_%d",
						 m_strtmp_counter++);
					DataDef *rt = ref_param_referent(pt);
					DataDef *vt = arg ? subst_datadef(
						m_prog, arg->datadef(), smap) : NULL;
					DataDef *tmp_type =
						(rt && rt != vt) ? rt : vt;
					if (!tmp_type)
						tmp_type = &ddINT64;
					Variable *tmp = new Variable(
						name, *tmp_type, 1, NULL, false);
					tmp->flags |= vfLOCAL;
					prefix_items.push_back(var_decl(tmp, arg));
					node_t assign = node2(N_ASSIGN,
						id(name, arg), value, arg);
					prefix_items.push_back(node2(N_EXPR,
						list(), assign, arg));
					return node1(N_ADDR, id(name, arg), arg);
				}
				return node1(N_ADDR, value, arg);
			}
			return copy_expr_under(arg, smap, pack_index,
					       pack_elem, pack_name);
		};

		std::vector<node_t> explicit_nodes;
		concrete_arg_pos = 0;
		for (size_t ai = 0; ai < ctor_args.size(); ++ai) {
			TokenBase *arg = ctor_args[ai];
			if (TokenPackExpansion *pe =
				    dynamic_cast<TokenPackExpansion *>(arg)) {
				DataDefTemplateParam *tp = pe->pattern
					? template_param_in_pack_pattern(pe->pattern)
					: NULL;
				if (!tp || !m_tsubst_active_type_arg_packs
				    || tp->param_index
				       >= m_tsubst_active_type_arg_packs->size()) {
					explicit_nodes.push_back(error_node(
						"tsubst: missing deferred-construction pack"));
					continue;
				}
				std::map<unsigned, DataDef *>::iterator pmi =
					m_tsubst_active_pack_params.find(
						tp->param_index);
				if (pmi == m_tsubst_active_pack_params.end()
				    || !pmi->second) {
					explicit_nodes.push_back(error_node(
						"tsubst: missing deferred-construction pack param"));
					continue;
				}
				const std::vector<DataDef *> &elems =
					(*m_tsubst_active_type_arg_packs)
						[tp->param_index];
				std::string value_name =
					pack_value_name_in_pattern(
						pe->pattern, tp->param_index);
				for (size_t e = 0; e < elems.size(); ++e) {
					std::map<DataDef *, DataDef *> elem_subst =
						*subst;
					elem_subst[pmi->second] = elems[e];
					size_t pi = concrete_arg_pos + 1;
					++concrete_arg_pos;
					if (!tsubst_bind_lockstep_packs(
						    pe->pattern, e, elem_subst)) {
						explicit_nodes.push_back(error_node(
							"tsubst: lockstep pack arity mismatch",
							pe->pattern));
						continue;
					}
					DataDef *pt =
						(ctor && pi < ctor->parameters.size())
							? ctor->parameters[pi] : NULL;
					bool refp = ctor && ctor->is_ref_param(pi);
					explicit_nodes.push_back(explicit_arg_node(
						pe->pattern, pt, refp, elem_subst,
						(int)tp->param_index, e,
						value_name.empty()
							? NULL : value_name.c_str()));
				}
				continue;
			}
			size_t pi = concrete_arg_pos + 1;
			++concrete_arg_pos;
			DataDef *pt = (ctor && pi < ctor->parameters.size())
					? ctor->parameters[pi] : NULL;
			bool refp = ctor && ctor->is_ref_param(pi);
			explicit_nodes.push_back(explicit_arg_node(
				arg, pt, refp, *subst, -1, 0, NULL));
		}
		node_t construct = ctor_call_assemble(
			this_addr(), concrete_class, ctor,
			explicit_nodes, origin);
		node_t items = list();
		for (node_t p : prefix_items)
			append(items, p);
		if (construct)
			append(items, construct);
		if (!yield_this_addr)
			return CIR_NODE(node2(N_BLOCK, list(), items, origin));
		append(items, node2(N_EXPR, list(), this_addr(), origin));
		return CIR_NODE(node1(N_STMTEXPR,
			node2(N_BLOCK, list(), items, origin), origin));
	}
	return NULL;   // no manual trigger: caller runs its simple path
}

// Deep-copy a concrete cir_node subtree into fresh arena nodes — the `tsubst`
// core. Contract is documented on the declaration in cir_builder.h. This is the
// safe-for-c2mir private materialization: c2mir mutates `attr` on every node_t
// it compiles, so each copied node owns a fresh node_t base (fresh uid, attr
// cleared, a private ops list). When `subst` is non-NULL the copy also
// substitutes template-parameter placeholders in each node's datadef (Phase 3).
cir_node *CirBuilder::copy_cir_subtree(cir_node *src,
				       const std::map<DataDef *, DataDef *> *subst)
{
	if (!src)
		return NULL;
	if (subst && src->base.code == N_IGNORE && src->datadef
	    && src->origin_id) {
		TokenNEW *tn =
			dynamic_cast<TokenNEW *>(madc_token_for_slot(src->origin_id));
		if (tn && tn->placement
		    && (template_param_under_type_layers(src->datadef)
			|| tsubst_args_have_pack_expansion(tn->ctor_args))) {
			DataDef *concrete = subst_datadef_active(src->datadef, *subst);
			if (!concrete || template_param_under_type_layers(concrete))
				return CIR_NODE(error_node(
					"tsubst: unbound placement-new constructed type"));
			DataDefCLASS *concrete_class =
				dynamic_cast<DataDefCLASS *>(concrete);
			if (concrete_class) {
				auto placement_addr = [&]() -> node_t {
					std::set<std::string> saved_refs =
						referenced_funcs;
					node_t raw = translate_expr(tn->placement);
					referenced_funcs = saved_refs;
					int saved_index = m_tsubst_copy_pack_index;
					size_t saved_elem = m_tsubst_copy_pack_elem;
					const char *saved_name =
						m_tsubst_copy_pack_value_name;
					m_tsubst_copy_pack_index = -1;
					m_tsubst_copy_pack_elem = 0;
					m_tsubst_copy_pack_value_name = NULL;
					cir_node *copied =
						copy_cir_subtree(CIR_NODE(raw), subst);
					m_tsubst_copy_pack_index = saved_index;
					m_tsubst_copy_pack_elem = saved_elem;
					m_tsubst_copy_pack_value_name = saved_name;
					return copied ? copied->as_node()
						      : error_node("tsubst: failed deferred-construction addr copy");
				};
				if (cir_node *done = tsubst_relower_deferred_construction(
						tn->ctor_args, tn, concrete_class,
						subst, placement_addr,
						/*yield_this_addr=*/true,
						/*relax_class_args=*/false))
					return done;
			}
			TokenNEW lowered;
			lowered.file = tn->file;
			lowered.line = tn->line;
			lowered.column = tn->column;
			lowered.placement = tn->placement;
			lowered.ctor_args = tn->ctor_args;
			lowered.array_size = tn->array_size;
			lowered.alloc_class = concrete_class;
			lowered.alloc_type = lowered.alloc_class ? NULL : concrete;
			bool saved_mode = m_tsubst_pattern_mode;
			std::set<std::string> saved_refs = referenced_funcs;
			m_tsubst_pattern_mode = false;
			node_t lowered_tree = translate_expr(&lowered);
			m_tsubst_pattern_mode = saved_mode;
			referenced_funcs = saved_refs;
			if (!lowered_tree)
				return CIR_NODE(error_node(
					"tsubst: failed to lower placement new"));
			return copy_cir_subtree(CIR_NODE(lowered_tree), subst);
		}
		// A deferred local-declaration construction (`_Auto_node __an(*this,
		// std::forward<_Args>(__args)...)`): the Tree-1 pattern could not
		// select a ctor overload (pack arity is per-instantiation), so
		// translate_block emitted this marker. Re-lower with the expanded
		// concrete arguments; the declared variable's storage decl was
		// copied separately, so only the constructor call is built here.
		if (TokenDecl *tdcl = dynamic_cast<TokenDecl *>(
				    madc_token_for_slot(src->origin_id))) {
			if (tsubst_args_have_pack_expansion(tdcl->ctor_args)) {
				DataDef *concrete =
					subst_datadef_active(src->datadef, *subst);
				DataDefCLASS *concrete_class = concrete
					? dynamic_cast<DataDefCLASS *>(concrete) : NULL;
				if (!concrete_class
				    || template_param_under_type_layers(concrete))
					return CIR_NODE(error_node(
						"tsubst: unbound deferred-construction type"));
				auto var_addr = [&]() -> node_t {
					return node1(N_ADDR,
						id(tdcl->var.name.c_str(), tdcl), tdcl);
				};
				if (cir_node *done = tsubst_relower_deferred_construction(
						tdcl->ctor_args, tdcl, concrete_class,
						subst, var_addr,
						/*yield_this_addr=*/false,
						/*relax_class_args=*/true))
					return done;
				return CIR_NODE(error_node(
					"tsubst: unsupported deferred local construction"));
			}
		}
		// A deferred class-object ARGUMENT binding (`_KeyOfValue()(__v)` with
		// `_Arg&& __v`): the pattern could not decide bind-vs-convert for a
		// forwarding-reference param bound to a concrete class parameter
		// (the arg's type is per-instantiation). Re-decide here with the
		// substituted type: a matching (or derived) class binds directly —
		// the parameter's repr is a pointer for EVERY substitution, so the
		// value IS the object address. A mismatched type would need a
		// conversion-ctor temp — not covered yet, error -> clean fallback.
		if (TokenVar *tvar = dynamic_cast<TokenVar *>(
				    madc_token_for_slot(src->origin_id))) {
			DataDefCLASS *target =
				dynamic_cast<DataDefCLASS *>(src->datadef);
			if (target && tvar->datadef()
			    && tvar->datadef()->is_reference()
			    && template_param_under_type_layers(tvar->datadef())) {
				DataDef *sdd = subst_datadef_active(tvar->datadef(),
								    *subst);
				if (!sdd || template_param_under_type_layers(sdd))
					return CIR_NODE(error_node(
						"tsubst: unbound deferred-arg type", tvar));
				DataDefCLASS *ac = class_behind(sdd);
				if (ac != target
				    && !(ac && ac->is_or_derives_from(target)
					 && ac->base_offset_of(target) == 0))
					return CIR_NODE(error_node(
						"tsubst: deferred-arg conversion (not covered)",
						tvar));
				node_t addr = object_var_void_addr(tvar->var, tvar);
				cir_node *copied = copy_cir_subtree(CIR_NODE(addr), subst);
				return copied ? copied
					      : CIR_NODE(error_node(
							"tsubst: deferred-arg copy", tvar));
			}
		}
		TokenCallFunc *tc =
			dynamic_cast<TokenCallFunc *>(madc_token_for_slot(src->origin_id));
		TokenExplicitDtor *td =
			dynamic_cast<TokenExplicitDtor *>(madc_token_for_slot(src->origin_id));
		if (td && tsubst_explicit_dtor_marker_datadef(td)) {
			DataDef *concrete_obj =
				subst_datadef_active(src->datadef, *subst);
			DataDef *elem = concrete_obj;
			if (td->is_arrow) {
				DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(concrete_obj);
				elem = pdd ? pdd->base_type : NULL;
			}
			if (!elem || template_param_under_type_layers(elem))
				return CIR_NODE(error_node(
					"tsubst: unbound explicit destructor type"));
			std::set<std::string> saved_refs = referenced_funcs;
			node_t raw_obj = translate_expr(td->obj);
			referenced_funcs = saved_refs;
			cir_node *copied_obj =
				copy_cir_subtree(CIR_NODE(raw_obj), subst);
			node_t obj_ptr = copied_obj ? copied_obj->as_node() : NULL;
			if (!obj_ptr)
				return CIR_NODE(error_node(
					"tsubst: missing explicit destructor object"));
			if (!td->is_arrow)
				obj_ptr = node1(N_ADDR, obj_ptr, td);
			DataDefCLASS *ecls = as_class_instance(elem);
			if (!ecls || !class_needs_dtor(ecls)) {
				node_t items = list();
				append(items, node2(N_EXPR, list(), obj_ptr, td));
				append(items, node2(N_EXPR, list(), integer(0, td), td));
				return CIR_NODE(node1(N_STMTEXPR,
					node2(N_BLOCK, list(), items, td), td));
			}
			std::string dsym = class_complete_dtor_symbol(ecls);
			referenced_funcs.insert(dsym);
			need_output_extern(dsym.c_str(), false,
					   { { {N_VOID}, true } });
			node_t a = list();
			append(a, node2(N_CAST, void_ptr_type(), obj_ptr, td));
			return CIR_NODE(node2(N_CALL, id(dsym.c_str(), td), a, td));
		}
		bool destroy_marker = tsubst_destroy_marker_call(tc);
		if (!destroy_marker && tc && tc->parameters.size() == 1) {
			FuncDef *fd = call_target_funcdef(tc);
			destroy_marker = fd && fd->inline_builtin_kind == "destroy";
		}
		if (destroy_marker) {
			DataDef *concrete_arg = subst_datadef_active(src->datadef, *subst);
			DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(concrete_arg);
			DataDef *elem = pdd ? pdd->base_type : NULL;
			if (!elem || template_param_under_type_layers(elem))
				return CIR_NODE(error_node(
					"tsubst: unbound __destroy element type"));
			DataDefCLASS *ecls = as_class_instance(elem);
			if (!ecls || !class_needs_dtor(ecls))
				return CIR_NODE(integer(0, tc));
			std::string dsym = class_complete_dtor_symbol(ecls);
			referenced_funcs.insert(dsym);
			need_output_extern(dsym.c_str(), false,
					   { { {N_VOID}, true } });
			node_t a = list();
			append(a, node2(N_CAST, void_ptr_type(),
					translate_expr(tc->parameters[0]), tc));
			return CIR_NODE(node2(N_CALL, id(dsym.c_str(), tc), a, tc));
		}
	}

	// Local-class-in-template method call (g++ TAG_DEFN): a Tree-1 pattern call
	// to a local class's ctor/dtor (e.g. `_Guard g(this)` / scope-exit `~_Guard`)
	// was raw-copied with the PATTERN class's emit symbol. Retarget it to the
	// concrete per-instantiation method the parser already built. Keyed purely by
	// symbol, so it works whatever the call's origin token (a local-var TokenDecl
	// ctor has no TokenCallFunc for the block below to re-resolve).
	if (subst && !m_tsubst_local_method_remap.empty()
	    && src->base.code == N_CALL) {
		node_t callee = c2mir_node_op(src->as_node(), 0);
		if (callee && callee->code == N_ID && callee->u.s.s) {
			std::map<std::string, std::string>::iterator ri =
				m_tsubst_local_method_remap.find(callee->u.s.s);
			if (ri != m_tsubst_local_method_remap.end()) {
				TokenBase *origin = src->origin_id
					? madc_token_for_slot(src->origin_id) : NULL;
				referenced_funcs.insert(ri->second);
				node_t src_args = c2mir_node_op(src->as_node(), 1);
				cir_node *args_copy = src_args
					? copy_cir_subtree(CIR_NODE(src_args), subst)
					: NULL;
				node_t args = args_copy ? args_copy->as_node() : list();
				node_t call = node2(N_CALL,
						    id(ri->second.c_str(), origin),
						    args, origin);
				CIR_NODE(call)->tree1_origin = src;
				return CIR_NODE(call);
			}
		}
	}

	if (subst && src->base.code == N_CALL && src->origin_id) {
		TokenCallFunc *tcf =
			dynamic_cast<TokenCallFunc *>(madc_token_for_slot(src->origin_id));
		if (dynamic_cast<TokenMember *>(tcf))
			tcf = NULL;
		bool changed = false;
		std::vector<DataDef *> concrete_param_types;
		std::string err;
		Variable *winner = resolve_copied_dependent_call(
			tcf, subst, &changed, &concrete_param_types, &err);
		if (changed) {
			if (!winner)
				return CIR_NODE(error_node(
					err.empty()
					? "tsubst: unresolved dependent call"
					: err.c_str(), tcf));
			FuncDef *wfd = dynamic_cast<FuncDef *>(winner->type);
			std::string sym = func_emit_name(*winner, wfd);
			if (sym.empty())
				return CIR_NODE(error_node(
					"tsubst: unresolved dependent call symbol",
					tcf));
			if (!is_c2mir_builtin_call_name(tcf->var.name))
				referenced_funcs.insert(sym);
			node_t src_args = c2mir_node_op(src->as_node(), 1);
			node_t args = list();
			size_t i = 0;
			for (node_t a = src_args ? c2mir_node_first_op(src_args) : NULL;
			     a != NULL; a = c2mir_node_next_op(a), ++i) {
				TokenBase *arg = (i < tcf->parameters.size())
					       ? tcf->parameters[i] : NULL;
				DataDef *pt = (wfd && i < wfd->parameters.size())
					    ? wfd->parameters[i] : NULL;
				bool refp = wfd && wfd->is_ref_param(i);
				node_t rewritten =
					copied_call_arg_for_formal(arg, a, pt,
								   refp, subst);
				if (!rewritten)
					return CIR_NODE(error_node(
						"tsubst: failed dependent call arg copy",
						tcf));
				append(args, rewritten);
			}
			node_t call = node2(N_CALL, id(sym.c_str(), tcf), args, tcf);
			CIR_NODE(call)->tree1_origin = src;
			if (wfd && wfd->returns_reference()
			    && !m_tsubst_copy_under_deref) {
				node_t deref = node1(N_DEREF, call, tcf);
				CIR_NODE(deref)->tree1_origin = src;
				return CIR_NODE(deref);
			}
			return CIR_NODE(call);
		}
	}

	if (subst && src->base.code == N_ADDR && m_tsubst_copy_pack_index < 0) {
		TokenMember *tm = src->origin_id
			? dynamic_cast<TokenMember *>(madc_token_for_slot(src->origin_id))
			: NULL;
		node_t inner = c2mir_node_first_op(src->as_node());
		if (tm && !tm->parent_expr && tm->object.is_reference()
		    && inner && cir_id_spells(CIR_NODE(inner),
					     tm->object.name.c_str())) {
			cir_node *copied = copy_cir_subtree(CIR_NODE(inner), subst);
			if (!copied)
				return NULL;
			return copied;
		}
		if (inner && m_cur_method) {
			for (Variable *pv : m_cur_method->parameters) {
				if (pv && pv->is_reference()
				    && cir_id_spells(CIR_NODE(inner),
						     pv->name.c_str())) {
					cir_node *copied =
						copy_cir_subtree(CIR_NODE(inner), subst);
					if (!copied)
						return NULL;
					return copied;
				}
			}
		}
	}

	cir_node *dst = arena.alloc();           // zeroed (error_msg/tree1_origin NULL)
	dst->base.code = src->base.code;
	dst->base.uid  = c2mir_next_uid(c2m);    // uids must be unique per c2m ctx
	dst->base.attr = NULL;                   // sema is recomputed on the copy

	// Children. c2mir_node_first_op returns NULL for a leaf OR an empty interior;
	// in both cases copying the union wholesale is correct — a leaf's scalar
	// payload (incl. an interned string pointer, valid for this c2m context's
	// lifetime) is reproduced exactly, and an empty interior's {head,tail} =
	// {NULL,NULL} IS an initialized empty ops list. A non-empty interior takes
	// the recursive branch and never copies the (self-aliasing) union.
	node_t child0 = c2mir_node_first_op(src->as_node());
	if (child0 == NULL) {
		dst->base.u = src->base.u;
		rename_copied_pack_value_id(src, dst);
	} else {
		c2mir_init_node_ops(dst->as_node());
		size_t child_index = 0;
		for (node_t c = child0; c != NULL;
		     c = c2mir_node_next_op(c), ++child_index) {
			cir_node *cc = CIR_NODE(c);
			if (subst && src->base.code == N_SPEC_DECL
			    && child_index == 2 && src->datadef
			    && attr_list_has_cleanup(c)) {
				DataDef *concrete =
					subst_datadef_active(src->datadef, *subst);
				DataDefCLASS *cdd = as_class_instance(concrete);
				if (cdd) {
					if (class_needs_dtor(cdd)) {
						std::string dtor_sym =
							class_complete_dtor_symbol(cdd);
						referenced_funcs.insert(dtor_sym);
						node_t attr_args = list();
						append(attr_args,
						       id(dtor_sym.c_str(),
							  madc_token_for_slot(src->origin_id)));
						node_t attrs = list();
						append(attrs, node2(N_ATTR,
							id("cleanup",
							   madc_token_for_slot(src->origin_id)),
							attr_args,
							madc_token_for_slot(src->origin_id)));
						c2mir_op_append(c2m, dst->as_node(), attrs);
					} else {
						c2mir_op_append(c2m, dst->as_node(),
								ignore());
					}
					continue;
				}
			}
			if (subst && cc->tsubst_pack_expand
			    && m_tsubst_copy_pack_index < 0) {
				if (!m_tsubst_active_type_arg_packs
				    || cc->tsubst_pack_index
				       >= m_tsubst_active_type_arg_packs->size()) {
					append(dst->as_node(), error_node(
						"tsubst: missing type argument pack"));
					continue;
				}
				std::map<unsigned, DataDef *>::iterator pmi =
					m_tsubst_active_pack_params.find(
						cc->tsubst_pack_index);
				if (pmi == m_tsubst_active_pack_params.end()
				    || !pmi->second) {
					append(dst->as_node(), error_node(
						"tsubst: missing pack parameter"));
					continue;
				}
				const std::vector<DataDef *> &elems =
					(*m_tsubst_active_type_arg_packs)
						[cc->tsubst_pack_index];
				if (dst->base.code != N_LIST && elems.size() != 1) {
					append(dst->as_node(), error_node(
						"tsubst: pack expansion outside list"));
					continue;
				}
				for (size_t e = 0; e < elems.size(); ++e) {
					if (!elems[e]) {
						append(dst->as_node(), error_node(
							"tsubst: null pack element"));
						continue;
					}
					std::map<DataDef *, DataDef *> elem_subst = *subst;
					elem_subst[pmi->second] = elems[e];
					int saved_index = m_tsubst_copy_pack_index;
					size_t saved_elem = m_tsubst_copy_pack_elem;
					const char *saved_name =
						m_tsubst_copy_pack_value_name;
					m_tsubst_copy_pack_index =
						(int)cc->tsubst_pack_index;
					m_tsubst_copy_pack_elem = e;
					m_tsubst_copy_pack_value_name =
						cc->tsubst_pack_value_name;
					cir_node *fan = copy_cir_subtree(cc, &elem_subst);
					m_tsubst_copy_pack_index = saved_index;
					m_tsubst_copy_pack_elem = saved_elem;
					m_tsubst_copy_pack_value_name = saved_name;
					if (fan)
						append(dst->as_node(), fan->as_node());
				}
				continue;
			}
			// A deferred placeholder type-spec MARKER (N_IGNORE carrying a
			// template-parameter datadef, left by append_type_specs in pattern
			// mode): expand it IN PLACE to the concrete type's specs via
			// append_type_specs — the SAME lowering the re-parse path uses, so the
			// substituted tree is byte-identical. Unbound -> an error node so the
			// caller falls back to re-parse. Only in a tsubst copy (`subst`).
			if (subst && cc->base.code == N_IGNORE && cc->datadef
			    && !dynamic_cast<TokenNEW *>(
				    madc_token_for_slot(cc->origin_id))) {
				DataDefSTRUCT *marker_sdd =
					dynamic_cast<DataDefSTRUCT *>(cc->datadef);
				bool aggregate_marker =
					marker_sdd && struct_has_dependent_member(marker_sdd);
				if (!cc->datadef->is_template_param()
				    && !aggregate_marker) {
					cir_node *copied = copy_cir_subtree(cc, subst);
					c2mir_op_append(c2m, dst->as_node(),
							 copied->as_node());
					continue;
				}
				DataDef *concrete =
					subst_datadef_active(cc->datadef, *subst);
				DataDefSTRUCT *concrete_sdd =
					dynamic_cast<DataDefSTRUCT *>(concrete);
				if (!concrete
				    || concrete == cc->datadef
				    || concrete->is_template_param()
				    || (concrete_sdd
					&& struct_has_dependent_member(concrete_sdd))) {
					append(dst->as_node(), error_node(
						aggregate_marker
						? "tsubst: unsupported dependent local aggregate type marker"
						: "tsubst: unbound template parameter in type marker"));
				} else {
					node_t specs = type_list(concrete);
					for (node_t sp = c2mir_node_first_op(specs);
					     sp != NULL; sp = c2mir_node_next_op(sp))
						append(dst->as_node(), sp);
				}
				continue;
			}
			bool saved_under_deref = m_tsubst_copy_under_deref;
			m_tsubst_copy_under_deref = src->base.code == N_DEREF;
			cir_node *copied = copy_cir_subtree(cc, subst);
			m_tsubst_copy_under_deref = saved_under_deref;
			c2mir_op_append(c2m, dst->as_node(), copied->as_node());
		}
	}

	// madc extension fields (invisible to c2mir). The heavy type info is
	// REFERENCED, not duplicated — datadef/typedef_name/error_msg are
	// arena/Program-owned and share the source's lifetime (Tree-1 == ROM). Every
	// build-time madc field is preserved so the copy is a faithful twin; ONLY
	// c2mir's `attr` (cleared above) is dropped, because it is c2mir's per-compile
	// post-check scratch, recomputed when the copy is checked. In particular
	// error_msg MUST be carried over: it marks an untranslatable node, and
	// cir_report_errors rejects any tree containing one before c2mir sees it —
	// dropping it would silently defeat that gate.
	dst->origin_id         = src->origin_id;
	// datadef: in a tsubst copy (`subst` active) rewrite a template-parameter
	// placeholder to its concrete type; otherwise carry the source's type by
	// reference (Tree-1 == ROM, shared lifetime). subst==NULL => byte-identical
	// to the pre-Phase-3 behaviour.
	dst->datadef           = (subst && src->datadef)
				     ? subst_datadef_active(src->datadef, *subst)
				     : src->datadef;
	// Local-class TAG REBIND: class_tag_ref bakes the tag NAME string at
	// pattern build, so when the binding remaps the tag's class (pattern local
	// class -> the instantiation's concrete <owner>__<local> class) the copied
	// tag N_ID must be renamed too — otherwise the declaration keeps the
	// PATTERN's tag while the local class's ctor/dtor take the concrete struct
	// (c2mir "incompatible argument type for pointer type parameter" on every
	// basic_string::_M_construct hit's _Guard).
	if (subst && src->datadef && dst->datadef && dst->datadef != src->datadef
	    && (dst->base.code == N_STRUCT || dst->base.code == N_UNION)
	    && !dst->datadef->name.empty()) {
		node_t tag = c2mir_node_first_op(dst->as_node());
		cir_node *tn = tag ? CIR_NODE(tag) : NULL;
		if (tn && tn->base.code == N_ID) {
			const char *nm = dst->datadef->name.c_str();
			tn->base.u.s.s = c2mir_uniq_str(c2m, nm, strlen(nm) + 1);
			tn->base.u.s.len = strlen(nm) + 1;
		}
	}
	dst->typedef_name      = src->typedef_name;
	dst->error_msg         = src->error_msg;
	dst->src_lang          = src->src_lang;
	dst->synth_from_origin = src->synth_from_origin;
	dst->tree1_origin      = src;            // provenance back-ref
	dst->tsubst_pack_expand = src->tsubst_pack_expand;
	dst->tsubst_pack_index = src->tsubst_pack_index;
	dst->tsubst_pack_value_name = src->tsubst_pack_value_name;
	if (m_tsubst_copy_pack_index >= 0)
		dst->tsubst_pack_expand = false;
	rewrite_copied_dependent_call_id(src, dst, subst);

	// Mirror the source's c2mir position into node_positions for the fresh uid
	// (derived from the shared origin token, so identical to the source). Guard
	// on origin_id, matching make()'s "only when there is an origin".
	if (src->origin_id)
		set_pos(dst, src->src_file(), src->src_line(), src->src_column());

	return dst;
}

// `tsubst` proper (two-tree Phase 3): copy the immutable Tree-1 subtree into a
// fresh Tree-2 subtree, substituting template-parameter placeholders with the
// concrete types in `subst`. The saved pattern is left untouched (g++'s tsubst
// over DECL_SAVED_TREE). See the declaration in cir_builder.h.
cir_node *CirBuilder::tsubst_cir(cir_node *src,
				 const std::map<DataDef *, DataDef *> &subst)
{
	return copy_cir_subtree(src, &subst);
}

// -----------------------------------------------------------------------
// Type builders
// -----------------------------------------------------------------------

// A class instance (`class Foo { ... };`, including header-defined std classes)
// lowers to a plain C struct, matching the Cfront C++->C model. Returns the
// class DataDef when `dd` denotes a non-pointer value class, else NULL.
static DataDef *unqualified_type(DataDef *dd)
{
	if (DataDefCONST *cd = dynamic_cast<DataDefCONST *>(dd))
		return cd->base_type ? cd->base_type : dd;
	return dd;
}

static const DataDef *unqualified_type(const DataDef *dd)
{
	if (const DataDefCONST *cd = dynamic_cast<const DataDefCONST *>(dd))
		return cd->base_type ? cd->base_type : dd;
	return dd;
}

static DataDefCLASS *as_user_class(DataDef *dd)
{
	dd = unqualified_type(dd);
	if (!dd) return NULL;
	if (dd->basetype() != BaseType::btClass) return NULL;
	if (dd->is_reference()) return NULL;
	if (dd->rawtype() != DataType::dtRESERVED) return NULL;
	return dynamic_cast<DataDefCLASS *>(dd);
}

static const DataDefCLASS *as_user_class(const DataDef *dd)
{
	dd = unqualified_type(dd);
	if (!dd) return NULL;
	if (dd->basetype() != BaseType::btClass) return NULL;
	if (dd->is_reference()) return NULL;
	if (dd->rawtype() != DataType::dtRESERVED) return NULL;
	return dynamic_cast<const DataDefCLASS *>(dd);
}

static const DataDefENUM *as_enum_type(const DataDef *dd)
{
	dd = unqualified_type(dd);
	return dynamic_cast<const DataDefENUM *>(dd);
}

static bool same_enum_type(const DataDefENUM *a, const DataDefENUM *b)
{
	if (!a || !b) return false;
	const std::string &an = a->canonical_cpp_spelling.empty()
			      ? a->enum_name : a->canonical_cpp_spelling;
	const std::string &bn = b->canonical_cpp_spelling.empty()
			      ? b->enum_name : b->canonical_cpp_spelling;
	return an == bn;
}

// A non-pointer class value lowers to a real C struct and uses the class
// ctor/dtor/cleanup machinery.
static DataDefCLASS *as_class_instance(DataDef *dd)
{
	return as_user_class(dd);
}

static bool tsubst_is_class_object_arg(DataDef *dd)
{
	if (!dd)
		return false;
	if (DataDefREF *rd = dynamic_cast<DataDefREF *>(dd))
		return as_class_instance(rd->base_type) != NULL;
	if (dd->is_pointer())
		return false;
	return as_class_instance(dd) != NULL;
}

// First-class references (docs/plans/2026-06-17-first-class-references.md):
// a reference parameter or return type is a DataDefREF so is_reference() is the
// single source of truth (the parallel FuncDef::ref_params / returns_ref /
// vfREFERENCE side-channels are all retired). A DataDefREF renders its name as
// `T*`, so the emitted prototype/ABI is unchanged; class_behind() already unwraps
// it to the class for dispatch. Used by the operator/manipulator instantiation
// sites that synthesize FuncDefs (both param and return slots). Phase 4 routes it
// through the ONE reference-creation/collapse path (Program::getReferenceType).
DataDef *CirBuilder::as_reference_type(DataDef *dd)
{
	if (!dd) return dd;
	if (m_prog) return m_prog->getReferenceType(dd);
	if (dd->is_reference()) return dd;
	return new DataDefREF(*dd);   // m_prog-less fallback (collapse preserved)
}

// c2mir lowers these call names in-place as builtins (it even rejects user
// declarations of them); never emit an extern prototype for one.
static bool is_c2mir_builtin_call_name(const std::string &name)
{
	return name.compare(0, 13, "__builtin_va_") == 0
	    || name == "__builtin_add_overflow"
	    || name == "__builtin_sub_overflow"
	    || name == "__builtin_mul_overflow";
}

static DataDefCLASS *class_behind(DataDef *dd);

static DataDefCLASS *param_object_class(DataDef *dd, bool refp)
{
	if (refp) return class_behind(dd);
	return NULL;
}

static bool same_object_class(const DataDef *a, const DataDef *b)
{
	if (!a || !b) return false;
	a = unqualified_type(a);
	b = unqualified_type(b);
	if (a == b && a->is_object() && b->is_object())
		return true;
	const DataDefCLASS *ac = as_user_class(a);
	const DataDefCLASS *bc = as_user_class(b);
	return ac && bc && ac == bc;
}

static const DataDefCLASS *class_pointer_pointee(const DataDef *dd)
{
	dd = unqualified_type(dd);
	const DataDefPTR *ptr = dynamic_cast<const DataDefPTR *>(dd);
	if (!ptr || !ptr->base_type)
		return NULL;
	return as_user_class(ptr->base_type);
}

static std::vector<c2mir_node_code_t> native_scalar_specs(DataDef *dd)
{
	if (!dd) return {N_LONG};
	switch (dd->rawtype()) {
	case DataType::dtVOID:   return {};
	case DataType::dtBOOL:   return {N_BOOL};
	case DataType::dtFLOAT:  return {N_FLOAT};
	case DataType::dtDOUBLE: return {N_DOUBLE};
	case DataType::dtUINT8:
	case DataType::dtUINT16:
	case DataType::dtUINT32: return {N_UNSIGNED, N_INT};
	case DataType::dtUINT64: return {N_UNSIGNED, N_LONG};
	case DataType::dtINT8:
	case DataType::dtINT16:
	case DataType::dtINT32:  return {N_INT};
	case DataType::dtINT64:  return {N_LONG};
	case DataType::dtINT128: return {N_INT128};
	case DataType::dtUINT128: return {N_UNSIGNED, N_INT128};
	default:                 return {N_LONG};
	}
}

static CirBuilder::ExternParam native_param_shape(DataDef *dd, bool refp)
{
	if (param_object_class(dd, refp))
		return { {N_VOID}, true, NULL };
	if (DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd)) {
		if (p->base_type && p->base_type->rawtype() == DataType::dtCHAR)
			return { {N_CHAR}, true, NULL };
		return { {N_VOID}, true, NULL };
	}
	// A by-VALUE class/struct param (not a reference — that took the
	// param_object_class void* branch above): declare it as the struct
	// tag, by value. Without this the class fell through to
	// native_scalar_specs -> {N_LONG}, so the extern proto declared an
	// ARITHMETIC param while the call passed the struct value
	// (object_arg_value) -> c2mir "incompatible argument type for
	// arithmetic type parameter" (real vector's _M_fill_insert taking a
	// __normal_iterator by value).
	if (DataDefCLASS *vc = as_class_instance(dd))
		return { {}, false, vc };
	return { native_scalar_specs(dd), false, NULL };
}

static void native_func_shape(FuncDef *fd, bool &ret_ptr,
			      std::vector<c2mir_node_code_t> &ret_specs,
			      std::vector<CirBuilder::ExternParam> &params)
{
	// A C++ reference return IS an address return (T& comes back as T*),
	// same as the class-method extern shape.
	ret_ptr = fd && (fd->return_value_type().is_pointer() || fd->returns_reference());
	ret_specs = ret_ptr ? std::vector<c2mir_node_code_t>{N_VOID}
			    : native_scalar_specs(fd ? &fd->return_value_type() : NULL);
	if (ret_specs.empty())
		ret_ptr = false;
	if (!fd) return;
	for (size_t i = 0; i < fd->parameters.size(); i++) {
		bool refp = fd->is_ref_param(i);
		params.push_back(native_param_shape(fd->parameters[i], refp));
	}
}

node_t CirBuilder::pointer()
{
	return node1(N_POINTER, list());
}

// Append type specifier nodes for a DataDef into a LIST node.
void CirBuilder::append_type_specs(node_t lst, DataDef *dd)
{
	if (!dd) { append(lst, simple(N_INT)); return; }

	// An UNRESOLVED template parameter must never reach type lowering: a
	// pattern containing `T` is Tree-1 only, and tsubst replaces `T` with the
	// concrete argument before the per-TU tree is built. If one arrives here it
	// means a not-yet-substituted placeholder leaked into the c2mir-bound tree
	// (a Phase 2/3 substitution bug). Emit an error node so the cir_report_errors
	// gate rejects the tree LOUDLY, rather than silently mis-lowering `T` to the
	// N_VOID its dtVOID rawtype would otherwise select. (two-tree Phase 1.5.)
	if (dd->is_template_param()) {
		if (m_tsubst_pattern_mode) {
			// Building a Tree-1 recipe pattern: leave the placeholder as a
			// deferred type-spec MARKER — an N_IGNORE node carrying the
			// placeholder datadef — that tsubst expands to the concrete type's
			// specs per instantiation (g++'s TEMPLATE_TYPE_PARM in the saved
			// tree). The pattern is NEVER compiled directly, so the marker never
			// reaches c2mir un-substituted.
			node_t m = ignore();
			CIR_NODE(m)->datadef = dd;
			append(lst, m);
			return;
		}
		append(lst, error_node(
			("unsubstituted template parameter '" + dd->name
			 + "' reached type lowering").c_str()));
		return;
	}

	// SIMD/vector type: emit the ELEMENT type's specs followed by a vector_size
	// N_ATTR placed DIRECTLY in the spec-qual list. c2mir's type-spec checker
	// skips N_ATTR in the normal spec loop and then runs apply_vector_attr_list
	// over the same list (c2mir.c), rewriting the result into a real vector type.
	// This covers every alias-less site that builds a spec list from a
	// DataDefSIMD — casts `(V)x`, abstract type-names, params — uniformly. (A
	// typedef alias short-circuits in type_list before reaching here; the typedef
	// DEFINITION carries the attr on the N_SPEC_DECL attrs operand instead.)
	if (DataDefSIMD *simd = dynamic_cast<DataDefSIMD *>(dd)) {
		append_type_specs(lst, simd->element_type);
		node_t attr_args = list();
		append(attr_args, integer((long)simd->vector_bytes));
		append(lst, node2(N_ATTR, id("vector_size"), attr_args));
		return;
	}

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
	case DataType::dtBOOL:   append(lst, simple(N_BOOL)); break;
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
	case DataType::dtINT128: append(lst, simple(N_INT128)); break;
	case DataType::dtUINT128:
		append(lst, simple(N_UNSIGNED));
		append(lst, simple(N_INT128));
		break;
	case DataType::dtFLOAT:  append(lst, simple(N_FLOAT)); break;
	case DataType::dtDOUBLE: append(lst, simple(N_DOUBLE)); break;
	default:
		append(lst, simple(N_INT));
	}
}

// Build the attribute list for a vector type: N_LIST( N_ATTR(N_ID("vector_size"),
// N_LIST(<bytes>)) ), the shape c2mir's apply_vector_attr_list consumes at the
// declaration's attrs operand (N_SPEC_DECL op 2 / N_MEMBER op 2). Reused by every
// site that declares a DataDefSIMD-typed entity (typedef, member, var, cast).
node_t CirBuilder::simd_vector_attrs(size_t vector_bytes, TokenBase *origin)
{
	node_t attr_args = list();
	append(attr_args, integer((long)vector_bytes, origin));
	node_t attrs = list();
	append(attrs, node2(N_ATTR, id("vector_size", origin), attr_args, origin));
	return attrs;
}

// ---------------------------------------------------------------------------
// Generic class-object lowering
// ---------------------------------------------------------------------------

bool CirBuilder::is_class_object(DataDef *dd)
{
	return as_class_instance(dd) != NULL;
}

bool CirBuilder::is_class_object_value(TokenBase *arg)
{
	if (class_subscript_is_object(arg) || class_array_subscript_is_object(arg))
		return true;
	// type() discriminators gate the downcasts: TokenCallMethod derives
	// from TokenMember which derives from TokenCallFunc which derives from
	// TokenVar — an ungated downcast would classify a method CALL (an
	// rvalue when returning by value) as an addressable member/variable
	// lvalue. (dynamic_cast stays — TokenVar virtually inherits TokenBase.)
	if (arg && arg->type() == TokenType::ttMember)
		if (TokenMember *tm = dynamic_cast<TokenMember *>(arg))
			return as_class_instance(tm->datadef()) != NULL;
	if (arg && arg->type() == TokenType::ttVariable)
		if (TokenVar *tv = dynamic_cast<TokenVar *>(arg)) {
			if (tv->var.name.compare(0, 11, "__literal__") == 0)
				return false;
			if ((tv->var.is_reference()) && class_behind(tv->var.type))
				return true;
			return as_class_instance(tv->var.type) != NULL;
		}
	return false;
}

// A CALL to a madc-COMPILED function returning a non-trivial class by value
// (non-pointer). func_def lowers such a function through the __retbuf ABI, so
// the call site must materialize the result into a temp it owns. External /
// native class-returning functions keep their own ABI and are NOT rewritten;
// matching them here would inject a bogus __retbuf arg.
// The FuncDef behind a CALL token: either the called function directly, or the
// TARGET signature of a function-pointer variable being called indirectly
// (`auto f = [...]; f()`). Both use the same __retbuf ABI for object returns.
static FuncDef *call_target_funcdef_raw(TokenCallFunc *tcf)
{
	if (!tcf) return NULL;
	if (FuncDef *fd = dynamic_cast<FuncDef *>(tcf->var.type))
		return fd;
	if (DataDefFPTR *fp = dynamic_cast<DataDefFPTR *>(tcf->var.type))
		return fp->target;
	return NULL;
}

// Member wrapper: the ONE callee resolver every consumer (arg emission, retbuf
// classification, callee naming, fn-ptr decay) goes through, so a per-call
// instantiated FuncDef (a std:: free-function template bound mangled-direct via
// emit_symbol) is seen consistently by all of them. Discovery is pure data: a
// namespace function with captured free_function_overloads deduces and binds;
// everything else resolves as declared (no name-prefix gates).
Variable *CirBuilder::call_target_variable(TokenCallFunc *tcf, FuncDef **fd_out)
{
	if (fd_out)
		*fd_out = NULL;
	if (!tcf)
		return NULL;
	FuncDef *fd = call_target_funcdef_raw(tcf);
	Variable *callee_var = &tcf->var;
#if MADC_DEBUG_FNTPL
	if (tcf && tcf->var.name.compare(0, 5, "__ns_") == 0)
		fprintf(stderr, "FNTPL gate var=%s fd=%d ns='%s' disp='%s'\n",
			tcf->var.name.c_str(), fd != NULL,
			fd ? fd->namespace_name.c_str() : "",
			fd ? fd->function_display_name.c_str() : "");
#endif
	if (!fd) {
		if (fd_out)
			*fd_out = fd;
		return callee_var;
	}

	// Static member calls parsed before a template body is substituted can keep
	// the arity-selected overload while their argument tokens later acquire a
	// more precise type (e.g. `_Rb_tree::_S_key(_Base_ptr)` vs
	// `_S_key(_Link_type)`). Re-rank non-template static member overloads here,
	// using the same DataDefCLASS::findMethodOverload scorer the parser uses, so
	// the selected symbol and the selected parameter types stay in sync.
	// GUARD: a static-STORAGE variable that is merely callable — e.g. a file-scope
	// `static double (*fp)(float)` function pointer — also carries vfSTATIC, but
	// its `data` is NOT a Method (only a real function/method variable, whose
	// `type` IS a FuncDef, stores a Method there). Without the FuncDef-type check,
	// `(Method *)callee_var->data` reads garbage and `md->owner_class` segfaults
	// (gcc.c-torture func-ptr-1.c: a call through a static function pointer).
	if (m_prog && (callee_var->flags & vfSTATIC) && !fd->is_member_template
	    && dynamic_cast<FuncDef *>(callee_var->type)) {
		Method *md = (Method *)callee_var->data;
		DataDefCLASS *owner = md ? md->owner_class : NULL;
		std::string mname = method_candidate_display_name(callee_var, fd);
		if (owner && !mname.empty()) {
			std::vector<const DataDef *> at;
			at.reserve(tcf->parameters.size());
			for (TokenBase *p : tcf->parameters) {
				DataDef *adp = m_prog->array_decay_pointer(p);
				at.push_back(adp ? adp
						 : m_prog->operand_value_datadef(p));
			}
			if (Variable *winner = owner->findMethodOverload(mname, at)) {
				FuncDef *wfd = dynamic_cast<FuncDef *>(winner->type);
				if (wfd && (winner->flags & vfSTATIC)) {
					callee_var = winner;
					fd = wfd;
				}
			}
		}
	}

	// namespace_name may be EMPTY: tracked global-scope operator overload
	// sets (::operator new/delete) key as "::name" and rank identically.
	if (fd && !fd->function_display_name.empty()) {
		if (FuncDef *inst = std_free_function_instantiation(tcf, fd)) {
#if MADC_DEBUG_FNTPL
			if (tcf && tcf->var.name.compare(0, 5, "__ns_") == 0)
				fprintf(stderr, "FNTPL patternA var=%s -> emit=%s\n",
					tcf->var.name.c_str(),
					inst->emit_symbol.c_str());
#endif
			if (fd_out)
				*fd_out = inst;
			return callee_var;
		}
#if MADC_DEBUG_FNTPL
		if (tcf && tcf->var.name.compare(0, 5, "__ns_") == 0)
			fprintf(stderr, "FNTPL gate2 var=%s m_prog=%d\n",
				tcf->var.name.c_str(), m_prog != NULL);
#endif
		// NON-template namespace overload set (e.g. std::to_string's nine
		// inline definitions): rank the parsed overloads by the call's arg
		// types — the same generic ranking methods use — and resolve to the
		// winner. Its local_emit_name carries the unique symbol, so every
		// consumer of this resolver (args, retbuf classification, callee
		// naming) sees the selected overload consistently.
		if (m_prog) {
			// Rank only the USER-WRITTEN args: parse-time-appended
			// defaults came from one overload's declaration and must
			// not veto it (TokenCallFunc::user_argc).
			size_t n = tcf->parameters.size();
			if (tcf->user_argc != (size_t)-1 && tcf->user_argc < n)
				n = tcf->user_argc;
			std::vector<const DataDef *> at;
			std::vector<bool> zeros;
			for (size_t i = 0; i < n; i++) {
				// [conv.array]/[conv.func]: an array (or function)
				// argument used as a VALUE for overload resolution
				// decays to a pointer. A bare `int a[N]` argument types
				// as the element `int` (the array-ness is a Variable
				// flag, not a DataDefCArray), so without decay it failed
				// to match the instantiated iterator overload's `int*`
				// parameter and the call fell back to the declaration-
				// only placeholder (undefined `__ns_<fn>` import). The
				// parse-time re-rank (resolved_call_funcdef) decays the
				// same way. array_decay_pointer returns NULL otherwise.
				DataDef *adp = m_prog->array_decay_pointer(
						tcf->parameters[i]);
				at.push_back(adp ? adp
					: m_prog->operand_value_datadef(
						tcf->parameters[i]));
				zeros.push_back(is_zero_integer_literal(
						tcf->parameters[i]));
			}
			if (Variable *w = m_prog->find_namespace_function_overload(
					fd->namespace_name, fd->function_display_name,
					at, &zeros, &tcf->explicit_template_args))
				if (FuncDef *wfd = dynamic_cast<FuncDef *>(w->type)) {
					if (fd_out)
						*fd_out = wfd;
					return w;
				}
		}
	}
	if (fd_out)
		*fd_out = fd;
	return callee_var;
}

FuncDef *CirBuilder::call_target_funcdef(TokenCallFunc *tcf)
{
	FuncDef *fd = NULL;
	call_target_variable(tcf, &fd);
	return fd;
}

std::string CirBuilder::call_target_emit_name(TokenCallFunc *tcf,
					      FuncDef **fd_out)
{
	FuncDef *fd = NULL;
	Variable *v = call_target_variable(tcf, &fd);
	if (fd_out)
		*fd_out = fd;
	return v ? func_emit_name(*v, fd) : std::string();
}

// A CALL to a madc-COMPILED function returning a non-trivial class by value.
// Such a call is lowered (func_def/func_proto) through the __retbuf ABI, so the
// call site must materialize the result into a caller temp it owns. Gated on
// m_user_func_names: an external/native class-returning fn keeps its own ABI and
// must NOT be rewritten (a bogus __retbuf arg would be "too many arguments").
DataDefCLASS *CirBuilder::object_returning_call_class(TokenBase *arg)
{
	// Functional-construction temporary T(args) is a class rvalue too.
	if (TokenObjTemp *ot = dynamic_cast<TokenObjTemp *>(arg))
		return ot->obj_class;
	TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(arg);
	if (!tcf) return NULL;
	FuncDef *fd = NULL;
	std::string sym = call_target_emit_name(tcf, &fd);
	if (!fd) return NULL;
	if (fd->returns_reference()) return NULL;
	DataDefCLASS *cdd = class_return_via_retbuf(&fd->return_value_type());
	if (!cdd) return NULL;
	// A fn-ptr call inherits the retbuf ABI from its rendered target type; a
	// direct call is gated on m_user_func_names — by the call's RESOLVED emit
	// symbol (call_emit_symbol precedence), so an overload-set winner whose
	// symbol differs from the called Variable's name (local_emit_name) and a
	// call through a using-imported alias both classify by the function that
	// is actually emitted.
	if (dynamic_cast<DataDefFPTR *>(tcf->var.type))
		return cdd;
	// A deferred lazy body ([temp.inst]) materializes as a madc-emitted
	// definition (retbuf ABI) once referenced — classify it that way both
	// BEFORE materialization (still in deferred_lazy_bodies) and AFTER
	// (m_materialized_lib_syms; the deferred entry is erased on parse).
	if (!(m_user_func_names && m_user_func_names->count(sym) > 0)
	    && !m_materialized_lib_syms.count(sym)
	    && !(m_prog && m_prog->has_deferred_lazy_body(sym)))
		return NULL;
	return cdd;
}

// Words of opaque storage for a runtime-object class: one with a concrete ABI
// size but no parsed data members. Ordinary user classes return 0 because their
// structs are built from declared members.
size_t CirBuilder::object_class_words(DataDefCLASS *cdd) const
{
	if (!cdd || !cdd->members.empty()) return 0;
	if (cdd->size == 0) return 0;
	if (cdd->is_complete && cdd->size == 1 && !cdd->has_vptr_slot)
		return 0;
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

// Tag-REFERENCE node for an aggregate type: N_UNION when the class/struct
// uses union layout ([class.union] via DataDefSTRUCT::union_layout),
// N_STRUCT otherwise. EVERY reference site must agree with the definition's
// kind or c2mir rejects the module ("kind of tag X is unmatched with
// previous declaration" — real vector's _Temporary_value::_Storage union).
node_t CirBuilder::class_tag_ref(DataDef *dd, TokenBase *origin)
{
	DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(dd);
	node_t ref = node2(sdd && sdd->union_layout ? N_UNION : N_STRUCT,
			   id(dd->name.c_str(), origin), ignore());
	// Tree-1 recipe: carry the tag's class identity so tsubst can rebind a
	// pattern-LOCAL class tag to the instantiation's concrete class — the tag
	// NAME string baked here is deaf to datadef substitution. Pattern mode
	// only, so production trees are byte-identical.
	if (m_tsubst_pattern_mode)
		CIR_NODE(ref)->datadef = dd;
	return ref;
}

// N_TYPE node for a `struct Cls *` cast target (matches class_struct_def's
// `struct Cls` spec + one pointer level). Used for derived->base upcasts.
node_t CirBuilder::class_ptr_type(DataDefCLASS *cdd)
{
	node_t spec = list();
	append(spec, class_tag_ref(cdd));
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
	// A DEPENDENT pointee (a template-param placeholder, whose repr rawtype
	// is dtVOID) is "type not known yet", NOT void: the Tree-1 pattern must
	// not bake the size-1 `(char *)` rewrite into a shared body — under
	// substitution the operand types concretely from the instantiated
	// shell's declaration (g++ rebuilds the op at instantiation the same way).
	if (template_param_under_type_layers(p->base_type)) return false;
	return p->base_type->rawtype() == DataType::dtVOID;
}

static bool is_char_pointer(DataDef *dd)
{
	if (!dd || !dd->is_pointer()) return false;
	DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd);
	if (!p || !p->base_type) return false;
	return p->base_type->rawtype() == DataType::dtCHAR;
}

// The single-level pointee user-class of `dd` when `dd` is a `Cls *` pointing
// at a real (dtRESERVED) user class; NULL otherwise. Runtime-library classes
// without user-class layout metadata are deliberately excluded.
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

// Derived->base reference return conversion. A C++ `Base& f() { return d; }`
// lowers to returning the address of the selected base subobject, the same
// adjustment as a `Derived*` -> `Base*` conversion but with the derived class
// taken from the returned lvalue's own type.
node_t CirBuilder::upcast_class_ref_addr(node_t value, DataDefCLASS *base,
					 TokenBase *rhs, TokenBase *origin)
{
	DataDefCLASS *derived = as_user_class(rhs ? rhs->datadef() : NULL);
	if (!base || !derived || base == derived) return value;
	if (!derived->is_or_derives_from(base)) return value;
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
// The madc c2mir fork calls dtor_sym(&name) automatically at EVERY scope exit
// (fall-through, return, break, continue, goto) — proper RAII without per-exit
// dtor injection in the front end. We
// build the N_ATTR node directly (no source preprocessing, so the glibc
// `__attribute__`-emptying issue doesn't apply).
node_t CirBuilder::obj_storage_decl(const char *name, size_t words,
				    const char *dtor_sym, TokenBase *origin,
				    size_t align)
{
	node_t spec = list();
	// An object whose alignment exceeds the long[] buffer's natural 8
	// (e.g. the 16-aligned madc_value inside madc::value) declares it:
	// _Alignas(align) long name[words].
	if (align > alignof(long))
		append(spec, node1(N_ALIGNAS, integer((long)align, origin)));
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
	append(args, object_addr(name, origin));
	node_t call = node2(N_CALL, id(ctor_sym, origin), args, origin);
	CIR_NODE(call)->synth_from_origin = true;
	node_t stmt = node2(N_EXPR, list(), call, origin);
	CIR_NODE(stmt)->synth_from_origin = true;
	return stmt;
}

// Hidden return-slot pointer parameter for by-value object returns.
const char *CirBuilder::RETBUF_NAME = "__retbuf";

// ---- madc array (`array`) object lowering ----
// A madc `array` is a real madc::value C++ object (the unified public value
// type) using the runtime-object model: an alignof(madc::value) opaque buffer +
// madarray_construct/madarray_destruct (the wrappers in madc_mir_backend.cpp).
// Unlike a string it needs no const char* coercion: an array argument is
// always passed by pointer, and the buffer's long[] name decays to that
// pointer naturally at the call site.
bool CirBuilder::is_array_object(DataDef *dd)
{
	return dd && dd->rawtype() == DataType::dtARRAY && !dd->is_pointer();
}

size_t CirBuilder::array_obj_words() const
{
	return (sizeof(madc::value) + sizeof(long) - 1) / sizeof(long);
}

node_t CirBuilder::array_storage_decl(const char *name, TokenBase *origin)
{
	return obj_storage_decl(name, array_obj_words(), "madarray_destruct", origin,
				alignof(madc::value));
}

node_t CirBuilder::array_ctor_call(const char *name, TokenBase *origin)
{
	return obj_default_ctor_call(name, "madarray_construct", origin);
}

static FuncDef *class_method_def(DataDefCLASS *cdd, const char *name)
{
	if (!cdd || !name) return NULL;
	std::string key(name);
	Variable *mv = cdd->findMethod(key);
	return mv ? dynamic_cast<FuncDef *>(mv->type) : NULL;
}

static std::string class_method_symbol(DataDefCLASS *cdd, const char *name)
{
	FuncDef *fd = class_method_def(cdd, name);
	return CirBuilder::call_emit_symbol(fd, std::string(name ? name : ""));
}

// `(void*)&<name>` — an object instance address as void*. Used for synthetic
// temporaries; named program variables use object_var_void_addr so parameters
// lowered as pointers keep their stored address.
node_t CirBuilder::object_addr(const char *name, TokenBase *origin)
{
	node_t cast = node2(N_CAST, void_ptr_type(),
			    node1(N_ADDR, id(name, origin), origin), origin);
	CIR_NODE(cast)->synth_from_origin = true;
	return cast;
}

// The object address of a NAMED variable as void*. A by-value object parameter
// or reference lowered to a pointer uses the variable value; a local/global
// value instance uses `&name`.
node_t CirBuilder::object_var_void_addr(const Variable &v, TokenBase *origin)
{
	node_t cast = node2(N_CAST, void_ptr_type(), object_var_addr(v, origin), origin);
	CIR_NODE(cast)->synth_from_origin = true;
	return cast;
}

// The hidden capture-parameter type for a [&]-captured (or GNU-nested-fn-captured)
// variable. Capture-by-reference passes a pointer to the enclosing variable's
// storage. A REFERENCE variable is already stored as a pointer to its referent,
// so its capture parameter is a plain pointer to that referent (`int *` for
// `int &`), and the call site forwards the stored pointer VALUE rather than its
// address. Every other variable is captured as `T *` (a pointer to its storage),
// with the call site forwarding `&var`. All three synthesis sites
// (func_def / func_proto / fnptr_func_node) MUST use this so the definition,
// prototype, and fn-ptr type agree.
static DataDef *capture_param_type(const Variable *cv)
{
	if (!cv || !cv->type)
		return NULL;
	if (cv->type->is_reference())
		if (DataDef *referent = ref_param_referent(cv->type))
			return new DataDefPTR(*referent);
	return new DataDefPTR(*cv->type);
}

// Coerce an object argument with a c_str() method to const char*.
node_t CirBuilder::object_cstr_arg(TokenBase *arg)
{
	DataDefCLASS *cdd = as_class_instance(arg ? arg->datadef() : NULL);
	if (!cdd)
		cdd = class_behind(arg ? arg->datadef() : NULL);
	if (!cdd)
		return translate_expr(arg);
	std::string sym = class_method_symbol(cdd, "c_str");
	if (sym.empty() || sym == "c_str")
		return translate_expr(arg);
	need_output_extern(sym.c_str(), true, { { {N_VOID}, true } });
	node_t a = list();
	append(a, object_arg_addr(arg, cdd));
	node_t call = node2(N_CALL, id(sym.c_str(), arg), a, arg);
	CIR_NODE(call)->synth_from_origin = true;
	return call;
}

// Coerce an argument to a class-object pointer for an object/reference
// parameter. Existing object lvalues pass by address; a value accepted by a
// converting ctor is materialized into a scope-lived temp.
DataDef *CirBuilder::ref_returning_call_type(TokenBase *arg)
{
	TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(arg);
	if (!tcf) return NULL;
	FuncDef *raw_fd = call_target_funcdef_raw(tcf);
	FuncDef *cfd = call_target_funcdef(tcf);
	if (!tcf->returns_ref_override && !(cfd && cfd->returns_reference()))
		return NULL;
	// Type by the call_target_funcdef-RESOLVED callee: for a late-bound
	// overload set the parse-bound var is an arbitrary set member (the
	// parser defers resolution to CIR), so the token's own returns() can
	// name another overload's return type. A parse-time return_override still
	// wins for the original callee, but if CIR resolves the call to a different
	// reference-returning overload/template instantiation, that concrete
	// FuncDef's return is authoritative (e.g. std::get<I>(tuple<T...>) where a
	// stale non-type template argument override can otherwise name `I`).
	DataDef *r = (cfd && cfd->returns_reference()
		   && (!tcf->return_override || cfd != raw_fd))
		   ? &cfd->return_value_type() : tcf->returns();
	// Unwrap EXACTLY ONE reference level. return_value_type() already returned
	// the referent (no longer a reference); the tcf->returns() fallback may
	// still be the DataDefREF. Gate the strip on is_reference() — a plain
	// POINTER referent (the `base*` of `base*&`) must be PRESERVED, not stripped
	// to its pointee: stripping it made `_M_rightmost()`'s `base*&` resolve to
	// `base`, one level too deep, so `pair<base*,base*>(const base*&, …)` never
	// matched (the arg looked like the class, not a `base*`).
	if (r && r->is_reference())
		if (DataDefPTR *rp = dynamic_cast<DataDefPTR *>(r))
			if (rp->base_type) r = rp->base_type;
	return r;
}

node_t CirBuilder::object_arg_addr(TokenBase *arg, DataDefCLASS *target)
{
	if (!target) target = as_class_instance(arg ? arg->datadef() : NULL);
	if (!target) target = class_behind(arg ? arg->datadef() : NULL);
	if (!target)
		return node2(N_CAST, void_ptr_type(), translate_expr(arg), arg);

	if (m_tsubst_pattern_mode) {
		if (TokenPackExpansion *pe = dynamic_cast<TokenPackExpansion *>(arg)) {
			DataDefTemplateParam *tp = pe->pattern
				? template_param_in_pack_pattern(pe->pattern) : NULL;
			std::string value_name = tp
				? pack_value_name_in_pattern(pe->pattern, tp->param_index)
				: std::string();
			if (tp && !value_name.empty()
			    && ref_returning_call_type(pe->pattern)) {
				node_t slot = id(value_name.c_str(), arg);
				CIR_NODE(slot)->datadef = pe->pattern->datadef();
				node_t marker = node2(N_CAST, void_ptr_type(), slot, arg);
				cir_node *cn = CIR_NODE(marker);
				cn->tsubst_pack_expand = true;
				cn->tsubst_pack_index = tp->param_index;
				cn->tsubst_pack_value_name =
					arena.intern(value_name.c_str());
				return marker;
			}
		}
	}

	// A REFERENCE-returning call (std::move(x), a T&/T&& method): the call
	// VALUE already is the referenced object's address. Bind it directly
	// (with the base-subobject offset for a derived->base upcast) — the
	// temp-construction fallback below would re-select the same binding
	// and recurse.
	if (DataDef *rt = ref_returning_call_type(arg)) {
		DataDefCLASS *rc = as_class_instance(rt);
		if (!rc) rc = class_behind(rt);
		if (rc && (rc == target || rc->is_or_derives_from(target))) {
			// translate_expr AUTO-DEREFS a ref-returning call to its
			// value; re-take the address (&* folds in c2mir).
			node_t addr = node2(N_CAST, void_ptr_type(),
					    node1(N_ADDR, translate_expr(arg), arg),
					    arg);
			size_t off = rc != target ? rc->base_offset_of(target)
						  : 0;
			if (off != 0 && off != (size_t)-1) {
				node_t charp = node2(N_CAST,
					node2(N_TYPE, node1(N_LIST, simple(N_CHAR)),
					      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
					addr, arg);
				addr = node2(N_CAST, void_ptr_type(),
					node2(N_ADD, charp, integer((long)off), arg), arg);
			}
			return addr;
		}
		// A DEPENDENT ref return (`std::get<_Indexes1>(__t1)` translated
		// by the hit-time relower): the concrete return class is not
		// knowable here, but the call VALUE is still the referenced
		// object's address — keep the addr-of-call shape; the
		// per-element copy rewrites the callee to its concrete
		// instantiation. (No base-offset adjustment: substituted get/
		// tuple accessors return the exact element type. A concrete
		// derived->base bind would need the offset arm above.) Falling
		// through would materialize a `target` temp from an unknown-
		// typed arg — the copy-ctor recursion the tail below refuses.
		if (!rc && (template_param_under_type_layers(rt)
			    || dependent_placeholder_under_type_layers(rt)))
			return node2(N_CAST, void_ptr_type(),
				     node1(N_ADDR, translate_expr(arg), arg),
				     arg);
	}

	// A Derived object bound to a Base parameter: C++ binds the base SUBOBJECT
	// (reference binding / upcast) — take the object's own address and select
	// the base at its byte offset. Without this, a derived arg fell into the
	// converting-ctor temp fallback below, CONSTRUCTING a Base temp instead of
	// binding. Offset 0 (single-inheritance primary base) emits the same
	// own-address shape as before. Virtual bases keep the prior behavior
	// (base_offset_of -1 -> no static adjustment), matching upcast_class_ptr.
	DataDefCLASS *own = as_class_instance(arg ? arg->datadef() : NULL);
	if (own && own != target && own->is_or_derives_from(target)) {
		node_t addr = object_arg_addr(arg, own);
		size_t off = own->base_offset_of(target);
		if (off != 0 && off != (size_t)-1) {
			node_t charp = node2(N_CAST,
				node2(N_TYPE, node1(N_LIST, simple(N_CHAR)),
				      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
				addr, arg);
			addr = node2(N_CAST, void_ptr_type(),
				node2(N_ADD, charp, integer((long)off), arg), arg);
		}
		return addr;
	}

	if (is_class_object_value(arg)) {
		// A class-object element (`v[i]`): the bare operator[] call is the
		// element address. Cast it to void* instead of dereferencing it.
		if (TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(arg))
			if (class_subscript_is_object(arg))
				return node2(N_CAST, void_ptr_type(),
					class_subscript_addr(tsub, arg), arg);
		// A raw class-array element: translate_expr yields the element lvalue;
		// its address is the object pointer.
		if (class_array_subscript_is_object(arg))
			return node2(N_CAST, void_ptr_type(),
				node1(N_ADDR, translate_expr(arg), arg), arg);
		// An object member is embedded in the enclosing struct. Translate the
		// member access, take its address, and cast. (type()-gated like
		// is_class_object_value — a TokenCallMethod must NOT take the
		// &member or named-variable arms.)
		if (arg->type() == TokenType::ttMember)
			return node2(N_CAST, void_ptr_type(),
			    node1(N_ADDR, translate_expr(arg), arg), arg);
		if (arg->type() == TokenType::ttVariable)
			if (TokenVar *tv = dynamic_cast<TokenVar *>(arg))
				return object_var_void_addr(tv->var, arg);
		return node2(N_CAST, void_ptr_type(), translate_expr(arg), arg);
	}

	if (object_returning_call_class(arg) == target)
		return object_call_temp_addr(arg, target, arg);
	if (TokenObjTemp *ot = dynamic_cast<TokenObjTemp *>(arg))
		if (as_class_instance(ot->obj_class) == target)
			return object_call_temp_addr(arg, target, arg);
	// A call returning a TRIVIALLY-COPYABLE class BY VALUE lowers to a raw
	// call node — an rvalue; `&call` is not an lvalue and c2mir rejects it
	// (`b.base() - a.base()`). Fall through to the materializing tail,
	// where the implicit-copy fallback assigns the value into an
	// addressable temp ([class.temporary]). NON-trivial returns never get
	// here as raw calls: madc-compiled ones took the retbuf arm above, and
	// external sret calls materialize their own slot temp inside
	// translate_expr (so &translate_expr IS a temp lvalue — routing them
	// into the tail recursed object_arg_addr -> class_ctor_call forever
	// through the copy ctor's const-ref parameter).
	bool rvalue_call = (arg && (arg->type() == TokenType::ttCallFunc
				    || arg->type() == TokenType::ttCallMethod)
			    && !ref_returning_call_type(arg)
			    && class_trivially_copyable(target));
	if (!rvalue_call
	    && as_class_instance(arg ? arg->datadef() : NULL) == target)
		return node2(N_CAST, void_ptr_type(),
			     node1(N_ADDR, translate_expr(arg), arg), arg);

	// Pattern-mode construction deferral: an `arg` whose type is still a template
	// parameter (`_Args` etc.) cannot be materialized into a concrete `target`
	// temp here — overload resolution can't reject candidates for an unknown arg
	// type, so it picks target's copy ctor (param `target&`) and re-enters
	// object_arg_addr(arg, target) forever (the class_ctor_call<->object_arg_addr
	// recursion). This is the construction-level analogue of
	// tsubst_pattern_has_dependent_call: a dependent-arg construction is not
	// lowerable in the recipe — reject the pattern so the body falls back to the
	// parsed concrete body (where arg's type is known). See the keystone analysis
	// in 2026-06-24-two-tree-phase3-tsubst-consume-HANDOFF.md §0.
	// EXCEPT a named forwarding-reference parameter (`_Arg&& __v` — always a
	// reference for every substitution, so its repr is a pointer regardless):
	// the DECISION (bind vs convert) is per-instantiation, so defer it — emit
	// an N_IGNORE marker carrying the origin variable + the target class;
	// copy_cir_subtree re-lowers it with the substituted concrete type
	// (tsubst_relower_deferred_arg_addr). The statement-level analogue is the
	// local-decl deferral marker in translate_block.
	if (m_tsubst_pattern_mode && arg
	    && template_param_under_type_layers(arg->datadef())) {
		TokenBase *bind_src = arg;
		// An identity reference-cast call (`std::forward<_Arg>(__arg)`,
		// `std::move(x)` — inline_builtin_kind "forward", detected by BODY
		// SHAPE at header retention) yields its own argument's object, so
		// the bind-vs-convert deferral applies to the INNER variable; the
		// wrapper contributes nothing (cast-to-reference is a no-op on the
		// operand object, and the copy re-lowering re-verifies layout with
		// the substituted type before binding).
		if (TokenCallFunc *fw = dynamic_cast<TokenCallFunc *>(bind_src)) {
			FuncDef *ffd = call_target_funcdef(fw);
			if (!ffd)
				ffd = call_target_funcdef_raw(fw);
			if (ffd && ffd->inline_builtin_kind == "forward"
			    && fw->parameters.size() == 1 && fw->parameters[0])
				bind_src = fw->parameters[0];
		}
		if (bind_src->type() == TokenType::ttVariable
		    && bind_src->datadef() && bind_src->datadef()->is_reference()
		    && template_param_under_type_layers(bind_src->datadef())) {
			cir_node *marker = make(N_IGNORE, bind_src);
			marker->datadef = target;
			return marker->as_node();
		}
		return error_node("tsubst: dependent-arg object construction", arg);
	}
	// The same recursion exists at HIT time: the per-instantiation relower
	// translates raw pattern tokens with pattern mode OFF, and a
	// dependent-typed arg reaches this materializing tail — overload
	// selection picks target's copy ctor (param `target&`) and re-enters
	// object_arg_addr(arg, target) forever. A concrete temp can never be
	// grounded from an unknown-typed arg: refuse, so the relower's error
	// sweep falls back to the shell carrier.
	if (arg && (template_param_under_type_layers(arg->datadef())
		    || dependent_placeholder_under_type_layers(arg->datadef())))
		return error_node("tsubst: dependent-arg object construction", arg);
	char name[32];
	snprintf(name, sizeof(name), "__madc_objtmp_%d", m_strtmp_counter++);
	Variable *tmp = new Variable(name, *target, 1, NULL, false);
	tmp->flags |= vfLOCAL;
	m_pending_stmts.push_back(var_decl(tmp, arg));
	std::vector<TokenBase *> ctor_args;
	if (arg) ctor_args.push_back(arg);
	node_t cc = class_ctor_call(tmp, target, ctor_args, arg);
	if (cc) m_pending_stmts.push_back(cc);
	return object_addr(name, arg);
}

// Coerce an argument to a class-object VALUE for a by-value parameter.
// Matching object values pass as values. Non-object values accepted by a
// converting constructor are materialized into a scope-lived temp and that temp
// is passed by value.
node_t CirBuilder::object_arg_value(TokenBase *arg, DataDefCLASS *target)
{
	if (!target)
		return translate_expr(arg);
	// Keep class-valued pack expansions as marked expressions until tsubst can
	// substitute and rename the concrete pack element.
	if (dynamic_cast<TokenPackExpansion *>(arg)
	    && template_param_in_pack_pattern(arg))
		return translate_expr(arg);
	DataDefCLASS *arg_class = as_class_instance(arg ? arg->datadef() : NULL);
	if (arg_class == target)
		return translate_expr(arg);
	if (object_returning_call_class(arg) == target)
		return translate_expr(arg);
	if (TokenObjTemp *ot = dynamic_cast<TokenObjTemp *>(arg))
		if (as_class_instance(ot->obj_class) == target)
			return translate_expr(arg);
	if (TokenVar *tv = dynamic_cast<TokenVar *>(arg))
		if ((tv->var.is_reference()) && class_behind(tv->var.type) == target)
			return translate_expr(arg);

	// Pattern-mode construction deferral (see object_arg_addr): a dependent-typed
	// arg cannot be materialized into a concrete `target` temp in the recipe —
	// reject the pattern so the body falls back to its parsed concrete form.
	if (m_tsubst_pattern_mode && arg
	    && template_param_under_type_layers(arg->datadef()))
		return error_node("tsubst: dependent-arg object value", arg);
	char name[32];
	snprintf(name, sizeof(name), "__madc_objtmp_%d", m_strtmp_counter++);
	Variable *tmp = new Variable(name, *target, 1, NULL, false);
	tmp->flags |= vfLOCAL;
	m_pending_stmts.push_back(var_decl(tmp, arg));
	std::vector<TokenBase *> ctor_args;
	if (arg) ctor_args.push_back(arg);
	node_t cc = class_ctor_call(tmp, target, ctor_args, arg);
	if (cc) m_pending_stmts.push_back(cc);
	return id(name, arg);
}

bool CirBuilder::expr_is_nonaddressable_rvalue(TokenBase *arg)
{
	if (!arg)
		return false;
	TokenType t = arg->type();
	// A `new T(args)` expression lowers to a statement-expression yielding the
	// allocated pointer — a prvalue, so `&(new T())` is ill-formed. Binding it to
	// a reference parameter (vector<T*>::push_back(const T*&)) must spill it to an
	// addressable temp first. (TokenObjTemp already yields an lvalue local, so it
	// is addressable and not listed here.)
	if (dynamic_cast<TokenNEW *>(arg))
		return true;
	// An address-of expression `&x` yields an address VALUE — a prvalue pointer
	// (you cannot take `&(&x)`). Binding it to a reference parameter
	// (vector<int*>::push_back(const int*&) with `&local`) must spill it to an
	// addressable temp. The parser builds these as TokenAddrOf / TokenAddrExpr.
	if (dynamic_cast<TokenAddrOf *>(arg) || dynamic_cast<TokenAddrExpr *>(arg))
		return true;
	// A by-value-returning call is a prvalue; a reference-returning call is an
	// lvalue (its result is the referent — addressable).
	if (t == TokenType::ttCallFunc || t == TokenType::ttCallMethod)
		return ref_returning_call_type(arg) == NULL;
	// Literals are prvalues.
	if (t == TokenType::ttInteger || t == TokenType::ttReal
	    || t == TokenType::ttChar || t == TokenType::ttString)
		return true;
	if (TokenCast *tc = dynamic_cast<TokenCast *>(arg)) {
		// A scalar or pointer cast produces a prvalue. When it binds to a
		// reference parameter, mirror C++'s temporary materialization instead
		// of emitting the invalid C form `&((T)expr)`. Casts to reference
		// type are lvalue casts and translate_expr preserves the operand.
		return !(tc->cast_type && tc->cast_type->is_reference());
	}
	if (TokenOperator *op = dynamic_cast<TokenOperator *>(arg)) {
		TokenID id = op->id();
		// Postfix ++/-- (operand on the LEFT) yields the OLD value — a prvalue.
		// Prefix forms (operand on the right) are lvalues; leave them alone.
		if ((id == TokenID::tkInc || id == TokenID::tkDec) && op->left && !op->right)
			return true;
		// Builtin BINARY arithmetic/bitwise results are prvalues. Unary forms of
		// the same tokens are NOT here (`*p` deref and `&x` address-of are
		// lvalues / already addresses), so require argc()==2.
		if (op->argc() == 2)
			switch (id) {
			case TokenID::tkAdd: case TokenID::tkSub:
			case TokenID::tkMul: case TokenID::tkDiv: case TokenID::tkMod:
			case TokenID::tkBand: case TokenID::tkBor: case TokenID::tkXor:
			case TokenID::tkBSL: case TokenID::tkBSR:
				return true;
			default: break;
			}
	}
	return false;
}

static bool class_pointer_reference_conversion(DataDef *arg_type,
					       DataDef *referent_type)
{
	if (!arg_type || !referent_type || arg_type == referent_type)
		return false;
	const DataDefCLASS *ac = class_pointer_pointee(arg_type);
	const DataDefCLASS *rc = class_pointer_pointee(referent_type);
	return ac && rc && ac != rc && ac->is_or_derives_from(rc);
}

static bool const_ref_param(FuncDef *fd, size_t i)
{
	return fd && fd->is_ref_param(i) && i < fd->const_params.size()
	    && fd->const_params[i];
}

node_t CirBuilder::ref_param_arg_addr(TokenBase *arg, DataDef *expected_referent,
				      bool allow_converted_temp)
{
	DataDef *vt = arg ? arg->datadef() : NULL;
	bool needs_converted_temp = allow_converted_temp
	    && class_pointer_reference_conversion(vt, expected_referent);
	if (vt && (expr_is_nonaddressable_rvalue(arg) || needs_converted_temp)) {
		// The reference binds to the PARAMETER's referent type, and the rvalue
		// is converted to it ([dcl.init.ref]). When the referent differs from the
		// arg's own type — a `0`/null literal bound to a pointer `_Base_ptr&`
		// (std::_Rb_tree's `_Res(__j._M_node, 0)`), where the arg is `int` but the
		// referent is `_Rb_tree_node_base*` — the temp MUST be the referent type,
		// or `&temp` is a too-narrow `int*` (4 bytes) where the callee reads a
		// pointer (8 bytes) -> garbage high bits -> runtime corruption. The
		// same materialization is required for an addressable lvalue that binds
		// to a const reference through a derived-pointer-to-base-pointer
		// conversion: `&derived_ptr` is `Derived **`, not the `_Base_ptr *` the
		// callee reads. Use the referent when given and it differs; otherwise the
		// arg's own type.
		DataDef *tmp_type = (expected_referent && expected_referent != vt)
				  ? expected_referent : vt;
		char name[32];
		snprintf(name, sizeof(name), "__madc_objtmp_%d", m_strtmp_counter++);
		Variable *tmp = new Variable(name, *tmp_type, 1, NULL, false);
		tmp->flags |= vfLOCAL;
		m_pending_stmts.push_back(var_decl(tmp, arg));
		node_t rhs = translate_expr(arg);
		// When the temp is a base-class pointer and the value is a derived-class
		// pointer, make the derived->base conversion explicit (c2mir warns on an
		// implicit pointer-type change in the assignment). This is unconditional:
		// a prvalue spilled via expr_is_nonaddressable_rvalue (e.g.
		// `push_back(new B())` into vector<A*>, selecting the `A*&&` move overload
		// — a NON-const ref, so needs_converted_temp is false) reaches here with
		// tmp_type = base-pointer referent and the value a derived pointer, and
		// still needs the cast. upcast_class_ptr reads the derived class from the
		// expression itself (TokenNEW::alloc_class — NOT arg->datadef(), which is
		// a generic char* for `new`) and is a no-op when no upcast applies.
		rhs = upcast_class_ptr(rhs, tmp_type, arg, arg);
		node_t assign = node2(N_ASSIGN, id(name, arg), rhs, arg);
		m_pending_stmts.push_back(node2(N_EXPR, list(), assign, arg));
		return node1(N_ADDR, id(name, arg), arg);
	}
	return node1(N_ADDR, translate_expr(arg), arg);
}

// The referent type a reference parameter binds to (for ref_param_arg_addr temp
// typing): a reference DataDef (DataDefREF IS-A DataDefPTR) carries its referent
// as base_type. NULL for a non-reference / unknown param.
static DataDef *ref_param_referent(DataDef *pt)
{
	if (!pt || !pt->is_reference())
		return NULL;
	DataDefPTR *rp = dynamic_cast<DataDefPTR *>(pt);
	return rp ? rp->base_type : NULL;
}

// Translate a call's explicit arguments into `args`, coercing object
// parameters and numeric reference parameters. Does NOT inject hidden params
// (__this / __retbuf).
void CirBuilder::build_call_args(TokenCallFunc *tcf, node_t args,
				 size_t param_base)
{
	// Resolve the callee signature for both direct calls (FuncDef) AND indirect
	// calls through a function pointer / lambda variable (DataDefFPTR -> target).
		// Without the fptr fallback, a by-value object parameter of a lambda is
		// invisible here, so a convertible literal/scalar argument is passed raw
		// instead of being materialized into a class temporary.
		FuncDef *callee = call_target_funcdef(tcf);
	// A madc-instantiated member function template: the call token is bound to
	// the declaration-only placeholder (its var is a non-rebindable reference);
	// instantiate_member_fn_template_for_call recorded the instantiated
	// definition's symbol on the placeholder's local_emit_name but left the
	// placeholder's varargs / no-ref_params shape (deliberately — mutating it
	// would corrupt the parser's findMethodOverload arity gate). For ARGUMENT
	// COERCION we need the real parameters + ref_params, so resolve to the
	// instantiated definition here (metadata only — the emit symbol still comes
	// from the placeholder + local_emit_name). Without this a sibling
	// member-template call in an instantiated body (`_S_destroy(__a, __p, 0)`
	// forwarding `_Alloc2& __a`) dereferenced the reference arg (`(*__a)`) into a
	// pointer parameter.
	if (callee && callee->is_member_template
	    && !callee->local_emit_name.empty() && m_prog) {
		if (Variable *iv = m_prog->findVariable(callee->local_emit_name))  // allowed-exception: lookup key, not symbol build
			if (FuncDef *ifd = dynamic_cast<FuncDef *>(iv->type))
				if (ifd != callee)
					callee = ifd;
	}
	size_t nargs = tcf->parameters.size();
	if (tcf->var.name == "__builtin_va_start" && nargs > 1)
		nargs = 1;
	for (size_t i = 0; i < nargs; i++) {
		TokenBase *arg = tcf->parameters[i];
		// Explicit user arg i maps to the callee's parameter (i + param_base):
		// param_base skips leading hidden params the caller injected separately
		// (the method __this), so reference / object coercion reads the RIGHT
		// formal. Without it a method's __this slot (param 0) is read for arg 0
		// and a reference parameter is mis-lowered to a by-value argument.
		size_t pi = i + param_base;
		DataDef *pt = (callee && pi < callee->parameters.size())
				? callee->parameters[pi] : NULL;
		bool is_ref_param = callee && callee->is_ref_param(pi);
		if (DataDefCLASS *pc = param_object_class(pt, is_ref_param))
			append(args, object_arg_addr(arg, pc));
		else if (DataDefCLASS *vc = as_class_instance(pt))
			append(args, object_arg_value(arg, vc));
		else if (is_ref_param)
			// Numeric reference parameter (`int &x`): the callee takes a
			// pointer, so pass the argument's address. An lvalue translates
			// normally (a vfREFERENCE arg re-derefs to its own pointer, and
			// &(*p) folds to p); a prvalue arg is materialized into a temp.
			append(args, ref_param_arg_addr(arg, ref_param_referent(pt),
							const_ref_param(callee, pi)));
		else if (is_char_pointer(pt) && is_class_object_value(arg))
			append(args, object_cstr_arg(arg));
		else if (is_size1_pointer(pt) && is_class_object_value(arg))
			append(args, object_arg_addr(arg, NULL));
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
	// (it already holds &var); otherwise take its address here. A captured
	// REFERENCE variable is itself stored as a pointer to its referent, so we
	// forward that stored pointer VALUE (the referent's address) — matching the
	// `referent *` capture parameter from capture_param_type — rather than &var,
	// which would be a pointer-to-the-reference-slot of the wrong type.
	if (callee && !callee->captured_vars.empty()) {
		for (Variable *cv : callee->captured_vars) {
			if (!cv) continue;
			if (m_cur_captured_fd && m_cur_capture_set.count(cv)) {
				note_capture(cv);
				append(args, id(cv->name.c_str(), tcf));
			} else if (cv->type && cv->type->is_reference()) {
				append(args, id(cv->name.c_str(), tcf));
			} else {
				append(args, node1(N_ADDR, id(cv->name.c_str(), tcf), tcf));
			}
		}
	}
}

void CirBuilder::object_temp_decl(DataDefCLASS *cdd, char *name_buf,
				  size_t buf_sz, TokenBase *origin)
{
	if (!cdd) return;
	snprintf(name_buf, buf_sz, "__madc_objtmp_%d", m_strtmp_counter++);
	Variable *tmp = new Variable(name_buf, *cdd, 1, NULL, false);
	tmp->flags |= vfLOCAL;
	m_pending_stmts.push_back(var_decl(tmp, origin));
}

// Materialize a NON-TRIVIAL class-returning CALL into a cleanup-tagged temp of
// that class, via the __retbuf ABI. Declares
// `struct Cls __t __attribute__((cleanup(Cls___dtor)));` (var_decl attaches the
// cleanup), emits the void call `f(&__t, <args>)` whose callee copy-constructs
// the result into *__retbuf, and returns the temp's (void*) address. Pushes the
// decl and the call to m_pending_stmts.
node_t CirBuilder::object_call_temp_addr(TokenBase *call_tok, DataDefCLASS *cdd,
					 TokenBase *origin)
{
	// A by-value-returning METHOD call: class_method_call already materializes
	// its own sret temp and passes the Itanium (sret, this, args) shape with the
	// base-subobject adjustment. Delegate to it and take the address of the
	// resulting object lvalue — the free-function arg build below injects no
	// receiver, so a method (get_allocator()) was emitted one arg short
	// ("too few arguments": get_allocator(&sret) vs the (__retbuf, __this) body).
	// object_call_temp_addr is only reached for retbuf returns
	// (object_returning_call_class gates on class_return_via_retbuf), so
	// class_method_call takes its materializing sret branch and returns a temp
	// lvalue.
	if (call_tok && call_tok->type() == TokenType::ttCallMethod) {
		if (TokenMember *tcm = dynamic_cast<TokenMember *>(call_tok)) {
			node_t lv = class_method_call(tcm, origin);
			if (lv) {
				node_t addr = node2(N_CAST, void_ptr_type(),
						    node1(N_ADDR, lv, origin), origin);
				CIR_NODE(addr)->synth_from_origin = true;
				return addr;
			}
		}
	}
	TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(call_tok);
	char name[40];
	object_temp_decl(cdd, name, sizeof(name), origin);

	if (tcf) {
		FuncDef *cdf = NULL;
		std::string sym = call_target_emit_name(tcf, &cdf);
		referenced_funcs.insert(sym);
		node_t cargs = list();
		append(cargs, node1(N_ADDR, id(name, origin), origin));   // __retbuf = &__t
		build_call_args(tcf, cargs);
		node_t call = node2(N_CALL, id(sym.c_str(), origin), cargs, origin);
		CIR_NODE(call)->synth_from_origin = true;
		m_pending_stmts.push_back(node2(N_EXPR, list(), call, origin));
	} else if (TokenObjTemp *ot = dynamic_cast<TokenObjTemp *>(call_tok)) {
		// Functional construction T(args): construct directly into the temp
		// (class_ctor_call emits the ctor on &__t with the parsed args).
		Variable tmpv(name, *cdd, 1, NULL, false);
		tmpv.flags |= vfLOCAL;
		node_t cc = class_ctor_call(&tmpv, cdd, ot->ctor_args, origin);
		if (cc) m_pending_stmts.push_back(cc);
	}

	// The temp's (void*) address — the call's object-lvalue result.
	node_t addr = node2(N_CAST, void_ptr_type(),
			    node1(N_ADDR, id(name, origin), origin), origin);
	CIR_NODE(addr)->synth_from_origin = true;
	return addr;
}

// Peel array/pointer layers to the DataDefSTRUCT a typedef ultimately names.
DataDefSTRUCT *CirBuilder::struct_behind(DataDef *dd)
{
	dd = unqualified_type(dd);
	// Peel fixed-array layers first: `typedef struct Tag {..} NAME[N];`
	// carries the struct in DataDefCArray::element_type. Without this, an
	// array typedef of a tagged struct is not recognized as the tag's
	// body-definition point, so it never lands in emitted_structs and a
	// SECOND array typedef of the same tag re-emits the full body —
	// c2mir then rejects the duplicate ("tag X redeclaration"). This is
	// what blocked `typedef __gnuc_va_list va_list;` (both va_list and
	// __gnuc_va_list are `struct __madc_va_list_tag[1]`).
	while (DataDefCArray *ca = dynamic_cast<DataDefCArray *>(dd)) {
		if (!ca->element_type) break;
		dd = unqualified_type(ca->element_type);
	}
	DataDefSTRUCT *s = dynamic_cast<DataDefSTRUCT *>(dd);
	if (!s) {
		DataDefPTR *p = dynamic_cast<DataDefPTR *>(unqualified_type(dd));
		if (p) s = dynamic_cast<DataDefSTRUCT *>(unqualified_type(p->base_type));
	}
	return s;
}

// The C identifier to emit for typedef alias `alias` of type `dd`. For an
// alias that collides across namespaces (m_ambiguous_typedef_aliases), emit the
// underlying struct's already-unique tag so the two namespaces' types stay
// distinct C identifiers; otherwise the bare alias verbatim (the common case).
std::string CirBuilder::typedef_emit_name(const std::string &alias,
					  DataDef *dd) const
{
	if (alias.empty() || !m_ambiguous_typedef_aliases.count(alias))
		return alias;
	DataDefSTRUCT *sdd = struct_behind(dd);
#ifdef MADC_DEBUG_TYPEDEF_EMIT
	fprintf(stderr, "[typedef_emit_name] alias=%s dd=%p type=%d peeled=%s\n",
		alias.c_str(), (void *)dd, dd ? (int)dd->type() : -1,
		sdd ? sdd->name.c_str() : "(null)");
#endif
	return sdd ? sdd->name : alias;
}

// Build a type specifier LIST. If typedef_alias is set, emit ID("alias")
// instead of raw type nodes — c2mir's checker resolves the typedef.
node_t CirBuilder::type_list(DataDef *dd, const std::string &typedef_alias)
{
	node_t lst = list();

	// If a typedef name is available, emit ID("alias") — matches c2m's behavior.
	// An alias that collides across namespaces emits its unique struct tag.
	if (!typedef_alias.empty()) {
		append(lst, id(typedef_emit_name(typedef_alias, dd).c_str()));
		return lst;
	}

	// Struct types: LIST(STRUCT(ID("name"), IGNORE))
	// A user-defined class lowers to `struct ClassName` too (same shape), as does
	// any runtime-object class with opaque storage.
	// _Complex is a DataDefSTRUCT subclass but must use the native spec path.
	if (dd && (dd->is_struct() || as_class_instance(dd)) && !dd->is_complex()) {
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(unqualified_type(dd));
		if (sdd) {
			if (m_tsubst_pattern_mode && struct_has_dependent_member(sdd)) {
				node_t marker = ignore();
				CIR_NODE(marker)->datadef = sdd;
				append(lst, marker);
				return lst;
			}
			// Tag kind MUST match the definition (struct vs union), or
			// c2mir rejects it ("kind of tag X unmatched"). A union-typed
			// reference emits N_UNION.
			node_t sref = node2(sdd->union_layout ? N_UNION : N_STRUCT,
					    id(sdd->name.c_str()), ignore());
			// Tree-1 recipe: carry the tag's class identity so tsubst can
			// rebind a pattern-LOCAL class tag (concrete members, so no
			// dependent-member marker above) to the instantiation's concrete
			// <owner>__<local> class. Pattern mode only — production trees
			// are byte-identical.
			if (m_tsubst_pattern_mode)
				CIR_NODE(sref)->datadef = sdd;
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
// by-value non-trivial class return (not pointer/ref/multi) uses the __retbuf
// ABI. Returns the returned object type so the fn-ptr renders
// `void (*)(T*, params)`; NULL otherwise.
DataDef *CirBuilder::fnptr_retbuf_type(FuncDef *fd)
{
	if (!fd) return NULL;
	DataDef *ret_dd = &fd->return_value_type();
	if (ret_dd->is_pointer() || fd->returns_reference() || fd->is_multi_return())
		return NULL;
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

	// A capturing nested fn / [&] lambda gains hidden `T *name` capture params
	// (FuncDef::captured_vars) that func_def/func_proto append to the emitted
	// signature. The fn-ptr TYPE must include them too, or a capturing lambda
	// assigned to a fn-ptr variable mismatches: `void (*add)(int) = __lambda`
	// where the lambda is `void(int, int *total)` warned ("incompatible types in
	// assignment to a pointer"). Kept in lock-step with func_proto.
	bool has_capture_params = fd->has_captures && !fd->captured_vars.empty();

	if (nparam == 0 && !has_capture_params) {
		if (fd->is_void_params && !retbuf_dd) {
			node_t void_spec = node1(N_LIST, simple(N_VOID));
			node_t void_decl = node2(N_DECL, ignore(), list());
			append(param_list, node2(N_TYPE, void_spec, void_decl));
		}
		// else: bare () — unspecified parameter list (K&R), or retbuf-only
	} else {
		for (size_t i = 0; i < nparam; i++) {
			std::string ptypedef;
			if (i < fd->param_typedef_names.size())
				ptypedef = fd->param_typedef_names[i];
			append(param_list, param_decl(fd->parameters[i], "",
						      ptypedef));
		}
	}
	if (has_capture_params)
		for (Variable *cv : fd->captured_vars) {
			if (!cv) continue;
			DataDef *capt_ptr = capture_param_type(cv);
			append(param_list, param_decl(capt_ptr, "", std::string()));
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
				   const std::vector<carray_dim_t> &lead_dims)
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
	DataDef *ret_dd = fd ? &fd->return_value_type() : NULL;
	int ret_stars = 0;
	while (ret_dd && ret_dd->is_pointer()) {
		DataDefPTR *p = dynamic_cast<DataDefPTR *>(unqualified_type(ret_dd));
		if (!p || !p->base_type) break;
		ret_dd = p->base_type;
		ret_stars++;
	}
	if (ret_dd && ret_dd->is_struct() && !ret_dd->is_complex()) {
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(unqualified_type(ret_dd));
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
		append(spec_list, class_tag_ref(ret_dd));
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
	flat_datatype_map_iter it = m_prog->datatype_map.find(alias);
	if (it == m_prog->datatype_map.end() || !*it) return 1;
	// A Form-2 pointer-to-function typedef already carries the pointer (0
	// extra stars); a Form-1 function typedef does not, so a fn-ptr use of
	// the alias needs one explicit `*`.
	if (DataDefFPTR *afp = dynamic_cast<DataDefFPTR *>(&(*it)->definition))
		return afp->ptr_syntax ? 0 : 1;
	return 1;
}

// -----------------------------------------------------------------------
// Declaration builders
// -----------------------------------------------------------------------

static int dd_ptr_depth(DataDef *dd);      // defined below; counts int** -> 2
static int dd_peel_pointers(DataDef *&dd); // defined below; peels to pointee

// Peel DataDefCArray layers off a type, collecting fixed-array dimensions
// outermost-first. `typedef unsigned long T[2]` -> elem=unsigned long,
// dims=[2]. A runtime-sized CArray (count_expr != NULL) stops the peel and
// contributes no dimension (it decays/uses a VLA path elsewhere).
// Returns the innermost element type; appends each fixed dim to `dims`.
static DataDef *peel_carray_dims(DataDef *dd, std::vector<carray_dim_t> &dims)
{
	while (DataDefCArray *ca = dynamic_cast<DataDefCArray *>(dd)) {
		if (ca->has_runtime_size() || !ca->element_type)
			break;
		dims.push_back((carray_dim_t)ca->count);
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
// The retbuf type is the returned class/struct type.
node_t CirBuilder::retbuf_param(DataDef *retdd, TokenBase *origin)
{
	node_t pspec = type_list(retdd ? retdd : &ddVOID);
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
	FuncDef *cdf = NULL;
	std::string sym = call_target_emit_name(tcf, &cdf);
	referenced_funcs.insert(sym);
	node_t cargs = list();
	append(cargs, id(nm, origin));
	build_call_args(tcf, cargs);
	node_t call = node2(N_CALL, id(sym.c_str(), origin), cargs, origin);
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

	// Array parameters are lowered to `void *`: a pointer to the first element.
	// Class value parameters keep their concrete `struct T` type so member
	// access inside the callee remains typed; references are already pointer
	// DataDefs by the time they reach this emitter.
	if (ptype) {
		// rawtype() always strips the pointer/reference band, so it never
		// returns a dtARRAYref (20000+) value — the old `|| dtARRAYref` arm
		// was dead. An array reference reduces to dtARRAY here (tag-arith
		// retirement: stop naming the ref-band tag).
		DataType pdt = ptype->rawtype();
		if (pdt == DataType::dtARRAY) {
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
				  std::vector<carray_dim_t>());
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
		std::vector<carray_dim_t> adims;
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
				    const std::vector<c2mir_node_code_t> &ret_specs,
				    DataDefCLASS *ret_cls, DataDef *ret_dd)
{
	if (m_output_externs.count(symbol)) return;

	node_t ext_list = list();
	append(ext_list, simple(N_EXTERN));
	// Return base type: a by-value class return is the class's struct/union tag
	// (a trivially-copyable register return — _M_erase/_M_insert_rval yield
	// __normal_iterator by value); otherwise N_VOID by default, or the
	// caller-supplied specs (e.g. {N_LONG} for a long-returning runtime fn — a
	// void base would silently truncate/misread a value used in arithmetic).
	int ret_decl_stars = ret_ptr ? 1 : 0;
	if (ret_cls) {
		append(ext_list, class_tag_ref(ret_cls));
		ret_decl_stars = 0;   // a by-value struct return is not a pointer
	} else if (ret_dd) {
		DataDef *ret_base = ret_dd;
		ret_decl_stars = dd_peel_pointers(ret_base);
		if (!ret_base)
			ret_base = &ddVOID;
		if ((ret_base->is_struct() || as_class_instance(ret_base))
		    && !ret_base->is_complex())
			append(ext_list, class_tag_ref(ret_base));
		else
			append_type_specs(ext_list, ret_base);
	} else if (ret_specs.empty()) {
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
			// A by-value struct/union param: one tag-ref spec (struct X
			// / union X per union_layout), no pointer. Otherwise the
			// scalar spec list, optionally one pointer level.
			if (params[i].cls) {
				append(specs, class_tag_ref(params[i].cls));
			} else {
				for (size_t j = 0; j < params[i].specs.size(); j++)
					append(specs, simple(params[i].specs[j]));
			}
			node_t pdecl_list = list();
			// A pointer level applies to scalar params AND to a class/struct
			// param when ptr is set (`struct X *` — e.g. a typed forward proto
			// for an in-module member/base dtor `void d(struct X *)`). cls with
			// ptr=false stays by-value (the default for native_param_shape).
			if (params[i].ptr)
				append(pdecl_list, pointer());
			node_t pdecl = node2(N_DECL, ignore(), pdecl_list);
			append(param_list, node2(N_TYPE, specs, pdecl));
		}
	}

	node_t func_inner = node1(N_FUNC, param_list);
	node_t decl_list = list();
	append(decl_list, func_inner);
	for (int rs = 0; rs < ret_decl_stars; rs++)
		append(decl_list, pointer());
	node_t decl = node2(N_DECL, id(symbol), decl_list);

	node_t proto = simple(N_SPEC_DECL);
	append(proto, share);
	append(proto, decl);
	append(proto, ignore());
	append(proto, ignore());
	append(proto, ignore());
	m_output_externs[symbol] = proto;
}

void CirBuilder::need_output_extern_unprototyped(
	const char *symbol, bool ret_ptr,
	const std::vector<c2mir_node_code_t> &ret_specs)
{
	if (m_output_externs.count(symbol)) return;

	node_t ext_list = list();
	append(ext_list, simple(N_EXTERN));
	if (ret_specs.empty()) {
		append(ext_list, simple(N_VOID));
	} else {
		for (size_t i = 0; i < ret_specs.size(); i++)
			append(ext_list, simple(ret_specs[i]));
	}
	node_t share = node1(N_SHARE, ext_list);
	node_t func_inner = node1(N_FUNC, list());
	node_t decl_list = list();
	append(decl_list, func_inner);
	if (ret_ptr) append(decl_list, pointer());
	node_t decl = node2(N_DECL, id(symbol), decl_list);
	node_t proto = simple(N_SPEC_DECL);
	append(proto, share);
	append(proto, decl);
	append(proto, ignore());
	append(proto, ignore());
	append(proto, ignore());
	m_output_externs[symbol] = proto;
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
		// (possibly anonymous struct/union) layout from the typedef. A
		// cross-namespace-colliding alias emits its unique struct tag.
		append(spec, id(typedef_emit_name(typedef_name, dd).c_str()));
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
	// Runtime-object classes flow through the general class-instance path below:
	// opaque storage via class_struct_def plus cleanup naming the parsed external
	// destructor when one exists. Construction is injected as a separate
	// class_ctor_call by translate_block (the 1->N C++ lowering).

	// madc array object (`array a;`, a madc::value): same opaque-storage
	// model as runtime-object classes.
	// madarray_construct is emitted as a separate statement by translate_block;
	// the cleanup attribute on the storage handles scope-exit destruction.
	if (is_array_object(v->type))
		return array_storage_decl(v->name.c_str(), origin);

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
	std::vector<carray_dim_t> ptr_array_dims;
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
		std::vector<carray_dim_t> fnptr_dims;
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
			append(new_list, id(typedef_emit_name(v->typedef_name, v->type).c_str()));
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
			append(new_list, id(typedef_emit_name(v->typedef_name, v->type).c_str()));
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
	std::string emitted_var_name = var_emit_name(*v);
	node_t var_id = id(emitted_var_name.c_str(), origin);
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
			flat_datatype_map_iter it = m_prog->datatype_map.find(v->typedef_name);
			if (it != m_prog->datatype_map.end() && *it) {
				std::vector<carray_dim_t> tdims;
				peel_carray_dims(&(*it)->definition, tdims);
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

	// For a function-type-typedef variable the parser recorded the EXACT
	// explicit '*' count (the type stays a bare DataDefFPTR), so emit exactly
	// that — `DO_FUN do_look;` (0) renders as a C function declaration,
	// `DO_FUN *fp;` (1) as a function-pointer variable. Other variables keep
	// the legacy depth-derived count.
	int decl_stars;
	if (fnptr)
		decl_stars = -1;
	else if (v->fnptr_explicit_stars >= 0)
		decl_stars = v->fnptr_explicit_stars;
	else
		decl_stars = explicit_star_count(v->type, v->typedef_name);
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
		// A class instance is initialized by a constructor call emitted separately
		// (translate_block), never by a C initializer on the storage declaration.
		// Leave init_node empty for such instances.
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
	CIR_NODE(spec_decl)->datadef = base_dd;
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

// Peel ALL pointer levels off `dd` to its base type, returning the star
// count. The function-return emitters use this so a multi-star return
// (`const unsigned short **__ctype_b_loc(void)`) keeps every level — the
// old peel-one-level pattern emitted `unsigned short *`, and the ctype
// macros' `(*__ctype_b_loc())[i]` then subscripted a scalar.
static int dd_peel_pointers(DataDef *&dd)
{
	int depth = 0;
	while (dd && dd->is_pointer()) {
		DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd);
		if (!p || !p->base_type) break;
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
		flat_datatype_map_iter it = m_prog->datatype_map.find(alias);
		if (it != m_prog->datatype_map.end() && *it) {
			base_depth = dd_ptr_depth(&(*it)->definition);
			fnptr_alias = (dynamic_cast<DataDefFPTR *>(
					&(*it)->definition) != NULL);
		}
	}
	int stars = full_depth - base_depth;
	if (stars < 0) stars = 0;
	// A function-typedef alias (`typedef void DO_FUN(args)`) names a bare
	// FUNCTION type, so a use of it is a pointer-to-function and carries one
	// implicit '*'. The variable/member/return paths keep the bare DataDefFPTR
	// (no DataDefPTR wrapper) so the expression parser's fn-ptr-call detection
	// — which keys on `var.type` being DataDefFPTR — keeps working; there the
	// implicit star is the ONLY pointer (full_depth == base_depth, stars == 0).
	// But a PARAMETER declared `DO_FUN *p` is wrapped by the param parser in a
	// real DataDefPTR, so stars already counts that explicit '*'. The implicit
	// decay star and an explicit '*' are the SAME pointer level — they must NOT
	// stack (else `SPEC_FUN *special` renders as `SPEC_FUN **special`, a
	// spurious pointer-to-function-pointer that c2mir flags "incompatible
	// pointer types in comparison" against a plain function pointer). Take the
	// MAX, not the sum: 0 explicit stars -> the implicit 1; N>=1 explicit stars
	// -> N (the first explicit '*' IS the function-pointer level).
	// `DO_FUN fn` (a bare-function declaration) never reaches here — it is
	// represented as a FuncDef and emitted on the function path.
	if (fnptr_alias) {
		int implicit = fnptr_alias_stars(alias);
		if (implicit > stars)
			stars = implicit;
	}
	return stars;
}

node_t CirBuilder::anon_members_list(DataDefSTRUCT *anon)
{
	node_t ml = list();
	std::map<size_t, const DataDefSTRUCT *> anon_group_starts;
	std::map<size_t, size_t> anon_group_counts;
	std::set<size_t> anon_grouped_members;
	for (const DataDefSTRUCT::AnonymousAggregateInfo &ag
	     : anon->anonymous_aggregates) {
		if (!ag.aggregate || ag.aggregate == anon || ag.member_count == 0)
			continue;
		anon_group_starts[ag.first_member] = ag.aggregate;
		anon_group_counts[ag.first_member] = ag.member_count;
		for (size_t j = 0; j < ag.member_count; j++)
			anon_grouped_members.insert(ag.first_member + j);
	}
	for (size_t i = 0; i < anon->members.size(); i++) {
		auto gi = anon_group_starts.find(i);
		if (gi != anon_group_starts.end()) {
			append(ml, anonymous_aggregate_member_node(
				const_cast<DataDefSTRUCT *>(gi->second)));
			size_t cnt = anon_group_counts[i];
			if (cnt > 0) i += cnt - 1;
			continue;
		}
		if (anon_grouped_members.count(i))
			continue;
		append(ml, member_node(anon->members[i], anon));
	}
	return ml;
}

node_t CirBuilder::anon_inline_spec(DataDefSTRUCT *anon)
{
	return node1(N_LIST, node2(anon->union_layout ? N_UNION : N_STRUCT,
				   ignore(), anon_members_list(anon)));
}

node_t CirBuilder::anonymous_aggregate_member_node(DataDefSTRUCT *anon)
{
	node_t member = simple(N_MEMBER);
	append(member, node1(N_SHARE, anon_inline_spec(anon)));
	append(member, ignore());
	append(member, ignore());
	append(member, ignore());
	return member;
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
	std::vector<carray_dim_t> mdims;
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
			const std::vector<carray_dim_t> *dv = owner->m_dims(mname);
			if (dv && !dv->empty()) {
				mdims = *dv;
			} else {
				size_t c = owner->m_count(mname);
				if (c >= 1) mdims.push_back((carray_dim_t)c);
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

	// A class object member embeds the lowered class struct directly. Its
	// construction and destruction are driven by the enclosing class ctor/dtor;
	// members have no independent cleanup attribute. An ARRAY of class objects
	// (`_Words _M_local_word[8]` in libstdc++'s ios_base) still needs its N_ARR
	// declarator dims — without them the member collapses to a single element,
	// undersizing the struct (ios_base lost 7*sizeof(_Words)=112B, so a
	// libstdc++-constructed ofstream overflowed madc's stack slot and crashed in
	// the dtor). Build the declarator with the same dims the scalar path uses.
	if (is_class_object(mtype)) {
		node_t mspec = type_list(mtype);   // -> struct string
		node_t mdecl_list = list();
		if (m_flexible)
			append(mdecl_list, node3(N_ARR, ignore(), list(), ignore()));
		for (size_t d = 0; d < mdims.size(); d++)
			append(mdecl_list, node3(N_ARR, ignore(), list(), integer(mdims[d])));
		node_t mmember = simple(N_MEMBER, m.origin);
		append(mmember, node1(N_SHARE, mspec));
		append(mmember, node2(N_DECL, id(m.first.c_str(), m.origin), mdecl_list));
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
	node_t member_list = anon_members_list(sdd);

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
// Externally-owned classes are NOT emitted here (their ABI owns the _ZTI*).
// Returns an N_LIST of file-scope definitions (extern decls + _ZTS + _ZTI), or
// NULL for a non-polymorphic class.
// The vtable / typeinfo SYMBOL to reference for cdd. madc-emitted for a class
// madc defines; the REAL libstdc++ _ZTVSt.../_ZTISt... for an externally-defined
// class (whose machinery madc does not synthesize). For an un-namespaced user
// class the encoded form equals source_name, so these reduce to the prior names.
std::string CirBuilder::class_vtable_symbol(DataDefCLASS *cdd)
{
	if (cdd && cdd->is_externally_defined())
		return itanium_vtable_sym_cpp(cdd->canonical_cpp_spelling);
	return (cdd ? cdd->name : std::string()) + "__vtable";
}

std::string CirBuilder::class_typeinfo_symbol(DataDefCLASS *cdd)
{
	if (cdd && cdd->is_externally_defined())
		return itanium_typeinfo_sym_cpp(cdd->canonical_cpp_spelling);
	return itanium_typeinfo_sym(cdd ? cdd->name : std::string());
}

// `extern void *SYM[];` (deduped). NULL if already emitted this module.
node_t CirBuilder::data_extern_decl(const std::string &sym)
{
	if (m_rtti_data_externs.count(sym)) return NULL;
	m_rtti_data_externs.insert(sym);
	node_t spec = list();
	append(spec, simple(N_EXTERN));
	append(spec, simple(N_VOID));
	node_t dl = list();
	append(dl, node3(N_ARR, ignore(), list(), ignore())); // []
	append(dl, pointer());                                 // void* element
	node_t decl = node2(N_DECL, id(sym.c_str()), dl);
	node_t sd = simple(N_SPEC_DECL);
	append(sd, node1(N_SHARE, spec));
	append(sd, decl);
	append(sd, ignore());
	append(sd, ignore());
	append(sd, ignore());
	return sd;
}

node_t CirBuilder::class_typeinfo_def(DataDefCLASS *cdd)
{
	if (!cdd || !cdd->has_vtable)
		return NULL;
	// libstdc++ owns an externally-defined class's typeinfo (see Pass 1.5).
	if (cdd->is_externally_defined())
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
	// libstdc++ owns an externally-defined class's vtable (see Pass 1.5).
	if (cdd->is_externally_defined())
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
		fnptr_decl_pieces(ov, true, ret_spec, throwaway, std::vector<carray_dim_t>());
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
			node2(N_TYPE, node1(N_LIST, class_tag_ref(mowner)),
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
			node1(N_LIST, class_tag_ref(cdd)),
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

node_t CirBuilder::class_member_list(DataDefCLASS *cdd)
{
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
		// Opaque runtime-object class: `long _w[words];`
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
		struct Field {
			size_t off;
			size_t sz;
			int kind; // 0=vptr, 1=member, 2=anonymous aggregate
			size_t midx;
			const DataDefSTRUCT *anon;
		};
		std::vector<Field> fields;
		if (cdd->has_vptr_slot)
			fields.push_back(Field{0, 8, 0, 0, NULL}); // primary vptr @0
		for (DataDefCLASS *o : cdd->secondary_vptr_owners)
			for (size_t b = 0; b < cdd->bases.size(); b++)
				if (cdd->bases[b].base == o) {
					fields.push_back(Field{cdd->bases[b].offset, 8, 0, 0, NULL});
					break;
				}
		std::set<size_t> anon_grouped_members;
		for (const DataDefSTRUCT::AnonymousAggregateInfo &ag
		     : cdd->anonymous_aggregates) {
			if (!ag.aggregate || ag.member_count == 0)
				continue;
			size_t off = ag.offset;
			if (ag.first_member < cdd->member_offsets.size()) {
				size_t child_off = 0;
				if (!ag.aggregate->member_offsets.empty())
					child_off = ag.aggregate->member_offsets[0];
				size_t member_off = cdd->member_offsets[ag.first_member];
				off = member_off >= child_off
				    ? member_off - child_off : member_off;
			}
			fields.push_back(Field{off, ag.aggregate->size, 2, 0,
					      ag.aggregate});
			for (size_t j = 0; j < ag.member_count; j++)
				anon_grouped_members.insert(ag.first_member + j);
		}
		for (size_t i = 0; i < cdd->members.size(); i++) {
			if (anon_grouped_members.count(i))
				continue;
			size_t msz = cdd->members[i].second->size
				   * (i < cdd->member_counts.size() ? cdd->member_counts[i] : 1);
			fields.push_back(Field{cdd->member_offsets[i], msz, 1, i, NULL});
		}
		std::map<std::string, size_t> member_name_counts;
		std::set<std::string> own_member_names;
		for (size_t i = 0; i < cdd->members.size(); i++) {
			const std::string &mn = cdd->members[i].first;
			member_name_counts[mn]++;
			int origin = (i < cdd->member_origin.size()) ? cdd->member_origin[i] : -1;
			if (origin < 0 && !cdd->member_vbase.count(i))
				own_member_names.insert(mn);
		}
		std::sort(fields.begin(), fields.end(),
			  [](const Field &a, const Field &b){
				  if (a.off != b.off) return a.off < b.off;
				  return a.kind < b.kind;
			  });
		size_t cursor = 0; int synth = 0;
		std::set<std::string> emitted_member_names;
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
				if (f.kind == 2) {
					append(member_list,
					       anonymous_aggregate_member_node(
						   const_cast<DataDefSTRUCT *>(f.anon)));
					cursor += f.sz;
					continue;
				}
				memberpair_t out = cdd->members[f.midx];
				int origin = (f.midx < cdd->member_origin.size())
					? cdd->member_origin[f.midx] : -1;
				bool inherited = origin >= 0 || cdd->member_vbase.count(f.midx);
				bool duplicate = member_name_counts[out.first] > 1;
				bool hidden_by_own = own_member_names.count(out.first) && inherited;
				if ((duplicate && hidden_by_own)
				    || emitted_member_names.count(out.first)) {
					std::string base = out.first;
					out.first += "__flat" + std::to_string(f.midx);
					while (emitted_member_names.count(out.first))
						out.first = base + "__flat" + std::to_string(++synth);
				}
				emitted_member_names.insert(out.first);
				append(member_list, member_node(out, cdd));
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

	return member_list;
}

node_t CirBuilder::class_struct_def(DataDefCLASS *cdd)
{
	node_t struct_id = id(cdd->name.c_str());
	node_t member_list = class_member_list(cdd);
	// A class-parsed union ([class.union]: union with ctors/methods
	// delegates to the class parser) must DEFINE as N_UNION — every
	// reference site (class_tag_ref) already follows union_layout, and a
	// struct-kind definition mismatches them all.
	node_t struct_node = node2(cdd->union_layout ? N_UNION : N_STRUCT,
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

void CirBuilder::emit_class_member_deps(
	DataDefSTRUCT *sdd, node_t top_list,
	std::set<std::string> &emitted_structs,
	std::set<DataDefCLASS *> &emitted_classes,
	std::set<DataDefCLASS *> &emitting_classes)
{
	if (!sdd) return;
	DataDefCLASS *owner_class = dynamic_cast<DataDefCLASS *>(sdd);
	for (const memberpair_t &m : sdd->members) {
		DataDef *dd = m.second;
		if (!dd || dd->is_pointer())
			continue;
		std::vector<carray_dim_t> dims;
		DataDef *base = peel_carray_dims(dd, dims);
		DataDefCLASS *dep = as_user_class(base);
		if (dep && dep != owner_class) {
			emit_class_struct_with_deps(dep, top_list, emitted_structs,
						    emitted_classes, emitting_classes);
			continue;
		}
		// A by-value PLAIN struct/union member (e.g. a libstdc++ class that
		// embeds pthread_mutex_t, whose union holds `struct __pthread_mutex_s`):
		// c2mir needs that struct complete before the owner. The class-dep walk
		// above skips it (not a class), so hoist its definition here too.
		DataDefSTRUCT *sbase = dynamic_cast<DataDefSTRUCT *>(base);
		if (sbase && sbase != sdd)
			emit_struct_with_deps(sbase, top_list, emitted_structs,
					      emitted_classes, emitting_classes);
	}
}

void CirBuilder::emit_struct_with_deps(
	DataDefSTRUCT *sdd, node_t top_list,
	std::set<std::string> &emitted_structs,
	std::set<DataDefCLASS *> &emitted_classes,
	std::set<DataDefCLASS *> &emitting_classes)
{
	if (!sdd || !sdd->is_complete)
		return;
	// A generic dependent local struct (placeholder members) is a Tree-1 pattern
	// artifact — never emitted globally; its concrete per-instantiation clone is.
	if (struct_has_dependent_member(sdd)) return;
	// A class member routes through the class path (vtable/ctor machinery).
	if (DataDefCLASS *c = as_user_class(sdd)) {
		emit_class_struct_with_deps(c, top_list, emitted_structs,
					    emitted_classes, emitting_classes);
		return;
	}
	// Already emitted (by name) — its body and deps are out. Anonymous aggregates
	// have a unique synthetic tag never recorded here, so they fall through to the
	// recurse-only path below (their body is inlined at the use site).
	if (!sdd->is_anonymous && emitted_structs.count(sdd->name))
		return;
	// By-value embedding is acyclic in valid C (an incomplete type can't be a
	// by-value member), so recursing members first cannot loop.
	emit_class_member_deps(sdd, top_list, emitted_structs,
			       emitted_classes, emitting_classes);
	// Emit the named struct's own body once; anonymous aggregates are spelled
	// inline at the use site, so only their member deps (hoisted above) matter.
	if (!sdd->is_anonymous && !emitted_structs.count(sdd->name)) {
		auto cti = m_combined_typedef_alias.find(sdd->name);
		if (cti != m_combined_typedef_alias.end()) {
			// The struct's def-point is a combined `typedef struct X {...} Y;`.
			// Emit the WHOLE combined decl (body + alias Y together) so Y is in
			// scope for dependents hoisted after it (a member typed `Y` would
			// otherwise reference an undefined alias). typedef_decl emits the body
			// because emitted_structs doesn't yet contain the tag; record Y so the
			// source-order Pass 0 skips re-emitting this typedef.
			Program::TopDecl *td = cti->second;
			node_t n = typedef_decl(typedef_emit_name(td->name, td->dd),
						td->dd, emitted_structs, false);
			emitted_structs.insert(sdd->name);
			m_hoisted_combined_aliases.insert(td->name);
			if (n) append(top_list, n);
		} else {
			emitted_structs.insert(sdd->name);
			node_t sd = struct_def(sdd);
			if (sd) append(top_list, sd);
		}
	}
}

void CirBuilder::emit_class_struct_with_deps(
	DataDefCLASS *cdd, node_t top_list,
	std::set<std::string> &emitted_structs,
	std::set<DataDefCLASS *> &emitted_classes,
	std::set<DataDefCLASS *> &emitting_classes)
{
	if (!cdd) return;
	// A generic dependent local class (placeholder members) is a Tree-1 pattern
	// artifact — never emitted globally; its concrete per-instantiation clone is.
	if (struct_has_dependent_member(cdd)) return;
	if (emitted_classes.count(cdd) || emitted_structs.count(cdd->name))
		return;
	if (emitting_classes.count(cdd))
		return;
	emitting_classes.insert(cdd);
	emit_class_member_deps(cdd, top_list, emitted_structs,
			       emitted_classes, emitting_classes);
	emitting_classes.erase(cdd);
	if (emitted_classes.count(cdd) || emitted_structs.count(cdd->name))
		return;
	emitted_classes.insert(cdd);
	emitted_structs.insert(cdd->name);
	node_t cd = class_struct_def(cdd);
	if (cd) append(top_list, cd);
}

// Resolve the class behind a (possibly pointer-wrapped) DataDef. Recognizes
// ordinary user classes AND runtime-object classes so method calls route through
// the class path.
static DataDefCLASS *class_behind(DataDef *dd)
{
	if (!dd) return NULL;
	if (DataDefCLASS *c = as_class_instance(dd)) return c;
	if (DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd))
		return as_class_instance(p->base_type);
	return NULL;
}

// A reference operand IS the referenced object in every expression context
// (C++ semantics); madc stores a reference variable as DataDefPTR(T) +
// vfREFERENCE, so resolution must unwrap it or the operand looks like a
// pointer and no class operator/ctor overload ever matches.
DataDefCLASS *CirBuilder::operand_object_class(TokenBase *t)
{
	if (!t) return NULL;
	DataDef *dd = t->datadef();
	if (DataDefCLASS *c = as_class_instance(dd))
		return c;
	// A reference-typed result (a DataDefREF — e.g. the result of a T&-returning
	// operator/method, now that references live in the type) denotes the
	// referenced class. as_class_instance/as_user_class deliberately answer NULL
	// for a reference; class_behind unwraps it. This is the single receiver-class
	// unwrap (first-class refs Phase 2 — replaces the returns_ref flag's role).
	if (dd && dd->is_reference())
		if (DataDefCLASS *c = class_behind(dd))
			return c;
	if (TokenVar *tv = dynamic_cast<TokenVar *>(t))
		if (tv->var.is_reference())
			return class_behind(tv->var.type);
	return NULL;
}

// The scalar twin of operand_object_class: a reference variable (`int &x`,
// vfREFERENCE + DataDefPTR(int)) reads as its referee in every expression
// context (its value use auto-derefs — see the TokenVar translation), so its
// value DOMAIN is the pointee, not the pointer. Everything else answers with
// the expression's own datadef(); plain pointers keep pointer semantics.
DataDef *CirBuilder::operand_value_datadef(TokenBase *t)
{
	if (!t) return NULL;
	if (TokenVar *tv = dynamic_cast<TokenVar *>(t))
		if ((tv->var.is_reference()) && tv->var.type)
			if (DataDefPTR *p = dynamic_cast<DataDefPTR *>(tv->var.type))
				return p->base_type;
	return t->datadef();
}

static DataDef *reference_member_referent(TokenMember *tm)
{
	if (!tm || !tm->var.is_reference())
		return NULL;
	DataDefPTR *rp = dynamic_cast<DataDefPTR *>(tm->var.type);
	return rp ? rp->base_type : NULL;
}

static bool reference_member_value_use_deref(TokenMember *tm)
{
	DataDef *referent = reference_member_referent(tm);
	return referent && (referent->is_numeric() || referent->is_pointer());
}

static bool reference_member_value_is_stored_address(TokenBase *tb)
{
	if (!tb || tb->type() != TokenType::ttMember)
		return false;
	TokenMember *tm = dynamic_cast<TokenMember *>(tb);
	return tm && tm->var.is_reference()
	    && reference_member_referent(tm)
	    && !reference_member_value_use_deref(tm);
}

// True when a NAMED variable holds the object's address rather than the object
// itself. For these the object address is the variable's value (`name`); for a
// value object lvalue it is `&name`. This is the single addressing rule shared
// by every object-class receiver.
static bool var_is_pointer_stored(const Variable &v)
{
	if (v.type && v.type->is_pointer()) return true;
	if (v.is_reference()) return true;
	return false;
}

// The raw object address of a NAMED variable (NOT cast to void*): the pointer
// itself when pointer-stored, else `&name`. The single source of truth for
// object-instance addressing; object_var_void_addr wraps this in a void* cast.
node_t CirBuilder::object_var_addr(const Variable &v, TokenBase *origin)
{
	std::string emitted = var_emit_name(v);
	node_t base = id(emitted.c_str(), origin);
	// GNU nested-function / [&]-lambda capture of a class object: the hidden
	// capture parameter is a `Class *name` that ALREADY holds the enclosing
	// object's address (capture-by-reference). Inside the body the variable is
	// pointer-stored, exactly like the value-read deref path (see the TokenVar
	// chokepoint in translate_expr). Record the capture so func_def synthesizes
	// the parameter, and return the pointer itself — NOT &name (which would be
	// `Class **`). Without this, a captured object used by address (`cout << msg`,
	// `msg.method()`) emitted `&msg` referencing an undeclared outer name.
	if (note_capture(const_cast<Variable *>(&v)))
		return base;
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
		recv_class = class_behind(recv_type);
		if (!recv_class) return NULL;
		// A by-value object-returning CALL receiver (`*begin()` — begin()
		// returns the iterator by value) is a prvalue; `&call` is not an lvalue
		// and c2mir rejects it. object_arg_addr materializes it into an
		// addressable temp ([class.temporary]: a retbuf return via
		// object_call_temp_addr, a trivially-copyable register return via a
		// copy into a temp). Gated to the CALL case so an lvalue receiver keeps
		// its direct &obj — object_arg_addr would otherwise copy a non-matching
		// lvalue into a temp and call the method on the copy. Detected before
		// translate_expr so the call is emitted once (inside object_arg_addr).
		bool recv_is_ptr = recv_type && recv_type->is_pointer();
		if (!recv_is_ptr
		    && (tm->parent_expr->type() == TokenType::ttCallFunc
			|| tm->parent_expr->type() == TokenType::ttCallMethod)
		    && !ref_returning_call_type(tm->parent_expr))
			return object_arg_addr(tm->parent_expr, recv_class);
		recv_node = translate_expr(tm->parent_expr);
		return recv_is_ptr ? recv_node : node1(N_ADDR, recv_node, origin);
	}
	recv_type = tm->object.type;
	recv_node = id(tm->object.name.c_str(), origin);
	from_var = true;
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
// return (c_str()) or reference return (operator=(), _M_append()) is declared
// via ret_ptr; an integer return (size()/length() are unsigned long, the
// comparison family is int) needs a real integer base of the right
// width/signedness so the value reads correctly; everything else defaults to a
// void base. `ret_ptr` is out-param.
static std::vector<c2mir_node_code_t> emit_symbol_ret_specs(FuncDef *fd, bool &ret_ptr)
{
	ret_ptr = fd && (fd->return_value_type().is_pointer() || fd->returns_reference());
	if (ret_ptr) return {};   // void* / char* — base is void, ret_ptr carries the star
	if (fd && fd->return_value_type().is_integer()) {
		// An integer return MUST declare a real integer base — a void base drops
		// the value. length()/size() are `unsigned long` (was wrongly emitted as
		// void because the old check matched only signed dtINT32/dtINT64); the
		// comparison family is `int`. Match width + signedness (mirrors
		// append_type_specs) so the result reads correctly. (embedded-headers:
		// declare real return types — never let the fallback turn it into void.)
		switch (fd->return_value_type().rawtype()) {
		case DataType::dtBOOL:   return { N_BOOL };
		case DataType::dtUINT64: return { N_UNSIGNED, N_LONG };
		case DataType::dtUINT32: return { N_UNSIGNED, N_INT };
		case DataType::dtINT64:  return { N_LONG };
		case DataType::dtINT32:  return { N_INT };
		case DataType::dtINT128: return { N_INT128 };
		case DataType::dtUINT128: return { N_UNSIGNED, N_INT128 };
		default:                 return { N_LONG };
		}
	}
	return {};
}

static bool is_constant_evaluated_call(TokenBase *tb)
{
	TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(tb);
	if (!tcf || !tcf->parameters.empty())
		return false;
	const std::string &name = tcf->var.name;
	return name == "__builtin_is_constant_evaluated"
	    || name == "__ns_std___is_constant_evaluated"
	    || name == "__ns_std_is_constant_evaluated";
}

// Lower a call to a class method bound to an external symbol (emit_symbol).
// Declares the extern from the FuncDef's real signature, passes the receiver
// address as void*, and coerces explicit args (object params -> object_arg_addr).
// `empty()` is lowered to `size()==0`.
node_t CirBuilder::emit_symbol_method_call(TokenMember *tm, FuncDef *callee,
					   const std::string &sym, node_t this_arg,
					   TokenBase *origin)
{
	const std::string &method = tm->var.name;

	// empty() -> (size() == 0). The real size() member returns a clean size_t;
	// the bool-returning empty() returns a 1-byte value (garbage upper bits as
	// a 64-bit int). Same observable result, robust read. (g++ canon.)
	if (method == "empty") {
		DataDefCLASS *owner = (!callee->parameters.empty())
				    ? class_behind(callee->parameters[0]) : NULL;
		std::string size_sym = class_method_symbol(owner, "size");
		if (size_sym.empty() || size_sym == "size")
			return NULL;
		need_output_extern(size_sym.c_str(), false,
				   { { {N_VOID}, true } }, { N_UNSIGNED, N_LONG });
		node_t a = list();
		append(a, node2(N_CAST, void_ptr_type(), this_arg, origin));
		node_t call = node2(N_CALL, id(size_sym.c_str(), origin), a, origin);
		CIR_NODE(call)->synth_from_origin = true;
		node_t eq = node2(N_EQ, call, integer(0, origin), origin);
		CIR_NODE(eq)->synth_from_origin = true;
		return eq;
	}

	// A by-value NON-TRIVIAL class return uses the Itanium sret ABI: hidden
	// result-slot FIRST argument (before __this), void call, and the call
	// expression is the materialized cleanup-tagged temp's lvalue (g++
	// canon: get_allocator() receives the sret slot in %rdi, this in %rsi).
	DataDefCLASS *retc = (!callee->returns_reference() && !callee->return_value_type().is_pointer())
			     ? class_return_via_retbuf(&callee->return_value_type()) : NULL;
	char sret_tmp[40] = { 0 };
	if (retc)
		object_temp_decl(retc, sret_tmp, sizeof(sret_tmp), origin);

	// Build the parameter shapes for the extern declaration and the args.
	// Param 0 is the hidden __this (a void* object pointer). Each subsequent
	// param mirrors the method's declared parameter type.
	std::vector<ExternParam> eparams;
	node_t args = list();
	if (retc) {
		eparams.push_back({ {N_VOID}, true });   // sret slot (void*)
		append(args, node2(N_CAST, void_ptr_type(),
				   node1(N_ADDR, id(sret_tmp, origin), origin),
				   origin));
	}
	eparams.push_back({ {N_VOID}, true });   // this (void*)
	append(args, node2(N_CAST, void_ptr_type(), this_arg, origin));   // this (void*)
	for (size_t i = 0; i < tm->parameters.size(); i++) {
		TokenBase *arg = tm->parameters[i];
		size_t pi = i + 1;   // +1 to skip __this
		DataDef *pt = (pi < callee->parameters.size())
				? callee->parameters[pi] : NULL;
		bool refp = callee->is_ref_param(pi);
		if (DataDefCLASS *pc = param_object_class(pt, refp)) {
			eparams.push_back({ {N_VOID}, true });
			append(args, object_arg_addr(arg, pc));
		} else if (DataDefCLASS *vc = as_class_instance(pt)) {
			eparams.push_back(native_param_shape(pt, false));
			append(args, object_arg_value(arg, vc));
		} else if (refp) {
			eparams.push_back(native_param_shape(pt, true));
			append(args, ref_param_arg_addr(arg, ref_param_referent(pt),
							const_ref_param(callee, pi)));
		} else if (pt && pt->is_pointer()) {
			eparams.push_back(native_param_shape(pt, false));
			append(args, translate_expr(arg));
		} else {
			eparams.push_back({ {N_LONG}, false });
			append(args, translate_expr(arg));
		}
	}

	bool ret_ptr = false;
	std::vector<c2mir_node_code_t> ret_specs = emit_symbol_ret_specs(callee, ret_ptr);
	// A trivially-copyable class returned BY VALUE (register-returned, not via
	// retbuf): declare the extern returning the class's struct tag, else the
	// scalar-spec path falls to a void base and `return <call>(...)` in a
	// struct-returning caller mismatches (_M_erase/_M_insert_rval -> iterator).
	DataDefCLASS *ret_cls = NULL;
	if (retc) {
		ret_ptr = false;
		ret_specs = { N_VOID };
	} else if (!callee->returns_reference() && !callee->return_value_type().is_pointer()) {
		ret_cls = as_class_instance(&callee->return_value_type());
	}
	DataDef *ret_dd = (!retc && callee) ? &callee->returns : NULL;
	need_output_extern(sym.c_str(), ret_ptr, eparams, ret_specs, ret_cls,
			   ret_dd);
	node_t call = node2(N_CALL, id(sym.c_str(), origin), args, origin);
	CIR_NODE(call)->synth_from_origin = true;
	if (retc) {
		// sret: the call runs as a statement; the expression value is the
		// temp's object lvalue.
		m_pending_stmts.push_back(node2(N_EXPR, list(), call, origin));
		return id(sret_tmp, origin);
	}
	// A T&-returning method returns an address; deref so the call expression is
	// the referenced lvalue, matching the non-external method/operator paths.
	if (callee && callee->returns_reference())
		return node1(N_DEREF, call, origin);
	return call;
}

node_t CirBuilder::class_method_call(TokenMember *tm, TokenBase *origin)
{
	DataDefCLASS *recv_class = NULL;
	node_t this_arg = class_this_arg(tm, recv_class, origin);
	// Two-tree: DEPENDENT member/functor call — the receiver's type is still
	// a template-param placeholder (`__node_gen(std::forward<_Arg>(__v))`,
	// the dependent operator() member call the pattern parse builds), so no
	// class or method can resolve yet. Emit the call with a REWRITABLE callee
	// id (the placeholder's own emit name) and the receiver's stored pointer
	// as `this`; copy-time rewrite_copied_dependent_call_id re-resolves it
	// per instantiation (substituted receiver -> findMethodOverload ->
	// member-template instantiate) and renames the id, or sets an error node
	// -> self-detecting fallback. NAMED reference-param receivers only (a
	// pointer-stored variable whose value IS the referent address); other
	// receiver shapes stay NULL -> pattern error -> fallback.
	if (!recv_class && m_tsubst_pattern_mode && !tm->parent_expr
	    && template_param_under_type_layers(tm->object.type)
	    && tm->object.is_reference()
	    && tsubst_call_can_rewrite_after_subst(tm)) {
		FuncDef *callee = dynamic_cast<FuncDef *>(tm->var.type);
		std::string sym = func_emit_name(tm->var, callee);
		if (!sym.empty()) {
			node_t args = list();
			append(args, id(tm->object.name.c_str(), origin));
			// Unknown formals (deduced at instantiation): a REFERENCE-
			// returning call argument (std::forward/std::move) forwards
			// its POINTER — its call value already IS the object/scalar
			// address, and a deduced forwarding-ref formal is always
			// pointer-lowered (same convention as the unresolved-callee
			// arm in the member-template call path).
			for (TokenBase *arg : tm->parameters) {
				if (ref_returning_call_type(arg))
					append(args, ref_param_arg_addr(arg));
				else
					append(args, translate_expr(arg));
			}
			return node2(N_CALL, id(sym.c_str(), origin), args, origin);
		}
	}
	if (!recv_class) return NULL;

	// The method's call symbol (`ClassName__method`) and signature. When the
	// FuncDef carries an explicit emit_symbol, call through that instead of
	// the default scheme.
	FuncDef *callee = dynamic_cast<FuncDef *>(tm->var.type);
	const std::string sym = call_emit_symbol(tm->var, callee);

	// A method bound to an external symbol (emit_symbol) has no madc-emitted
	// body and is not in funcdef_map under its mangled name, so the
	// referenced-funcs prototype
	// pass won't declare it. Declare it here from its real signature (return
	// type matters: length()/size() return `long`, c_str() returns char* —
	// an implicit/void return would misread the value), and route the call
	// directly. `empty()` is lowered to `size() == 0` (the bool-returning
	// libstdc++ empty() returns a 1-byte value that reads as garbage upper
	// bits when taken as a 64-bit int), matching g++'s observable result.
	// An inherited method's __this must point at the OWNER's subobject within
	// the receiver: 0 for a primary base, non-zero for a secondary base, and
	// the static vbase_offset for a VIRTUAL base (basic_ios within ifstream —
	// inf.good() reads _M_streambuf_state from that subobject). Applies to
	// EVERY dispatch flavor below — the externally-bound (emit_symbol) and
	// member-template paths previously returned early WITHOUT this adjustment,
	// so the real libstdc++ good()/rdstate() read the wrong bytes.
	DataDefCLASS *owner = (callee && !callee->parameters.empty())
				? class_behind(callee->parameters[0]) : NULL;
	if (owner && recv_class && owner != recv_class) {
		size_t boff = recv_class->base_offset_of(owner);
		if (boff != 0 && boff != (size_t)-1) {
			// (void*)((char*)this_arg + boff)
			node_t charp = node2(N_CAST,
				node2(N_TYPE, node1(N_LIST, simple(N_CHAR)),
				      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
				this_arg, origin);
			this_arg = node2(N_CAST, void_ptr_type(),
				node2(N_ADD, charp, integer((long)boff), origin),
				origin);
		}
		// Cast even when the base subobject starts at offset 0: the C ABI
		// prototype still expects the inherited method's owner pointer type
		// (`__new_allocator<T>*`), not the derived receiver type
		// (`allocator<T>*`).
		this_arg = node2(N_CAST,
			node2(N_TYPE,
			      node1(N_LIST, class_tag_ref(owner)),
			      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
			this_arg, origin);
	}

	if (m_tsubst_pattern_mode && callee && callee->is_member_template
	    && callee->declaration_only
	    && tsubst_call_can_rewrite_after_subst(tm)
	    && tsubst_call_has_pack_expansion_arg(tm)) {
		node_t args = list();
		append(args, this_arg);
		for (TokenBase *arg : tm->parameters) {
			node_t n = NULL;
			if (TokenPackExpansion *pe =
			    dynamic_cast<TokenPackExpansion *>(arg)) {
				DataDefTemplateParam *tp = pe->pattern
					? template_param_in_pack_pattern(pe->pattern)
					: NULL;
				std::string value_name = tp
					? pack_value_name_in_pattern(
						pe->pattern, tp->param_index)
					: std::string();
				if (tp && !value_name.empty()
				    && ref_returning_call_type(pe->pattern)) {
					n = id(value_name.c_str(), arg);
					cir_node *cn = CIR_NODE(n);
					cn->datadef = pe->pattern->datadef();
					cn->tsubst_pack_expand = true;
					cn->tsubst_pack_index = tp->param_index;
					cn->tsubst_pack_value_name =
						arena.intern(value_name.c_str());
				}
			}
			append(args, n ? n : translate_expr(arg));
		}
		node_t mcall = node2(N_CALL, id(sym.c_str(), origin), args, origin);
		if (callee->returns_reference())
			return node1(N_DEREF, mcall, origin);
		return mcall;
	}

	if (callee && callee->is_member_template && callee->declaration_only)
		if (node_t mt = member_template_method_call(tm, callee, this_arg, origin))
			return mt;
	if (callee && !callee->emit_symbol.empty())
		return emit_symbol_method_call(tm, callee, sym, this_arg, origin);

	// A method returning a NON-TRIVIAL class by value uses the __retbuf ABI
	// (func_def / func_proto / fnptr_decl_pieces all lower the signature with
	// a hidden LEADING `struct <T> *__retbuf` ahead of __this): the call site
	// materializes a cleanup-tagged temp of the return class, passes its
	// address as that leading arg, and the expression value is the temp
	// lvalue — the method-call mirror of object_call_temp_addr. Without this
	// the call was emitted bare: one arg short, and non-addressable as a
	// value (`__x.get_allocator() == __a` in real _Vector_base).
	DataDefCLASS *ret_obj = (callee && !callee->returns_reference()
				 && !callee->is_multi_return())
				? class_return_via_retbuf(&callee->return_value_type()) : NULL;
	char ret_tmp[40] = { 0 };
	if (ret_obj)
		object_temp_decl(ret_obj, ret_tmp, sizeof(ret_tmp), origin);

	// Build the argument list: hidden __this first, then the explicit args.
	// Coerce each explicit arg to its declared parameter shape (string object
	// / numeric reference), mirroring the free-function call path. The
	// callee's parameter 0 is __this, so explicit arg i maps to parameter i+1.
	// A madc-instantiated member function template binds the call to the
	// declaration-only PLACEHOLDER (varargs, no ref_params — its signature is
	// deliberately left unmutated so the parser's findMethodOverload arity gate
	// stays correct). For ARGUMENT COERCION we need the instantiated
	// definition's real parameters + ref_params, resolved via the placeholder's
	// local_emit_name (the same resolution build_call_args does for the free /
	// static path). Without this the forwarding-reference pack parameter of
	// `_M_realloc_insert(iterator, _Args&&... __args)` is read from the
	// placeholder as a plain by-value pointer, so the reference argument `__x`
	// is auto-dereferenced to a struct VALUE and passed to the pointer parameter
	// (c2mir hard error for a class element; int tolerates it as the int<->ptr
	// warning). The emit `sym` still comes from the placeholder + local_emit_name.
	FuncDef *arg_callee = callee;
	if (callee && callee->is_member_template
	    && !callee->local_emit_name.empty() && m_prog) {
		if (Variable *iv = m_prog->findVariable(callee->local_emit_name))  // allowed-exception: lookup key, not symbol build
			if (FuncDef *ifd = dynamic_cast<FuncDef *>(iv->type))
				if (ifd != callee)
					arg_callee = ifd;
	}
	node_t args = list();
	if (ret_obj)
		append(args, node1(N_ADDR, id(ret_tmp, origin), origin));
	append(args, this_arg);
	for (size_t i = 0; i < tm->parameters.size(); i++) {
		TokenBase *arg = tm->parameters[i];
		size_t pi = i + 1;   // +1 to skip __this
		DataDef *pt = (arg_callee && pi < arg_callee->parameters.size())
				? arg_callee->parameters[pi] : NULL;
		bool is_ref_param = arg_callee && arg_callee->is_ref_param(pi);
		if (DataDefCLASS *pc = param_object_class(pt, is_ref_param))
			append(args, object_arg_addr(arg, pc));
		else if (DataDefCLASS *vc = as_class_instance(pt))
			append(args, object_arg_value(arg, vc));
		else if (is_ref_param)
			append(args, ref_param_arg_addr(arg, ref_param_referent(pt),
							const_ref_param(arg_callee, pi)));
		else if (!pt && ref_returning_call_type(arg))
			// Unresolved callee (a member-template instantiation binds to a
			// declaration-only placeholder with no parameters/ref_params):
			// a REFERENCE-returning call argument (std::forward/std::move) is
			// a reference, so forward the POINTER (its call value already IS
			// the object/scalar address) rather than auto-dereferencing to a
			// value — the placeholder param is a forwarding reference, and a
			// struct VALUE passed to the pointer param is a c2mir hard error
			// (allocator_traits::construct -> __new_allocator::construct with
			// `std::forward<Args>(args)` of a string element).
			append(args, ref_param_arg_addr(arg));
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
		node_t vcall = node2(N_CALL, fn, args, origin);
		if (ret_obj) {
			m_pending_stmts.push_back(node2(N_EXPR, list(), vcall, origin));
			return id(ret_tmp, origin);
		}
		return vcall;
	}

	referenced_funcs.insert(sym);
	node_t mcall = node2(N_CALL, id(sym.c_str(), origin), args, origin);
	// __retbuf ABI: the void call writes the result into the temp; the
	// expression value is the materialized temp lvalue.
	if (ret_obj) {
		m_pending_stmts.push_back(node2(N_EXPR, list(), mcall, origin));
		return id(ret_tmp, origin);
	}
	// A T&-returning method returns the address of its result; deref so the
	// call expression is the referenced lvalue (read: *p; write: *p = rhs).
	if (callee && callee->returns_reference())
		return node1(N_DEREF, mcall, origin);
	return mcall;
}

bool CirBuilder::class_has_object_members(DataDefCLASS *cdd)
{
	if (!cdd) return false;
	for (const auto &m : cdd->members)
		if (as_class_instance(m.second)) return true;
	return false;
}

static std::string ctor_call_symbol(DataDefCLASS *cdd, FuncDef *ctor);

static FuncDef *class_default_ctor_def(DataDefCLASS *cdd)
{
	if (!cdd) return NULL;
	for (Variable *cv : cdd->ctors) {
		FuncDef *fd = cv ? dynamic_cast<FuncDef *>(cv->type) : NULL;
		if (!fd) continue;
		size_t req = fd->required_param_count();
		if (req <= 1) return fd;   // only hidden __this is required
	}
	return NULL;
}

static FuncDef *class_copy_ctor_def(DataDefCLASS *cdd)
{
	if (!cdd) return NULL;
	for (Variable *cv : cdd->ctors) {
		FuncDef *fd = cv ? dynamic_cast<FuncDef *>(cv->type) : NULL;
		if (!fd || fd->parameters.size() < 2) continue;
		bool refp = fd->is_ref_param(1);
		DataDef *p1 = fd->parameters[1];
		DataDef *bind = p1;
		if (refp && p1 && p1->is_pointer()) {
			DataDefPTR *pp = dynamic_cast<DataDefPTR *>(p1);
			if (pp && pp->base_type) bind = pp->base_type;
		}
		if (same_object_class(cdd, bind)) return fd;
	}
	return NULL;
}

static FuncDef *class_assign_operator_def(DataDefCLASS *cdd)
{
	if (!cdd) return NULL;
	const std::string opname = "operator=";
	const std::string mangled = cdd->name + "__" + opname;
	for (Variable *mv : cdd->methods) {
		if (!mv) continue;
		FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
		if (!fd || fd->parameters.size() < 2) continue;
		if (mv->name != opname && mv->name != mangled
		    && fd->method_display_name != opname)
			continue;
		bool refp = fd->is_ref_param(1);
		DataDef *p1 = fd->parameters[1];
		DataDef *bind = p1;
		if (refp && p1 && p1->is_pointer()) {
			DataDefPTR *pp = dynamic_cast<DataDefPTR *>(p1);
			if (pp && pp->base_type) bind = pp->base_type;
		}
		if (same_object_class(cdd, bind)) return fd;
	}
	return NULL;
}

static FuncDef *class_assign_cstr_operator_def(DataDefCLASS *cdd)
{
	if (!cdd) return NULL;
	const std::string opname = "operator=";
	const std::string mangled = cdd->name + "__" + opname;
	for (Variable *mv : cdd->methods) {
		if (!mv) continue;
		FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
		if (!fd || fd->parameters.size() < 2) continue;
		if (mv->name != opname && mv->name != mangled
		    && fd->method_display_name != opname)
			continue;
		if (is_char_pointer(fd->parameters[1]))
			return fd;
	}
	return NULL;
}

static std::string class_method_call_symbol(DataDefCLASS *cdd, FuncDef *fd,
					    const std::string &name)
{
	return CirBuilder::call_emit_symbol(fd, cdd ? cdd->name + "__" + name : name);
}

void CirBuilder::flush_pending_stmts(std::vector<node_t> &out)
{
	if (m_pending_stmts.empty()) return;
	for (node_t p : m_pending_stmts)
		out.push_back(p);
	m_pending_stmts.clear();
}

bool CirBuilder::class_member_construct(DataDefCLASS *cdd,
					std::vector<node_t> &out, TokenBase *origin,
					const std::set<std::string> *skip)
{
	if (!cdd) return false;
	bool any = false;
	for (const auto &m : cdd->members) {
		if (skip && skip->count(m.first)) continue;
		DataDefCLASS *mc = as_class_instance(m.second);
		if (!mc) continue;
#ifdef MADC_DEBUG_CTORINIT
		fprintf(stderr, "[ctorinit] member-default owner=%s member=%s mclass=%s\n",
			cdd->name.c_str(), m.first.c_str(), mc->name.c_str());
#endif
		node_t fld = node2(N_DEREF_FIELD, id("__this", origin),
				   id(m.first.c_str(), origin));
		node_t stmt = class_ctor_call_addr(node1(N_ADDR, fld, origin),
						   mc, std::vector<TokenBase *>(),
						   origin);
		if (!stmt) continue;
		flush_pending_stmts(out);
		out.push_back(stmt);
		any = true;
	}
	return any;
}

static std::string last_scope_part(const std::string &name)
{
	size_t p = name.rfind("::");
	return p == std::string::npos ? name : name.substr(p + 2);
}

static DataDef *class_alias_lookup_cir(DataDefCLASS *cls,
				       const std::string &name,
				       std::set<DataDefCLASS *> &seen)
{
	if (!cls || seen.count(cls)) return NULL;
	seen.insert(cls);
	auto it = cls->type_aliases.find(name);
	if (it != cls->type_aliases.end())
		return it->second;
	std::string short_name = last_scope_part(name);
	if (short_name != name) {
		it = cls->type_aliases.find(short_name);
		if (it != cls->type_aliases.end())
			return it->second;
	}
	for (const BaseSpec &bs : cls->bases)
		if (DataDef *bd = class_alias_lookup_cir(bs.base, name, seen))
			return bd;
	if (cls->base_class)
		if (DataDef *bd = class_alias_lookup_cir(cls->base_class, name, seen))
			return bd;
	if (cls->enclosing_class)
		return class_alias_lookup_cir(cls->enclosing_class, name, seen);
	return NULL;
}

// Does mem-initializer `name` denote `target` — by class name, canonical C++
// spelling, or a type alias visible from `owner`? Shared by the base-
// initializer and delegating-constructor classifiers.
static bool ctor_initializer_names_class(DataDefCLASS *owner,
					 DataDefCLASS *target,
					 const std::string &name)
{
	if (!owner || !target) return false;
	std::string short_name = last_scope_part(name);
	if (name == target->name || short_name == target->name)
		return true;
	if (!target->canonical_cpp_spelling.empty()) {
		std::string tshort = last_scope_part(target->canonical_cpp_spelling);
		size_t lt = tshort.find('<');
		if (lt != std::string::npos)
			tshort = tshort.substr(0, lt);
		if (name == tshort || short_name == tshort)
			return true;
	}
	std::set<DataDefCLASS *> seen;
	return as_user_class(class_alias_lookup_cir(owner, name, seen)) == target;
}

static bool ctor_initializer_targets_base(DataDefCLASS *owner,
					  DataDefCLASS *base,
					  const std::string &name)
{
	if (!owner || !base) return false;
	// A mem-initializer naming the constructor's OWN class is a DELEGATING
	// constructor ([class.base.init]p6), never a base initializer — without
	// this guard the alias clause below matches it against any base the
	// owner derives from (real vector's `: vector(__rv, __m, true_type{})`
	// fired its 3 args at _Vector_base's 2-param ctors).
	if (ctor_initializer_names_class(owner, owner, name)) {
#ifdef MADC_DEBUG_CTORINIT
		fprintf(stderr, "[ctorinit] base-match DELEGATING owner=%s ci=%s -> 0\n",
			owner->name.c_str(), name.c_str());
#endif
		return false;
	}
	if (ctor_initializer_names_class(owner, base, name)) {
#ifdef MADC_DEBUG_CTORINIT
		fprintf(stderr, "[ctorinit] base-match NAMED owner=%s base=%s ci=%s\n",
			owner->name.c_str(), base->name.c_str(), name.c_str());
#endif
		return true;
	}
	std::set<DataDefCLASS *> seen;
	DataDefCLASS *alias_cls = as_user_class(class_alias_lookup_cir(owner, name, seen));
#ifdef MADC_DEBUG_CTORINIT
	fprintf(stderr, "[ctorinit] base-match ALIAS? owner=%s base=%s ci=%s alias_cls=%s -> %d (owner.encl=%s owner.base_class=%s)\n",
		owner->name.c_str(), base->name.c_str(), name.c_str(),
		alias_cls ? alias_cls->name.c_str() : "(null)",
		(int)(alias_cls && alias_cls->is_or_derives_from(base)),
		owner->enclosing_class ? owner->enclosing_class->name.c_str() : "(null)",
		owner->base_class ? owner->base_class->name.c_str() : "(null)");
#endif
	return alias_cls && alias_cls->is_or_derives_from(base);
}

static const FuncDef::CtorInitializer *find_base_initializer(
	DataDefCLASS *owner, DataDefCLASS *base, FuncDef *fd)
{
	if (!fd) return NULL;
	for (const FuncDef::CtorInitializer &ci : fd->ctor_initializers)
		if (ctor_initializer_targets_base(owner, base, ci.name))
			return &ci;
	return NULL;
}

// The mem-initializer (if any) naming the constructor's own class — a C++11
// DELEGATING constructor ([class.base.init]p6: it must then be the only
// initializer, and the targeted ctor performs the complete initialization).
static const FuncDef::CtorInitializer *find_delegating_initializer(
	DataDefCLASS *owner, FuncDef *fd)
{
	if (!fd) return NULL;
	for (const FuncDef::CtorInitializer &ci : fd->ctor_initializers)
		if (ctor_initializer_names_class(owner, owner, ci.name))
			return &ci;
	return NULL;
}

static const FuncDef::CtorInitializer *find_member_initializer(
	FuncDef *fd, const std::string &name)
{
	if (!fd) return NULL;
	for (const FuncDef::CtorInitializer &ci : fd->ctor_initializers)
		if (ci.name == name || last_scope_part(ci.name) == name)
			return &ci;
	return NULL;
}

bool CirBuilder::class_ctor_initializer_stmts(DataDefCLASS *cdd, FuncDef *fd,
					std::vector<node_t> &out, TokenBase *origin)
{
	if (!cdd || !fd || fd->ctor_initializers.empty()) return false;
	bool any = false;
	for (const auto &m : cdd->members) {
		const FuncDef::CtorInitializer *ci = find_member_initializer(fd, m.first);
		if (!ci) continue;
#ifdef MADC_DEBUG_CTORINIT
		fprintf(stderr, "[ctorinit] member-init owner=%s member=%s ci=%s nargs=%zu mclass=%s\n",
			cdd->name.c_str(), m.first.c_str(), ci->name.c_str(),
			ci->args.size(),
			as_class_instance(m.second) ? as_class_instance(m.second)->name.c_str() : "(non-class)");
#endif
		node_t fld = node2(N_DEREF_FIELD, id("__this", origin),
				   id(m.first.c_str(), origin));
		if (DataDefCLASS *mc = as_class_instance(m.second)) {
			node_t stmt = class_ctor_call_addr(node1(N_ADDR, fld, origin),
							   mc, ci->args, origin);
			if (!stmt) continue;
			flush_pending_stmts(out);
			out.push_back(stmt);
			any = true;
			continue;
		}
		if (ci->args.empty()) {
			// Value-initialization `member()` ([dcl.init]p8): a scalar
			// or pointer member ZERO-initializes (real
			// `_Vector_impl_data() : _M_start(), _M_finish(), ...` —
			// skipping it left garbage that the dtor then freed).
			// Class members took the ctor-call arm above.
			if (m.second && (m.second->is_numeric()
					 || m.second->is_pointer())) {
				node_t z = node2(N_ASSIGN, fld,
						 integer(0L, origin), origin);
				out.push_back(node2(N_EXPR, list(), z, origin));
				any = true;
			}
			continue;
		}
		if (ci->args.size() != 1 || !ci->args[0])
			continue;
		node_t init = translate_expr(ci->args[0]);
		// A reference member (`T& m`, lowered to a pointer slot) BINDS to its
		// initializer's address rather than copying a value: `m(e)` means
		// `m = &e`. translate_expr yields the referent lvalue (e.g. `*x` for a
		// reference argument), so address-of recovers the pointer (`&*x == x`).
		// Without this the struct VALUE was stored into the pointer slot
		// ("incompatible types in assignment to a pointer") — hit by
		// _Rb_tree::_Auto_node's `_Rb_tree& _M_t` init `_M_t(__t)`.
		if (m.second && m.second->is_reference())
			init = node1(N_ADDR, init, origin);
		node_t asgn = node2(N_ASSIGN, fld, init, origin);
		flush_pending_stmts(out);
		out.push_back(node2(N_EXPR, list(), asgn, origin));
		any = true;
	}
	return any;
}

bool CirBuilder::emit_member_default_inits(DataDefCLASS *cdd, const char *recv,
					   bool arrow, std::vector<node_t> &out,
					   TokenBase *origin,
					   const std::set<std::string> *skip)
{
	if (!cdd || cdd->member_default_inits.empty()) return false;
	bool any = false;
	// Declaration order ([class.base.init]p11): iterate the members, not the map.
	for (const auto &m : cdd->members) {
		// Object members value-init through the existing member-construction
		// path; an NSDMI `= T()` on such a member IS that same value-init, so
		// it carries no translatable scalar initializer here.
		if (as_class_instance(m.second)) continue;
		std::map<std::string, TokenBase *>::const_iterator di =
			cdd->member_default_inits.find(m.first);
		if (di == cdd->member_default_inits.end() || !di->second) continue;
		// An explicit ctor member-init / aggregate-init OVERRIDES the NSDMI.
		if (skip && skip->count(m.first)) continue;
		node_t fld = node2(arrow ? N_DEREF_FIELD : N_FIELD,
				   id(recv, origin), id(m.first.c_str(), origin));
		node_t asgn = node2(N_ASSIGN, fld, translate_expr(di->second), origin);
		flush_pending_stmts(out);
		out.push_back(node2(N_EXPR, list(), asgn, origin));
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
		DataDefCLASS *mc = as_class_instance(m.second);
		if (!mc || !class_needs_dtor(mc)) continue;
		std::string sym = class_dtor_symbol(mc);
		bool external = false;
		if (Variable *dv = class_own_dtor(mc)) {
			FuncDef *dt = dynamic_cast<FuncDef *>(dv->type);
			external = dt && !dt->emit_symbol.empty();
		}
		if (external) {
			need_output_extern(sym.c_str(), false, { { {N_VOID}, true } });
		} else {
			referenced_funcs.insert(sym);
			// Emit a TYPED forward prototype `void sym(struct mc *)` for the
			// in-module member dtor. Without a forward proto, an aggregate dtor
			// emitted before the member dtor's definition calls it as an
			// implicit-int K&R variadic function — which mis-wires the `this`
			// argument (ABI mismatch: a non-variadic 1-arg callee reads `this`
			// from the wrong place) → null-`this` deref in the member dtor (the
			// std::map/std::set `_Rb_tree` empty-container dtor SIGSEGV@0x8). The
			// proto matches the definition's param type, so no conflicting-types.
			need_output_extern(sym.c_str(), false, { { {}, true, mc } });
		}
		node_t fld = node2(N_DEREF_FIELD, id("__this", origin),
				   id(m.first.c_str(), origin));
		node_t addr = node1(N_ADDR, fld, origin);
		if (external)
			addr = node2(N_CAST, void_ptr_type(), addr, origin);
		node_t a = list();
		append(a, addr);
		node_t call = node2(N_CALL, id(sym.c_str(), origin), a, origin);
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

Variable *CirBuilder::class_own_dtor(DataDefCLASS *cdd)
{
	if (!cdd) return NULL;
	// A "~"-prefixed method_map key marks a destructor (own or inherited). The
	// own dtor is the one whose Variable is also in cdd->methods; inherited dtors
	// are copied into method_map alone (parser.cpp base-merge ~20600). This is
	// name-independent: a nested/instantiated class keeps its source-tag key
	// ("~Inner") while cdd->name is the composed name ("Outer_int32_t__Inner").
	for (auto &kv : cdd->method_map) {
		if (kv.first.empty() || kv.first[0] != '~' || !kv.second)
			continue;
		if (std::find(cdd->methods.begin(), cdd->methods.end(), kv.second)
		    != cdd->methods.end())
			return kv.second;
	}
	return NULL;
}

bool CirBuilder::class_has_own_user_dtor(DataDefCLASS *cdd)
{
	return class_own_dtor(cdd) != NULL;
}

static std::string ctor_call_symbol(DataDefCLASS *cdd, FuncDef *ctor);

// The class that `dd` denotes IF returning it by value must use the
// struct-return (__retbuf) ABI — i.e. a non-trivial class: one that needs a
// destructor (object members / user dtor / non-trivial base). A bit-copy return
// of such a class would double-free its members' resources at the two scope
// exits, so the callee must deep-copy into *__retbuf. A trivial struct keeps
// c2mir's native struct return. Returns NULL otherwise.
DataDefCLASS *CirBuilder::class_return_via_retbuf(DataDef *dd)
{
	if (!dd || dd->is_pointer()) return NULL;
	DataDefCLASS *cdd = as_class_instance(dd);
	if (!cdd) return NULL;
	return class_needs_dtor(cdd) ? cdd : NULL;
}

// Copy-construct `cdd` from `src` into the __retbuf return slot. Prefer an
// available copy constructor; otherwise use the legacy bit-copy plus member
// reconstruction fallback. `src` is a TokenBase whose translate_expr yields the
// source object lvalue (the `return obj;` operand).
void CirBuilder::class_copy_construct_into_retbuf(DataDefCLASS *cdd,
						  TokenBase *src,
						  std::vector<node_t> &out,
						  TokenBase *origin)
{
	std::vector<TokenBase *> copy_args;
	if (src) copy_args.push_back(src);
	FuncDef *copy_ctor = select_ctor_overload(cdd, copy_args);
	if (copy_ctor && src) {
		std::string sym = ctor_call_symbol(cdd, copy_ctor);
		node_t args = list();
		append(args, id(RETBUF_NAME, origin));   // __retbuf is already T*
		auto append_ctor_arg = [&](TokenBase *arg, size_t pi) {
			DataDef *pt = (pi < copy_ctor->parameters.size())
					? copy_ctor->parameters[pi] : NULL;
			bool refp = copy_ctor->is_ref_param(pi);
			if (DataDefCLASS *pc = param_object_class(pt, refp))
				append(args, object_arg_addr(arg, pc));
			else if (DataDefCLASS *vc = as_class_instance(pt))
				append(args, object_arg_value(arg, vc));
			else if (refp)
				append(args, ref_param_arg_addr(arg, ref_param_referent(pt),
								const_ref_param(copy_ctor, pi)));
			else
				append(args, translate_expr(arg));
		};
		for (size_t i = 0; i < copy_args.size(); i++)
			append_ctor_arg(copy_args[i], i + 1);
		for (size_t pi = copy_args.size() + 1;
		     pi < copy_ctor->parameters.size()
		     && pi < copy_ctor->param_defaults.size()
		     && copy_ctor->param_defaults[pi]; pi++)
			append_ctor_arg(copy_ctor->param_defaults[pi], pi);
		if (copy_ctor->ctor_trailing_self)
			append(args, id(RETBUF_NAME, origin));
		if (!copy_ctor->emit_symbol.empty()) {
			std::vector<ExternParam> eparams;
			eparams.push_back({ {N_VOID}, true });   // this
			for (size_t pi = 1; pi < copy_ctor->parameters.size(); pi++) {
				DataDef *cpt = copy_ctor->parameters[pi];
				bool crefp = copy_ctor->is_ref_param(pi);
				if (param_object_class(cpt, crefp) || crefp)
					eparams.push_back({ {N_VOID}, true });
				else if (cpt && cpt->is_pointer())
					eparams.push_back({ {N_CHAR}, true });
				else
					eparams.push_back({ {N_LONG}, false });
			}
			if (copy_ctor->ctor_trailing_self)
				eparams.push_back({ {N_VOID}, true });
			need_output_extern(sym.c_str(), false, eparams);
		}
		referenced_funcs.insert(sym);
		node_t call = node2(N_CALL, id(sym.c_str(), origin), args, origin);
		CIR_NODE(call)->synth_from_origin = true;
		out.push_back(node2(N_EXPR, list(), call, origin));
		return;
	}

	// 1) Bit-copy the source object into *__retbuf (covers scalar members and
	//    establishes the layout). `*__retbuf = src;` — c2mir struct assignment.
	node_t retderef = node1(N_DEREF, id(RETBUF_NAME, origin), origin);
	node_t bitcopy = node2(N_ASSIGN, retderef, translate_expr(src), origin);
	out.push_back(node2(N_EXPR, list(), bitcopy, origin));
	// 2) Re-construct each object member from the source's member, so the slot
	//    owns its own resources (the bit-copy aliased them).
	for (const auto &m : cdd->members) {
		DataDefCLASS *mc = as_class_instance(m.second);
		if (!mc) continue;
		FuncDef *copy_ctor = class_copy_ctor_def(mc);
		if (!copy_ctor) continue;
		std::string sym = ctor_call_symbol(mc, copy_ctor);
		bool external = !copy_ctor->emit_symbol.empty();
		if (external)
			need_output_extern(sym.c_str(), false,
					   { { {N_VOID}, true }, { {N_VOID}, true } });
		else
			referenced_funcs.insert(sym);
		node_t dstfld = node2(N_DEREF_FIELD, id(RETBUF_NAME, origin),
				      id(m.first.c_str(), origin));
		node_t dst = node1(N_ADDR, dstfld, origin);
		if (external)
			dst = node2(N_CAST, void_ptr_type(), dst, origin);
		node_t srcfld = node2(N_FIELD, translate_expr(src),
				      id(m.first.c_str(), origin));
		node_t srcp = node1(N_ADDR, srcfld, origin);
		if (external)
			srcp = node2(N_CAST, void_ptr_type(), srcp, origin);
		node_t a = list();
		append(a, dst);
		append(a, srcp);
		node_t call = node2(N_CALL, id(sym.c_str(), origin), a, origin);
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
// self-assignment-safe. The scalar members are assigned individually (NO
// whole-struct bit-copy).
// `lhs`/`rhs` are object lvalues; the builder re-translates them per member.
// Per-member copy-assignment core: given two already-bound `struct Cls *` local
// names (`lname` = &lhs, `rname` = &rhs), emit `lname->m`-from-`rname->m` for
// each member (class object -> operator= when available, scalar -> plain =).
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
		DataDefCLASS *mc = as_class_instance(m.second);
		if (mc) {
			FuncDef *op = class_assign_operator_def(mc);
			if (!op) {
				node_t asn = node2(N_ASSIGN, lfld, rfld, origin);
				CIR_NODE(asn)->synth_from_origin = true;
				out.push_back(node2(N_EXPR, list(), asn, origin));
				continue;
			}
			std::string sym = class_method_call_symbol(mc, op, "operator=");
			bool external = !op->emit_symbol.empty();
			if (external)
				need_output_extern(sym.c_str(), true,
						   { { {N_VOID}, true }, { {N_VOID}, true } });
			else
				referenced_funcs.insert(sym);
			node_t ld = node1(N_ADDR, lfld, origin);
			node_t rd = node1(N_ADDR, rfld, origin);
			if (external) {
				ld = node2(N_CAST, void_ptr_type(), ld, origin);
				rd = node2(N_CAST, void_ptr_type(), rd, origin);
			}
			node_t a = list();
			append(a, ld);
			append(a, rd);
			node_t call = node2(N_CALL, id(sym.c_str(), origin), a, origin);
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
		class_tag_ref(cdd, origin))));
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
		node2(N_TYPE, node1(N_LIST, class_tag_ref(cdd, origin)),
			node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
		rhs_addr, origin);
	out.push_back(class_ptr_bind(cdd, lname,
		node1(N_ADDR, translate_expr(lhs), origin), origin));
	out.push_back(class_ptr_bind(cdd, rname, rcast, origin));
	class_copy_assign_members(cdd, lname, rname, out, origin);
}

// A class for which Pass 1.6 synthesizes the base destructor (Cls___dtor):
// non-trivial (needs a dtor), no user-written dtor, and not a libstdc++-owned
// (externally-defined) class. The single source of truth shared by the Pass-1.6
// emission loop and the tsubst completeness check, so "this dtor symbol is
// emittable" and "Pass 1.6 will emit it" can never drift apart.
bool CirBuilder::class_gets_synth_dtor(DataDefCLASS *cdd)
{
	return cdd && !class_has_own_user_dtor(cdd) && class_needs_dtor(cdd)
	    && !cdd->is_externally_defined();
}

std::string CirBuilder::class_dtor_symbol(DataDefCLASS *cdd)
{
	if (!cdd) return std::string();
		// A class whose destructor is bound to an external symbol (emit_symbol set)
		// uses that symbol directly as the cleanup function; madc synthesizes no
		// Cls___dtor body for it (synth_dtor_def early-returns on has_user_dtor).
		// The external dtor takes only `this`, matching the cleanup attribute's
		// `void (*)(T*)` shape.
	if (Variable *dv = class_own_dtor(cdd)) {
		FuncDef *dt = dynamic_cast<FuncDef *>(dv->type);
		// The one call-symbol resolver (emit_symbol ?: local_emit_name ?:
		// default): a FUNCTION-LOCAL class's dtor registers under a nested
		// unique symbol carried by local_emit_name; reading only
		// emit_symbol emitted a dangling Cls___dtor cleanup reference.
		if (dt)
			return call_emit_symbol(dt, cdd->name + "___dtor");
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
	// An externally-defined class's real D1 IS the complete-object destructor:
	// libstdc++ destroys the whole object (virtual bases included), so use it
	// directly instead of a synthesized `Cls___dtor_complete` wrapper — which is
	// never emitted for such a class (Pass 1.7 skips it), causing an undefined
	// symbol at link if referenced by a cleanup attribute.
	if (cdd->is_externally_defined())
		return class_dtor_symbol(cdd);
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
			node1(N_LIST, class_tag_ref(vb)),
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
	node_t pspec = node1(N_LIST, class_tag_ref(cdd));
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
	node_t pspec = node1(N_LIST, class_tag_ref(cdd));
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
	node_t pspec = node1(N_LIST, class_tag_ref(cdd));
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
		DataDefCLASS *mc = as_class_instance(m.second);
		if (!mc) continue;
		FuncDef *ctor = class_default_ctor_def(mc);
		if (!ctor) continue;
		std::string sym = ctor_call_symbol(mc, ctor);
		bool external = !ctor->emit_symbol.empty();
		if (external)
			need_output_extern(sym.c_str(), false, { { {N_VOID}, true } });
		else
			referenced_funcs.insert(sym);
		node_t fld = node2(N_FIELD, id(inst, origin),
				   id(m.first.c_str(), origin));
		node_t addr = node1(N_ADDR, fld, origin);
		if (external)
			addr = node2(N_CAST, void_ptr_type(), addr, origin);
		node_t a = list();
		append(a, addr);
		node_t call = node2(N_CALL, id(sym.c_str(), origin), a, origin);
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
	node_t pspec = node1(N_LIST, class_tag_ref(cdd));
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
			node1(N_LIST, class_tag_ref(b)),
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
			  std::vector<carray_dim_t>());
	(void)owner;
	return node2(N_TYPE, spec_list, node2(N_DECL, ignore(), decl_list));
}

// Generic overload-resolution ranking (declared in madc.h). No type is
// special-cased; runtime/library classes fall out of the class-object +
// converting-ctor rules like any other class.
int score_arg_to_param(const DataDef *adc, const DataDef *pdc,
		       bool param_is_ref, bool allow_udc,
		       bool arg_is_zero_literal)
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
		if (adc->is_object() && same_object_class(adc, pdc))
			return 5;   // exact class match (copy / same object)
		// Derived-to-base binding ([conv.ptr]/[dcl.init.ref]): a DERIVED
		// argument binds a BASE class parameter (base copy/move ctor from
		// a derived object — `_Tp_alloc_type(std::move(__x))` slicing the
		// allocator base out of _Vector_impl). Ranked below exact, above
		// a user-defined conversion.
		if (adc->is_object()) {
			const DataDefCLASS *ac = as_user_class(adc);
			const DataDefCLASS *pc2 = as_user_class(pdc);
			if (ac && pc2 && ac->is_or_derives_from(pc2))
				return 3;
		}
		if (!allow_udc)
			return -1;  // no further user-defined conversion permitted
		const DataDefCLASS *pc = as_user_class(pdc);
		if (!pc)
			return -1;
		int best = -1;
		for (Variable *cv : pc->ctors) {
			FuncDef *fd = cv ? dynamic_cast<FuncDef *>(cv->type) : NULL;
			// A converting ctor must be CALLABLE WITH ONE explicit argument:
			// __this + at least one user param, and every user param after the
			// first must be DEFAULTED. The bare `== 2` test missed converting
			// ctors with trailing defaulted params — e.g. real libstdc++
			// `basic_string(const char*, const _Alloc& = _Alloc())` has 3
			// parameters (__this + const char* + defaulted allocator), so a
			// `const char*` argument scored -1 against a `std::string` parameter,
			// the proper set::insert(const string&)/(string&&) overloads were
			// rejected, and a template insert wildcard-matched instead (the
			// undefined `_insert__o5` import — set-wall bug-7).
			if (!fd || fd->parameters.size() < 2
			    || fd->required_param_count() > 2)
				continue;
			bool cref = fd->is_ref_param(1);
			int s = score_arg_to_param(adc, fd->parameters[1], cref, false,
						   arg_is_zero_literal);
			if (s > best)
				best = s;
		}
		return best >= 0 ? 2 : -1;   // a viable converting ctor => conversion
	}
	// A class-object ARGUMENT only binds the same-class parameter (handled
	// above); there is no implicit object->scalar/pointer conversion here.
	if (adc->is_object())
		return -1;
	// Enums lower to integer storage in MC11-IR, but C++ overload
	// resolution still treats a named enum as its own domain. Without this,
	// `std::size_t` expressions can score as an exact match for
	// `std::align_val_t` because both happen to lower to integer-like CIR
	// types, selecting the wrong global operator delete overload.
	const DataDefENUM *a_enum = as_enum_type(adc);
	const DataDefENUM *p_enum = as_enum_type(pdc);
	if (a_enum || p_enum) {
		if (a_enum && p_enum)
			return same_enum_type(a_enum, p_enum) ? 5 : -1;
		if (p_enum)
			return -1;
		// Existing enum-to-integer ranking stays a standard conversion;
		// unscoped-enum uses rely on this, and scoped enums still prefer
		// their exact enum overload when one exists.
		return pdc->is_numeric() ? 4 : -1;
	}
	// A function-pointer PARAMETER. Function-to-pointer decay: a bare function
	// argument binds it (`__stoa(&std::strtol, ...)`), discriminated by
	// SIGNATURE (C++ has no implicit conversion between incompatible
	// function-pointer types; the __stoa<float>/<double> instantiations differ
	// ONLY here). A NON-function argument does NOT bind a fn-ptr param — C++ has
	// no integer/float→function-pointer conversion (only the null-pointer
	// constant). This rejection is load-bearing: DataDefFPTR::is_numeric() is
	// true, so without it a fn-ptr param falls through to the numeric branch
	// below and a 64-bit integer argument spuriously matched the ostream
	// manipulator `operator<<(ostream& (*)(ostream&))`, tying (rawtype 64==64)
	// and — registered first — beating `operator<<(long)` (`cout << (long)`
	// mangled the bogus _ZNSolsESo; int/float/double were unaffected).
	if (dynamic_cast<const DataDefFPTR *>(pdc)) {
		const bool arg_is_fn = adc->is_function()
		    || dynamic_cast<const DataDefFPTR *>(adc);
		if (!arg_is_fn)
			return arg_is_zero_literal ? 3 : -1; // null binds; else reject
		const FuncDef *pf =
		    dynamic_cast<const DataDefFPTR *>(pdc)->target;
		const FuncDef *af = dynamic_cast<const FuncDef *>(adc);
		if (!af)
			if (const DataDefFPTR *afp =
			    dynamic_cast<const DataDefFPTR *>(adc))
				af = afp->target;
		if (!pf || !af)
			return 4;
		if (pf->parameters.size() != af->parameters.size()
		    || pf->is_varargs != af->is_varargs
		    || pf->return_value_type().rawtype() != af->return_value_type().rawtype())
			return -1;
		for (size_t i = 0; i < pf->parameters.size(); i++) {
			if (!pf->parameters[i] || !af->parameters[i])
				continue;
			if (pf->parameters[i]->rawtype()
			    != af->parameters[i]->rawtype())
				return -1;
		}
		return 5;
	}
	bool p_ptr = pdc->is_pointer(), a_ptr = adc->is_pointer();
	bool p_num = pdc->is_numeric(), a_num = adc->is_numeric();
	if (p_ptr || a_ptr) {
		if (p_ptr && a_ptr) {
			const DataDefCLASS *ac = class_pointer_pointee(adc);
			const DataDefCLASS *pc = class_pointer_pointee(pdc);
			if (ac && pc) {
				if (ac == pc)
					return 5;
				return ac->is_or_derives_from(pc) ? 4 : -1;
			}
			return adc->rawtype() == pdc->rawtype() ? 5 : 4;
		}
		// A null-pointer constant (integer literal 0) binds any pointer
		// parameter — a standard conversion ([conv.ptr]), ranked below a
		// pointer argument but above a user-defined conversion.
		if (p_ptr && arg_is_zero_literal)
			return 3;
		return -1;
	}
	if (p_num && a_num)
		return adc->rawtype() == pdc->rawtype() ? 5 : 4;
	return 0;            // unrecognized pairing: neutral
}

// The call symbol for a constructor of `cdd`. Precedence: an external ABI
// symbol (emit_symbol) > a
// disambiguated-overload symbol (local_emit_name — the 2nd+ same-arity ctor,
// which mangles to its own ClassName__ClassName__oN) > the default
// ClassName__ClassName scheme (the first / only ctor).
static std::string ctor_call_symbol(DataDefCLASS *cdd, FuncDef *ctor)
{
	return CirBuilder::call_emit_symbol(ctor, cdd->name + "__" + cdd->name);
}

// Select the constructor overload of `cdd` matching the initializer arguments by
// ARGUMENT TYPE (general overload resolution, not arity alone), via the shared
// generic score_arg_to_param ranking. The candidate whose parameter types best
// match the argument types wins; a candidate that cannot bind any argument is
// rejected. No type is special-cased — a same-class argument selects the copy
// ctor, and a convertible argument selects a converting ctor. Relies on
// operator-result initializers being
// correctly TYPED (Program::resolve_object_operator_type), so a `T c = a + b`
// copy-init sees the operator's class return type, not a pointer/arithmetic type.
// `cdd->ctors` lists the overloads. Returns NULL when no overload set is recorded
// (caller falls back to the single ctor keyed under the class name).
// The expression class/type a ctor (or copy-fallback) ARGUMENT denotes for
// overload matching — the ONE resolver shared by select_ctor_overload and
// try_implicit_copy_construct so they can never disagree.
DataDef *CirBuilder::ctor_arg_datadef(TokenBase *arg)
{
	if (!arg) return NULL;
	// A class-returning CALL argument types by the RESOLVED callee's
	// return class — the call token's own datadef may still be a
	// body-less template placeholder's `long` (the overload-set winner
	// is ranked at CIR time). Without this, `return f(...);` in a
	// retbuf function missed the copy ctor and bit-copied the result
	// (aliasing the heap buffer -> double free).
	if (DataDefCLASS *rc = object_returning_call_class(arg))
		return rc;
	// A REFERENCE-returning call argument (std::move(x), a T&/T&&
	// returning method) denotes the referenced lvalue: unwrap the
	// pointer representation, like the vfREFERENCE variable below.
	if (DataDef *rt = ref_returning_call_type(arg))
		return rt;
	if (TokenVar *tv = dynamic_cast<TokenVar *>(arg)) {
		// A reference variable's datadef is the pointer
		// representation (T&  ->  T*); for overload matching the
		// expression names a T lvalue. Same unwrap as
		// select_operator_overload's overload_arg_datadef — without
		// it a `const A &p` argument scores as A* and the copy
		// ctor never matches (A local(p) dropped construction).
		if (tv->var.is_reference() && tv->var.type) {
			// The reference variable names its REFERENT lvalue for
			// overload matching — unwrap exactly ONE reference level.
			// The referent may be a class (A), a pointer (base*), or a
			// scalar (int); class_behind would dig through a pointer
			// referent to its class (losing a level) and miss a scalar
			// referent entirely — leaving a `const int&` arg as `int*`,
			// so `tuple<const int&>(const int&)` never matched.
			if (DataDefPTR *rp =
			    dynamic_cast<DataDefPTR *>(tv->var.type))
				if (rp->base_type)
					return rp->base_type;
		}
		if (tv->var.is_fixed_array() && tv->var.type && m_prog)
			return m_prog->getPointerType(tv->var.type);
	}
	if (TokenMember *tm = dynamic_cast<TokenMember *>(arg)) {
		if (tm->is_fixed_array_member() && tm->var.type && m_prog)
			return m_prog->getPointerType(tm->var.type);
	}
	if (DataDefCArray *ca = dynamic_cast<DataDefCArray *>(arg->datadef())) {
		if (ca->element_type && m_prog)
			return m_prog->getPointerType(ca->element_type);
	}
	return arg->datadef();
}

FuncDef *CirBuilder::select_ctor_overload(DataDefCLASS *cdd,
					  const std::vector<TokenBase *> &ctor_args)
{
	if (!cdd || cdd->ctors.empty()) return NULL;
	FuncDef *best = NULL;
	Variable *best_var = NULL;
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
			bool refp = fd->is_ref_param(pi);
			DataDef *adc = ctor_arg_datadef(ctor_args[i]);
			int s = score_arg_to_param(adc, pt, refp, true,
					is_zero_integer_literal(ctor_args[i]));
			if (s < 0) { ok = false; break; }
			total += s;
		}
		if (ok && total > best_score) {
			best_score = total;
			best = fd;
			best_var = cv;
		}
	}
	if (best && best_var && best->local_emit_name.empty()) {
		std::string default_sym = cdd->name + "__" + cdd->name;
		if (best_var->name != default_sym)
			best->local_emit_name = best_var->name;
	}
	return best;
}

// A class with user constructors whose initializer matched none of them.
// Previously this case returned NULL and every caller's `if (cc)` guard
// silently dropped the construction — the object was left unconstructed and
// the program read garbage or crashed far from the cause. Emit an error node
// instead so the pre-c2mir gate rejects the tree (same policy as the
// unhandled-expression tail of translate_expr); the origin token carries the
// source file:line:col, printed by cir_report_errors.
node_t CirBuilder::no_ctor_match_error(DataDefCLASS *cdd,
				       const std::vector<TokenBase *> &ctor_args,
				       TokenBase *origin)
{
	std::string args_desc;
	for (size_t i = 0; i < ctor_args.size(); i++) {
		if (i) args_desc += ", ";
		DataDef *ad = ctor_args[i] ? ctor_args[i]->datadef() : NULL;
		if (DataDefCLASS *rc = object_returning_call_class(ctor_args[i]))
			ad = rc;
		args_desc += (ad && !ad->name.empty()) ? ad->name
						       : std::string("<unknown>");
	}
	char buf[256];
	snprintf(buf, sizeof(buf),
		 "no matching constructor for call to '%s(%s)'",
		 cdd ? cdd->name.c_str() : "<class>", args_desc.c_str());
#ifdef MADC_DEBUG_CTORINIT
	fprintf(stderr, "[ctorinit] NO-MATCH cdd=%s nctors=%zu origin=%s:%d\n",
		cdd ? cdd->name.c_str() : "(null)",
		cdd ? cdd->ctors.size() : 0,
		origin && origin->file ? origin->file : "?", origin ? origin->line : 0);
	for (size_t i = 0; i < ctor_args.size(); i++) {
		TokenBase *a = ctor_args[i];
		TokenCallFunc *tcf = dynamic_cast<TokenCallFunc *>(a);
		FuncDef *cfd = tcf ? call_target_funcdef(tcf) : NULL;
		DataDef *resolved = ctor_arg_datadef(a);
		fprintf(stderr, "[ctorinit]   arg%zu tok=%s line=%d datadef=%s resolved=%s callee=%s ret=%s ref=%d\n",
			i, a ? typeid(*a).name() : "(null)",
			a ? a->line : 0,
			a && a->datadef() ? a->datadef()->name.c_str() : "(none)",
			resolved ? resolved->name.c_str() : "(none)",
			tcf ? tcf->var.name.c_str() : "(not-call)",
			cfd ? cfd->return_value_type().name.c_str() : "(no-cfd)",
			cfd ? (int)cfd->returns_reference() : -1);
	}
	if (cdd)
		for (Variable *cv : cdd->ctors) {
			FuncDef *cf = cv ? dynamic_cast<FuncDef *>(cv->type) : NULL;
			if (!cf) {
				fprintf(stderr, "[ctorinit]   cand %s (NOT FuncDef: type=%s)\n",
					cv ? cv->name.c_str() : "(null)",
					cv && cv->type ? cv->type->name.c_str() : "(null)");
				continue;
			}
			std::string ps;
			for (size_t pi = 1; pi < cf->parameters.size(); pi++) {
				if (pi > 1) ps += ", ";
				ps += cf->parameters[pi] ? cf->parameters[pi]->name : "?";
			}
			fprintf(stderr, "[ctorinit]   cand %s(%s) req=%zu\n",
				cv->name.c_str(), ps.c_str(), cf->required_param_count());
		}
#endif
	return error_node(buf, origin);
}

// Recursive trivial-copyability ([class.prop], the subset madc models): no
// own user dtor, no user copy ctor, no vtable, and every class-type member
// and base is itself trivially copyable. NOTE class_needs_dtor is the WRONG
// gate for copyability — it treats ANY object member as non-trivial, but
// move_iterator{__normal_iterator{int*}} is bit-copyable.
bool CirBuilder::class_trivially_copyable(DataDefCLASS *cdd,
					  std::set<DataDefCLASS *> &seen)
{
	if (!cdd) return false;
	if (seen.count(cdd)) return true;   // already under verification
	seen.insert(cdd);
	if (class_has_own_user_dtor(cdd) || cdd->has_vtable) return false;
	if (class_copy_ctor_def(cdd)) return false;
	for (const auto &m : cdd->members)
		if (DataDefCLASS *mc = as_class_instance(m.second))
			if (!class_trivially_copyable(mc, seen)) return false;
	for (const BaseSpec &bs : cdd->bases)
		if (bs.base && !class_trivially_copyable(bs.base, seen))
			return false;
	if (cdd->base_class && !class_trivially_copyable(cdd->base_class, seen))
		return false;
	return true;
}

bool CirBuilder::class_trivially_copyable(DataDefCLASS *cdd)
{
	std::set<DataDefCLASS *> seen;
	return class_trivially_copyable(cdd, seen);
}

// IMPLICIT COPY CONSTRUCTOR ([class.copy.ctor]): a same-class initializer
// argument that matched no user ctor selects the implicitly-declared copy
// ctor — C++ synthesizes one whenever the class declares no copy ctor, even
// with other user ctors present (real __normal_iterator/move_iterator: only
// a default and an `explicit (iterator&)` ctor exist). For a trivially-
// copyable class that is a member-wise bit copy, expressed as a struct
// assignment into the destination lvalue. A class WITH a user copy ctor
// never takes this fallback — its no-match stays a loud error (bit-copying
// past a real copy ctor would mask an overload-scoring bug). Non-trivially-
// copyable classes need member-wise copy-construction; deferred — they
// declare copy ctors in practice. Returns NULL when the fallback does not
// apply.
node_t CirBuilder::try_implicit_copy_construct(node_t dst_lvalue,
					       DataDefCLASS *cdd,
					       const std::vector<TokenBase *> &ctor_args,
					       TokenBase *origin)
{
	if (!dst_lvalue || !cdd) return NULL;
	if (ctor_args.size() != 1 || !ctor_args[0]) return NULL;
	// Resolve the argument's class with the SAME resolver overload scoring
	// uses (ref-returning calls like std::move(x), reference variables,
	// resolved callees) — the raw token datadef may be a stale overload-set
	// binding (move_iterator's `: _M_current(std::move(__i))`).
	if (as_class_instance(ctor_arg_datadef(ctor_args[0])) != cdd) return NULL;
	if (!class_trivially_copyable(cdd)) return NULL;
	node_t src = translate_expr(ctor_args[0]);
	node_t asgn = node2(N_ASSIGN, dst_lvalue, src, origin);
	return node2(N_EXPR, list(), asgn, origin);
}

node_t CirBuilder::class_ctor_call_addr(node_t this_addr, DataDefCLASS *cdd,
				   const std::vector<TokenBase *> &ctor_args,
				   TokenBase *origin)
{
	if (!this_addr || !cdd) return NULL;

	if (!cdd->has_user_ctor) {
		if (!cdd->has_vtable) return NULL;
		std::string vname = class_vtable_symbol(cdd);
		node_t blk = list();
		for (size_t g = 0; g < cdd->vtable_groups.size(); g++) {
			const DataDefCLASS::VtableGroup &G = cdd->vtable_groups[g];
			std::string fld = (G.this_offset == 0)
				? "__vptr" : ("__vptr_" + std::to_string(G.this_offset));
			node_t lhs = node2(N_DEREF_FIELD, this_addr,
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

	FuncDef *ctor = select_ctor_overload(cdd, ctor_args);
	if (!ctor) {
		node_t dst = node1(N_DEREF,
			node2(N_CAST, class_ptr_type(cdd), this_addr, origin),
			origin);
		if (node_t cc = try_implicit_copy_construct(dst, cdd, ctor_args,
							    origin))
			return cc;
		if (cdd->ctors.empty() || ctor_args.empty()) {
			auto it = cdd->method_map.find(cdd->name);
			if (it != cdd->method_map.end() && it->second)
				ctor = dynamic_cast<FuncDef *>(it->second->type);
		}
	}
	if (!ctor)
		return no_ctor_match_error(cdd, ctor_args, origin);

	std::string sym = ctor_call_symbol(cdd, ctor);

	std::vector<node_t> explicit_nodes;
	for (size_t i = 0; i < ctor_args.size(); i++) {
		TokenBase *arg = ctor_args[i];
		size_t pi = i + 1;
		DataDef *pt = (ctor && pi < ctor->parameters.size())
				? ctor->parameters[pi] : NULL;
		bool is_ref_param = ctor && ctor->is_ref_param(pi);
		if (DataDefCLASS *pc = param_object_class(pt, is_ref_param))
			explicit_nodes.push_back(object_arg_addr(arg, pc));
		else if (DataDefCLASS *vc = as_class_instance(pt))
			explicit_nodes.push_back(object_arg_value(arg, vc));
		else if (is_ref_param)
			explicit_nodes.push_back(ref_param_arg_addr(arg,
				ref_param_referent(pt), const_ref_param(ctor, pi)));
		else
			// Derived->base pointer argument (`B*` arg -> `A*` parameter):
			// make the implicit upcast explicit so c2mir does not warn
			// (mirrors the general call path's argument coercion).
			explicit_nodes.push_back(upcast_class_ptr(translate_expr(arg), pt, arg, arg));
	}
	return ctor_call_assemble(this_addr, cdd, ctor, explicit_nodes, origin);
}

// The ONE constructor-call assembler: completes trailing default arguments
// (e.g. libstdc++'s allocator parameter), appends the trailing-self pattern,
// declares the extern for an externally-bound ctor, and builds the call.
// `explicit_nodes` are already-translated argument expressions — the token
// path (class_ctor_call_addr) and node-level synthesizers (host-call shims)
// both end here, so default-arg completion can never diverge between them.
node_t CirBuilder::ctor_call_assemble(node_t this_addr, DataDefCLASS *cdd,
				      FuncDef *ctor,
				      const std::vector<node_t> &explicit_nodes,
				      TokenBase *origin)
{
	if (!this_addr || !cdd || !ctor) return NULL;
	std::string sym = ctor_call_symbol(cdd, ctor);

	// An externally-bound (emit_symbol) ctor is declared with a void* this
	// (need_output_extern below) — cast the receiver to match, exactly like
	// emit_symbol_method_call and the external-dtor paths. An uncast
	// struct* arg against the void* extern decl trips c2mir's pointer
	// check (the iostream:80 __ioinit warning).
	bool external_ctor = !ctor->emit_symbol.empty();
	node_t args = list();
	append(args, external_ctor
		     ? node2(N_CAST, void_ptr_type(), this_addr, origin)
		     : this_addr);
	for (node_t en : explicit_nodes)
		append(args, en);
	for (size_t pi = explicit_nodes.size() + 1;
	     pi < ctor->parameters.size() && pi < ctor->param_defaults.size()
	     && ctor->param_defaults[pi]; pi++) {
		TokenBase *darg = ctor->param_defaults[pi];
		DataDef *pt = ctor->parameters[pi];
		bool is_ref_param = ctor->is_ref_param(pi);
		if (DataDefCLASS *pc = param_object_class(pt, is_ref_param))
			append(args, object_arg_addr(darg, pc));
		else if (DataDefCLASS *vc = as_class_instance(pt))
			append(args, object_arg_value(darg, vc));
		else if (is_ref_param)
			append(args, ref_param_arg_addr(darg, ref_param_referent(pt),
							const_ref_param(ctor, pi)));
		else
			append(args, translate_expr(darg));
	}
	if (ctor->ctor_trailing_self)
		append(args, external_ctor
			     ? node2(N_CAST, void_ptr_type(), this_addr, origin)
			     : this_addr);
	if (external_ctor) {
		std::vector<ExternParam> eparams;
		eparams.push_back({ {N_VOID}, true });
		for (size_t pi = 1; pi < ctor->parameters.size(); pi++) {
			DataDef *pt = ctor->parameters[pi];
			bool refp = ctor->is_ref_param(pi);
			if (param_object_class(pt, refp) || refp)
				eparams.push_back({ {N_VOID}, true });
			else if (pt && pt->is_pointer())
				eparams.push_back({ {N_CHAR}, true });
			else
				eparams.push_back({ {N_LONG}, false });
		}
		if (ctor->ctor_trailing_self)
			eparams.push_back({ {N_VOID}, true });
		need_output_extern(sym.c_str(), false, eparams);
	}
	referenced_funcs.insert(sym);
	node_t call = node2(N_CALL, id(sym.c_str(), origin), args, origin);
	return node2(N_EXPR, list(), call, origin);
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
		std::string vname = class_vtable_symbol(cdd);
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
		// IMPLICIT COPY CONSTRUCTOR: `T c = <T value>` with no matching
		// ctor (see try_implicit_copy_construct).
		if (node_t cc = try_implicit_copy_construct(
				id(v->name.c_str(), origin), cdd, ctor_args,
				origin))
			return cc;
		if (cdd->ctors.empty() || ctor_args.empty()) {
			auto it = cdd->method_map.find(cdd->name);
			if (it != cdd->method_map.end() && it->second)
				ctor = dynamic_cast<FuncDef *>(it->second->type);
		}
	}
	if (!ctor)
		return no_ctor_match_error(cdd, ctor_args, origin);

	std::string sym = ctor_call_symbol(cdd, ctor);

	// Externally-bound ctor: declared below with a void* this — cast the
	// receiver to match (emit_symbol_method_call convention; an uncast
	// struct* arg against the void* extern trips c2mir's pointer check —
	// the iostream:80 __ioinit warning).
	bool external_ctor = ctor && !ctor->emit_symbol.empty();
	node_t args = list();
	node_t self_addr = node1(N_ADDR, id(v->name.c_str(), origin), origin);
	append(args, external_ctor
		     ? node2(N_CAST, void_ptr_type(), self_addr, origin)
		     : self_addr); // &v
	for (size_t i = 0; i < ctor_args.size(); i++) {
		TokenBase *arg = ctor_args[i];
		size_t pi = i + 1;   // skip __this
		DataDef *pt = (ctor && pi < ctor->parameters.size())
				? ctor->parameters[pi] : NULL;
		bool is_ref_param = ctor && ctor->is_ref_param(pi);
		if (DataDefCLASS *pc = param_object_class(pt, is_ref_param)) {
			append(args, object_arg_addr(arg, pc));
		} else if (DataDefCLASS *vc = as_class_instance(pt))
			append(args, object_arg_value(arg, vc));
		else if (is_ref_param)
			append(args, ref_param_arg_addr(arg, ref_param_referent(pt),
							const_ref_param(ctor, pi)));
		else
			// Derived->base pointer argument (`B*` arg -> `A*` parameter):
			// make the implicit upcast explicit so c2mir does not warn
			// (mirrors the general call path's argument coercion).
			append(args, upcast_class_ptr(translate_expr(arg), pt, arg, arg));
	}
	// C++ default arguments: fill omitted trailing parameters from their default
	// expressions (param_defaults is index-aligned with parameters). Same arg
	// shaping as the user args above.
	for (size_t pi = ctor_args.size() + 1;
	     ctor && pi < ctor->parameters.size() && pi < ctor->param_defaults.size()
	     && ctor->param_defaults[pi]; pi++) {
		TokenBase *darg = ctor->param_defaults[pi];
		DataDef *pt = ctor->parameters[pi];
		bool is_ref_param = ctor->is_ref_param(pi);
		if (DataDefCLASS *pc = param_object_class(pt, is_ref_param))
			append(args, object_arg_addr(darg, pc));
		else if (DataDefCLASS *vc = as_class_instance(pt))
			append(args, object_arg_value(darg, vc));
		else if (is_ref_param)
			append(args, ref_param_arg_addr(darg, ref_param_referent(pt),
							const_ref_param(ctor, pi)));
		else
			append(args, translate_expr(darg));
	}
		// Some external constructors have a trailing reference parameter for which
		// the parsed declaration supplied no call-site value. See
		// FuncDef::ctor_trailing_self.
	if (ctor && ctor->ctor_trailing_self) {
		node_t self2 = node1(N_ADDR, id(v->name.c_str(), origin), origin);
		append(args, external_ctor
			     ? node2(N_CAST, void_ptr_type(), self2, origin)
			     : self2);
	}
		// A ctor bound to an EXTERNAL symbol MUST be declared with a typed
		// prototype, exactly like emit_symbol_method_call does for methods. Without
		// it c2mir/MIR marshals the call as an implicit function and shifts the
		// arguments. User-class ctors are defined in-module (emit_symbol empty) ->
		// no extern. Build the param shapes in lockstep with the args appended
		// above: this(void*) + each ctor param + the optional trailing self pointer.
	if (external_ctor) {
		std::vector<ExternParam> eparams;
		eparams.push_back({ {N_VOID}, true });   // this
		for (size_t pi = 1; pi < ctor->parameters.size(); pi++) {
			DataDef *pt = ctor->parameters[pi];
			bool refp = ctor->is_ref_param(pi);
			if (param_object_class(pt, refp) || refp)
				eparams.push_back({ {N_VOID}, true });
			else if (pt && pt->is_pointer())
				eparams.push_back({ {N_CHAR}, true });
			else
				eparams.push_back({ {N_LONG}, false });
		}
		if (ctor->ctor_trailing_self)
			eparams.push_back({ {N_VOID}, true });
		need_output_extern(sym.c_str(), false, eparams);
	}
	referenced_funcs.insert(sym);
	node_t call = node2(N_CALL, id(sym.c_str(), origin), args, origin);
	node_t ctor_stmt = node2(N_EXPR, list(), call, origin);

	// Complete-object construction: build the (transitive, deduped) virtual bases
	// FIRST (base-most order), then run the ctor. The ctor itself constructs only
	// non-virtual bases (it skips vbases), so this is the single place each shared
	// vbase is constructed. No vbases -> just the ctor call (byte-identical).
	//
	// EXCEPTION: an externally-defined class bound to its real Itanium C1 ctor
	// (emit_symbol set, e.g. std::basic_ofstream) — that complete-object ctor lives
	// in libstdc++ and constructs the ENTIRE object, virtual bases and vptrs
	// included. madc must not also construct the vbases here (it would call the
	// vbase's ctor with the wrong overload — the basic_ios "too few arguments").
	std::vector<DataDefCLASS *> vbs; std::set<DataDefCLASS *> vseen;
	cdd->collect_vbases(vbs, vseen);
	if (vbs.empty() || !ctor->emit_symbol.empty())
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
			node1(N_LIST, class_tag_ref(vb)),
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
	case TokenID::tk3Eq:    return "===";
	case TokenID::tk3NotEq: return "!==";
	case TokenID::tkLT:     return "<";
	case TokenID::tkGT:     return ">";
	case TokenID::tkLE:     return "<=";
	case TokenID::tkGE:     return ">=";
	case TokenID::tk3Way:   return "<=>";
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
// single explicit parameter (param 1; param 0 = __this) best matches the RHS
// using the shared argument-to-parameter scorer. Falls back to the first by-name
// match only when the argument shape is unknown. NULL when none is found.
FuncDef *CirBuilder::select_operator_overload(DataDefCLASS *cls,
					      const std::string &mname,
					      TokenBase *rhs)
{
	auto overload_arg_datadef = [this](TokenBase *arg) -> DataDef * {
		if (!arg) return NULL;
		if (TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(arg)) {
			DataDefCLASS *ccls = class_behind(tsub->object.type);
			std::string opname = "operator[]";
			Variable *omv = ccls ? ccls->findMethod(opname) : NULL;
			FuncDef *ofd = omv ? dynamic_cast<FuncDef *>(omv->type) : NULL;
			if (ofd && ofd->returns_reference()) {
				DataDef *rd = &ofd->return_value_type();
				if (rd && rd->is_pointer()) {
					DataDefPTR *rp = dynamic_cast<DataDefPTR *>(rd);
					if (rp && rp->base_type)
						return rp->base_type;
				}
				return rd;
			}
		}
		if (TokenVar *tv = dynamic_cast<TokenVar *>(arg)) {
			if ((tv->var.is_reference()) && tv->var.type) {
				if (DataDefCLASS *rc = class_behind(tv->var.type))
					return rc;
				// A SCALAR reference (`int &r`): a reference IS its referent for
				// overload resolution ([over.match]), so score by the referent
				// type — not the reference's pointer repr (DataDefREF == `int *`),
				// which would match operator<<(const void*) and print `cout << r`
				// as a pointer (`0x5`) with an int-to-pointer warning.
				if (DataDef *referent = ref_param_referent(tv->var.type))
					return referent;
			}
			if (tv->var.is_fixed_array() && tv->var.type && m_prog)
				return m_prog->getPointerType(tv->var.type);
		}
		if (TokenMember *tm = dynamic_cast<TokenMember *>(arg)) {
			if (tm->is_fixed_array_member() && tm->var.type && m_prog)
				return m_prog->getPointerType(tm->var.type);
		}
		if (DataDefCArray *ca = dynamic_cast<DataDefCArray *>(arg->datadef())) {
			if (ca->element_type && m_prog)
				return m_prog->getPointerType(ca->element_type);
		}
		return arg->datadef();
	};
	DataDef *rhs_dd = overload_arg_datadef(rhs);

	// Scan the methods vector for same-name overloads.
	FuncDef *first = NULL;
	FuncDef *best = NULL;
	Variable *best_var = NULL;
	int best_score = -1;
	for (Variable *mv : cls->methods) {
		if (!mv || mv->name != mname) continue;
		FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
		if (!fd) continue;
		if (!first) first = fd;
		if (fd->parameters.size() <= 1) continue;
		DataDef *p1dd = (fd->parameters.size() > 1)
			      ? fd->parameters[1] : NULL;
		bool refp = fd->is_ref_param(1);
		int score = score_arg_to_param(rhs_dd, p1dd, refp);
		if (score >= 0 && score > best_score) {
			best_score = score;
			best = fd;
			best_var = mv;
		}
	}
	if (best) {
		if (best_var && best->local_emit_name.empty()
		    && best_var->name != mname)
			best->local_emit_name = best_var->name;
		return best;
	}
	if (first && !rhs_dd) return first;
	// User-class operators carry the MANGLED name (ClassName__operatorX) in the
	// methods vector, so the unmangled-name scan above misses them. Scan the
	// mangled family and prefer the BINARY (parameterized, params > 1) overload
	// — this is the binary dispatch. A same-name unary peer (params == 1, the
	// `_un`-suffixed nullary) is skipped here; select_unary_operator_overload
	// owns it. (P2.1b gaps 3 & 4.)
	std::string mangled_canon = cls->name + "__" + mname;
	std::string mangled_un = mangled_canon + "_un";
	std::string mangled_overload_prefix = mangled_canon + "__o";
	FuncDef *any = NULL;
	best = NULL;
	best_var = NULL;
	best_score = -1;
	for (Variable *mv : cls->methods) {
		if (!mv || (mv->name != mangled_canon
		    && mv->name != mangled_un
		    && mv->name.compare(0, mangled_overload_prefix.size(),
					mangled_overload_prefix) != 0))
			continue;
		FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
		if (!fd) continue;
		if (!any) any = fd;
		if (fd->parameters.size() <= 1) continue;   // unary
		DataDef *p1dd = fd->parameters[1];
		bool refp = fd->is_ref_param(1);
		int score = score_arg_to_param(rhs_dd, p1dd, refp);
		if (score >= 0 && score > best_score) {
			best_score = score;
			best = fd;
			best_var = mv;
		}
	}
	if (best) {
		if (best_var && best->local_emit_name.empty()
		    && best_var->name != mname)
			best->local_emit_name = best_var->name;
		return best;
	}
	if (any && !rhs_dd) return any;
	if (rhs_dd)
		return NULL;
	// Fall back to the keyed lookup (method_map under the unmangled name) only
	// when the RHS shape is unknown; a known RHS with no scored match is not a
	// viable member candidate.
	std::string key = mname;
	Variable *mv = cls->findMethod(key);
	return mv ? dynamic_cast<FuncDef *>(mv->type) : NULL;
}

// --- W2: non-member operator overload resolution helpers (file-local) ---
namespace {

// Strip a leading "std::" (or any leading "ns::") qualifier — for comparing the
// PRIMARY template name of a captured (namespace-relative) spelling against an
// lhs class's fully-qualified canonical spelling.
std::string strip_leading_qualifier(const std::string &s)
{
	size_t pos = s.rfind("::");
	return pos == std::string::npos ? s : s.substr(pos + 2);
}

// Strip leading const/volatile words and trailing &/&&/whitespace.
std::string strip_cv_ref_w2(const std::string &in, bool *was_ref = NULL)
{
	std::string s = in;
	// trailing reference
	while (!s.empty() && (s.back() == '&' || s.back() == ' '))
	{
		if (s.back() == '&' && was_ref) *was_ref = true;
		s.pop_back();
	}
	// leading specifiers
	for (;;)
	{
		size_t i = 0;
		while (i < s.size() && s[i] == ' ') ++i;
		static const char *cv[] = { "const", "volatile" };
		bool stripped = false;
		for (const char *w : cv)
		{
			size_t n = strlen(w);
			if (s.compare(i, n, w) == 0
			    && (i + n >= s.size() || s[i + n] == ' '))
			{
				s.erase(0, i + n);
				stripped = true;
				break;
			}
		}
		if (!stripped) break;
	}
	// trim
	size_t a = s.find_first_not_of(' ');
	size_t b = s.find_last_not_of(' ');
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Normalized form for an EXACT type-match comparison (param vs argument):
// drop cv/ref and all whitespace. "const char*" and "char*" both -> "char*".
std::string norm_type_w2(const std::string &s)
{
	std::string t = strip_cv_ref_w2(s);
	std::string out;
	for (char c : t) if (c != ' ') out += c;
	return out;
}

static std::string strip_ref_ws_only(const std::string &in)
{
	std::string s = in;
	while (!s.empty() && (s.back() == '&' || s.back() == ' '))
		s.pop_back();
	size_t a = s.find_first_not_of(' ');
	size_t b = s.find_last_not_of(' ');
	return a == std::string::npos ? std::string() : s.substr(a, b - a + 1);
}

// Split "head<a,b,c>" into head + top-level args (whitespace-trimmed). Returns
// false if there is no top-level template-id. Nested <> are preserved in args.
bool split_template_id_w2(const std::string &in, std::string &head,
			  std::vector<std::string> &args)
{
	std::string s = strip_cv_ref_w2(in);
	size_t lt = std::string::npos;
	int depth = 0;
	for (size_t i = 0; i < s.size(); ++i)
	{
		if (s[i] == '<') { if (depth == 0) { lt = i; } ++depth; break; }
	}
	if (lt == std::string::npos) { head = s; return false; }
	head = s.substr(0, lt);
	// trim head
	size_t hb = head.find_last_not_of(' ');
	if (hb != std::string::npos) head = head.substr(0, hb + 1);
	depth = 0;
	size_t start = lt + 1;
	for (size_t i = lt; i < s.size(); ++i)
	{
		char c = s[i];
		if (c == '<') ++depth;
		else if (c == '>')
		{
			if (--depth == 0)
			{
				std::string a = s.substr(start, i - start);
				size_t x = a.find_first_not_of(' ');
				size_t y = a.find_last_not_of(' ');
				if (x != std::string::npos) args.push_back(a.substr(x, y - x + 1));
				break;
			}
		}
		else if (c == ',' && depth == 1)
		{
			std::string a = s.substr(start, i - start);
			size_t x = a.find_first_not_of(' ');
			size_t y = a.find_last_not_of(' ');
			if (x != std::string::npos) args.push_back(a.substr(x, y - x + 1));
			start = i + 1;
		}
	}
	return true;
}

// A minimal C++ type spelling for an argument's DataDef — enough to compare
// (normalized) against a free operator's parameter spelling. Class -> canonical
// spelling; pointer -> "<pointee>*"; otherwise the DataDef's own name.
std::string datadef_cpp_spelling_w2(DataDef *dd)
{
	if (!dd) return "";
	if (DataDefCLASS *c = dynamic_cast<DataDefCLASS *>(dd))
		return c->canonical_cpp_spelling.empty() ? c->name
						         : c->canonical_cpp_spelling;
	if (DataDefPTR *p = dynamic_cast<DataDefPTR *>(dd))
		return datadef_cpp_spelling_w2(p->base_type) + "*";
	return dd->name;
}

// Deduce a free std stream function's template-param bindings from the lhs
// class and build the $Tn-parameterized, namespace-qualified stream type (what
// itanium_mangle_std_free_template wants for the return / stream parameter).
// ov.param_spellings[0] is the stream-parameter pattern (e.g.
// "basic_ostream<char,_Traits>&"); match it against the lhs class's canonical
// template-id, binding each template-param position to the lhs's concrete arg,
// requiring concrete (non-template-param) positions to match. Returns false if
// it does not deduce-match. The binding may be PARTIAL — params that appear
// only in later parameters (operator<<'s _Alloc appears only in the rhs
// basic_string) are deduced by the caller; completeness is checked there via
// targs_from_binding. Shared by the binary-operator and manipulator paths.
bool deduce_free_stream_call(const std::string &lhs_spelling,
		const Program::FreeOperatorOverload &ov,
		std::map<std::string, std::string> &binding, std::string &stream_type)
{
	std::string lhs_head; std::vector<std::string> lhs_args;
	if (!split_template_id_w2(lhs_spelling, lhs_head, lhs_args)) return false;
	std::string p0head; std::vector<std::string> p0args;
	if (ov.param_spellings.empty()
	    || !split_template_id_w2(ov.param_spellings[0], p0head, p0args)) return false;
	if (strip_leading_qualifier(p0head) != strip_leading_qualifier(lhs_head)) return false;
	if (p0args.size() != lhs_args.size()) return false;
	for (size_t i = 0; i < p0args.size(); ++i) {
		bool is_tp = false;
		for (const std::string &tp : ov.template_params)
			if (tp == p0args[i]) { is_tp = true; break; }
		if (is_tp) binding[p0args[i]] = lhs_args[i];
		else if (norm_type_w2(p0args[i]) != norm_type_w2(lhs_args[i])) return false;
	}
	std::string out = (p0head.find("::") != std::string::npos
			   ? p0head : ov.ns + "::" + p0head) + "<";
	for (size_t i = 0; i < p0args.size(); ++i) {
		if (i) out += ",";
		int tpi = -1;
		for (size_t k = 0; k < ov.template_params.size(); ++k)
			if (ov.template_params[k] == p0args[i]) { tpi = (int)k; break; }
		out += (tpi >= 0) ? ("$T" + std::to_string(tpi)) : p0args[i];
	}
	out += ">&";
	stream_type = out;
	return true;
}

// A class-or-base candidate for free-function template deduction: the canonical
// spelling, the byte offset of the (base) subobject within the most-derived
// object, and the class itself (so an instantiated FuncDef can carry the
// matched base class as its parameter type).
struct BaseCand {
	std::string spelling;
	size_t off;
	DataDefCLASS *cls;
};

// Collect candidates for a class AND its non-virtual bases, transitively,
// most-derived first. Lets a free operator whose stream parameter is a BASE
// (operator<<(ostream&,...)) bind when called on a DERIVED stream (ofstream):
// we match the base spelling and adjust the passed `this` to the base
// subobject. Virtual bases are skipped (their offset is vtable-relative, and
// no std free operator takes a virtual-base stream by ref).
static void collect_self_and_base_spellings(DataDefCLASS *cls, size_t base_off,
		std::vector<BaseCand> &out)
{
	if (!cls) return;
	std::string sp = cls->canonical_cpp_spelling.empty() ? cls->name
		       : cls->canonical_cpp_spelling;
	for (const auto &e : out) if (e.spelling == sp) return;   // dedup
	out.push_back(BaseCand{sp, base_off, cls});
	for (const BaseSpec &b : cls->bases)
		if (b.base && !b.is_virtual)
			collect_self_and_base_spellings(b.base, base_off + b.offset, out);
	if (cls->base_class)   // single-inheritance primary base (offset 0)
		collect_self_and_base_spellings(cls->base_class, base_off, out);
}

// Build the template-arg list (in template-param order) from a binding map;
// false when a template param was never bound (deduction incomplete).
static bool targs_from_binding(const Program::FreeOperatorOverload &ov,
		const std::map<std::string, std::string> &binding,
		std::vector<std::string> &targs)
{
	targs.clear();
	for (const std::string &tp : ov.template_params) {
		auto it = binding.find(tp);
		if (it == binding.end()) return false;
		targs.push_back(it->second);
	}
	return true;
}

static bool w2_datadef_involves_template_param(DataDef *dd)
{
	for (int guard = 0; dd && guard < 16; ++guard) {
		if (dd->is_template_param())
			return true;
		if (DataDefCONST *cd = dynamic_cast<DataDefCONST *>(dd)) {
			dd = cd->base_type;
			continue;
		}
		if (DataDefPTR *pd = dynamic_cast<DataDefPTR *>(dd)) {
			dd = pd->base_type;
			continue;
		}
		break;
	}
	return false;
}

static bool w2_is_lvalue_ref_spelling(const std::string &p)
{
	return p.size() >= 2 && p.back() == '&' && p[p.size() - 2] != '&';
}

static bool w2_is_scalar_return_datadef(DataDef *dd)
{
	if (!dd || dd->is_reference())
		return false;
	if (as_class_instance(dd) || dd->is_struct())
		return false;
	return true;
}

// Deduce-match ONE template-id parameter spelling against an argument class
// (its self or a non-virtual base): bind each template-param position to the
// class's concrete arg, require concrete positions to match, and keep the
// bindings consistent with any made by earlier parameters. Reports the matched
// base-subobject byte offset (pass the arg adjusted to that base) and the
// matched class's fully-qualified head (for requalify_head — the captured
// spelling is unqualified since it lives inside `namespace std`). Shared by
// the named-free-fn (getline) and free-operator (cout<<string) paths.
static bool deduce_param_against_class(const std::string &pspell,
		DataDefCLASS *acls, const std::vector<std::string> &tparams,
		std::map<std::string, std::string> &binding,
		size_t &off, std::string &qhead, DataDefCLASS **matched_cls = NULL)
{
	if (!acls) return false;
	std::string phead; std::vector<std::string> pargs;
	if (!split_template_id_w2(pspell, phead, pargs)) return false;
	std::vector<BaseCand> cands;
	collect_self_and_base_spellings(acls, 0, cands);
	for (const auto &cand : cands) {
		std::string chead; std::vector<std::string> cargs;
		if (!split_template_id_w2(cand.spelling, chead, cargs)) continue;
		if (strip_leading_qualifier(chead) != strip_leading_qualifier(phead))
			continue;
		if (cargs.size() != pargs.size()) continue;
		std::map<std::string, std::string> local = binding;
		bool good = true;
		for (size_t k = 0; k < pargs.size(); k++) {
			bool is_tp = false;
			for (const std::string &tp : tparams)
				if (tp == pargs[k]) { is_tp = true; break; }
			if (is_tp) {
				auto it = local.find(pargs[k]);
				if (it != local.end()
				    && norm_type_w2(it->second) != norm_type_w2(cargs[k]))
					{ good = false; break; }
				local[pargs[k]] = cargs[k];
			} else if (norm_type_w2(pargs[k]) != norm_type_w2(cargs[k]))
				{ good = false; break; }
		}
		if (good) {
			binding = local;
			off = cand.off;
			qhead = chead;
			if (matched_cls) *matched_cls = cand.cls;
			return true;
		}
	}
	return false;
}

static std::string template_placeholder_spelling(
	const std::string &in, const std::vector<std::string> &params)
{
	std::string out;
	for (size_t i = 0; i < in.size(); ) {
		unsigned char c = (unsigned char)in[i];
		if (isalpha(c) || in[i] == '_') {
			size_t j = i + 1;
			while (j < in.size()) {
				unsigned char d = (unsigned char)in[j];
				if (!isalnum(d) && in[j] != '_') break;
				++j;
			}
			std::string ident = in.substr(i, j - i);
			int pidx = -1;
			for (size_t k = 0; k < params.size(); ++k)
				if (params[k] == ident) { pidx = (int)k; break; }
			if (pidx >= 0)
				out += "$T" + std::to_string(pidx);
			else
				out += ident;
			i = j;
			continue;
		}
		out += in[i++];
	}
	return out;
}

static bool bind_member_template_param(const std::string &pattern,
	const std::string &actual, const std::vector<std::string> &params,
	std::map<std::string, std::string> &binding)
{
	std::string p = strip_cv_ref_w2(pattern);
	std::string a = strip_ref_ws_only(actual);
	for (const std::string &tp : params) {
		if (p == tp) {
			std::map<std::string, std::string>::iterator it = binding.find(tp);
			if (it == binding.end()) {
				binding[tp] = a;
				return true;
			}
			return norm_type_w2(it->second) == norm_type_w2(a);
		}
		std::string ptr_pat = tp + "*";
		if (norm_type_w2(p) == norm_type_w2(ptr_pat) && !a.empty()
		    && a[a.size() - 1] == '*') {
			std::string base = a.substr(0, a.size() - 1);
			std::map<std::string, std::string>::iterator it = binding.find(tp);
			if (it == binding.end()) {
				binding[tp] = base;
				return true;
			}
			return norm_type_w2(it->second) == norm_type_w2(base);
		}
	}
	if (template_placeholder_spelling(p, params) != p)
		return true;
	return norm_type_w2(p) == norm_type_w2(a);
}

} // namespace

node_t CirBuilder::member_template_method_call(TokenMember *tm, FuncDef *callee,
					       node_t this_arg,
					       TokenBase *origin)
{
	if (!tm || !callee || !callee->is_member_template
	    || !callee->declaration_only || !callee->is_varargs)
		return NULL;
	DataDefCLASS *owner = !callee->parameters.empty()
			    ? class_behind(callee->parameters[0]) : NULL;
	if (!owner || owner->canonical_cpp_spelling.empty())
		return NULL;
	if (!owner->is_externally_defined() && !owner->is_extern_template_instantiated)
		return NULL;
	if (callee->template_param_names.empty()
	    || callee->template_return_spelling.empty()
	    || callee->template_param_spellings.size() != tm->parameters.size())
		return NULL;

	auto arg_spelling = [&](TokenBase *arg) -> std::string {
		if (TokenVar *tv = dynamic_cast<TokenVar *>(arg)) {
			if (m_cur_method) {
				FuncDef *curfd = dynamic_cast<FuncDef *>(m_cur_method->returns.type);
				for (size_t i = 0; curfd && i < m_cur_method->parameters.size(); ++i)
					if ((m_cur_method->parameters[i] == &tv->var
					    || (m_cur_method->parameters[i]
						&& m_cur_method->parameters[i]->name
						   == tv->var.name))
					    && i < curfd->param_cpp_spellings.size()
					    && !curfd->param_cpp_spellings[i].empty())
						return curfd->param_cpp_spellings[i];
			}
		}
		return datadef_cpp_spelling_w2(arg ? arg->datadef() : NULL);
	};

	std::map<std::string, std::string> binding;
	for (size_t i = 0; i < tm->parameters.size(); ++i) {
		std::string actual = arg_spelling(tm->parameters[i]);
		if (!bind_member_template_param(callee->template_param_spellings[i],
						actual, callee->template_param_names,
						binding))
			return NULL;
	}
	std::vector<std::string> targs;
	for (const std::string &tp : callee->template_param_names) {
		std::map<std::string, std::string>::iterator it = binding.find(tp);
		if (it == binding.end())
			return NULL;
		targs.push_back(it->second);
	}
	std::vector<std::string> params;
	for (const std::string &p : callee->template_param_spellings)
		params.push_back(template_placeholder_spelling(
			p, callee->template_param_names));
	std::string ret = template_placeholder_spelling(
		callee->template_return_spelling, callee->template_param_names);
	const std::string &mname = callee->method_display_name.empty()
				 ? tm->var.name : callee->method_display_name;
	std::string sym = itanium_mangle_member_template_sub(
		owner->canonical_cpp_spelling, mname, targs, ret, params,
		callee->is_const_method);
	if (sym.empty() || sym[0] != '_')
		return NULL;

	node_t args = list();
	append(args, this_arg);
	for (TokenBase *arg : tm->parameters) {
		DataDefCLASS *vc = as_class_instance(arg ? arg->datadef() : NULL);
		if (vc)
			append(args, object_arg_value(arg, vc));
		else
			append(args, translate_expr(arg));
	}
	bool ret_ptr = false;
	std::vector<c2mir_node_code_t> ret_specs =
		emit_symbol_ret_specs(callee, ret_ptr);
	need_output_extern_unprototyped(sym.c_str(), ret_ptr, ret_specs);
	node_t call = node2(N_CALL, id(sym.c_str(), origin), args, origin);
	CIR_NODE(call)->synth_from_origin = true;
	if (callee->returns_reference())
		return node1(N_DEREF, call, origin);
	return call;
}

static std::string substitute_tparams(const std::string &spell,
		const std::vector<std::string> &tparams);
static std::string requalify_head(const std::string &spell, const std::string &qhead);

// Pattern A for free namespace OPERATORS (W2 step D — emit-symbol
// unification): ONE scan over the captured free operators covering both
// call shapes, producing an instantiated FuncDef whose emit_symbol carries
// the mangled Itanium symbol. The reference-returning stream shape
// (operator<<(basic_ostream&, x) -> basic_ostream&) and the by-value
// class-return shape (operator+(const basic_string&, const basic_string&)
// -> basic_string; libstdc++ exports the char instantiations weak) bind
// mangled-direct; rvalue-ref (&&) overloads never do (inline-only).
// Member arbitration: an exact-match member vetoes a stream-shaped free
// candidate (it would win or tie under normal overload rules); a by-value
// free candidate binds only when the class declares NO matching member.
// Stream-shaped candidates take precedence over by-value ones (they bind
// the operator itself returning the lhs stream). Memoized per operator
// token and per symbol — the emission (class_operator_external_call) and
// every other consumer see one FuncDef per symbol.
// Replace whole-identifier occurrences of `name` with `val` in a type
// spelling — substitutes a DEDUCED template binding back into a parameter
// spelling (`const _CharT*` + {_CharT->char} -> `const char*`).
static std::string subst_bound_ident(const std::string &spell,
		const std::string &name, const std::string &val)
{
	std::string out;
	size_t i = 0, n = spell.size();
	while (i < n) {
		char c = spell[i];
		if (isalpha((unsigned char)c) || c == '_') {
			size_t j = i + 1;
			while (j < n && (isalnum((unsigned char)spell[j]) || spell[j] == '_'))
				++j;
			std::string w = spell.substr(i, j - i);
			out += (w == name) ? val : w;
			i = j;
			continue;
		}
		out += c;
		++i;
	}
	return out;
}

FuncDef *CirBuilder::std_free_operator_instantiation(TokenOperator *top,
		DataDefCLASS *lcls, const std::string &mname, FuncDef *member_callee)
{
	if (!m_prog || !top || !top->left || !top->right) return NULL;
	// lcls may be NULL for the literal-lhs mixed shape (`"pre" + s`) —
	// then the rhs must be the class operand (Pass 2b below).
	if (!lcls && !operand_object_class(top->right)) return NULL;
	if (m_prog->free_operator_overloads.empty()) return NULL;
	auto mit = m_free_op_inst_by_call.find(top);
	if (mit != m_free_op_inst_by_call.end()) return mit->second;
	m_free_op_inst_by_call[top] = NULL;   // negative-cache; success overwrites
	m_free_op_body_by_call[top] = NULL;

	// lhs class candidates: self AND non-virtual bases, most-derived first
	// (a free operator taking a BASE stream binds a derived lhs).
	std::vector<BaseCand> lhs_cands;
	if (lcls)
		collect_self_and_base_spellings(lcls, 0, lhs_cands);
	auto deduce_lhs = [&](const Program::FreeOperatorOverload &ov,
			      std::map<std::string, std::string> &binding,
			      std::string &stype, DataDefCLASS **cls) -> bool {
		for (const auto &cand : lhs_cands) {
			binding.clear();
			if (deduce_free_stream_call(cand.spelling, ov, binding, stype)) {
				if (cls) *cls = cand.cls;
				return true;
			}
		}
		return false;
	};

	// rhs argument type spelling (for matching the candidate's 2nd param).
	// A reference rhs denotes the referenced class — without the unwrap the
	// spelling is the pointer representation and no candidate ever matches
	// (`cout << s` with a `const string &s` parameter bound the bogus
	// member streambuf* overload).
	DataDef *rhs_dd = top->right->datadef();
	if (!as_class_instance(rhs_dd))
		if (DataDefCLASS *rdc = operand_object_class(top->right))
			rhs_dd = rdc;
	std::string rhs_norm = norm_type_w2(datadef_cpp_spelling_w2(rhs_dd));

	// member candidate's 2nd-parameter spelling (param 0 is __this): an
	// exact-match member keeps the call (normal overload rules).
	std::string member_p1_norm;
	if (member_callee && member_callee->param_cpp_spellings.size() > 1)
		member_p1_norm = norm_type_w2(member_callee->param_cpp_spellings[1]);
	bool member_exact = !member_p1_norm.empty() && member_p1_norm == rhs_norm;

	const Program::FreeOperatorOverload *best = NULL;
	std::vector<std::string> best_targs;
	std::string best_ret_spell;              // mangler return ($Tn form)
	std::vector<std::string> best_params;    // mangler params ($Tn form)
	DataDefCLASS *best_lcls = NULL;          // matched lhs (base) class
	DataDefCLASS *best_rcls = NULL;          // matched rhs (base) class, or NULL
	DataDefCLASS *best_retc = NULL;          // return class
	bool best_ret_ref = false;
	Variable *best_body_var = NULL;          // local retained-template body

	// Pass 1 — the reference-returning stream shape. param[0] deduces
	// against the lhs class; param[1] matches the rhs exactly OR is a
	// by-reference template-id deducing against the rhs CLASS (the free
	// operator<<(ostream&, const basic_string<_C,_T,_A>&) — _Alloc is
	// deducible only from the rhs).
	for (const Program::FreeOperatorOverload &ov : m_prog->free_operator_overloads)
	{
		if (ov.opname != mname || ov.param_spellings.size() < 2) continue;
		bool ret_is_ref = !ov.return_spelling.empty()
				  && ov.return_spelling.back() == '&';
		if (!ret_is_ref) continue;
		std::map<std::string, std::string> binding;
		std::string stype;
		DataDefCLASS *c0 = NULL;
		if (!deduce_lhs(ov, binding, stype, &c0)) continue;
		bool rhs_deduced = false;
		DataDefCLASS *c1 = NULL;
		std::string rhs_param = ov.param_spellings[1];
		if (norm_type_w2(ov.param_spellings[1]) != rhs_norm)
		{
			const std::string &rspell = ov.param_spellings[1];
			// Only a by-reference class param can deduce here.
			bool rhs_is_ref = !rspell.empty() && rspell.back() == '&';
			if (!rhs_is_ref) continue;
			size_t rhs_off = 0;
			std::string rhs_qhead;
			if (!deduce_param_against_class(rspell, as_class_instance(rhs_dd),
							ov.template_params, binding,
							rhs_off, rhs_qhead, &c1))
				continue;
			rhs_deduced = true;
			rhs_param = substitute_tparams(
				requalify_head(rspell, rhs_qhead), ov.template_params);
		}
		std::vector<std::string> targs;
		if (!targs_from_binding(ov, binding, targs)) continue;
		// Most-specialized = fewest template params (the char partial
		// spec beats the general template -> the symbol g++ calls).
		if (best && ov.template_params.size() >= best->template_params.size())
			continue;
		best = &ov;
		best_targs = targs;
		best_ret_spell = stype;              // stream ops return param[0]'s type
		best_params.clear();
		best_params.push_back(stype);
		best_params.push_back(rhs_param);
		best_lcls = c0;
		best_rcls = rhs_deduced ? (c1 ? c1 : as_class_instance(rhs_dd)) : NULL;
		best_retc = c0;                      // ...the (base) stream class itself
		best_ret_ref = true;
	}
	if (best && member_callee && member_exact)
		return NULL;   // negative-cached above

	// Pass 2 — the by-value class-return shape, only when no stream-shaped
	// candidate bound and the class declares NO matching member at all.
	if (!best && !member_callee && as_class_instance(rhs_dd))
	{
		DataDefCLASS *rcls = as_class_instance(rhs_dd);
		for (const Program::FreeOperatorOverload &ov : m_prog->free_operator_overloads)
		{
			if (ov.opname != mname || ov.param_spellings.size() != 2) continue;
			const std::string &rspell = ov.return_spelling;
			// by-value class return: a template-id, no trailing &/*.
			if (rspell.empty() || rspell.back() == '&' || rspell.back() == '*')
				continue;
			// Both params lvalue refs; && overloads never bind mangled-direct.
			auto is_lvalue_ref = [](const std::string &p) {
				return p.size() >= 2 && p.back() == '&'
				       && p[p.size() - 2] != '&';
			};
			if (!is_lvalue_ref(ov.param_spellings[0])
			    || !is_lvalue_ref(ov.param_spellings[1]))
				continue;
			std::map<std::string, std::string> binding;
			size_t loff = 0, roff = 0, retoff = 0;
			std::string qh0, qh1, qhr;
			DataDefCLASS *c0 = NULL, *c1 = NULL, *cr = NULL;
			if (!deduce_param_against_class(ov.param_spellings[0], lcls,
					ov.template_params, binding, loff, qh0, &c0))
				continue;
			if (!deduce_param_against_class(ov.param_spellings[1], rcls,
					ov.template_params, binding, roff, qh1, &c1))
				continue;
			// The return class: the return template-id must deduce against
			// one of the operand classes WITHOUT extending or changing the
			// binding (operator+ returns the operand string class itself).
			std::map<std::string, std::string> b2 = binding;
			if (!(deduce_param_against_class(rspell, c0 ? c0 : lcls,
					ov.template_params, b2, retoff, qhr, &cr)
			      && b2 == binding && retoff == 0)) {
				b2 = binding; retoff = 0; qhr.clear(); cr = NULL;
				if (!(deduce_param_against_class(rspell, c1 ? c1 : rcls,
						ov.template_params, b2, retoff, qhr, &cr)
				      && b2 == binding && retoff == 0))
					continue;
			}
			// Only a NON-TRIVIAL class result fits the sret/__retbuf shape;
			// a trivial by-value class returns in registers (native paths).
			if (!cr || !class_return_via_retbuf(cr)) continue;
			std::vector<std::string> targs;
			if (!targs_from_binding(ov, binding, targs)) continue;
			if (best && ov.template_params.size() >= best->template_params.size())
				continue;
			auto qp = [&](const std::string &sp, const std::string &qh) {
				return substitute_tparams(requalify_head(sp, qh),
							  ov.template_params);
			};
			best = &ov;
			best_targs = targs;
			best_ret_spell = qp(rspell, qhr);
			best_params.clear();
			best_params.push_back(qp(ov.param_spellings[0], qh0));
			best_params.push_back(qp(ov.param_spellings[1], qh1));
			best_lcls = c0;
			best_rcls = c1 ? c1 : rcls;
			best_retc = cr;
			best_ret_ref = false;
		}
	}

	// Pass 2b — the literal-lhs mixed shape: `"pre" + s`. param[1] (the
	// class, by const-ref) deduces the binding FIRST; param[0] (a
	// non-class pointer/value, `const _CharT*`) must then SUBSTITUTE to
	// the lhs type exactly. Binds the exported
	// operator+(const char*, const string&) mangled-direct.
	if (!best && !lcls && !member_callee && as_class_instance(rhs_dd))
	{
		DataDefCLASS *rcls = as_class_instance(rhs_dd);
		DataDef *lhs_dd = top->left->datadef();
		std::string lhs_norm = lhs_dd
			? norm_type_w2(datadef_cpp_spelling_w2(lhs_dd)) : std::string();
		auto is_lvalue_ref = [](const std::string &p) {
			return p.size() >= 2 && p.back() == '&'
			       && p[p.size() - 2] != '&';
		};
		for (const Program::FreeOperatorOverload &ov : m_prog->free_operator_overloads)
		{
			if (ov.opname != mname || ov.param_spellings.size() != 2) continue;
			const std::string &rspell = ov.return_spelling;
			if (rspell.empty() || rspell.back() == '&' || rspell.back() == '*')
				continue;	// by-value class return shapes only
			if (is_lvalue_ref(ov.param_spellings[0]))
				continue;	// class-lhs shapes belong to Pass 2
			if (!is_lvalue_ref(ov.param_spellings[1]))
				continue;
			std::map<std::string, std::string> binding;
			size_t roff = 0, retoff = 0;
			std::string qh1, qhr;
			DataDefCLASS *c1 = NULL, *cr = NULL;
			if (!deduce_param_against_class(ov.param_spellings[1], rcls,
					ov.template_params, binding, roff, qh1, &c1))
				continue;
			std::string p0sub = ov.param_spellings[0];
			for (const auto &b : binding)
				p0sub = subst_bound_ident(p0sub, b.first, b.second);
			if (lhs_norm.empty() || norm_type_w2(p0sub) != lhs_norm)
				continue;
			std::map<std::string, std::string> b2 = binding;
			if (!(deduce_param_against_class(rspell, c1 ? c1 : rcls,
					ov.template_params, b2, retoff, qhr, &cr)
			      && b2 == binding && retoff == 0))
				continue;
			if (!cr || !class_return_via_retbuf(cr)) continue;
			std::vector<std::string> targs;
			if (!targs_from_binding(ov, binding, targs)) continue;
			if (best && ov.template_params.size() >= best->template_params.size())
				continue;
			auto qp = [&](const std::string &sp, const std::string &qh) {
				return substitute_tparams(requalify_head(sp, qh),
							  ov.template_params);
			};
			best = &ov;
			best_targs = targs;
			best_ret_spell = qp(rspell, qhr);
			best_params.clear();
			best_params.push_back(qp(ov.param_spellings[0], std::string()));
			best_params.push_back(qp(ov.param_spellings[1], qh1));
			best_lcls = NULL;
			best_rcls = c1 ? c1 : rcls;
			best_retc = cr;
			best_ret_ref = false;
		}
	}
	// Pass 3 — scalar-returning free operators on class operands. Iterator
	// arithmetic/comparison templates take class operands by lvalue reference
	// but return a scalar (`difference_type`, `bool`, ...), so they are neither
	// stream refs (Pass 1) nor by-value class returns (Pass 2). These inline
	// templates need a real local body, not a bodyless external import.
	if (!best && lcls && as_class_instance(rhs_dd))
	{
		DataDefCLASS *rcls = as_class_instance(rhs_dd);
		for (const Program::FreeOperatorOverload &ov : m_prog->free_operator_overloads)
		{
			if (ov.opname != mname || ov.param_spellings.size() != 2) continue;
			const std::string &rspell = ov.return_spelling;
			if (rspell.empty() || rspell.back() == '&')
				continue;
			if (!w2_is_lvalue_ref_spelling(ov.param_spellings[0])
			    || !w2_is_lvalue_ref_spelling(ov.param_spellings[1]))
				continue;
			std::map<std::string, std::string> binding;
			size_t loff = 0, roff = 0, retoff = 0;
			std::string qh0, qh1, qhr;
			DataDefCLASS *c0 = NULL, *c1 = NULL, *cr = NULL;
			if (!deduce_param_against_class(ov.param_spellings[0], lcls,
					ov.template_params, binding, loff, qh0, &c0))
				continue;
			if (!deduce_param_against_class(ov.param_spellings[1], rcls,
					ov.template_params, binding, roff, qh1, &c1))
				continue;
			std::vector<std::string> targs;
			if (!targs_from_binding(ov, binding, targs)) continue;
			if (best && ov.template_params.size() >= best->template_params.size())
				continue;
			std::map<std::string, std::string> b2 = binding;
			(void)deduce_param_against_class(rspell, c0 ? c0 : lcls,
					ov.template_params, b2, retoff, qhr, &cr);
			auto qp = [&](const std::string &sp, const std::string &qh) {
				return substitute_tparams(requalify_head(sp, qh),
							  ov.template_params);
			};
			best = &ov;
			best_targs = targs;
			best_ret_spell = qp(rspell, qhr);
			best_params.clear();
			best_params.push_back(qp(ov.param_spellings[0], qh0));
			best_params.push_back(qp(ov.param_spellings[1], qh1));
			best_lcls = c0;
			best_rcls = c1 ? c1 : rcls;
			best_retc = NULL;
			best_ret_ref = false;
		}
		if (best)
		{
			Variable *callee = NULL;
			DataDef *ret = m_prog->instantiate_free_operator_template(
				mname, top->left, top->right, &callee);
			if (!callee || !w2_is_scalar_return_datadef(ret))
				best = NULL;
			else
				best_body_var = callee;
		}
	}
	if (best && best_body_var)
	{
		FuncDef *inst = dynamic_cast<FuncDef *>(best_body_var->type);
		if (!inst)
			return NULL;
		DBG(std::cout << "[W2] instantiate free-body " << best->ns
		    << "::" << mname << " -> " << best_body_var->name
		    << std::endl);
		m_free_op_body_by_call[top] = best_body_var;
		m_free_op_inst_by_call[top] = inst;
		return inst;
	}
	if (!best || !best_retc) return NULL;

	std::string sym = itanium_mangle_std_free_template(
		best->opname.substr(strlen("operator")), best_targs,
		best_ret_spell, best_params);
	if (sym.empty() || sym[0] != '_') return NULL;
	auto sit = m_free_fn_inst_by_sym.find(sym);
	if (sit != m_free_fn_inst_by_sym.end()) {
		m_free_op_inst_by_call[top] = sit->second;
		return sit->second;
	}

	FuncDef *inst = new FuncDef(best_ret_ref ? *as_reference_type(best_retc)
						 : *(DataDef *)best_retc);
	inst->emit_symbol = sym;
	inst->declaration_only = true;
	inst->function_display_name = mname;
	inst->namespace_name = best->ns;
	if (best_lcls || lcls) {
		inst->parameters.push_back(as_reference_type(best_lcls ? (DataDef *)best_lcls
								    : (DataDef *)lcls));
	} else {
		// Literal-lhs (Pass 2b): param[0] is the non-class lhs type
		// itself (a pointer/value, never a reference).
		DataDef *lhs_dd0 = top->left->datadef();
		inst->parameters.push_back(lhs_dd0 ? lhs_dd0 : (DataDef *)&ddINT64);
	}
	bool rhs_is_ptr = !best->param_spellings[1].empty()
			  && best->param_spellings[1].back() == '*';
	if (best_rcls) {
		inst->parameters.push_back(as_reference_type(best_rcls));
	} else {
		bool rhs_ref = !rhs_is_ptr
			&& !best->param_spellings[1].empty()
			&& best->param_spellings[1].back() == '&';
		DataDef *rhs0 = rhs_dd ? rhs_dd : (DataDef *)&ddINT64;
		inst->parameters.push_back(rhs_ref ? as_reference_type(rhs0) : rhs0);
	}
	DBG(std::cout << "[W2] instantiate free " << best->ns << "::" << mname
	    << " -> " << sym << std::endl);
	m_free_fn_inst_by_sym[sym] = inst;
	m_free_op_inst_by_call[top] = inst;
	return inst;
}

// Emit `lhs <op> rhs` against an emit_symbol-bound callee (an external
// member operator OR an instantiated free operator — one emission for
// both). Hidden-arg order is the Itanium one: [sret slot,] lhs, rhs.
node_t CirBuilder::class_operator_external_call(TokenOperator *top,
		DataDefCLASS *lcls, FuncDef *callee, TokenBase *origin,
		node_t ret_slot, DataDefCLASS *slot_cls, bool *slot_used)
{
	if (!callee || callee->emit_symbol.empty()) return NULL;
	// lhs target: a free operator's param[0] names the (base) class the
	// lhs binds to — object_arg_addr's base walk does the upcast. A member
	// callee's param[0] is the __this pointer (not a reference param), so
	// param_object_class yields NULL and the lhs class is used unchanged.
	DataDef *pt0 = callee->parameters.empty() ? NULL : callee->parameters[0];
	bool refp0 = callee->is_ref_param(0);
	DataDefCLASS *lhs_target = param_object_class(pt0, refp0);
	if (!lhs_target) lhs_target = lcls;

	std::vector<ExternParam> eparams;
	node_t args = list();

	// A NON-TRIVIAL by-value class return uses the Itanium sret ABI ==
	// madc's __retbuf shape: hidden result-slot FIRST argument, void call
	// (g++ canon for `string c = a + b`: ONE call _ZStpl(&c,&a,&b) —
	// guaranteed copy elision when the caller provides the declared
	// variable as ret_slot; expression contexts materialize a
	// cleanup-tagged temp and yield its object lvalue).
	DataDefCLASS *retc = (!callee->returns_reference() && !callee->return_value_type().is_pointer())
			     ? class_return_via_retbuf(&callee->return_value_type()) : NULL;
	char objtmp[40] = { 0 };
	bool slot_consumed = false;
	if (retc) {
		node_t slot;
		if (ret_slot && slot_cls == retc) {
			slot = ret_slot;
			slot_consumed = true;
			if (slot_used) *slot_used = true;
		} else {
			snprintf(objtmp, sizeof(objtmp), "__madc_objtmp_%d",
				 m_strtmp_counter++);
			Variable *tmp = new Variable(objtmp, *retc, 1, NULL, false);
			tmp->flags |= vfLOCAL;
			m_pending_stmts.push_back(var_decl(tmp, origin));
			slot = node1(N_ADDR, id(objtmp, origin), origin);
		}
		eparams.push_back({ {N_VOID}, true });
		append(args, slot);
	}

	if (lhs_target) {
		eparams.push_back({ {N_VOID}, true });
		append(args, object_arg_addr(top->left, lhs_target));
	} else {
		// Literal-lhs free operator: param[0] is a non-class
		// pointer/value — shape it like the rhs's non-class branches.
		eparams.push_back(native_param_shape(pt0, false));
		append(args, translate_expr(top->left));
	}
	// Single explicit RHS argument, shaped by parameters[1].
	DataDef *pt = (callee->parameters.size() > 1) ? callee->parameters[1] : NULL;
	bool refp = callee->is_ref_param(1);
	if (DataDefCLASS *pc = param_object_class(pt, refp)) {
		eparams.push_back({ {N_VOID}, true });
		append(args, object_arg_addr(top->right, pc));
	} else if (DataDefCLASS *vc = as_class_instance(pt)) {
		eparams.push_back(native_param_shape(pt, false));
		append(args, object_arg_value(top->right, vc));
	} else if (refp) {
		eparams.push_back(native_param_shape(pt, true));
		append(args, node1(N_ADDR, translate_expr(top->right), top->right));
	} else {
		eparams.push_back(native_param_shape(pt, false));
		append(args, translate_expr(top->right));
	}
	bool ret_ptr = false;
	std::vector<c2mir_node_code_t> ret_specs =
		emit_symbol_ret_specs(callee, ret_ptr);
	if (callee->returns_reference()) {
		ret_ptr = true;
		ret_specs = { N_VOID };
	}
	if (retc) {
		ret_ptr = false;
		ret_specs.clear();   // void sret call; the slot carries the result
	}
	need_output_extern(callee->emit_symbol.c_str(), ret_ptr, eparams, ret_specs);
	node_t ocall = node2(N_CALL, id(callee->emit_symbol.c_str(), origin),
			     args, origin);
	CIR_NODE(ocall)->synth_from_origin = true;
	if (retc) {
		if (slot_consumed)
			return ocall;   // the caller wraps the bare call as a statement
		m_pending_stmts.push_back(node2(N_EXPR, list(), ocall, origin));
		return id(objtmp, origin);   // materialized object lvalue
	}
	if (callee->returns_reference())
		return node1(N_DEREF, ocall, origin);
	return ocall;
}

node_t CirBuilder::try_free_operator_call(TokenOperator *top, DataDefCLASS *lcls,
		const std::string &mname, FuncDef *member_callee, TokenBase *origin)
{
	if (!m_prog || top == NULL || !top->right) return NULL;
	// NULL lcls = the literal-lhs mixed shape; the rhs must be the class.
	if (!lcls && !operand_object_class(top->right)) return NULL;
	if (m_prog->free_operator_overloads.empty()) return NULL;

	// W2 manipulator: `os << endl` — the parser built a (0-arg) call node for
	// the manipulator free function `endl`. The correct lowering passes the
	// STREAM to the manipulator: endl(&os), bound mangled-direct, returning
	// the stream. (The member manipulator operator<< just forwards to
	// pf(*this).) The instantiated FuncDef is memoized per symbol like every
	// other Pattern-A binding; the matched (base) stream class rides its
	// parameters[0], and object_arg_addr's base walk adjusts a derived lhs.
	if (mname == "operator<<" && lcls) {
		if (TokenCallFunc *rc = dynamic_cast<TokenCallFunc *>(top->right)) {
			FuncDef *rfd = dynamic_cast<FuncDef *>(rc->var.type);
			std::string fname = (rfd && !rfd->function_display_name.empty())
					    ? rfd->function_display_name : rc->var.name;
			std::string fns = rfd ? rfd->namespace_name : std::string();
			std::vector<BaseCand> lhs_cands;
			collect_self_and_base_spellings(lcls, 0, lhs_cands);
			for (const Program::FreeOperatorOverload &ov : m_prog->free_operator_overloads) {
				if (ov.opname != fname || ov.param_spellings.size() != 1) continue;
				if (!fns.empty() && ov.ns != fns) continue;
				std::map<std::string, std::string> binding;
				std::string stype;
				DataDefCLASS *scls = NULL;
				for (const auto &cand : lhs_cands) {
					binding.clear();
					if (deduce_free_stream_call(cand.spelling, ov,
								    binding, stype)) {
						scls = cand.cls;
						break;
					}
				}
				if (!scls) continue;
				std::vector<std::string> targs;
				if (!targs_from_binding(ov, binding, targs)) continue;
				std::string msym = itanium_mangle_std_free_template(
					ov.opname, targs, stype, { stype });
				if (msym.empty() || msym[0] != '_') continue;
				FuncDef *minst = NULL;
				auto msit = m_free_fn_inst_by_sym.find(msym);
				if (msit != m_free_fn_inst_by_sym.end())
					minst = msit->second;
				if (!minst) {
					minst = new FuncDef(*as_reference_type(scls));
					minst->emit_symbol = msym;
					minst->declaration_only = true;
					minst->function_display_name = fname;
					minst->namespace_name = ov.ns;
					minst->parameters.push_back(as_reference_type(scls));
					m_free_fn_inst_by_sym[msym] = minst;
				}
				DBG(std::cout << "[W2] bind manipulator " << ov.ns << "::"
				    << ov.opname << " -> " << minst->emit_symbol << std::endl);
				node_t a = list();
				append(a, object_arg_addr(top->left,
					as_class_instance(minst->parameters[0])));
				need_output_extern(minst->emit_symbol.c_str(), /*ret_ptr*/true,
						   { { {N_VOID}, true } }, { N_VOID });
				node_t call = node2(N_CALL,
					id(minst->emit_symbol.c_str(), origin), a, origin);
				CIR_NODE(call)->synth_from_origin = true;
				return node1(N_DEREF, call, origin);   // ostream&
			}
		}
	}

	// Binary free operator (Pattern A): instantiate a FuncDef carrying the
	// mangled symbol on emit_symbol (member arbitration inside), then emit
	// through the same external-call path as member operators.
	FuncDef *inst = std_free_operator_instantiation(top, lcls, mname,
							member_callee);
	if (!inst) return NULL;
	auto bit = m_free_op_body_by_call.find(top);
	if (bit != m_free_op_body_by_call.end() && bit->second) {
		TokenCallFunc *tc = new TokenCallFunc(*bit->second);
		tc->file = top->file;
		tc->line = top->line;
		tc->column = top->column;
		tc->parameters.push_back(top->left);
		tc->parameters.push_back(top->right);
		return translate_expr(tc);
	}
	return class_operator_external_call(top, lcls, inst, origin);
}

// Replace each template-param NAME (whole identifier) in a type spelling with the
// mangler's $Tn marker (parse_type maps $Tn -> Itanium T_/T0_/...). E.g.
// "basic_istream<_CharT,_Traits>&" + [_CharT,_Traits,_Alloc] -> "basic_istream<$T0,$T1>&".
static std::string substitute_tparams(const std::string &spell,
		const std::vector<std::string> &tparams)
{
	std::string out;
	size_t i = 0, n = spell.size();
	while (i < n) {
		char c = spell[i];
		if (isalpha((unsigned char)c) || c == '_') {
			size_t j = i + 1;
			while (j < n && (isalnum((unsigned char)spell[j]) || spell[j] == '_'))
				++j;
			std::string word = spell.substr(i, j - i);
			int tpi = -1;
			for (size_t k = 0; k < tparams.size(); ++k)
				if (tparams[k] == word) { tpi = (int)k; break; }
			if (tpi >= 0) out += "$T" + std::to_string(tpi);
			else out += word;
			i = j;
		} else { out += c; ++i; }
	}
	return out;
}

// Replace the leading template-id head of `spell` with the fully-qualified
// `qhead` (from the matched class), so the mangler emits the right namespace
// (St13basic_istream, NSt7__cxx1112basic_string, ...). The declaration captured
// the head UNqualified (it lives inside `namespace std`), which would mangle as a
// bare `13basic_istream`/`12basic_string` and never link.
static std::string requalify_head(const std::string &spell, const std::string &qhead)
{
	if (qhead.empty()) return spell;
	size_t lt = spell.find('<');
	if (lt == std::string::npos) return spell;
	// Replace only the head NAME — keep leading cv-qualifiers, or
	// "const basic_string<...>&" would lose its const (RK -> R, wrong symbol).
	size_t h = 0;
	for (;;) {
		while (h < lt && spell[h] == ' ') ++h;
		if (spell.compare(h, 6, "const ") == 0) { h += 6; continue; }
		if (spell.compare(h, 9, "volatile ") == 0) { h += 9; continue; }
		break;
	}
	return spell.substr(0, h) + qhead + spell.substr(lt);
}

// Instantiate a NAMED std:: (real-libstdc++) free-function template for a call,
// binding it MANGLED-DIRECT on an instantiated FuncDef: the Itanium symbol
// lives on FuncDef::emit_symbol (Pattern A), call_emit_symbol reads it, and the
// GENERIC call path emits the args (class refs by address via object_arg_addr's
// derived->base binding) and derefs the reference return. Selects an overload
// by arity, deduces template args from the call's arg types (matching each
// template-id param against the arg's class self/bases), $Tn-parameterizes the
// spellings, mangles, and memoizes one FuncDef per symbol (negative results per
// call token). Returns NULL when the call is not such a function — the caller
// keeps the resolved declaration. See free_function_overloads +
// project_cpp_mangled_direct.
FuncDef *CirBuilder::std_free_function_instantiation(TokenCallFunc *tcf, FuncDef *cdf)
{
	if (!m_prog || !tcf || !cdf
	    || cdf->function_display_name.empty() || cdf->namespace_name.empty()
	    || !cdf->emit_symbol.empty())
		return NULL;
	if (tcf->src_node && !dynamic_cast<FuncDef *>(tcf->var.type))
		return NULL;
	auto mit = m_free_fn_inst_by_call.find(tcf);
	if (mit != m_free_fn_inst_by_call.end()) return mit->second;
	m_free_fn_inst_by_call[tcf] = NULL;   // negative-cache; success overwrites
	const std::string ns = cdf->namespace_name;
	const std::string name = cdf->function_display_name;
	size_t argc = tcf->parameters.size();

	const Program::FreeOperatorOverload *best = NULL;
	std::vector<std::string> best_targs, best_param_spell;
	std::string best_ret;
	std::vector<DataDefCLASS *> best_pcls;          // matched (base) class per param
	std::map<std::string, std::string> best_binding;
	std::map<std::string, std::string> best_qmap;   // unqualified head -> qualified head
	for (const Program::FreeOperatorOverload &ov : m_prog->free_function_overloads) {
		if (ov.ns != ns || ov.opname != name) continue;
		if (ov.param_spellings.size() != argc) continue;
		std::map<std::string, std::string> binding;
		std::map<std::string, std::string> qmap;   // phead -> qualified head (this overload)
		std::vector<size_t> offs(argc, 0);
		std::vector<DataDefCLASS *> pcls(argc, NULL);
		bool ok = true;
		for (size_t i = 0; i < argc && ok; i++) {
			std::string pspell = ov.param_spellings[i];
			// Only template-id reference params participate in deduction; a
			// trailing scalar such as getline's `_CharT __delim` can match once
			// an earlier class param has bound `_CharT`.
			if (pspell.size() >= 2 && pspell.compare(pspell.size() - 2, 2, "&&") == 0)
				{ ok = false; break; }   // prefer the lvalue-ref overload
			DataDef *argdd = m_prog->operand_value_datadef(tcf->parameters[i]);
			if (!argdd)
				argdd = tcf->parameters[i]->datadef();
			DataDefCLASS *acls = as_class_instance(argdd);
			if (acls) {
				std::string qhead;
				if (!deduce_param_against_class(pspell, acls, ov.template_params,
								binding, offs[i], qhead, &pcls[i])) {
					ok = false; break;
				}
				std::string phead; std::vector<std::string> pargs;
				split_template_id_w2(pspell, phead, pargs);
				qmap[phead] = qhead;   // fully-qualified head for mangling
				FFDBG(fprintf(stderr, "[FFCALL] %s param[%zu] phead=%s -> qhead='%s' off=%zu\n",
					name.c_str(), i, phead.c_str(), qhead.c_str(), offs[i]));
			} else if (!bind_member_template_param(pspell,
					datadef_cpp_spelling_w2(argdd),
					ov.template_params, binding)) {
				ok = false;
				break;
			}
		}
		if (!ok) continue;
		std::vector<std::string> targs;
		for (const std::string &tp : ov.template_params) {
			auto it = binding.find(tp);
			if (it == binding.end()) { targs.clear(); break; }
			targs.push_back(it->second);
		}
		if (targs.size() != ov.template_params.size()) continue;
		if (best && ov.template_params.size() >= best->template_params.size()) continue;
		best = &ov;
		best_targs = targs;
		best_pcls = pcls;
		best_binding = binding;
		best_qmap = qmap;
	}
	if (!best) return NULL;
	if (m_prog->fn_template_map.count(ns + "::" + name)
	    && norm_type_w2(best->return_spelling) == "void")
		return NULL;

	// Build the mangler inputs: requalify each template-id head with the matched
	// class's fully-qualified spelling (St / NSt7__cxx11...), then map template-param
	// names to $Tn markers (parse_type -> T_/T0_/...).
	auto qualify_and_param = [&](const std::string &spell) -> std::string {
		std::string h; std::vector<std::string> a;
		std::string q = (split_template_id_w2(spell, h, a) && best_qmap.count(h))
				? best_qmap[h] : std::string();
		return substitute_tparams(requalify_head(spell, q), best->template_params);
	};
	best_ret = qualify_and_param(best->return_spelling);
	best_param_spell.clear();
	for (const std::string &p : best->param_spellings)
		best_param_spell.push_back(qualify_and_param(p));

	std::string sym = itanium_mangle_std_free_template(name, best_targs, best_ret,
							   best_param_spell);
	if (sym.empty() || sym[0] != '_') return NULL;
	auto sit = m_free_fn_inst_by_sym.find(sym);
	if (sit != m_free_fn_inst_by_sym.end()) {
		m_free_fn_inst_by_call[tcf] = sit->second;
		return sit->second;
	}

	// Return type: void, or a reference to a class deducible from the matched
	// param classes (a stream fn returns one of its ref params' classes). A
	// by-value class or unmapped builtin return bails — those shapes never
	// bound mangled-direct here before either.
	const std::string &rspell = best->return_spelling;
	bool ret_ref = !rspell.empty() && rspell.back() == '&';
	DataDef *ret_dd = NULL;
	if (!ret_ref && norm_type_w2(rspell) == "void")
		ret_dd = &ddVOID;
	else if (ret_ref) {
		for (size_t i = 0; i < best_pcls.size() && !ret_dd; i++) {
			if (!best_pcls[i]) continue;
			std::map<std::string, std::string> b2 = best_binding;
			size_t off2 = 0; std::string qh2; DataDefCLASS *mc = NULL;
			if (deduce_param_against_class(rspell, best_pcls[i],
						       best->template_params,
						       b2, off2, qh2, &mc)
			    && b2 == best_binding && off2 == 0)
				ret_dd = mc;
		}
	}
	if (!ret_dd) return NULL;

	FuncDef *inst = new FuncDef(ret_ref ? *as_reference_type(ret_dd) : *ret_dd);
	inst->emit_symbol = sym;
	inst->declaration_only = true;
	inst->function_display_name = name;
	inst->namespace_name = ns;
	for (size_t i = 0; i < argc; i++) {
		const std::string &pspell = best->param_spellings[i];
		bool is_ref = !pspell.empty() && pspell.back() == '&';
		DataDef *argdd = m_prog->operand_value_datadef(tcf->parameters[i]);
		if (!argdd)
			argdd = tcf->parameters[i]->datadef();
		DataDef *pdd = (is_ref && best_pcls[i])
			       ? static_cast<DataDef *>(best_pcls[i])
			       : argdd;
		DataDef *pdd0 = pdd ? pdd : (DataDef *)&ddINT64;
		inst->parameters.push_back(is_ref ? as_reference_type(pdd0) : pdd0);
	}
	// Extern proto now (mirrors the class-member emit_symbol binds): ref params
	// are pointers, a reference return comes back as an address.
	bool ret_ptr = false;
	std::vector<c2mir_node_code_t> ret_specs;
	std::vector<ExternParam> eparams;
	native_func_shape(inst, ret_ptr, ret_specs, eparams);
	need_output_extern(sym.c_str(), ret_ptr, eparams, ret_specs);
	DBG(std::cout << "[FFCALL] instantiate " << ns << "::" << name
	    << " -> " << sym << std::endl);
	m_free_fn_inst_by_sym[sym] = inst;
	m_free_fn_inst_by_call[tcf] = inst;
	return inst;
}

node_t CirBuilder::class_operator_call(TokenOperator *top, TokenBase *origin,
				       const char *opsym_override)
{
	if (!top || !top->left || !top->right) return NULL;
	auto free_operator_body_call = [&](const std::string &mname) -> node_t {
		if (!m_prog)
			return NULL;
		DataDef *ld = m_prog->free_operator_arg_datadef(top->left);
		DataDef *rd = m_prog->free_operator_arg_datadef(top->right);
		if (w2_datadef_involves_template_param(ld)
		    || w2_datadef_involves_template_param(rd))
			return NULL;
		Variable *callee = NULL;
		if (!m_prog->instantiate_free_operator_template(mname, top->left,
								top->right,
								&callee)
		    || !callee)
			return NULL;
		TokenCallFunc *tc = new TokenCallFunc(*callee);
		tc->file = top->file;
		tc->line = top->line;
		tc->column = top->column;
		tc->parameters.push_back(top->left);
		tc->parameters.push_back(top->right);
		return translate_expr(tc);
	};
	// The operator LHS must be a class object lvalue (or a reference to one),
	// not a pointer to one: `T s; s = x` is operator=, but `T *p; p = ...`
	// is a plain pointer assignment. operand_object_class resolves the class
	// without unwrapping plain pointers (only the reference representation).
	DataDefCLASS *lcls = operand_object_class(top->left);
	if (!lcls && class_subscript_is_object(top->left)) {
		TokenSubscript *lsub = dynamic_cast<TokenSubscript *>(top->left);
		DataDefCLASS *ccls = lsub ? class_behind(lsub->object.type) : NULL;
		std::string opname = "operator[]";
		Variable *omv = ccls ? ccls->findMethod(opname) : NULL;
		FuncDef *ofd = omv ? dynamic_cast<FuncDef *>(omv->type) : NULL;
		if (ofd) lcls = class_behind(&ofd->return_value_type());
	}
	if (!lcls) {
		// `"pre" + s`: a non-class lhs with a CLASS rhs — only the free
		// operator set's mixed shape can bind (no member candidate).
		if (!operand_object_class(top->right)) return NULL;
		const char *opsym0 = opsym_override ? opsym_override
						    : binop_overload_symbol(top->id());
		if (!opsym0[0]) return NULL;
		std::string mname0 = std::string("operator") + opsym0;
		if (node_t freecall = try_free_operator_call(top, NULL,
							     mname0, NULL,
							     origin))
			return freecall;
		return free_operator_body_call(mname0);
	}
	const char *opsym = opsym_override ? opsym_override
					   : binop_overload_symbol(top->id());
	if (!opsym[0]) return NULL;

	std::string mname = std::string("operator") + opsym;
	// Pick the overload matching the RHS. A class without this operator yields
	// NULL -> fall through to generic handling.
	FuncDef *callee = select_operator_overload(lcls, mname, top->right);
	// W2: a NON-member operator declared at namespace scope (e.g. the free
	// std::operator<<(ostream&, const char*)) may match rhs more exactly than the
	// member candidate (member operator<<(const void*) needs a const char*->void*
	// conversion). If so, bind it mangled-direct instead.
	if (node_t freecall = try_free_operator_call(top, lcls, mname, callee, origin))
		return freecall;
	if (!callee || (operand_object_class(top->right)
			&& lcls->binary_operator_only_takes_nonclass(mname)))
		if (node_t freebody = free_operator_body_call(mname))
			return freebody;
	if (!callee) return NULL;

	// A class-bound external operator names its real symbol via emit_symbol and
	// is declared from its parsed signature; otherwise the default
	// ClassName__operator<op> scheme + the referenced-funcs prototype pass.
	// One emission serves external members AND instantiated free operators.
	if (!callee->emit_symbol.empty())
		return class_operator_external_call(top, lcls, callee, origin);

	// The lhs `this` address for a user-class operator: a NAMED variable
	// honours the unified pointer-stored rule; any other lhs expression is
	// addressed by &expr (a value lvalue). type()==ttVariable, not a
	// TokenVar downcast — TokenMember/TokenCallFunc derive from TokenVar,
	// and the downcast emitted an implicit-this member's bare name
	// (reverse_iterator's `current + __n`).
	node_t this_arg;
	TokenVar *ltv = dynamic_cast<TokenVar *>(top->left);
	if (ltv && top->left->type() == TokenType::ttVariable)
		this_arg = object_var_addr(ltv->var, origin);
	else
		// Expression receivers route through object_arg_addr (member ->
		// &__this->member, ref-returning call -> the address, by-value
		// object-returning call -> a materialized cleanup temp — a bare
		// &call is not an lvalue; `__x.base() - __y.base()` chains).
		this_arg = object_arg_addr(top->left, lcls);

	// ClassName__operator== by default; an arity-disambiguated same-name
	// overload (P2.1b gaps 3 & 4) carries its real symbol in local_emit_name.
	// (emit_symbol is empty here — the external-bind branch above returned.)
	std::string sym = call_emit_symbol(callee, lcls->name + "__" + mname);

	// Part B: an operator returning a class object BY VALUE (e.g. V operator+(V&))
	// yields an rvalue; materialize it into an addressable cleanup temp so the
	// result can be copy-constructed from, passed by reference, or have members
		// called. A NON-TRIVIAL return was compiled with the
	// __retbuf ABI (hidden return-slot first param) -> pass &__t as that slot and
	// the void call writes *__retbuf; a TRIVIAL (native struct) return is assigned
	// into __t. The expression value is then the temp's object lvalue.
	DataDefCLASS *retc = as_class_instance(&callee->return_value_type());
	bool by_value_object = retc && !callee->returns_reference()
			       && !callee->return_value_type().is_pointer();
	bool via_retbuf = by_value_object
			  && class_return_via_retbuf(&callee->return_value_type()) != NULL;
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
		bool refp = callee->is_ref_param(1);
		if (DataDefCLASS *pc = param_object_class(pt, refp))
			append(args, object_arg_addr(top->right, pc));
		else if (DataDefCLASS *vc = as_class_instance(pt))
			append(args, object_arg_value(top->right, vc));
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
	if (callee && callee->returns_reference())
		return node1(N_DEREF, ocall, origin);
	return ocall;
}

// C++20 builtin three-way comparison ([expr.spaceship]), per g++ -O0 canon
// (tmp/spaceship.s): NO call — declare a comparison-category temp and store
// the inline byte-select into its _M_value (offset 0, __cmp_cat::type):
//   integral/pointer: (l < r ? -1 : l > r ? 1 : 0)            -> strong_ordering
//   floating:         (l < r ? -1 : l > r ? 1 : l == r ? 0 : 2) -> partial_ordering
//                     (2 = __cmp_cat::_Ncmp::_Unordered)
// The category class was resolved at parse time from the parsed <compare>
// (Program::comparison_category_class); class operands with an operator<=>
// were dispatched through the operator machinery before reaching here. Each
// operand is materialized into a typed temp so it is evaluated exactly once.
node_t CirBuilder::three_way_builtin_lowering(TokenOperator *top, TokenBase *origin)
{
	DataDefCLASS *cat = as_class_instance(top ? top->datadef() : NULL);
	if (!cat || !top->left || !top->right)
		return error_node("'<=>' needs the comparison-category types — "
				  "#include <compare> (C++20)", origin);
	DataDef *ldd = top->left->datadef();
	DataDef *rdd = top->right->datadef();
	if (!ldd || !rdd)
		return error_node("'<=>' operand has no type", origin);
	char oname[40], lname[40], rname[40];
	snprintf(oname, sizeof(oname), "__madc_objtmp_%d", m_strtmp_counter++);
	snprintf(lname, sizeof(lname), "__madc_swtmp_%d", m_strtmp_counter++);
	snprintf(rname, sizeof(rname), "__madc_swtmp_%d", m_strtmp_counter++);
	Variable *ot = new Variable(oname, *cat, 1, NULL, false);
	ot->flags |= vfLOCAL;
	Variable *lt = new Variable(lname, *ldd, 1, NULL, false);
	lt->flags |= vfLOCAL;
	Variable *rt = new Variable(rname, *rdd, 1, NULL, false);
	rt->flags |= vfLOCAL;
	m_pending_stmts.push_back(var_decl(ot, origin));
	m_pending_stmts.push_back(var_decl(lt, origin));
	m_pending_stmts.push_back(var_decl(rt, origin));
	m_pending_stmts.push_back(node2(N_EXPR, list(),
		node2(N_ASSIGN, id(lname, origin), translate_expr(top->left),
		      origin), origin));
	m_pending_stmts.push_back(node2(N_EXPR, list(),
		node2(N_ASSIGN, id(rname, origin), translate_expr(top->right),
		      origin), origin));
	bool floating = ldd->is_real() || rdd->is_real();
	node_t tail = floating
	    ? node3(N_COND,
		    node2(N_EQ, id(lname, origin), id(rname, origin), origin),
		    integer(0, origin), integer(2, origin), origin)
	    : integer(0, origin);
	node_t sel = node3(N_COND,
		node2(N_LT, id(lname, origin), id(rname, origin), origin),
		integer(-1, origin),
		node3(N_COND,
		      node2(N_GT, id(lname, origin), id(rname, origin), origin),
		      integer(1, origin), tail, origin), origin);
	node_t fld = node2(N_FIELD, id(oname, origin), id("_M_value", origin),
			   origin);
	m_pending_stmts.push_back(node2(N_EXPR, list(),
		node2(N_ASSIGN, fld, sel, origin), origin));
	return id(oname, origin);   // the category-typed object lvalue
}

// A side-effect-free literal constant token: numeric / char / string
// literal (TokenNullptr is a TokenInt). Identifier reads are excluded —
// only true literals are folded by strict_equality_lowering below.
static bool is_literal_constant_token(TokenBase *tb)
{
	return dynamic_cast<TokenInt *>(tb)
	    || dynamic_cast<TokenReal *>(tb)
	    || dynamic_cast<TokenChar *>(tb)
	    || dynamic_cast<TokenStr *>(tb);
}

// Strict equality `===` / `!==` (STD_MADC dialect): type-domain identity AND
// value equality. Scalars use DataDef::same_representation; class operands
// strict-compare inside the class domain via the SAME operator== dispatch ==
// uses (a user operator===/!== was already tried by class_operator_call at
// the translate_expr binary entry). A statically-false compare still
// evaluates both operands: (l, r, 0|1).
// Spec: docs/superpowers/specs/2026-06-11-strict-equality-design.md §2-3.
node_t CirBuilder::strict_equality_lowering(TokenOperator *top, TokenBase *origin)
{
	bool neq = top->id() == TokenID::tk3NotEq;
	DataDefCLASS *lcls = operand_object_class(top->left);
	DataDefCLASS *rcls = operand_object_class(top->right);
	if (lcls || rcls) {
		// Domain rule: defer to the class's operator== machinery.
		node_t eq = class_operator_call(top, origin, "==");
		if (eq)
			return neq ? node1(N_NOT, eq, origin) : eq;
		if (lcls && lcls == rcls) {
			std::string msg = "no match for strict equality on '"
				+ lcls->name
				+ "' (no operator=== or operator==)";
			return error_node(msg.c_str(), origin);
		}
	} else {
		DataDef *ldd = operand_value_datadef(top->left);
		DataDef *rdd = operand_value_datadef(top->right);
		if (ldd && rdd && ldd->same_representation(*rdd)) {
			node_t left = translate_expr(top->left);
			node_t right = translate_expr(top->right);
			return node2(neq ? N_NE : N_EQ, left, right, origin);
		}
	}
	// Different domains: constant result, operands still evaluated.
	// Two pure literals have no side effects to preserve, and the comma
	// form is NOT a C constant expression — fold to the bare integer so
	// file-scope initializers like `int g = (1 === 1.0);` stay constant.
	if (is_literal_constant_token(top->left)
	 && is_literal_constant_token(top->right))
		return integer(neq ? 1 : 0, origin);
	node_t left = translate_expr(top->left);
	node_t right = translate_expr(top->right);
	return node2(N_COMMA, node2(N_COMMA, left, right, origin),
		     integer(neq ? 1 : 0, origin), origin);
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
	// pointer-stored rule; any other operand expression is addressed by
	// &expr. The discriminator is type()==ttVariable, NOT a TokenVar
	// downcast — TokenMember/TokenCallFunc DERIVE from TokenVar, and the
	// downcast caught an implicit-this member (`--current` in
	// reverse_iterator::operator++), emitting its bare member name
	// instead of &__this->member.
	node_t this_arg;
	TokenVar *otv = dynamic_cast<TokenVar *>(operand);
	if (otv && operand->type() == TokenType::ttVariable)
		this_arg = object_var_addr(otv->var, origin);
	else
		// object_arg_addr handles every expression receiver shape:
		// member -> &__this->member, ref-returning call -> the address
		// itself, by-value object-returning call -> a materialized
		// cleanup temp (a bare &call is not an lvalue and c2mir
		// rejects it — reverse_iterator's `--base()`-style chains).
		this_arg = object_arg_addr(operand, cls);

	// A class-bound external operator names its real symbol via emit_symbol;
	// otherwise the default ClassName__operator<sym> scheme + the
	// referenced-funcs prototype pass (a madc-emitted method body). An
	// arity-disambiguated same-name overload (P2.1b gaps 3 & 4 — the unary peer
	// of a binary of the same name) carries its real symbol in local_emit_name.
	std::string sym = call_emit_symbol(callee, cls->name + "__" + mname);
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
	if (callee->returns_reference())
		return node1(N_DEREF, ocall, origin);
	return ocall;
}

// Build the bare `ClassName__operator[](&obj, i)` call — the raw method result.
// For a T&-returning operator[] this is the element ADDRESS (a T*); callers that
// want the element lvalue deref it (class_subscript_call). NULL when the object
// is not a class with an operator[].
// Receiver-generic operator[] dispatch core: `recv_addr` is the receiver's
// ADDRESS node (value object -> &obj, pointer receiver -> its value). Shared
// by the named-variable (TokenSubscript) and expression-receiver
// (TokenSubscriptExpr) subscript paths.
node_t CirBuilder::class_subscript_addr_on(DataDefCLASS *cls, node_t recv_addr,
					   TokenBase *index, TokenBase *origin)
{
	if (!cls || !recv_addr) return NULL;
	std::string opname = "operator[]";
	Variable *mv = cls->findMethod(opname);
	if (!mv) return NULL;
	FuncDef *callee = dynamic_cast<FuncDef *>(mv->type);

	// A class-bound external operator[] names its real C++ symbol via
	// emit_symbol and has no madc-emitted body. Emit that symbol and declare it
	// as an extern from its signature, mirroring class_operator_call's
	// emit_symbol branch, instead of the default ClassName__operator[] +
	// referenced-funcs scheme for user-class method bodies. T& returns arrive as
	// pointers; class_subscript_call derefs them so obj[i] is an lvalue.
	if (callee && !callee->emit_symbol.empty()) {
		node_t this_arg = node2(N_CAST, void_ptr_type(), recv_addr, origin);
		CIR_NODE(this_arg)->synth_from_origin = true;
		// Param 0 = this (void*); param 1 = size_t index (a 64-bit scalar).
		std::vector<ExternParam> eparams = { { {N_VOID}, true },
						     { {N_LONG}, false } };
		node_t args = list();
		append(args, this_arg);
		append(args, translate_expr(index));
		// T& -> pointer return; class_subscript_call derefs it to the element
		// lvalue so obj[i] reads and obj[i] = x writes.
		need_output_extern(callee->emit_symbol.c_str(), true, eparams,
				   { N_CHAR });
		node_t call = node2(N_CALL, id(callee->emit_symbol.c_str(), origin),
				    args, origin);
		CIR_NODE(call)->synth_from_origin = true;
		return call;
	}

	std::string sym = cls->name + "__operator[]";   // ClassName__operator[]
	node_t args = list();
	append(args, recv_addr);
	// Index argument (operator[] parameter 1; parameter 0 = __this).
	DataDef *idx_pt = (callee && callee->parameters.size() > 1)
			? callee->parameters[1] : NULL;
	bool refp = callee && callee->is_ref_param(1);
	if (DataDefCLASS *pc = param_object_class(idx_pt, refp))
		append(args, object_arg_addr(index, pc));
	else if (DataDefCLASS *vc = as_class_instance(idx_pt))
		append(args, object_arg_value(index, vc));
	else if (refp)
		// A scalar reference parameter (`operator[](const key_type& k)`):
		// an lvalue index folds to `&index`, but a prvalue index (a literal
		// `m[1]`, an arithmetic result) is non-addressable — `&1` is ill-formed.
		// ref_param_arg_addr materializes a temp for those, exactly as every
		// other reference-argument site does, instead of taking `&<literal>`.
		append(args, ref_param_arg_addr(index, ref_param_referent(idx_pt),
						const_ref_param(callee, 1)));
	else
		append(args, translate_expr(index));

	referenced_funcs.insert(sym);
	return node2(N_CALL, id(sym.c_str(), origin), args, origin);
}

node_t CirBuilder::class_subscript_addr(TokenSubscript *tsub, TokenBase *origin)
{
	if (!tsub) return NULL;
	DataDefCLASS *cls = class_behind(tsub->object.type);
	if (!cls) return NULL;
	std::string opname = "operator[]";
	Variable *mv = cls->findMethod(opname);
	if (!mv) return NULL;
	FuncDef *callee = dynamic_cast<FuncDef *>(mv->type);
	node_t recv_addr;
	if (callee && !callee->emit_symbol.empty())
		// (the core adds the void* cast — together = object_var_void_addr)
		recv_addr = object_var_addr(tsub->object, origin);
	else {
		// __this: value receiver -> &obj, pointer receiver -> obj.
		bool recv_is_ptr = tsub->object.type && tsub->object.type->is_pointer();
		node_t recv = id(tsub->object.name.c_str(), origin);
		recv_addr = recv_is_ptr ? recv : node1(N_ADDR, recv, origin);
	}
	return class_subscript_addr_on(cls, recv_addr, tsub->index, origin);
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
	if (callee && callee->returns_reference())
		return node1(N_DEREF, call, origin);
	return call;
}

/*static*/ bool CirBuilder::class_subscript_is_object(TokenBase *arg)
{
	TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(arg);
	if (!tsub) return false;
	DataDefCLASS *cls = class_behind(tsub->object.type);
	if (!cls) return false;
	std::string opname = "operator[]";
	Variable *mv = cls->findMethod(opname);
	if (!mv) return false;
	FuncDef *callee = dynamic_cast<FuncDef *>(mv->type);
	return callee && class_behind(&callee->return_value_type()) != NULL;
}

/*static*/ bool CirBuilder::class_array_subscript_is_object(TokenBase *arg)
{
	// Exclude the operator[]-container case:
	// there the base CLASS itself owns an operator[] returning string&.
	if (class_subscript_is_object(arg)) return false;
	if (TokenSubscript *tsub = dynamic_cast<TokenSubscript *>(arg)) {
		if (!is_class_object(tsub->datadef())) return false;
		DataDef *bt = tsub->object.type;
		if (!bt) return false;
		if (tsub->object.is_fixed_array())
			return as_class_instance(bt) != NULL;
		DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(bt);
		return pdd && pdd->base_type
		    && as_class_instance(pdd->base_type) != NULL;
	}
	if (TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(arg)) {
		if (!is_class_object(tse->datadef())) return false;
		DataDef *bt = tse->base_expr ? tse->base_expr->datadef() : NULL;
		DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(bt);
		return pdd && pdd->base_type
		    && as_class_instance(pdd->base_type) != NULL;
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
					  std::vector<carray_dim_t>());
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

	// A SIMD/vector typedef `typedef ELEM NAME __attribute__((vector_size(N)))`,
	// parsed by madc into a DataDefSIMD. Emit the ELEMENT type's specs and attach
	// the vector_size attribute to the declaration; c2mir's apply_vector_attrs
	// (c2mir.c) then rewrites the typedef's type into a real vector type, so every
	// later use of NAME inherits it and c2mir handles vector literals / subscript /
	// arithmetic natively. Tier-1 lowering: reuse c2mir's vector machinery rather
	// than duplicating it. (Needs the fork's <=16B SIMD support, MIR_COMMIT 2ffebff+.)
	if (DataDefSIMD *simd = dynamic_cast<DataDefSIMD *>(dd)) {
		node_t sl = list();
		append(sl, simple(N_TYPEDEF));
		append_type_specs(sl, simd->element_type);
		node_t share = node1(N_SHARE, sl);
		node_t decl = node2(N_DECL, id(alias.c_str()), list());
		node_t spec_decl = simple(N_SPEC_DECL);
		append(spec_decl, share);
		append(spec_decl, decl);
		append(spec_decl, simd_vector_attrs(simd->vector_bytes));
		append(spec_decl, ignore());
		append(spec_decl, ignore());
		return spec_decl;
	}

	node_t tl = list();
	append(tl, simple(N_TYPEDEF));

	// Peel any fixed-array layers: `typedef T NAME[2][3]` carries the element
	// type in DataDefCArray::element_type and the dims as nested CArrays. The
	// element type drives the spec list; the dims become N_ARR declarator
	// suffixes below. Without this the CArray's dtRESERVED rawtype defaults to
	// `int` and the dimensions are dropped entirely.
	std::vector<carray_dim_t> arr_dims;
	dd = peel_carray_dims(dd, arr_dims);

	// Unwrap pointer for base type
	DataDef *base_dd = dd;
	bool is_ptr = dd->is_pointer();
	DataDefPTR *ptr_dd = is_ptr ? dynamic_cast<DataDefPTR *>(dd) : NULL;
	if (ptr_dd && ptr_dd->base_type)
		base_dd = ptr_dd->base_type;

	// Type specifier. A user/std:: CLASS instance is a DataDefCLASS (is-a
	// DataDefSTRUCT) but reports is_struct() false (basetype btClass), so it must
	// be recognized via as_class_instance too — matching type_list. Without this a
	// `typedef <class> NAME` fell through to append_type_specs and emitted
	// `typedef int NAME`, sizing the variable as a 4-byte int; the ctor then wrote
	// the full object through it -> stack smash / SIGSEGV. (emit-c-vs-g++ oracle.)
	if ((base_dd->is_struct() || as_class_instance(base_dd)) && !base_dd->is_complex()) {
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
				// Emit definition inline (typedef struct/union { ... } NAME).
				// A class with a vptr slot (polymorphic, e.g. std::basic_ios)
				// MUST use the class member list so __vptr (and any secondary
				// vptrs / layout padding) is present — anon_members_list emits
				// only the plain data members, dropping the vptr the inline ctor
				// installs and the libstdc++-matching object layout.
				DataDefCLASS *cdd_inline = as_user_class(base_dd);
				node_t mbrs = (cdd_inline && cdd_inline->has_vptr_slot)
					    ? class_member_list(cdd_inline)
					    : anon_members_list(sdd);
				append(tl, node2(agg, id(sdd->name.c_str()), mbrs));
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

	DataDef *ret_dd = &fd->return_value_type();
	bool ret_is_ptr = ret_dd && ret_dd->is_pointer();
	bool ret_is_ref = fd->returns_reference();   // T& -> returned by address (one more *)
	int ret_star_depth = dd_peel_pointers(ret_dd);

	// A by-value non-trivial class return uses the __retbuf ABI (void return +
	// hidden `struct <T> *__retbuf` first param); keep the prototype in
	// lock-step with func_def's lowering. A trivial struct keeps c2mir's native
	// struct return.
	DataDefCLASS *ret_obj = (!ret_is_ptr && !ret_is_ref && !fd->is_multi_return())
				? class_return_via_retbuf(ret_dd) : NULL;
	bool ret_via_retbuf = ret_obj != NULL;
	DataDef *retbuf_dd = (DataDef *)ret_obj;
	bool ret_is_multi = fd->is_multi_return();   // void ret + `long *__retbuf` first param

	// Function returning a function pointer — `RET (*f(params))(fp-params)`.
	// Mirror func_def (see the comment there): type_list would render the
	// DataDefFPTR return as a bare `long`. Kept in lock-step with func_def.
	DataDefFPTR *ret_fnptr = (!ret_via_retbuf && !ret_is_multi && !ret_is_ref
				 && fd->return_typedef_name.empty())
				? dynamic_cast<DataDefFPTR *>(ret_dd) : NULL;

	int ret_decl_stars = ret_star_depth;
	node_t ret_type = NULL;
	if (ret_via_retbuf || ret_is_multi) {
		ret_type = type_list(&ddVOID);
		ret_decl_stars = 0;
	} else if (ret_fnptr) {
		ret_type = list();   // spec filled by fnptr_decl_pieces at decl_list
		ret_decl_stars = 0;
	} else if (!fd->return_typedef_name.empty()) {
		ret_type = type_list(&fd->return_value_type(), fd->return_typedef_name);
		ret_decl_stars = explicit_star_count(&fd->return_value_type(),
						     fd->return_typedef_name);
	} else {
		ret_type = type_list(ret_dd);
	}
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
			if (ptypedef.empty() && i < fd->param_typedef_names.size())
				ptypedef = fd->param_typedef_names[i];
			append(param_list, param_decl(ptype, pname, ptypedef));
		}
	}
	if (fd->has_captures)
		for (Variable *cv : fd->captured_vars) {
			if (!cv) continue;
			DataDef *capt_ptr = capture_param_type(cv);
			append(param_list, param_decl(capt_ptr, cv->name.c_str(),
						      std::string()));
		}
	if (fd->is_varargs)
		append(param_list, simple(N_DOTS));

	node_t func_inner = node1(N_FUNC, param_list);

	node_t func_id = id(tf->var.name.c_str(), tf);
	node_t decl_list = list();
	append(decl_list, func_inner);
	if (ret_fnptr) {
		// `RET (*f(params))(fp-params)`: append the `*(fp-params)` suffix and
		// fill ret_type with the pointed-to function's return-type specs.
		fnptr_decl_pieces(ret_fnptr->target, true, ret_type, decl_list,
				  std::vector<carray_dim_t>());
	} else {
		for (int rs = 0; rs < ret_decl_stars; rs++)
			append(decl_list, pointer());
		// T&-returning method: returned by address (one extra pointer level), so
		// the prototype matches the definition and call sites can deref.
		if (ret_is_ref)
			append(decl_list, pointer());
	}

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

	if (TokenPackExpansion *tpe = dynamic_cast<TokenPackExpansion *>(tb)) {
		node_t n = translate_expr(tpe->pattern);
		cir_node *cn = CIR_NODE(n);
		DataDefTemplateParam *tp = tpe->pattern
			? template_param_in_pack_pattern(tpe->pattern)
			: NULL;
		if (!tp)
			return error_node("pack expansion without template pack", tb);
		cn->tsubst_pack_expand = true;
		cn->tsubst_pack_index = tp->param_index;
		std::string value_name =
			pack_value_name_in_pattern(tpe->pattern, tp->param_index);
		if (!value_name.empty())
			cn->tsubst_pack_value_name =
				arena.intern(value_name.c_str());
		return n;
	}

	// throw-EXPRESSION ([expr.throw]): `(throw X())`, `cond ? a : throw e`.
	// translate_throw already returns an N_EXPR wrapping the __madc_throw_*
	// call (the same node the throw STATEMENT lowers to), so it drops straight
	// into expression position — control never returns, so its "value" is
	// unreachable. libstdc++'s _GLIBCXX_THROW_OR_ABORT(x) = `(throw (x))` makes
	// the __throw_* helpers (allocator/uninitialized-copy chain) reach here.
	if (TokenTHROW *th = dynamic_cast<TokenTHROW *>(tb))
		return translate_throw_call(th);

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
		std::string src_ti = class_typeinfo_symbol(srcC);
		std::string dst_ti = class_typeinfo_symbol(dstC);
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
			node1(N_LIST, class_tag_ref(dstC)),
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
			std::string sym = class_typeinfo_symbol(c);
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
		std::string sym = class_typeinfo_symbol(ec);
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
	// Functional construction `T(args)` -> materialize a scope-local cleanup temp,
	// construct into it, and yield the temp as an object LVALUE (id(name)). One
	// uniform object-rvalue representation: &temp for a by-reference argument,
	// temp.member for member access, and a plain struct copy for by-value
	// passing / copy-initialization. (Same temp + RAII-cleanup machinery as
	// object_call_temp_addr, minus the address-of.)
	if (TokenObjTemp *ot = dynamic_cast<TokenObjTemp *>(tb)) {
		DataDefCLASS *ocdd = ot->obj_class;
		char otname[40];
		snprintf(otname, sizeof(otname), "__madc_objtmp_%d", m_strtmp_counter++);
		Variable *otmp = new Variable(otname, *ocdd, 1, NULL, false);
		otmp->flags |= vfLOCAL;
		m_pending_stmts.push_back(var_decl(otmp, tb));
		node_t occ = class_ctor_call(otmp, ocdd, ot->ctor_args, tb);
		if (occ) m_pending_stmts.push_back(occ);
		return id(otname, tb);
	}
	if (TokenNEW *tn = dynamic_cast<TokenNEW *>(tb)) {
		DataDef *pattern_alloc = tn->alloc_type
				       ? tn->alloc_type : tn->alloc_class;
		if (m_tsubst_pattern_mode && tn->placement && pattern_alloc
		    && (template_param_under_type_layers(pattern_alloc)
			|| tsubst_args_have_pack_expansion(tn->ctor_args))) {
			cir_node *marker = make(N_IGNORE, tb);
			marker->datadef = pattern_alloc;
			return marker->as_node();
		}
		// -------- Placement new: `new (addr) T(args)` --------
		// Construct at the given address, no allocation. The placement
		// address is already T* (e.g. &data[len] where data is T*), so the
		// element lvalue is *addr and the class __this is addr directly.
		// Yields the address (a statement expression), like g++.
		if (tn->placement) {
			// A node must not appear twice in the tree; the placement
			// address is a pure expression, so re-translate per use.
			auto addr = [&]() -> node_t { return translate_expr(tn->placement); };
			node_t construct = NULL;
			if (tn->alloc_class) {
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
						bool refp = ctor && ctor->is_ref_param(pi);
						if (DataDefCLASS *pc = param_object_class(pt, refp))
							append(a, object_arg_addr(arg, pc));
						else if (DataDefCLASS *vc = as_class_instance(pt))
							append(a, object_arg_value(arg, vc));
						else if (refp)
							append(a, ref_param_arg_addr(arg,
								ref_param_referent(pt),
								const_ref_param(ctor, pi)));
						else
							append(a, translate_expr(arg));
					}
					if (ctor && ctor->ctor_trailing_self)
						append(a, addr());   // trailing allocator& == &this
					if (ctor && !ctor->emit_symbol.empty()) {
						std::vector<ExternParam> eparams;
						eparams.push_back({ {N_VOID}, true });
						for (size_t pi = 1; pi < ctor->parameters.size(); pi++) {
							DataDef *pt = ctor->parameters[pi];
							bool refp = ctor->is_ref_param(pi);
							if (param_object_class(pt, refp) || refp)
								eparams.push_back({ {N_VOID}, true });
							else if (pt && pt->is_pointer())
								eparams.push_back({ {N_CHAR}, true });
							else
								eparams.push_back({ {N_LONG}, false });
						}
						if (ctor->ctor_trailing_self)
							eparams.push_back({ {N_VOID}, true });
						need_output_extern(csym.c_str(), false, eparams);
					}
					construct = node2(N_CALL, id(csym.c_str(), tb), a, tb);
				}
			} else {
				// Scalar T: *(T *)addr = arg (or zero-init when no args).
				// Cast the placement address to the CONSTRUCTED type's
				// pointer before the store: placement new constructs a T at
				// the address regardless of the pointer's static type, and
				// libstdc++ writes `::new((void *)__p) _Up(...)` — storing
				// through the raw `(void *)` yields "assignment of incompatible
				// value". A no-op when addr is already T* (e.g. `new(&buf) int`).
				node_t rhs = tn->ctor_args.empty()
						? integer(0, tb) : translate_expr(tn->ctor_args[0]);
				int levels = 1;
				DataDef *t = tn->alloc_type;
				while (t && t->is_pointer()) {
					DataDefPTR *p = dynamic_cast<DataDefPTR *>(t);
					if (!p) break;
					t = p->base_type;
					levels++;
				}
				node_t decls = list();
				for (int i = 0; i < levels; i++) append(decls, pointer());
				node_t tptr = node2(N_TYPE, type_list(t),
						    node2(N_DECL, ignore(), decls));
				node_t typed_addr = node2(N_CAST, tptr, addr(), tb);
				construct = node2(N_ASSIGN, node1(N_DEREF, typed_addr, tb),
						  rhs, tb);
			}
			// ({ <construct>; addr; }) — yields the placement address (T*).
			node_t items = list();
			if (construct)
				append(items, node2(N_EXPR, list(), construct, tb));
			append(items, node2(N_EXPR, list(), addr(), tb));
			return node1(N_STMTEXPR, node2(N_BLOCK, list(), items, tb), tb);
		}

		DataDefCLASS *cdd = tn->alloc_class;
		if (!cdd) {
			// Scalar (non-class) new: `new T`, `new T(v)`, `new T[n]`.
			DataDef *et = tn->alloc_type;
			if (!et) return error_node("new without a class type", tb);
			auto t_ptr_type = [&]() -> node_t {
				return node2(N_TYPE, type_list(et),
					     node2(N_DECL, ignore(), node1(N_LIST, pointer())));
			};
			auto t_sizeof = [&]() -> node_t {
				return node1(N_SIZEOF,
					node2(N_TYPE, type_list(et),
					      node2(N_DECL, ignore(), list())), tb);
			};
			if (tn->array_size) {
				// new T[n] -> (T*)calloc((size_t)n, sizeof(T)). calloc value-
				// zeroes — a safe superset of new[]'s default-init for scalars.
				need_output_extern("calloc", true,
					{ { {N_UNSIGNED, N_LONG}, false },
					  { {N_UNSIGNED, N_LONG}, false } });
				node_t cargs = list();
				append(cargs, translate_expr(tn->array_size));
				append(cargs, t_sizeof());
				node_t ccall = node2(N_CALL, id("calloc", tb), cargs, tb);
				return node2(N_CAST, t_ptr_type(), ccall, tb);
			}
			// new T / new T(v) -> ({ T *__newN = (T*)malloc(sizeof(T));
			//                        [*__newN = v;] __newN; })
			need_output_extern("malloc", true,
				{ { {N_UNSIGNED, N_LONG}, false } });
			char stmp[32];
			snprintf(stmp, sizeof(stmp), "__new%d", m_strtmp_counter++);
			node_t margs = list();
			append(margs, t_sizeof());
			node_t mcall = node2(N_CALL, id("malloc", tb), margs, tb);
			node_t scast = node2(N_CAST, t_ptr_type(), mcall, tb);
			node_t sdecl = simple(N_SPEC_DECL);
			append(sdecl, node1(N_SHARE, type_list(et)));
			append(sdecl, node2(N_DECL, id(stmp, tb), node1(N_LIST, pointer())));
			append(sdecl, ignore());
			append(sdecl, ignore());
			append(sdecl, scast);
			node_t sitems = list();
			append(sitems, sdecl);
			if (!tn->ctor_args.empty()) {
				node_t store = node2(N_ASSIGN,
					node1(N_DEREF, id(stmp, tb), tb),
					translate_expr(tn->ctor_args[0]), tb);
				append(sitems, node2(N_EXPR, list(), store, tb));
			}
			append(sitems, node2(N_EXPR, list(), id(stmp, tb), tb));
			return node1(N_STMTEXPR,
				     node2(N_BLOCK, list(), sitems, tb), tb);
		}
		char tmp[32];
		snprintf(tmp, sizeof(tmp), "__new%d", m_strtmp_counter++);
		// struct C * type node, reused for the decl and the cast.
		auto ptr_type = [&]() -> node_t {
			return node2(N_TYPE,
				node1(N_LIST, class_tag_ref(cdd)),
				node2(N_DECL, ignore(), node1(N_LIST, pointer())));
		};
		auto struct_type = [&]() -> node_t {
			return node2(N_TYPE,
				node1(N_LIST, class_tag_ref(cdd)),
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
			class_tag_ref(cdd))));
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
				bool refp = ctor && ctor->is_ref_param(pi);
				if (DataDefCLASS *pc = param_object_class(pt, refp))
					append(cargs, object_arg_addr(arg, pc));
				else if (DataDefCLASS *vc = as_class_instance(pt))
					append(cargs, object_arg_value(arg, vc));
				else if (refp)
					append(cargs, ref_param_arg_addr(arg,
						ref_param_referent(pt), const_ref_param(ctor, pi)));
				else
					append(cargs, translate_expr(arg));
			}
			if (ctor && ctor->ctor_trailing_self)
				append(cargs, id(tmp, tb));   // trailing allocator& == this
			if (ctor && !ctor->emit_symbol.empty()) {
				std::vector<ExternParam> eparams;
				eparams.push_back({ {N_VOID}, true });
				for (size_t pi = 1; pi < ctor->parameters.size(); pi++) {
					DataDef *pt = ctor->parameters[pi];
					bool refp = ctor->is_ref_param(pi);
					if (param_object_class(pt, refp) || refp)
						eparams.push_back({ {N_VOID}, true });
					else if (pt && pt->is_pointer())
						eparams.push_back({ {N_CHAR}, true });
					else
						eparams.push_back({ {N_LONG}, false });
				}
				if (ctor->ctor_trailing_self)
					eparams.push_back({ {N_VOID}, true });
				need_output_extern(csym.c_str(), false, eparams);
			}
			node_t ccall = node2(N_CALL, id(csym.c_str(), tb), cargs, tb);
			append(items, node2(N_EXPR, list(), ccall, tb));
		} else if (cdd->has_vtable) {
			// No user constructor, but a virtual (polymorphic) class: calloc
			// zeroes the storage, leaving __vptr NULL — a virtual call would
			// deref NULL and crash. A user ctor's body sets __vptr (func_def
			// prologue); with no ctor we must set it here so `new B()` yields a
			// usable polymorphic object. `__newN->__vptr = (void*)B__vtable`.
			std::string vname = class_vtable_symbol(cdd);
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
		if (cdd && !tdl->is_array && cdd->vtable_slot("~$deleting") >= 0) {
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
		// Array delete of a class with a dtor needs the new[] element-count
		// cookie to run the dtor per element; madc's `new` allocates a single
		// object (no array cookie), so for `delete[]` we free only (running a
		// single dtor on element[0] would be wrong). Trivial element types
		// (cdd==NULL) free either way. See feature-drops-audit (delete[] row).
		if (cdd && cdd->has_user_dtor && !tdl->is_array) {
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

	// obj.~T() / ptr->~T() — explicit/pseudo destructor call. Run T's complete
	// destructor on the object (no free(), unlike `delete`). The `this` is the
	// lhs pointer for `->`, the lhs object's address for `.`. When T is trivial
	// (class_needs_dtor false) or scalar (dtor_class NULL), it is a no-op — but
	// the object expression is still evaluated for side effects. Void-valued.
	if (TokenExplicitDtor *ted = dynamic_cast<TokenExplicitDtor *>(tb)) {
		if (m_tsubst_pattern_mode) {
			if (DataDef *marker_dd =
				    tsubst_explicit_dtor_marker_datadef(ted)) {
				cir_node *marker = make(N_IGNORE, tb);
				marker->datadef = marker_dd;
				return marker->as_node();
			}
		}
		DataDefCLASS *cdd = ted->dtor_class;
		node_t obj_ptr = ted->is_arrow
			? translate_expr(ted->obj)
			: node1(N_ADDR, translate_expr(ted->obj), tb);
		if (cdd && class_needs_dtor(cdd)) {
			std::string dsym = class_complete_dtor_symbol(cdd);
			referenced_funcs.insert(dsym);
			node_t dargs = list();
			append(dargs, obj_ptr);
			return node2(N_CALL, id(dsym.c_str(), tb), dargs, tb);
		}
		node_t items = list();
		append(items, node2(N_EXPR, list(), obj_ptr, tb));
		append(items, node2(N_EXPR, list(), integer(0, tb), tb));
		node_t block = node2(N_BLOCK, list(), items, tb);
		return node1(N_STMTEXPR, block, tb);
	}

	// String literal
	if (tb->type() == TokenType::ttString) {
		TokenStr *ti = dynamic_cast<TokenStr *>(tb);
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
			// EXCEPTION: vfCONSTBAKED marks the const-declared scalar whose
			// parse-time-known initializer WAS set() into data (an integral
			// constant expression, C++ [expr.const]) — fold it like an enum
			// value; this is what lets `const int H = G + 3;` emit a constant
			// file-scope initializer c2mir accepts.
			if (tv->var.is_constant() && tv->var.data
			    && (!(tv->var.flags & vfCONSTDECL)
				|| (tv->var.flags & vfCONSTBAKED)) && tv->var.type
			    && tv->var.type->is_integer() && !tv->var.type->is_pointer())
				return integer(tv->var.get<int64_t>(), tb);
			if (tv->var.name.compare(0, 11, "__literal__") == 0) {
				const std::string &content = tv->var.name.substr(11);
				return str(content.c_str(), content.size() + 1, tb);
			}
			// The same fold for a set()-valued `const char *` constant: a
			// host-installed expression/scope binding (libmadc) stores the
			// bound C-string pointer into var->data and marks the variable
			// constant. That pointer targets HOST memory the compiled
			// module cannot reference symbolically — left as a variable
			// read, the global emits as an undefined extern import and
			// MIR_link fails. The binding is a read-only snapshot by
			// contract, so bake it: fold the read to a string literal.
			if (tv->var.is_constant() && tv->var.data
			    && !(tv->var.flags & vfCONSTDECL) && tv->var.type
			    && tv->var.type->is_cstr()) {
				const char *text = *(const char **)tv->var.data;
				if (text)
					return str(text, strlen(text) + 1, tb);
			}
			// Value-use of a GNU nested function's in-scope alias (taking its
			// address / passing it as a callback) names the HOISTED symbol. Only
			// for a capture-free nested fn: a capturing one cannot be a plain
			// function pointer (no slot for the captures — GCC uses a trampoline),
			// so leaving the short name there yields a clean c2mir error rather
			// than a callback that reads garbage captures.
			if (FuncDef *nfd = dynamic_cast<FuncDef *>(tv->var.type)) {
				// A function used as a VALUE (address-taken / function-pointer
				// decay: `&abort`, `f = exit`, `{abort}`, or the hoisted symbol of
				// a capture-free GNU nested fn). Emit its call symbol and record it
				// so translate_module emits a prototype — without it c2mir sees an
				// "undeclared identifier" (call sites already record callees; a bare
				// value-use must too). EXCEPTION: a CAPTURING nested fn cannot be a
				// plain function pointer (no slot for the captures — GCC uses a
				// trampoline), so leave the short source name there to yield a clean
				// c2mir error rather than a callback that reads garbage captures.
				bool capturing_nested = !nfd->local_emit_name.empty()
							&& !nfd->captured_vars.empty();
				if (!capturing_nested) {
					std::string sym = call_emit_symbol(tv->var, nfd);
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
			if ((tv->var.is_reference()) && tv->var.type
			    && tv->var.type->is_pointer()) {
				// A [&]-captured reference: the hidden capture parameter is a
				// `referent *` already holding the same address the reference
				// stores (capture_param_type), so `(*name)` reads the referent
				// unchanged. Record the capture (so the parameter is synthesized
				// and the call site forwards the reference's pointer value); this
				// must run BEFORE the early return, or the reference path would
				// bypass note_capture and the captured reference would be left
				// undeclared in the body.
				note_capture(&tv->var);
				return node1(N_DEREF, id(tv->var.name.c_str(), tb), tb);
			}
			// Two-tree tsubst: while building a dependent-pattern body
			// (m_tsubst_pattern_mode), the body-bound TokenVar may have lost the
			// reference wrapper the method's PARAMETER still carries — the
			// dependent parse binds the body identifier to a non-reference
			// Variable while the recipe FuncDef records the param as a DataDefREF.
			// Without the deref a value-read of a reference parameter (`*p = a`
			// where `a` is `int& a`) emits the bare pointer id, so the
			// instantiated body becomes `*p = a` (pointer-into-int) instead of
			// `*p = *a`. Consult the current method's real parameter list (the
			// same source of truth arg_spelling uses in member_template_method_call)
			// and deref a value-read of a by-reference parameter.
			if (m_tsubst_pattern_mode && m_cur_method
			    && !tv->var.is_reference()) {
				for (Variable *pv : m_cur_method->parameters)
					if (pv && pv->is_reference()
					    && pv->type && pv->type->is_pointer()
					    && pv->name == tv->var.name)
						return node1(N_DEREF,
							id(tv->var.name.c_str(), tb), tb);
			}
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
			return id(ti->spelling(), tb);
	}

	// Ternary
	{
		TokenTerQ *tq = dynamic_cast<TokenTerQ *>(tb);
		if (tq) {
			node_t cond = translate_expr(tq->condition);
			// A throw-expression branch ([expr.cond]/2): the conditional's
			// type is the OTHER (non-throw) branch's; the throw branch is a
			// void prvalue that never returns. c2mir's N_COND needs both
			// branches type-compatible, so wrap the throw side in a comma
			// yielding the other branch's type — `c ? (throw, x) : x`. The
			// comma's right operand is unreachable at runtime (the throw
			// transfers control); it only carries the type. Without this the
			// void-typed N_COND branch null-derefs downstream.
			bool t_throw = dynamic_cast<TokenTHROW *>(tq->true_expr) != NULL;
			bool f_throw = dynamic_cast<TokenTHROW *>(tq->false_expr) != NULL;
			if (t_throw && !f_throw)
				return node3(N_COND, cond,
					node2(N_COMMA, translate_expr(tq->true_expr),
					      translate_expr(tq->false_expr), tb),
					translate_expr(tq->false_expr), tb);
			if (f_throw && !t_throw)
				return node3(N_COND, cond,
					translate_expr(tq->true_expr),
					node2(N_COMMA, translate_expr(tq->false_expr),
					      translate_expr(tq->true_expr), tb), tb);
			return node3(N_COND, cond,
				translate_expr(tq->true_expr),
				translate_expr(tq->false_expr), tb);
		}
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
			if (ta->var.is_reference())
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
			// operator[] on a class-object SUB-EXPRESSION receiver
			// (`(*this)[n]` in vector::at, `__this->_M_current[n]` in a
			// move_iterator) — dispatch to the class's operator[] like the
			// named-variable TokenSubscript path, instead of a raw N_IND that
			// c2mir rejects ("subscripted value is neither array nor pointer").
			if (DataDefCLASS *bcls = as_class_instance(
				    tse->base_expr ? tse->base_expr->datadef() : NULL)) {
				std::string subop = "operator[]";
				Variable *mv = bcls->findMethod(subop);
				FuncDef *callee = mv ? dynamic_cast<FuncDef *>(mv->type) : NULL;
				if (callee) {
					node_t recv_addr = object_arg_addr(tse->base_expr, bcls);
					node_t addr = class_subscript_addr_on(bcls, recv_addr,
									      tse->index, tb);
					if (addr)
						// operator[] returns T& -> a pointer; deref to the
						// element lvalue (mirror of class_subscript_call).
						return callee->returns_reference()
						       ? node1(N_DEREF, addr, tb) : addr;
				}
			}
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
	// below — otherwise `obj.method()` can mis-lower to a bare field access.
	// Header-declared external methods route through class_method_call via the
	// FuncDef's C++ emit_symbol; each lowering helper returns NULL for an
	// unrecognized receiver so the others fall through.
	if (tb->type() == TokenType::ttCallMethod) {
		TokenMember *tcm = dynamic_cast<TokenMember *>(tb);
		if (tcm) {
			// Class method call: c.method(args) -> the emit_symbol /
			// ClassName__method call. Returns NULL when the receiver is not a
			// class instance.
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
			node_t fld = node2(code, obj, member, tb);
			// A SCALAR reference MEMBER (`int& m`, lowered to a pointer slot)
			// denotes its REFERENT lvalue in every value use — read through the
			// pointer (`this->m` -> `*this->m`), exactly like a scalar reference
			// variable/parameter (the is_reference() arm in the variable read
			// above). Without this, returning a scalar reference member from a
			// T&-returning function took `&(this->m)` — a T** where T* was
			// expected ("incompatible return-expr type in function returning a
			// pointer"; std::tuple<const int&>'s _Head_base::_M_head_impl).
			// A CLASS reference member (`A& a`, the _Auto_node `_Rb_tree& _M_t`
			// shape) is EXCLUDED: its member/method access (`a.v` / `a.f()`)
			// already resolves through the referent via the ptr_like -> '->'
			// path, so a read-deref here would double-dereference (testrefmember).
			// Deref ONLY a SCALAR reference member (`int& m`, `T*& m`) read as
			// a value: its referent lowers to a register value, so the value use
			// reads through the pointer (`this->m` -> `*this->m`), mirroring the
			// scalar reference-variable arm above. An AGGREGATE reference member
			// (`struct&`/`class&`) is EXCLUDED — its member/method access
			// (`a.v` / `a.f()`) already unwraps the pointer via the ptr_like ->
			// '->' path, so a read-deref here would double-dereference. Test the
			// REFERENT's scalar-ness directly, not class-ness: a reference to a
			// plain `struct A` (no object members) is a DataDefSTRUCT, for which
			// class_behind() answers NULL (testrefmember's `A& a`).
			if (reference_member_value_use_deref(tm))
				return node1(N_DEREF, fld, tb);
			return fld;
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
			// A cast to REFERENCE type is a no-op on the operand
			// OBJECT ([expr.static.cast]p3 — static_cast<T&&>(x) IS
			// x): emit the operand lvalue unchanged so `&` /
			// ref-return lowerings still see an addressable object.
			if (cast_dd && cast_dd->is_reference())
				return translate_expr(tc->expr);
			if (DataDefCLASS *cast_class = as_class_instance(cast_dd))
				return object_arg_value(tc->expr, cast_class);
			// Function-pointer cast `(RET (*)(params)) expr` (qsort comparator,
			// atexit handler, ...). The generic scalar/pointer path below renders
			// a DataDefFPTR via type_list as a bare `long`, so the cast emitted as
			// `(long)expr` — an integer passed to a pointer parameter. Build the
			// real `RET (*)(params)` type node from the target signature instead.
			if (DataDefFPTR *fp = dynamic_cast<DataDefFPTR *>(cast_dd)) {
				node_t fspec = list();
				node_t fdecl_list = list();
				fnptr_decl_pieces(fp->target, true, fspec, fdecl_list,
						  std::vector<carray_dim_t>());
				node_t type_node = node2(N_TYPE, fspec,
					node2(N_DECL, ignore(), fdecl_list));
				return node2(N_CAST, type_node,
					     translate_expr(tc->expr), tb);
			}
			bool cast_is_ptr = cast_dd && cast_dd->is_pointer();
			// Peel ALL pointer levels and emit that many '*' — a `(char **)`
			// cast must not collapse to `(char *)` (which mismatches a char**
			// target: "incompatible types in assignment to a pointer"). Only
			// DataDefPTR chains carry stackable levels; a non-DataDefPTR
			// pointer (e.g. DataDefFPTR) keeps the single-pointer fallback.
			int cast_ptr_levels = 0;
			while (cast_dd) {
				DataDefPTR *p = dynamic_cast<DataDefPTR *>(cast_dd);
				if (!p || !p->base_type) break;
				cast_dd = p->base_type;
				cast_ptr_levels++;
			}

			node_t tl = type_list(cast_dd);
			node_t cast_decl_list = list();
			if (cast_ptr_levels > 0) {
				for (int s = 0; s < cast_ptr_levels; s++)
					append(cast_decl_list, pointer());
			} else if (cast_is_ptr) {
				append(cast_decl_list, pointer());
			}
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
					// type()==ttVariable, not a TokenVar downcast:
					// TokenMember/TokenCallFunc derive from TokenVar
					// (see class_unary_operator_call's receiver).
					node_t this_arg;
					TokenVar *otv = dynamic_cast<TokenVar *>(operand_tb);
					if (otv && operand_tb->type() == TokenType::ttVariable)
						this_arg = object_var_addr(otv->var, tb);
					else
						this_arg = object_arg_addr(operand_tb, icls);
					std::string sym = call_emit_symbol(post, icls->name + "__" + opmname);
					referenced_funcs.insert(sym);
					node_t pargs = list();
					append(pargs, this_arg);
					append(pargs, integer(0, tb));   // dummy int (postfix marker)
					node_t pcall = node2(N_CALL, id(sym.c_str(), tb), pargs, tb);
					CIR_NODE(pcall)->synth_from_origin = true;
					if (post->returns_reference())
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

			// Operator overloading on a class lvalue: `c <op> rhs` where c's
			// class defines `operator<op>` lowers to the operator call. For
			// header-declared external classes this uses the mangled C++ symbol
			// carried by the FuncDef; user classes use the emitted method body.
			// The overload is selected by RHS type.
			{
				node_t ov = class_operator_call(top, tb);
				if (ov) return ov;
			}
			// Pattern-mode operator deferral: if generic operator resolution
			// above cannot materialize a class/operator call, keep the Tree-1
			// recipe incomplete and fall back rather than emitting a builtin
			// operation on an object representation. Operators that can be
			// resolved by kind (member, external, or retained free-template body)
			// have already returned.
			if (m_tsubst_pattern_mode && !is_assign_op(tb->id())) {
				if (operand_object_class(top->left)
				    || operand_object_class(top->right))
					return error_node(
						"tsubst: unresolved class operator in pattern",
						tb);
			}

			// Implicit memberwise copy-ASSIGNMENT for a NON-TRIVIAL class
			// (`lhs = rhs`, both the same class needing a dtor, declaring no
			// user operator= — class_operator_call returned NULL above, so a
			// user-declared operator= still wins). A bit-copy N_ASSIGN would
			// alias object-member resources and double-free at cleanup;
			// assign each member instead. Trivial classes fall through to the
			// native bit-copy. The statement-expr yields the lhs object.
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

			// C++20 builtin <=> ([expr.spaceship]): category-typed temp +
			// inline byte-select (class operands with operator<=> were
			// dispatched by class_operator_call / the parse-time free-
			// operator lowering above).
			if (tb->id() == TokenID::tk3Way)
				return three_way_builtin_lowering(top, tb);

			// madc dialect strict equality === / !== — spec
			// docs/superpowers/specs/2026-06-11-strict-equality-design.md.
			// (A user operator===/!== was already dispatched by
			// class_operator_call above.)
			if (tb->id() == TokenID::tk3Eq || tb->id() == TokenID::tk3NotEq)
				return strict_equality_lowering(top, tb);

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
			default: {
				// No CIR lowering for this operator. Previously this
				// silently fell back to N_ADD — `a <=> b` compiled as
				// a + b. Reject via the pre-c2mir gate instead (same
				// policy as the unhandled-expression tail).
				char buf[160];
				snprintf(buf, sizeof(buf),
					 "unhandled binary operator: %s (CIR builder "
					 "has no lowering for token id %d)",
					 describe_token(tb).c_str(), (int)tb->id());
				return error_node(buf, tb);
			}
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
			if (is_constant_evaluated_call(tcf))
				return integer(0, tb);
			FuncDef *inline_fd = call_target_funcdef(tcf);
			if (((inline_fd && inline_fd->inline_builtin_kind == "addressof")
			     || tcf->var.name == "__builtin_addressof")
			    && tcf->parameters.size() == 1) {
				return node1(N_ADDR, translate_expr(tcf->parameters[0]), tb);
			}
			auto lower_destroy_arg = [&](TokenBase *parg,
						     bool allow_deferred_marker) -> node_t {
				DataDef *argdd = parg ? parg->datadef() : NULL;
				if (allow_deferred_marker && m_tsubst_pattern_mode) {
					DataDef *marker_dd =
						tsubst_destroy_marker_datadef(parg, argdd);
					if (marker_dd) {
						cir_node *marker = make(N_IGNORE, tb);
						marker->datadef = marker_dd;
						return marker->as_node();
					}
				}
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
			};
			if (inline_fd && inline_fd->inline_builtin_kind == "destroy"
			    && tcf->parameters.size() == 1)
				return lower_destroy_arg(tcf->parameters[0], true);
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
			// __atomic_thread_fence(m) / __atomic_fetch_add(p,v,m): gcc/clang
			// INLINE these; c2mir emits unresolvable external calls. Lower to
			// the gcc-compiled madc runtime wrappers (real fence / atomic add).
			// Tier-1 (madc owns the front end — lowering-vs-raising.md), same
			// as __builtin_conj above. libstdc++ <ext/atomicity.h> refcount
			// path (mem barriers + __exchange_and_add) -> the vector chain.
			if ((tcf->var.name == "__atomic_thread_fence"
			     || tcf->var.name == "__atomic_signal_fence")
			    && tcf->parameters.size() == 1) {
				const char *fsym =
				    tcf->var.name == "__atomic_signal_fence"
					? "__madc_atomic_signal_fence"
					: "__madc_atomic_thread_fence";
				need_output_extern(fsym, false, { { {N_INT}, false } });
				node_t a = list();
				append(a, translate_expr(tcf->parameters[0]));
				return node2(N_CALL, id(fsym, tb), a, tb);
			}
			if (tcf->var.name == "__atomic_fetch_add"
			    && tcf->parameters.size() == 3) {
				// Atomic add returning the OLD value. Pick the width-specific
				// wrapper from the pointee type of `p` (int _Atomic_word is the
				// common refcount case; long for an 8-byte target).
				DataDef *pd = tcf->parameters[0]
					? tcf->parameters[0]->datadef() : NULL;
				DataDef *pointee = (pd && pd->is_pointer())
					? static_cast<DataDefPTR *>(pd)->base_type : NULL;
				bool wide = pointee && pointee->size == 8;
				const char *sym = wide ? "__madc_atomic_fetch_add_l"
						       : "__madc_atomic_fetch_add_i";
				c2mir_node_code_t w = wide ? N_LONG : N_INT;
				need_output_extern(sym, false,
					{ { {w}, true }, { {w}, false }, { {N_INT}, false } },
					{ w });
				node_t a = list();
				for (size_t i = 0; i < 3; i++)
					append(a, translate_expr(tcf->parameters[i]));
				return node2(N_CALL, id(sym, tb), a, tb);
			}
			// __destroy(ptr): compiler intrinsic — destruct the pointed-to
			// object element. Lowers to the ELEMENT TYPE's class destructor
			// (external C++ symbol or madc-emitted user-class dtor), or to
			// NOTHING for a scalar/pointer element type (no dtor). Generic and
			// element-type-driven so header templates can destruct live elements
			// before free() for any T while the same body is a no-op for scalar
			// elements. The argument
			// keeps its original (uncoerced) type, so `data + i` reports the
			// real `T*`; the pointed-to T is base_type of that DataDefPTR.
			if (tcf->var.name == "__destroy" && tcf->parameters.size() == 1)
				return lower_destroy_arg(tcf->parameters[0], true);
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
					// printstr/puts take char*: object values need a
					// character-pointer view; literals are already const char*.
					if (is_class_object_value(p))
						append(a, object_cstr_arg(p));
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
			if (tcf->src_node && !dynamic_cast<FuncDef *>(tcf->var.type)) {
				func_id = translate_expr(tcf->src_node);
			} else {
				// A call to a GNU nested function resolves the in-scope
				// source-named alias, but the function is HOISTED to a unique
				// top-level symbol — emit that hoisted name (FuncDef::
				// local_emit_name), not the source name.
				FuncDef *cdf = NULL;
				std::string callee_name = call_target_emit_name(tcf, &cdf);
				Method *native_method = (Method *)tcf->var.data;
				if (tcf->var.name == "system" && cdf
				    && native_method && native_method->x86code && m_prog) {
					auto sit = m_prog->external_symbol_map.find(
						reinterpret_cast<uintptr_t>(native_method->x86code));
					if (sit != m_prog->external_symbol_map.end()
					    && !sit->second.empty()) {
						callee_name = sit->second;
						bool ret_ptr = false;
						std::vector<c2mir_node_code_t> ret_specs;
						std::vector<ExternParam> eparams;
						native_func_shape(cdf, ret_ptr, ret_specs, eparams);
						need_output_extern(callee_name.c_str(), ret_ptr,
								   eparams, ret_specs);
					}
				}
				// c2mir intrinsics (e.g. __builtin_va_start) are
				// recognized by name and lowered in-place; emitting an
				// extern prototype for one shadows the intrinsic and
				// turns it into an undefined external symbol at MIR-link.
				// So skip the proto for them. (__builtin_va_arg has its
				// own translation path above.)
				if (!is_c2mir_builtin_call_name(tcf->var.name))
					referenced_funcs.insert(callee_name);
				func_id = id(callee_name.c_str(), tb);
			}
			// A by-value NON-TRIVIAL class-returning call uses the same __retbuf
			// ABI: materialize a cleanup-tagged temp of the class, emit the void
			// call writing into it, and yield the temp as a `struct Cls` lvalue
			// (`*(struct Cls*)&temp`) — so member access / further use reads it.
			if (DataDefCLASS *ocls = object_returning_call_class(tcf)) {
				node_t addr = object_call_temp_addr(tcf, ocls, tb);
				node_t cp_type = node2(N_TYPE,
					node1(N_LIST, class_tag_ref(ocls, tb)),
					node2(N_DECL, ignore(), node1(N_LIST, pointer())));
				node_t as_cp = node2(N_CAST, cp_type, addr, tb);
				return node1(N_DEREF, as_cp, tb);
			}
			node_t args = list();
			build_call_args(tcf, args);
			node_t call = node2(N_CALL, func_id, args, tb);
			FuncDef *cdf = call_target_funcdef(tcf);
			if (cdf && cdf->returns_reference())
				return node1(N_DEREF, call, tb);
			return call;
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

	// Runtime-eval scope capture (the parser appends a TokenScopeContext
	// argument to the madc:: *_ctx publics when scope access is enabled —
	// see parseCallFunc). The context_var is a parser-allocated `array`
	// LOCAL of the enclosing compound (declared/constructed by the normal
	// block lowering); fill it with the captured scope variables' CURRENT
	// values via the cstr-key runtime setters (the captured key set is
	// fixed per call site, so values simply overwrite on re-entry — no
	// reset needed, unlike the asmjit compile which reconstructed), then
	// the expression value is the ctx array LVALUE: the call's
	// reference-parameter emission takes its address for the publics'
	// `array &` parameter (an extra N_ADDR here double-wrapped it and the
	// host read a pointer slot as a null value), and a pointer-shaped
	// legacy consumer gets the same address via C array decay.
	if (TokenScopeContext *tsc = dynamic_cast<TokenScopeContext *>(tb)) {
		const char *cn = tsc->context_var.name.c_str();
		for (Variable *sv : tsc->scope_vars) {
			if (!sv || !sv->type) continue;
			const char *setter;
			node_t val;
			ExternParam val_shape;
			DataType raw = sv->type->rawtype();
			if (sv->type->is_pointer()) {
				continue;   // a pointer is not a capturable value
			} else if (raw == DataType::dtBOOL || sv->type->is_integer()) {
				setter = "__madc_scope_set_int_runtime";
				val = id(sv->name.c_str(), tb);
				val_shape = { {N_LONG}, false };
			} else if (sv->type->is_real()) {
				setter = "__madc_scope_set_real_runtime";
				val = id(sv->name.c_str(), tb);
				val_shape = { {N_DOUBLE}, false };
			} else if (sv->type->marshals_value_text()) {
				// the host reads the text object by pointer
				setter = "__madc_scope_set_string_runtime";
				val = node1(N_ADDR, id(sv->name.c_str(), tb), tb);
				val_shape = { {N_VOID}, true };
			} else if (raw == DataType::dtARRAY) {
				setter = "__madc_scope_set_array_runtime";
				val = node1(N_ADDR, id(sv->name.c_str(), tb), tb);
				val_shape = { {N_VOID}, true };
			} else {
				continue;   // collector admits bool/int/real/text/array
			}
			need_output_extern(setter, /*ret_ptr*/true,
					   { { {N_VOID}, true },
					     { {N_CHAR}, true }, val_shape });
			node_t args = list();
			append(args, node1(N_ADDR, id(cn, tb), tb));
			append(args, str(sv->name.c_str(), sv->name.size() + 1, tb));
			append(args, val);
			node_t call = node2(N_CALL, id(setter, tb), args, tb);
			CIR_NODE(call)->synth_from_origin = true;
			m_pending_stmts.push_back(node2(N_EXPR, list(), call, tb));
		}
		return id(cn, tb);
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
			node_t expr = translate_expr(tr->returns);
			for (node_t p : m_pending_stmts)
				append(items, p);
			m_pending_stmts.clear();
			append(items, node2(N_EXPR, list(), expr));
			append_deferred_stmts(items, 0);
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
		append_deferred_stmts(items, 0);
		append(items, node2(N_RETURN, list(), ignore(), tr));
		return node2(N_BLOCK, list(), items, tr);
	}
	// A by-value NON-TRIVIAL class return uses the __retbuf ABI: deep-copy
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
		append_deferred_stmts(items, 0);
		append(items, node2(N_RETURN, list(), ignore(), tr));
		return node2(N_BLOCK, list(), items, tr);
	}
	// Return-value copy-initialization ([stmt.return]/[dcl.init]): a function
	// returning a class BY VALUE via c2mir's native struct return (trivially
	// copyable — the non-trivial case took the __retbuf arm above) whose
	// `return expr;` operand is a DIFFERENT class (or a scalar) accepted by a
	// converting constructor must construct the result object via that ctor,
	// not emit the raw (wrong-typed) expression. Mirrors object_arg_value's
	// parameter copy-initialization. This is std::set's
	// `iterator find(){ return _M_t.find(x); }` (tree `iterator` ->
	// `const_iterator`); without it c2mir rejects "incompatible return-expr
	// type in function returning a struct/union".
	if (m_cur_func_returns_value_class && tr->returns) {
		DataDefCLASS *rc = m_cur_func_returns_value_class;
		DataDefCLASS *ec = operand_object_class(tr->returns);
		if (ec != rc) {
			std::vector<TokenBase *> ctor_args;
			ctor_args.push_back(tr->returns);
			if (select_ctor_overload(rc, ctor_args)) {
				char name[32];
				snprintf(name, sizeof(name), "__madc_retconv_%d",
					 m_strtmp_counter++);
				Variable *tmp = new Variable(name, *rc, 1, NULL, false);
				tmp->flags |= vfLOCAL;
				m_pending_stmts.push_back(var_decl(tmp, tr));
				node_t cc = class_ctor_call(tmp, rc, ctor_args, tr);
				if (cc) m_pending_stmts.push_back(cc);
				node_t items = list();
				for (node_t p : m_pending_stmts)
					append(items, p);
				m_pending_stmts.clear();
				append_deferred_stmts(items, 0);
				append(items, node2(N_RETURN, list(), id(name, tr), tr));
				return node2(N_BLOCK, list(), items, tr);
			}
		}
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
	if (m_cur_func_returns_ref && tr->returns) {
		if (!reference_member_value_is_stored_address(tr->returns))
			expr = node1(N_ADDR, expr, tr);
		if (m_cur_func_returns_class_ptr)
			expr = upcast_class_ref_addr(expr, m_cur_func_returns_class_ptr,
						     tr->returns, tr);
	}
	// Derived->base pointer return (`A *f() { return bptr; }`): make the
	// implicit upcast explicit so c2mir does not warn.
	else if (m_cur_func_returns_class_ptr && tr->returns) {
		DataDefCLASS *base = m_cur_func_returns_class_ptr;
		DataDefCLASS *derived = expr_pointee_class(tr->returns);
		if (base && derived && base != derived
		    && derived->is_or_derives_from(base))
			expr = node2(N_CAST, class_ptr_type(base), expr, tr);
	}
	// Pending `defer`red statements run between the return expression's
	// evaluation and the actual return (old-backend/Go ordering): hoist the
	// value into a temp of the function's C return type, run the deferred
	// statements, return the temp. A valueless return (or one with no
	// hoistable C return type) just emits the deferred statements first —
	// the synthesized zero-fill expr is a constant, so ordering is moot.
	if (!m_defer_scopes.empty()) {
		node_t items = list();
		for (node_t p : m_pending_stmts)
			append(items, p);
		m_pending_stmts.clear();
		if (tr->returns && m_cur_func_ret_spec_dd) {
			char tmp[48];
			snprintf(tmp, sizeof(tmp), "__madc_defer_ret%d",
				 m_defer_tmp_counter++);
			node_t sd = simple(N_SPEC_DECL, tr);
			append(sd, m_cur_func_ret_spec_alias.empty()
				   ? type_list(m_cur_func_ret_spec_dd)
				   : type_list(m_cur_func_ret_spec_dd,
					       m_cur_func_ret_spec_alias));
			node_t dl = list();
			for (int rs = 0; rs < m_cur_func_ret_stars; rs++)
				append(dl, pointer());
			append(sd, node2(N_DECL, id(tmp, tr), dl));
			append(sd, ignore());
			append(sd, ignore());
			append(sd, expr);
			append(items, sd);
			append_deferred_stmts(items, 0);
			append(items, node2(N_RETURN, list(), id(tmp, tr), tr));
		} else {
			append_deferred_stmts(items, 0);
			append(items, node2(N_RETURN, list(), expr, tr));
		}
		return node2(N_BLOCK, list(), items, tr);
	}
	if (!m_pending_stmts.empty()) {
		node_t items = list();
		for (node_t p : m_pending_stmts)
			append(items, p);
		m_pending_stmts.clear();
		append(items, node2(N_RETURN, list(), expr, tr));
		return node2(N_BLOCK, list(), items, tr);
	}
	return node2(N_RETURN, list(), expr, tr);
}

node_t CirBuilder::translate_branch_stmt(TokenBase *tb)
{
	if (!tb) return ignore();
	node_t body = translate_stmt_required(tb);
	// A compound branch flushed its own temps inside translate_block, leaving
	// m_pending_stmts empty. A NON-compound branch (a bare expression-statement
	// such as `_M_move_assign(std::move(__x), true_type());`) leaves the temps
	// it materialized (the `true_type()` temp) on m_pending_stmts — they must be
	// scoped to THIS branch. Without the wrap they leak past the branch and get
	// swept into the next translated block's flush (a then-branch temp landing,
	// undeclared at its use, inside the else block — real vector
	// _M_move_assign(false_type)). Wrap temps + statement into one block: the
	// temps are declared before the statement and cleaned up at branch exit.
	if (m_pending_stmts.empty())
		return body;
	std::vector<node_t> temps;
	flush_pending_stmts(temps);
	node_t items = list();
	for (node_t p : temps)
		append(items, p);
	append(items, body);
	return node2(N_BLOCK, list(), items, tb);
}

// A loop body's own temporaries (`W(i)` in `for(...) take(W(i));`) must be
// constructed INSIDE the body so they re-run each iteration. translate_branch_stmt
// wraps a non-compound body's pending temps into a block — but a loop's init /
// cond / incr are translated BEFORE the body and may have left their own temps
// pending; those must NOT be swept into the body wrap (they belong to the
// enclosing scope, as before). Stash them across the body translation, then
// restore so the enclosing block still flushes them. (translate_branch_stmt
// leaves m_pending_stmts empty, so a plain restore is exact.)
node_t CirBuilder::translate_loop_body(TokenBase *tb)
{
	std::vector<node_t> saved;
	saved.swap(m_pending_stmts);
	node_t body = translate_branch_stmt(tb);
	m_pending_stmts = saved;
	return body;
}

node_t CirBuilder::translate_if(TokenIF *ti)
{
	// C++17 init-statement: `if (init; cond) S else E` lowers to
	// `{ init; if (cond) S else E }` — the init-statement (and its temps)
	// run before the condition and share the same enclosing block scope.
	if (!ti->init_stmt)
		return translate_if_core(ti);
	node_t init = translate_stmt(ti->init_stmt);
	std::vector<node_t> init_temps;
	flush_pending_stmts(init_temps);
	node_t core = translate_if_core(ti);
	node_t items = list();
	for (node_t p : init_temps)
		append(items, p);
	if (init)
		append(items, init);
	append(items, core);
	return node2(N_BLOCK, list(), items, ti);
}

node_t CirBuilder::translate_if_core(TokenIF *ti)
{
	if (ti->condition_decl) {
		node_t cond = translate_expr(ti->condition);
		// Temps materialized by the CONDITION (m_pending_stmts) must be
		// declared before the IF — otherwise the then/else block's own
		// statement loop swallows them into the wrong scope.
		std::vector<node_t> cond_temps;
		flush_pending_stmts(cond_temps);
		node_t then_body = translate_branch_stmt(ti->statement);
		node_t else_body = ti->elsestmt ? translate_branch_stmt(ti->elsestmt) : ignore();
		node_t if_node = node4(N_IF, list(), cond, then_body, else_body, ti);
		node_t items = list();
		node_t decl = translate_stmt(ti->condition_decl);
		if (decl) append(items, decl);
		for (node_t p : cond_temps)
			append(items, p);
		append(items, if_node);
		return node2(N_BLOCK, list(), items, ti);
	}
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
	if (is_constant_evaluated_call(ti->condition))
		return ti->elsestmt ? translate_stmt_required(ti->elsestmt) : ignore();
	node_t cond = translate_expr(ti->condition);
	// Temps materialized by the CONDITION must be emitted ahead of the IF
	// (see the condition_decl arm above).
	std::vector<node_t> cond_temps;
	flush_pending_stmts(cond_temps);
	node_t then_body = translate_branch_stmt(ti->statement);
	node_t else_body = ti->elsestmt ? translate_branch_stmt(ti->elsestmt) : ignore();
	node_t if_node = node4(N_IF, list(), cond, then_body, else_body, ti);
	if (cond_temps.empty())
		return if_node;
	node_t items = list();
	for (node_t p : cond_temps)
		append(items, p);
	append(items, if_node);
	return node2(N_BLOCK, list(), items, ti);
}

node_t CirBuilder::translate_while(TokenBase *tw)
{
	TokenWHILE *w = dynamic_cast<TokenWHILE *>(tw);
	if (!w) return ignore();
	node_t cond = translate_expr(w->condition);
	return node3(N_WHILE, list(), cond,
		     translate_loop_body(w->statement), tw);
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
		if (td && !tf->init_extras.empty()) {
			// Typed multi-declarator init `for (int x=1, y=2, z=3; ...)`.
			// Every declarator's variable is hoisted into the enclosing scope
			// (the single-declarator case hoists too — `int i;` precedes the
			// loop), so the for-init's job is purely to INITIALIZE. Lower it as
			// the comma of the declarators' initializing assignments: a bare
			// var_decl (N_SPEC_DECL) can't be folded into an N_COMMA, and no
			// multi-declarator N_SPEC_DECL is built here, so the assignments to
			// the already-hoisted vars are the faithful equivalent. (The old
			// code dropped init_extras whenever `initialize` was a TokenDecl,
			// leaving y/z uninitialized -> garbage / infinite loop:
			// testfortypedcomma.)
			std::vector<node_t> parts;
			auto add_clause = [&](TokenBase *clause) {
				if (TokenDecl *ctd = dynamic_cast<TokenDecl *>(clause)) {
					if (ctd->initialize)
						parts.push_back(translate_expr(ctd->initialize));
					// no initializer: the var is hoisted; nothing to run
				} else {
					parts.push_back(translate_expr(clause));
				}
			};
			add_clause(tf->initialize);
			for (TokenBase *ex : tf->init_extras)
				add_clause(ex);
			if (parts.empty())
				init = ignore();
			else {
				init = parts[0];
				for (size_t i = 1; i < parts.size(); i++)
					init = node2(N_COMMA, init, parts[i]);
			}
		} else {
			init = td ? var_decl(&td->var, td) : translate_expr(tf->initialize);
			// Comma-separated expression init (`for (a=0, b=1; ...)`) keeps the
			// first in `initialize` and the rest in init_extras. Fold them into a
			// left-associative N_COMMA so all run, in order, in the init slot.
			if (!td)
				for (TokenBase *ex : tf->init_extras)
					init = node2(N_COMMA, init, translate_expr(ex));
		}
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
	node_t body = translate_loop_body(tf->statement);
	return node5(N_FOR, list(), init, cond, incr, body, tf);
}

// throw lowering: `throw <expr>` -> __madc_throw_int/double/cstr(expr) by the
// operand type; bare `throw;` -> __madc_rethrow(). The runtime sets the
// exception, unwinds the cleanup stack to the active try's mark, and
// longjmp's to the try's setjmp (or aborts if no try is active).
// Build the throw CALL node — a VALUE-expression: `__madc_throw_int/double/cstr(expr)`
// by the operand type; bare `throw;` -> `__madc_rethrow()`. Returned bare (NOT
// wrapped in N_EXPR), so it composes as a value — a throw-EXPRESSION operand of a
// ternary/comma ([expr.throw]). c2mir's N_EXPR is an expression-STATEMENT node with
// no value attribute; feeding it into a binary/comma/cond operand position
// null-derefs in c2mir's check() (confirmed via gdb at c2mir.c:10168). translate_throw
// wraps this for the statement context.
node_t CirBuilder::translate_throw_call(TokenTHROW *th)
{
	if (!th->throw_expr) {
		// bare `throw;` — rethrow the in-flight exception.
		need_output_extern("__madc_rethrow", false, {});
		node_t call = node2(N_CALL, id("__madc_rethrow", th), list(), th);
		CIR_NODE(call)->synth_from_origin = true;
		return call;
	}
	DataDef *edd = th->throw_expr->datadef();
	DataType dt = edd ? edd->rawtype() : DataType::dtINT64;
	const char *sym;
	ExternParam ep;
	bool throw_object_cstr = is_class_object_value(th->throw_expr)
			      || object_returning_call_class(th->throw_expr);
	if (dt == DataType::dtDOUBLE || dt == DataType::dtFLOAT) {
		sym = "__madc_throw_double";
		ep = { {N_DOUBLE}, false };
	} else if ((edd && edd->is_pointer()) || throw_object_cstr) {
		sym = "__madc_throw_cstr";
		ep = { {N_CHAR}, true };
	} else {
		sym = "__madc_throw_int";
		ep = { {N_LONG}, false };
	}
	need_output_extern(sym, false, { ep });
	node_t args = list();
	append(args, throw_object_cstr ? object_cstr_arg(th->throw_expr)
				       : translate_expr(th->throw_expr));
	node_t call = node2(N_CALL, id(sym, th), args, th);
	CIR_NODE(call)->synth_from_origin = true;
	return call;
}

// throw STATEMENT lowering: wrap the throw call as an expression-statement (N_EXPR).
node_t CirBuilder::translate_throw(TokenTHROW *th)
{
	return node2(N_EXPR, list(), translate_throw_call(th), th);
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
	// An externally-bound C++ dtor (emit_symbol set) has NO madc body, so it
	// must be declared extern here so `(void*)dtor_sym` is a well-typed function
	// address (not "undeclared").
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
						// keep TokenCpnd's name index in sync with the erase
						cbody->var_index.clear();
						cbody->var_indexed = 0;
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

// Range-based for over a madc array (madc::value): `for (T x : arr) body`.
// The parser already declared `x` in the ENCLOSING scope (so translate_block
// emits its storage + ctor once, and the cleanup attribute destructs it), so
// here we only emit:
//   for (long __fe_i = 0; __fe_i < madarray_size((void*)arr); __fe_i += 1) {
//       <fill x from arr[__fe_i]>;   <body>
//   }
// Class element -> php_array_get((void*)x, (void*)arr, __fe_i) (assigns into
// the pre-constructed object). Numeric element -> x = php_array_get_int().
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
	// subscript — no madc-array runtime helper. Emit an ordinary indexed for-loop
	// `for (long i = 0; i < N; i += 1) { T x = a[i]; <body> }`, exactly what g++
	// lowers a range-for over an array to. (A VLA / runtime-sized array has no
	// compile-time bound and is NOT handled here — it falls through.) Without
	// this a plain array hit the madc-array fallback below, reading its raw bytes
	// as a madc::value header -> garbage length -> out-of-bounds get -> SIGSEGV.
	if (TokenVar *ctv = dynamic_cast<TokenVar *>(fe->container)) {
		if (ctv->var.is_fixed_array() && !ctv->var.is_vla()
		    && ctv->var.total_elements() > 0)
			return translate_foreach_carray(fe, ctv);
	}

	// Reference loop var over a madc array (php/perl dynamic array) is not
	// supported: its elements are tagged madc::value entries fetched by
	// __php_array_get_int (a value copy), with no stable element address to
	// alias — so a `T&` can't faithfully mutate the source. Reject rather than
	// silently copy (the reference contract would be violated). The aliasing
	// `T&` forms ARE handled above for raw arrays and size()/operator[]
	// containers (translate_foreach_carray / translate_foreach_class).
	if (fe->elem_is_ref)
		return error_node("range-for by reference (T&) is not supported over "
				  "this container (only raw arrays and "
				  "size()/operator[] containers)", fe);

	// (void*)container — the madc::value object address (the var name decays to
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
	if (DataDefCLASS *ec = as_class_instance(fe->elemtype)) {
		FuncDef *op = class_assign_cstr_operator_def(ec);
		if (!op)
			return error_node("range-for class elements over a madc array require "
					  "operator=(char*) on the element type", fe);
		need_output_extern("__php_array_get_cstr", true,
				   { { {N_VOID}, true }, { {N_LONG}, false } });
		node_t ga = list();
		append(ga, container_addr());
		append(ga, id(idx, fe));
		node_t getcall = node2(N_CALL, id("__php_array_get_cstr", fe), ga, fe);

		std::string sym = class_method_call_symbol(ec, op, "operator=");
		bool external = !op->emit_symbol.empty();
		if (external)
			need_output_extern(sym.c_str(), true,
					   { { {N_VOID}, true }, { {N_CHAR}, true } });
		else
			referenced_funcs.insert(sym);
		node_t a = list();
		node_t dst = node1(N_ADDR, id(fe->elemname.c_str(), fe), fe);
		if (external)
			dst = node2(N_CAST, void_ptr_type(), dst, fe);
		append(a, dst);
		append(a, getcall);
		node_t fill = node2(N_CALL, id(sym.c_str(), fe), a, fe);
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

	node_t user_body = translate_loop_body(fe->statement);
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
	} else if (DataDefCLASS *ec = as_class_instance(fe->elemtype)) {
		FuncDef *op = class_assign_operator_def(ec);
		node_t a = list();
		if (op) {
			std::string sym = class_method_call_symbol(ec, op, "operator=");
			bool external = !op->emit_symbol.empty();
			if (external)
				need_output_extern(sym.c_str(), true,
						   { { {N_VOID}, true }, { {N_VOID}, true } });
			else
				referenced_funcs.insert(sym);
			node_t dst = node1(N_ADDR, id(fe->elemname.c_str(), fe), fe);
			node_t src = op_addr();
			if (external) {
				dst = node2(N_CAST, void_ptr_type(), dst, fe);
				src = node2(N_CAST, void_ptr_type(), src, fe);
			}
			append(a, dst);
			append(a, src);
			node_t fill = node2(N_CALL, id(sym.c_str(), fe), a, fe);
			append(body_items, node2(N_EXPR, list(), fill, fe));
		} else {
			node_t elem = opfd && opfd->returns_reference()
					? node1(N_DEREF, op_addr(), fe) : op_addr();
			node_t assign = node2(N_ASSIGN, id(fe->elemname.c_str(), fe),
					      elem, fe);
			append(body_items, node2(N_EXPR, list(), assign, fe));
		}
	} else {
		// Scalar element: x = *(c[__fe_i])  (load through the reference).
		node_t elem = opfd && opfd->returns_reference()
				? node1(N_DEREF, op_addr(), fe) : op_addr();
		node_t assign = node2(N_ASSIGN, id(fe->elemname.c_str(), fe), elem, fe);
		append(body_items, node2(N_EXPR, list(), assign, fe));
	}

	node_t user_body = translate_loop_body(fe->statement);
	if (user_body) append(body_items, user_body);
	node_t body = node2(N_BLOCK, list(), body_items, fe);

	return node5(N_FOR, list(), init, cond, incr, body, fe);
}

// Range-for over a raw fixed-size C array: `for (T x : a)` ->
//   for (long i = 0; i < N; i += 1) { x = a[i]; <body> }
// N is the array's compile-time element count; `a[i]` is a direct subscript
// (N_IND), element stride sizeof(T) — c2mir handles it natively, exactly like
// g++'s array range-for lowering. No madc-array runtime helper.
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
	} else if (DataDefCLASS *ec = as_class_instance(fe->elemtype)) {
		FuncDef *op = class_assign_operator_def(ec);
		node_t a = list();
		if (op) {
			std::string sym = class_method_call_symbol(ec, op, "operator=");
			bool external = !op->emit_symbol.empty();
			if (external)
				need_output_extern(sym.c_str(), true,
						   { { {N_VOID}, true }, { {N_VOID}, true } });
			else
				referenced_funcs.insert(sym);
			node_t dst = node1(N_ADDR, id(fe->elemname.c_str(), fe), fe);
			node_t src = node1(N_ADDR, elem_lvalue(), fe);
			if (external) {
				dst = node2(N_CAST, void_ptr_type(), dst, fe);
				src = node2(N_CAST, void_ptr_type(), src, fe);
			}
			append(a, dst);
			append(a, src);
			node_t fill = node2(N_CALL, id(sym.c_str(), fe), a, fe);
			append(body_items, node2(N_EXPR, list(), fill, fe));
		} else {
			node_t assign = node2(N_ASSIGN, id(fe->elemname.c_str(), fe),
					      elem_lvalue(), fe);
			append(body_items, node2(N_EXPR, list(), assign, fe));
		}
	} else {
		// Scalar element: x = a[__fe_i].
		node_t assign = node2(N_ASSIGN, id(fe->elemname.c_str(), fe),
				      elem_lvalue(), fe);
		append(body_items, node2(N_EXPR, list(), assign, fe));
	}

	node_t user_body = translate_loop_body(fe->statement);
	if (user_body) append(body_items, user_body);
	node_t body = node2(N_BLOCK, list(), body_items, fe);

	return node5(N_FOR, list(), init, cond, incr, body, fe);
}

node_t CirBuilder::translate_do(TokenDO *td)
{
	return node3(N_DO, list(), translate_expr(td->condition),
		     translate_loop_body(td->statement), td);
}

node_t CirBuilder::translate_switch(TokenSWITCH *ts)
{
	// C++17 init-statement: `switch (init; expr)` lowers to
	// `{ init; switch (expr) {...} }`. Translate the init-statement (and flush
	// its temps) FIRST so it is emitted before the switch expression, which it
	// may reference; the wrap is applied at the single return below.
	node_t init = NULL;
	std::vector<node_t> init_temps;
	if (ts->init_stmt) {
		init = translate_stmt(ts->init_stmt);
		flush_pending_stmts(init_temps);
	}

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
	// A statement may carry several labels (`case A: case B: stmt;`); the
	// N_LIST holds them all. `case LOW ... HIGH:` is a GNU extension c2mir
	// only accepts with a warning, so madc OWNS its lowering: expand it to
	// individual C11 labels `case LOW: case LOW+1: ... case HIGH:`. The bounds
	// are integer constant expressions folded by the parser (TokenInt), so the
	// expansion is exact and the emitted C is strict C11 (no extension note).
	const int64_t kMaxCaseRangeExpansion = 4096;
	auto emit_case = [&](TokenCASE *tc, bool is_default) {
		node_t labels = list();
		if (is_default) {
			append(labels, simple(N_DEFAULT));
		} else if (tc->range_high) {
			int64_t lo = tc->value->ival();
			int64_t hi = tc->range_high->ival();
			// Bound the expansion: an absurd range (or an inverted one) keeps
			// the GNU N_CASE(low, high) form rather than materializing billions
			// of labels. Such ranges do not occur in practice.
			if (hi < lo || (hi - lo) >= kMaxCaseRangeExpansion) {
				append(labels, node2(N_CASE, translate_expr(tc->value),
						     translate_expr(tc->range_high), tc));
			} else {
				for (int64_t v = lo; v <= hi; v++)
					append(labels, node1(N_CASE, integer((long)v, tc), tc));
			}
		} else {
			append(labels, node1(N_CASE, translate_expr(tc->value), tc));
		}
		append(block_items, node2(N_EXPR, labels, integer(0)));
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
	node_t sw = node3(N_SWITCH, list(), expr, body, ts);
	if (!ts->init_stmt)
		return sw;
	node_t items = list();
	for (node_t p : init_temps)
		append(items, p);
	if (init)
		append(items, init);
	append(items, sw);
	return node2(N_BLOCK, list(), items, ts);
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
		// A user-class struct is ALWAYS emitted at file scope (Pass 0.5):
		// a block-scope alias of one must REFERENCE that tag. Re-emitting
		// the body here defines a SHADOWING function-local tag — a local
		// `typedef basic_string<...> _Str;` made `*__retbuf = ...` an
		// incompatible struct assignment (and mis-flattened the SSO
		// union). Plain C block-scope struct typedefs keep the body.
		DataDef *ttd_base = ttd->target_type;
		while (DataDefPTR *tp = dynamic_cast<DataDefPTR *>(ttd_base))
			ttd_base = tp->base_type;
		bool class_alias = as_user_class(ttd_base) != NULL;
		node_t n = typedef_decl(ttd->alias, ttd->target_type,
					no_emitted_structs, class_alias);
		if (n) { CIR_NODE(n)->origin_id = madc_slot_id_for(ttd); set_pos(CIR_NODE(n), ttd); }
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

// Emit pending `defer`red statements into `items`: scopes from the innermost
// down to m_defer_scopes[from_scope] inclusive, each scope's list in reverse
// registration (LIFO) order. Each statement is re-translated at each exit
// site (fresh nodes — a cir node is single-parent), with any materialized
// temporaries flushed ahead of it, mirroring translate_block's statement loop.
void CirBuilder::append_deferred_stmts(node_t items, size_t from_scope)
{
	for (size_t si = m_defer_scopes.size(); si-- > from_scope; ) {
		TokenCpnd *sc = m_defer_scopes[si];
		for (auto it = sc->deferred.rbegin(); it != sc->deferred.rend(); ++it) {
			node_t s = translate_stmt(*it);
			if (!s) continue;
			for (node_t p : m_pending_stmts)
				append(items, p);
			m_pending_stmts.clear();
			append(items, s);
		}
	}
}

node_t CirBuilder::translate_block(TokenCpnd *tc)
{
	node_t empty_list = list();
	node_t items = list();

	// A compound carrying `defer`red statements becomes an active defer
	// scope for its whole translation: returns inside it (translate_return)
	// emit the pending deferred statements before leaving the function.
	const bool has_defers = !tc->deferred.empty();
	if (has_defers)
		m_defer_scopes.push_back(tc);

	std::set<std::string> decl_vars;
	for (auto *ts : tc->statements) {
		TokenDecl *td = dynamic_cast<TokenDecl *>((TokenBase *)ts);
		if (td)
			decl_vars.insert(td->var.name);
	}

	for (size_t vi = 0; vi < tc->variables.size(); vi++) {
		Variable *v = tc->variables[vi];
		if (v->flags & vfPARAM) continue;
		if (decl_vars.count(v->name)) continue;
		// A GNU nested function's in-scope alias is a Variable whose type is a
		// FuncDef — it names the hoisted function, it is not storage. Emit no
		// local declaration for it (calls resolve to the hoisted symbol). Without
		// this it rendered as `void Foo;`, which c2mir tolerates but gcc rejects,
		// breaking the portable `--emit=c11` output.
		if (dynamic_cast<FuncDef *>(v->type)) continue;
		append(items, var_decl(v));
		if (is_array_object(v->type)) {
			// `array a;` — default-construct the madc::value object. Scope-exit
			// destruction is handled by the cleanup attribute (array_storage_decl).
			append(items, array_ctor_call(v->name.c_str(), tc));
		} else if (DataDefCLASS *cdd = as_class_instance(v->type)) {
			// `Foo f;` / `string s;` — a class instance declared without an
			// explicit constructor-call (no ctor args). Default-construct it;
			// scope-exit destruction is via the cleanup attribute (var_decl). The
			// argful form `Foo f(a,b)` parses to a TokenDecl statement handled below.
			node_t cc = class_ctor_call(v, cdd,
						    std::vector<TokenBase *>(), tc);
			if (cc) append(items, cc);
			// A class with embedded object members but NO user ctor still
			// needs each member constructed.
			// (Member destruction is via the synthesized dtor referenced by
			// the cleanup attribute — see class_struct_def / var_decl.)
			else {
				if (class_has_object_members(cdd))
					class_instance_member_ctors(v->name.c_str(), cdd,
								    items, tc);
				// C++11 default member initializers on a no-ctor instance
				// (incl. a scalar-only struct promoted to a class purely
				// for its NSDMI): `recv.member = init` per declaration order.
				std::vector<node_t> nsdmi_stmts;
				if (emit_member_default_inits(cdd, v->name.c_str(), false,
							      nsdmi_stmts, tc, NULL))
					for (node_t s : nsdmi_stmts)
						append(items, s);
			}
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

		// Class-object declaration WITH initializer is handled by the generic
		// class-instance path below:
		// the initializer becomes the single ctor argument and select_ctor_overload
		// picks the const-char*/copy/default ctor by its type.
		{
			TokenDecl *sdcl = dynamic_cast<TokenDecl *>(stb);
			bool file_global = sdcl && !(sdcl->var.flags & vfLOCAL)
					&& !(sdcl->var.flags & vfSTATIC);
			// madc array object declaration: storage (via var_decl) + default ctor.
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
				// A construction whose arguments contain a pack expansion cannot
				// select a ctor overload in the shared Tree-1 pattern (pack arity
				// is per-instantiation; g++ defers the whole call). Emit a
				// deferred-construction marker; the tsubst copy re-lowers it with
				// the expanded concrete arguments (the placement-new deferral
				// shape, copy_cir_subtree).
				if (m_tsubst_pattern_mode
				    && tsubst_args_have_pack_expansion(sdcl->ctor_args)) {
					cir_node *marker = make(N_IGNORE, sdcl);
					marker->datadef = cdcl;
					append(items, marker->as_node());
					continue;
				}
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
				// Copy elision: `T b = T(args)` — the initializer is a
				// functional-construction temporary (TokenObjTemp) of the same
				// class. Construct b directly with the temp's ctor args
				// (guaranteed copy elision), instead of materializing a temp and
				// copy-constructing. Also keeps a TokenObjTemp out of the
				// call-NRVO path below, which dynamic_casts to TokenCallFunc.
				if (ctor_args.size() == 1) {
					if (TokenObjTemp *iot =
					    dynamic_cast<TokenObjTemp *>(ctor_args[0]))
						if (as_class_instance(iot->obj_class) == cdcl)
							ctor_args = iot->ctor_args;
				}
				if (ctor_args.size() == 1
				    && dynamic_cast<TokenCallFunc *>(ctor_args[0])
				    && object_returning_call_class(ctor_args[0]) == cdcl) {
					TokenCallFunc *itcf =
						dynamic_cast<TokenCallFunc *>(ctor_args[0]);
					FuncDef *ifd = NULL;
					std::string isym = call_target_emit_name(itcf, &ifd);
					referenced_funcs.insert(isym);
					node_t cargs = list();
					append(cargs, node1(N_ADDR,
						id(sdcl->var.name.c_str(), sdcl), sdcl));
					// A retbuf-returning METHOD call (`T v = obj.m();`,
					// TokenCallMethod : TokenMember) needs its hidden __this
					// receiver between the retbuf and the explicit args —
					// build_call_args emits only explicit args. Without it the
					// copy-elided call drops `this` (`m(&v)` instead of
					// `m(&v, &__this->obj)`) -> c2mir "too few arguments". The
					// free-function case (itcf not a TokenMember) is unchanged.
					bool imeth_call = false;
					if (TokenCallMethod *imeth =
					    dynamic_cast<TokenCallMethod *>(itcf)) {
						DataDefCLASS *rc = NULL;
						node_t this_arg =
							class_this_arg(imeth, rc, sdcl);
						if (this_arg) {
							append(cargs, this_arg);
							imeth_call = true;
						}
					}
					// A method callee's parameters[0] is the hidden __this
					// (injected above); explicit args start at parameter 1.
					build_call_args(itcf, cargs, imeth_call ? 1 : 0);
					node_t icall = node2(N_CALL,
						id(isym.c_str(), sdcl), cargs, sdcl);
					CIR_NODE(icall)->synth_from_origin = true;
					for (node_t p : m_pending_stmts)
						append(items, p);
					m_pending_stmts.clear();
					append(items, node2(N_EXPR, list(), icall, sdcl));
					continue;
				}
				// `T c = a + b;` where a FREE namespace operator on the
				// class operands returns T BY VALUE (std::operator+ on
				// strings): construct straight into c — the Itanium sret
				// slot is &c (guaranteed copy elision; g++ canon emits
				// exactly one call, _ZStpl(&c,&a,&b)). The instantiation
				// is pure (member arbitration inside) so nothing is
				// emitted unless this shape fully binds; member-operator
				// classes keep the class_ctor_call copy path below.
				if (ctor_args.size() == 1) {
					TokenOperator *iop =
						dynamic_cast<TokenOperator *>(ctor_args[0]);
					DataDefCLASS *ilcls = (iop && iop->left)
						? operand_object_class(iop->left)
						: NULL;
					const char *iopsym = iop
						? binop_overload_symbol(iop->id()) : "";
					FuncDef *iinst = NULL;
					if (ilcls && iopsym[0]) {
						std::string iopname =
							std::string("operator") + iopsym;
						iinst = std_free_operator_instantiation(
							iop, ilcls, iopname,
							select_operator_overload(
								ilcls, iopname, iop->right));
					}
					if (iinst && !iinst->returns_reference()
					    && class_return_via_retbuf(&iinst->return_value_type()) == cdcl) {
						node_t slot = node1(N_ADDR,
							id(sdcl->var.name.c_str(), sdcl),
							sdcl);
						bool slot_used = false;
						node_t oc = class_operator_external_call(
							iop, ilcls, iinst, sdcl,
							slot, cdcl, &slot_used);
						if (oc && slot_used) {
							for (node_t p : m_pending_stmts)
								append(items, p);
							m_pending_stmts.clear();
							append(items, node2(N_EXPR, list(),
									    oc, sdcl));
							continue;
						}
					}
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
				else {
					// No user ctor but embedded object members: construct each
					// member in place. With a
					// user ctor, the ctor body's prologue constructs them.
					if (class_has_object_members(cdcl))
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
					// C++11 default member initializers on a DEFAULT-constructed
					// no-ctor instance (`S s;` — incl. a scalar-only struct promoted
					// to a class purely for its NSDMI). A whole-object initializer
					// (the trivial-copy arm) supplies all members, so skip then.
					if (ctor_args.empty()) {
						std::vector<node_t> nsdmi_stmts;
						if (emit_member_default_inits(cdcl,
								sdcl->var.name.c_str(), false,
								nsdmi_stmts, sdcl, NULL))
							for (node_t ns : nsdmi_stmts)
								append(items, ns);
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
		// Materialized temporaries (e.g. a class object built for an argument)
		// are emitted just before the statement that uses them; their cleanup
		// attribute destructs them at scope exit. See object_arg_addr /
		// m_pending_stmts.
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

	// Fall-off end of a defer-carrying compound: run THIS scope's deferred
	// statements (LIFO), then retire the scope. Early exits (return) emit
	// them at the exit site instead; when the block ends in a return this
	// emission is unreachable dead code, which is valid C.
	if (has_defers) {
		append_deferred_stmts(items, m_defer_scopes.size() - 1);
		m_defer_scopes.pop_back();
	}

	// No manual scope-exit destruction: each eligible class object's storage
	// decl carries a cleanup attribute, so the c2mir fork emits
	// the destructor call on EVERY exit path (fall-through, return, break,
	// continue, goto) — correct RAII including early exits, with no front-end
	// dtor injection. See the object storage declaration path.
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

// Two-tree Phase 3: build a concrete instantiated member-template method's BODY
// by tsubst of its source template's Tree-1 recipe — g++'s instantiate_body
// (tsubst_stmt over DECL_SAVED_TREE), instead of lowering the re-parsed body.
// The concrete signature/shell stays on the existing parse path (hybrid B).
// Returns NULL for anything not yet covered, so func_def falls back to
// translate_block (the re-parse path stays the residual fallback until
// Phase-5 + deletion). DEFAULT since the flip; MADC_XTEST_DEP_PARSE=0 opts
// back into pure re-parse (soak escape hatch).
static bool tsubst_datadef_involves_template_param(DataDef *dd)
{
	if (!dd)
		return false;
	if (dd->is_template_param())
		return true;
	if (DataDefREF *rd = dynamic_cast<DataDefREF *>(dd))
		return tsubst_datadef_involves_template_param(rd->base_type);
	if (DataDefPTR *pd = dynamic_cast<DataDefPTR *>(dd))
		return tsubst_datadef_involves_template_param(pd->base_type);
	if (DataDefCONST *cd = dynamic_cast<DataDefCONST *>(dd))
		return tsubst_datadef_involves_template_param(cd->base_type);
	return false;
}

static bool tsubst_pattern_has_destroy_template_pointee(TokenBase *tb)
{
	if (!tb)
		return false;
	if (TokenPackExpansion *pe = dynamic_cast<TokenPackExpansion *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(pe->pattern);
	if (TokenCallFunc *tc = dynamic_cast<TokenCallFunc *>(tb)) {
		if (tsubst_destroy_call_has_template_pointee(tc))
			return true;
		for (TokenBase *p : tc->parameters)
			if (tsubst_pattern_has_destroy_template_pointee(p))
				return true;
		if (TokenMember *tm = dynamic_cast<TokenMember *>(tc))
			if (tsubst_pattern_has_destroy_template_pointee(tm->parent_expr))
				return true;
		return false;
	}
	if (TokenCpnd *cp = dynamic_cast<TokenCpnd *>(tb)) {
		for (TokenStmt *s : cp->statements)
			if (tsubst_pattern_has_destroy_template_pointee(s))
				return true;
		for (TokenBase *d : cp->deferred)
			if (tsubst_pattern_has_destroy_template_pointee(d))
				return true;
	}
	if (TokenDecl *td = dynamic_cast<TokenDecl *>(tb)) {
		if (tsubst_pattern_has_destroy_template_pointee(td->initialize))
			return true;
		for (TokenBase *i : td->init_list)
			if (tsubst_pattern_has_destroy_template_pointee(i))
				return true;
		for (TokenBase *a : td->ctor_args)
			if (tsubst_pattern_has_destroy_template_pointee(a))
				return true;
	}
	if (TokenOperator *op = dynamic_cast<TokenOperator *>(tb)) {
		if (TokenTerQ *tq = dynamic_cast<TokenTerQ *>(op)) {
			if (tsubst_pattern_has_destroy_template_pointee(tq->condition)
			    || tsubst_pattern_has_destroy_template_pointee(tq->true_expr)
			    || tsubst_pattern_has_destroy_template_pointee(tq->false_expr))
				return true;
		}
		if (tsubst_pattern_has_destroy_template_pointee(op->left)
		    || tsubst_pattern_has_destroy_template_pointee(op->right))
			return true;
	}
	if (TokenRETURN *tr = dynamic_cast<TokenRETURN *>(tb)) {
		if (tsubst_pattern_has_destroy_template_pointee(tr->returns))
			return true;
		for (TokenBase *r : tr->return_exprs)
			if (tsubst_pattern_has_destroy_template_pointee(r))
				return true;
	}
	if (TokenSubscript *ts = dynamic_cast<TokenSubscript *>(tb)) {
		if (tsubst_pattern_has_destroy_template_pointee(ts->index))
			return true;
		for (TokenBase *e : ts->extra_indices)
			if (tsubst_pattern_has_destroy_template_pointee(e))
				return true;
	}
	if (TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(tse->base_expr)
		    || tsubst_pattern_has_destroy_template_pointee(tse->index);
	if (TokenCast *tc = dynamic_cast<TokenCast *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(tc->expr);
	if (TokenAddrExpr *ta = dynamic_cast<TokenAddrExpr *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(ta->expr);
	if (TokenDerefExpr *td = dynamic_cast<TokenDerefExpr *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(td->expr);
	if (TokenComplexPart *tcp = dynamic_cast<TokenComplexPart *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(tcp->expr);
	if (TokenStructLit *tsl = dynamic_cast<TokenStructLit *>(tb))
		for (TokenBase *i : tsl->inits)
			if (tsubst_pattern_has_destroy_template_pointee(i))
				return true;
	if (TokenObjTemp *tot = dynamic_cast<TokenObjTemp *>(tb))
		for (TokenBase *a : tot->ctor_args)
			if (tsubst_pattern_has_destroy_template_pointee(a))
				return true;
	if (TokenNEW *tn = dynamic_cast<TokenNEW *>(tb)) {
		if (tsubst_pattern_has_destroy_template_pointee(tn->placement)
		    || tsubst_pattern_has_destroy_template_pointee(tn->array_size))
			return true;
		for (TokenBase *a : tn->ctor_args)
			if (tsubst_pattern_has_destroy_template_pointee(a))
				return true;
	}
	if (TokenDELETE *td = dynamic_cast<TokenDELETE *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(td->expr);
	if (TokenIF *ti = dynamic_cast<TokenIF *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(ti->init_stmt)
		    || tsubst_pattern_has_destroy_template_pointee(ti->condition)
		    || tsubst_pattern_has_destroy_template_pointee(ti->condition_decl)
		    || tsubst_pattern_has_destroy_template_pointee(ti->statement)
		    || tsubst_pattern_has_destroy_template_pointee(ti->elsestmt);
	if (TokenDO *tdo = dynamic_cast<TokenDO *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(tdo->statement)
		    || tsubst_pattern_has_destroy_template_pointee(tdo->condition);
	if (TokenWHILE *tw = dynamic_cast<TokenWHILE *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(tw->condition)
		    || tsubst_pattern_has_destroy_template_pointee(tw->statement);
	if (TokenFOR *tf = dynamic_cast<TokenFOR *>(tb)) {
		if (tsubst_pattern_has_destroy_template_pointee(tf->initialize)
		    || tsubst_pattern_has_destroy_template_pointee(tf->condition)
		    || tsubst_pattern_has_destroy_template_pointee(tf->increment)
		    || tsubst_pattern_has_destroy_template_pointee(tf->statement))
			return true;
		for (TokenBase *e : tf->init_extras)
			if (tsubst_pattern_has_destroy_template_pointee(e))
				return true;
		for (TokenBase *e : tf->incr_extras)
			if (tsubst_pattern_has_destroy_template_pointee(e))
				return true;
	}
	if (TokenFOREACH *tf = dynamic_cast<TokenFOREACH *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(tf->container)
		    || tsubst_pattern_has_destroy_template_pointee(tf->statement);
	if (TokenCASE *tc = dynamic_cast<TokenCASE *>(tb)) {
		if (tsubst_pattern_has_destroy_template_pointee(tc->value)
		    || tsubst_pattern_has_destroy_template_pointee(tc->range_high))
			return true;
		for (TokenBase *s : tc->statements)
			if (tsubst_pattern_has_destroy_template_pointee(s))
				return true;
	}
	if (TokenSWITCH *ts = dynamic_cast<TokenSWITCH *>(tb)) {
		if (tsubst_pattern_has_destroy_template_pointee(ts->init_stmt)
		    || tsubst_pattern_has_destroy_template_pointee(ts->expression))
			return true;
		for (TokenBase *s : ts->pre_case_stmts)
			if (tsubst_pattern_has_destroy_template_pointee(s))
				return true;
		for (TokenCASE *c : ts->cases)
			if (tsubst_pattern_has_destroy_template_pointee(c))
				return true;
		if (tsubst_pattern_has_destroy_template_pointee(ts->defaultcase))
			return true;
	}
	if (TokenTRY *tt = dynamic_cast<TokenTRY *>(tb)) {
		if (tsubst_pattern_has_destroy_template_pointee(tt->try_body))
			return true;
		for (TokenBase *b : tt->catch_bodies)
			if (tsubst_pattern_has_destroy_template_pointee(b))
				return true;
	}
	if (TokenTHROW *tt = dynamic_cast<TokenTHROW *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(tt->throw_expr);
	if (TokenLabel *tl = dynamic_cast<TokenLabel *>(tb))
		return tsubst_pattern_has_destroy_template_pointee(tl->labeled);
	if (TokenExplicitDtor *td = dynamic_cast<TokenExplicitDtor *>(tb))
		return tsubst_explicit_dtor_marker_datadef(td)
		    || tsubst_pattern_has_destroy_template_pointee(td->obj);
	return false;
}

static const char *tsubst_plain_var_name(TokenBase *tb)
{
	if (!tb)
		return NULL;
	if (tb->type() == TokenType::ttVariable) {
		if (TokenVar *tv = dynamic_cast<TokenVar *>(tb))
			return tv->var.name.c_str();
	}
	if (TokenIdent *ti = dynamic_cast<TokenIdent *>(tb))
		return ti->spelling();
	return NULL;
}

static bool tsubst_name_is(TokenBase *tb, const std::string &name)
{
	const char *n = tsubst_plain_var_name(tb);
	return n && n[0] && name == n;
}

static bool tsubst_pattern_mentions_deref_of_name(TokenBase *tb,
						  const std::string &name)
{
	if (!tb || name.empty())
		return false;
	if (TokenDeref *td = dynamic_cast<TokenDeref *>(tb))
		return td->var.name == name;
	if (TokenDerefExpr *td = dynamic_cast<TokenDerefExpr *>(tb))
		return tsubst_name_is(td->expr, name)
		    || tsubst_pattern_mentions_deref_of_name(td->expr, name);
	if (TokenAddrExpr *ta = dynamic_cast<TokenAddrExpr *>(tb))
		return tsubst_pattern_mentions_deref_of_name(ta->expr, name);
	if (TokenCast *tc = dynamic_cast<TokenCast *>(tb))
		return tsubst_pattern_mentions_deref_of_name(tc->expr, name);
	if (TokenCallFunc *tc = dynamic_cast<TokenCallFunc *>(tb)) {
		for (TokenBase *p : tc->parameters)
			if (tsubst_pattern_mentions_deref_of_name(p, name))
				return true;
		if (tsubst_pattern_mentions_deref_of_name(tc->src_node, name))
			return true;
		if (TokenMember *tm = dynamic_cast<TokenMember *>(tc))
			return tsubst_pattern_mentions_deref_of_name(
				tm->parent_expr, name);
		return false;
	}
	if (TokenOperator *op = dynamic_cast<TokenOperator *>(tb)) {
		if (tsubst_pattern_mentions_deref_of_name(op->left, name)
		    || tsubst_pattern_mentions_deref_of_name(op->right, name))
			return true;
		if (TokenTerQ *tq = dynamic_cast<TokenTerQ *>(op))
			return tsubst_pattern_mentions_deref_of_name(
				tq->condition, name)
			    || tsubst_pattern_mentions_deref_of_name(
				tq->true_expr, name)
			    || tsubst_pattern_mentions_deref_of_name(
				tq->false_expr, name);
	}
	if (TokenCpnd *cp = dynamic_cast<TokenCpnd *>(tb)) {
		for (TokenStmt *s : cp->statements)
			if (tsubst_pattern_mentions_deref_of_name(s, name))
				return true;
		for (TokenBase *d : cp->deferred)
			if (tsubst_pattern_mentions_deref_of_name(d, name))
				return true;
	}
	return false;
}

static bool tsubst_call_uses_deref_of_name(TokenBase *tb,
					   const std::string &name)
{
	if (!tb || name.empty())
		return false;
	if (TokenCallFunc *tc = dynamic_cast<TokenCallFunc *>(tb)) {
		for (TokenBase *p : tc->parameters)
			if (tsubst_pattern_mentions_deref_of_name(p, name))
				return true;
		if (TokenMember *tm = dynamic_cast<TokenMember *>(tc))
			if (tsubst_pattern_mentions_deref_of_name(tm->parent_expr,
								  name))
				return true;
	}
	if (TokenCpnd *cp = dynamic_cast<TokenCpnd *>(tb)) {
		for (TokenStmt *s : cp->statements)
			if (tsubst_call_uses_deref_of_name(s, name))
				return true;
		for (TokenBase *d : cp->deferred)
			if (tsubst_call_uses_deref_of_name(d, name))
				return true;
	}
	return false;
}

static bool tsubst_pattern_has_destroy_iterator_loop(TokenBase *tb)
{
	if (!tb)
		return false;
	if (TokenCpnd *cp = dynamic_cast<TokenCpnd *>(tb)) {
		for (TokenStmt *s : cp->statements)
			if (tsubst_pattern_has_destroy_iterator_loop(s))
				return true;
		for (TokenBase *d : cp->deferred)
			if (tsubst_pattern_has_destroy_iterator_loop(d))
				return true;
		return false;
	}
	TokenFOR *tf = dynamic_cast<TokenFOR *>(tb);
	if (!tf)
		return false;
	if (tf->initialize || !tf->init_extras.empty()
	    || !tf->incr_extras.empty())
		return false;
	TokenNotEq *ne = dynamic_cast<TokenNotEq *>(tf->condition);
	TokenInc *inc = dynamic_cast<TokenInc *>(tf->increment);
	if (!ne || !inc || !ne->left || !ne->right)
		return false;
	const char *first = tsubst_plain_var_name(ne->left);
	const char *last = tsubst_plain_var_name(ne->right);
	if (!first || !last || strcmp(first, last) == 0)
		return false;
	TokenBase *inc_operand = inc->left ? inc->left : inc->right;
	if (!tsubst_name_is(inc_operand, first))
		return false;
	if (!tsubst_datadef_involves_template_param(ne->left->datadef())
	    || !tsubst_datadef_involves_template_param(ne->right->datadef()))
		return false;
	return tsubst_call_uses_deref_of_name(tf->statement, first);
}

static bool tsubst_pattern_is_empty_body(TokenBase *tb)
{
	TokenCpnd *cp = dynamic_cast<TokenCpnd *>(tb);
	return cp && cp->statements.empty() && cp->deferred.empty();
}

static bool tsubst_pattern_has_dependent_call(TokenBase *tb)
{
	if (!tb)
		return false;
	if (TokenPackExpansion *pe = dynamic_cast<TokenPackExpansion *>(tb)) {
		if (!pe->pattern)
			return false;
		if (dynamic_cast<TokenVar *>(pe->pattern)
		    || dynamic_cast<TokenIdent *>(pe->pattern))
			return false;
		return tsubst_pattern_has_dependent_call(pe->pattern);
	}
	if (tb->type() == TokenType::ttMember)
		if (TokenMember *tm = dynamic_cast<TokenMember *>(tb))
			return tsubst_pattern_has_dependent_call(tm->parent_expr);
	if (TokenCallFunc *tc = dynamic_cast<TokenCallFunc *>(tb)) {
		// __destroy(ptr) lowers by inspecting the concrete pointee class at
		// CIR time. Direct placeholder-pointee helpers are represented by a
		// deferred marker in the Tree-1 recipe; broader destructor helper
		// shapes stay on the parsed-body path.
		if (tc->var.name == "__destroy")
			return !tsubst_destroy_call_has_template_pointee(tc);
		bool call_rewrite =
			tsubst_call_can_rewrite_after_subst(tc);
		if (FuncDef *fd = dynamic_cast<FuncDef *>(tc->var.type))
			if (!fd->function_display_name.empty() && !call_rewrite)
				return true;
		if (!call_rewrite
		    && tsubst_datadef_involves_template_param(tc->datadef()))
			return true;
		for (size_t i = 0; i < tc->parameters.size(); ++i) {
			TokenBase *p = tc->parameters[i];
			if (TokenPackExpansion *pe =
				    dynamic_cast<TokenPackExpansion *>(p)) {
				if (dynamic_cast<TokenCallFunc *>(pe->pattern))
					continue;
				if (tsubst_pattern_has_dependent_call(pe))
					return true;
				continue;
			}
			if (tsubst_pattern_has_dependent_call(p))
				return true;
			bool param_rewrite = false;
			if (TokenCallFunc *pc = dynamic_cast<TokenCallFunc *>(p))
				param_rewrite =
					tsubst_call_can_rewrite_after_subst(pc);
			if (p && !call_rewrite && !param_rewrite
			    && tsubst_datadef_involves_template_param(p->datadef()))
				return true;
		}
		if (tc->src_node && tsubst_pattern_has_dependent_call(tc->src_node))
			return true;
		if (TokenMember *tm = dynamic_cast<TokenMember *>(tc))
			if (tsubst_pattern_has_dependent_call(tm->parent_expr))
				return true;
		return false;
	}
	if (TokenCpnd *cp = dynamic_cast<TokenCpnd *>(tb)) {
		for (TokenStmt *s : cp->statements)
			if (tsubst_pattern_has_dependent_call(s))
				return true;
		for (TokenBase *d : cp->deferred)
			if (tsubst_pattern_has_dependent_call(d))
				return true;
	}
	if (TokenDecl *td = dynamic_cast<TokenDecl *>(tb)) {
		if (tsubst_pattern_has_dependent_call(td->initialize))
			return true;
		for (TokenBase *i : td->init_list)
			if (tsubst_pattern_has_dependent_call(i))
				return true;
		for (TokenBase *a : td->ctor_args)
			if (tsubst_pattern_has_dependent_call(a))
				return true;
	}
	if (TokenOperator *op = dynamic_cast<TokenOperator *>(tb)) {
		if (TokenTerQ *tq = dynamic_cast<TokenTerQ *>(op)) {
			if (tsubst_pattern_has_dependent_call(tq->condition)
			    || tsubst_pattern_has_dependent_call(tq->true_expr)
			    || tsubst_pattern_has_dependent_call(tq->false_expr))
				return true;
		}
		if (tsubst_pattern_has_dependent_call(op->left)
		    || tsubst_pattern_has_dependent_call(op->right))
			return true;
	}
	if (TokenRETURN *tr = dynamic_cast<TokenRETURN *>(tb)) {
		if (tsubst_pattern_has_dependent_call(tr->returns))
			return true;
		for (TokenBase *r : tr->return_exprs)
			if (tsubst_pattern_has_dependent_call(r))
				return true;
	}
	if (TokenSubscript *ts = dynamic_cast<TokenSubscript *>(tb)) {
		if (tsubst_pattern_has_dependent_call(ts->index))
			return true;
		for (TokenBase *e : ts->extra_indices)
			if (tsubst_pattern_has_dependent_call(e))
				return true;
	}
	if (TokenSubscriptExpr *tse = dynamic_cast<TokenSubscriptExpr *>(tb))
		if (tsubst_pattern_has_dependent_call(tse->base_expr)
		    || tsubst_pattern_has_dependent_call(tse->index))
			return true;
	if (TokenCast *tc = dynamic_cast<TokenCast *>(tb))
		if (tsubst_pattern_has_dependent_call(tc->expr))
			return true;
	if (TokenAddrExpr *ta = dynamic_cast<TokenAddrExpr *>(tb))
		if (tsubst_pattern_has_dependent_call(ta->expr))
			return true;
	if (TokenDerefExpr *td = dynamic_cast<TokenDerefExpr *>(tb))
		if (tsubst_pattern_has_dependent_call(td->expr))
			return true;
	if (TokenComplexPart *tcp = dynamic_cast<TokenComplexPart *>(tb))
		if (tsubst_pattern_has_dependent_call(tcp->expr))
			return true;
	if (TokenStructLit *tsl = dynamic_cast<TokenStructLit *>(tb))
		for (TokenBase *i : tsl->inits)
			if (tsubst_pattern_has_dependent_call(i))
				return true;
	if (TokenObjTemp *tot = dynamic_cast<TokenObjTemp *>(tb))
		for (TokenBase *a : tot->ctor_args)
			if (tsubst_pattern_has_dependent_call(a))
				return true;
	if (TokenNEW *tn = dynamic_cast<TokenNEW *>(tb)) {
		if (tsubst_pattern_has_dependent_call(tn->placement)
		    || tsubst_pattern_has_dependent_call(tn->array_size))
			return true;
		for (TokenBase *a : tn->ctor_args)
			if (tsubst_pattern_has_dependent_call(a))
				return true;
	}
	if (TokenDELETE *td = dynamic_cast<TokenDELETE *>(tb))
		if (tsubst_pattern_has_dependent_call(td->expr))
			return true;
	if (TokenIF *ti = dynamic_cast<TokenIF *>(tb))
		return tsubst_pattern_has_dependent_call(ti->init_stmt)
		    || tsubst_pattern_has_dependent_call(ti->condition)
		    || tsubst_pattern_has_dependent_call(ti->condition_decl)
		    || tsubst_pattern_has_dependent_call(ti->statement)
		    || tsubst_pattern_has_dependent_call(ti->elsestmt);
	if (TokenDO *tdo = dynamic_cast<TokenDO *>(tb))
		return tsubst_pattern_has_dependent_call(tdo->statement)
		    || tsubst_pattern_has_dependent_call(tdo->condition);
	if (TokenWHILE *tw = dynamic_cast<TokenWHILE *>(tb))
		return tsubst_pattern_has_dependent_call(tw->condition)
		    || tsubst_pattern_has_dependent_call(tw->statement);
	if (TokenFOR *tf = dynamic_cast<TokenFOR *>(tb)) {
		if (tsubst_pattern_has_dependent_call(tf->initialize)
		    || tsubst_pattern_has_dependent_call(tf->condition)
		    || tsubst_pattern_has_dependent_call(tf->increment)
		    || tsubst_pattern_has_dependent_call(tf->statement))
			return true;
		for (TokenBase *e : tf->init_extras)
			if (tsubst_pattern_has_dependent_call(e))
				return true;
		for (TokenBase *e : tf->incr_extras)
			if (tsubst_pattern_has_dependent_call(e))
				return true;
	}
	if (TokenFOREACH *tf = dynamic_cast<TokenFOREACH *>(tb))
		return tsubst_pattern_has_dependent_call(tf->container)
		    || tsubst_pattern_has_dependent_call(tf->statement);
	if (TokenCASE *tc = dynamic_cast<TokenCASE *>(tb)) {
		if (tsubst_pattern_has_dependent_call(tc->value)
		    || tsubst_pattern_has_dependent_call(tc->range_high))
			return true;
		for (TokenBase *s : tc->statements)
			if (tsubst_pattern_has_dependent_call(s))
				return true;
	}
	if (TokenSWITCH *ts = dynamic_cast<TokenSWITCH *>(tb)) {
		if (tsubst_pattern_has_dependent_call(ts->init_stmt)
		    || tsubst_pattern_has_dependent_call(ts->expression))
			return true;
		for (TokenBase *s : ts->pre_case_stmts)
			if (tsubst_pattern_has_dependent_call(s))
				return true;
		for (TokenCASE *c : ts->cases)
			if (tsubst_pattern_has_dependent_call(c))
				return true;
		if (tsubst_pattern_has_dependent_call(ts->defaultcase))
			return true;
	}
	if (TokenTRY *tt = dynamic_cast<TokenTRY *>(tb)) {
		if (tsubst_pattern_has_dependent_call(tt->try_body))
			return true;
		for (TokenBase *b : tt->catch_bodies)
			if (tsubst_pattern_has_dependent_call(b))
				return true;
	}
	if (TokenTHROW *tt = dynamic_cast<TokenTHROW *>(tb))
		if (tsubst_pattern_has_dependent_call(tt->throw_expr))
			return true;
	if (TokenLabel *tl = dynamic_cast<TokenLabel *>(tb))
		if (tsubst_pattern_has_dependent_call(tl->labeled))
			return true;
	if (TokenExplicitDtor *td = dynamic_cast<TokenExplicitDtor *>(tb))
		if (tsubst_pattern_has_dependent_call(td->obj))
			return true;
	return false;
}

node_t CirBuilder::tsubst_method_body(TokenFunc *tf, FuncDef *fd,
				      const char **reason_out)
{
	// Record WHY a covered-shaped method fell back, so the --show-stats
	// fallback profile is a self-diagnosing worklist (the campaign drives
	// these reasons down one at a time). Default reason: a true fall-through.
	auto bail = [&](const char *why) -> node_t {
		if (reason_out)
			*reason_out = why;
		return NULL;
	};
	if (!madc_tsubst_dep_parse_enabled())
		return NULL;
	m_tsubst_body_carries_meminits = false;
	// Phase-5 slice 2 soak lever: force EVERY covered body to bail so the
	// skipped-body materialization fallback is exercisable suite-wide (the
	// gated runs never bail — 0 fallbacks — so the fallback would otherwise
	// be dead code until a real bail appears). Manual runs only; dies with
	// the re-parse machinery (slice 4).
	if (getenv("MADC_XTEST_TSUBST_FORCE_BAIL"))
		return bail("forced bail (slice-2 soak lever)");
	FuncDef *source = fd ? fd->tsubst_source : NULL;
	if (!source)
		return bail("no tsubst source");
	if (!source->dependent_pattern) {
		// Distinguish "the eligibility gate rejected this shape" (and WHICH
		// clause) from "eligible but the recipe re-parse failed". This makes
		// the --show-stats worklist name the exact gate to widen next.
		const char *elig = NULL;
		if (m_prog && !m_prog->tsubst_eligible(source, &elig))
			return bail(elig ? elig : "ineligible");
		return bail("recipe parse failed");
	}
	TokenFunc *recipe = source->dependent_pattern;
	// Destroy helpers are safe on Tree-1 when the retained body either already
	// contains a direct `__destroy(T*)` marker or is the structural iterator
	// loop that destroys `addressof(*it)` while walking `[first,last)`.
	if (source->method_display_name == "__destroy"
	    && !tsubst_pattern_has_destroy_template_pointee(recipe)
	    && !tsubst_pattern_has_destroy_iterator_loop(recipe)
	    && !tsubst_pattern_is_empty_body(recipe))
		return bail("destroy-helper guard");
	// Reference-parameter value reads (scalar AND pack) are covered: the
	// Tree-1 recipe lowers each value read to an UNTYPED N_DEREF of the
	// parameter id, which c2mir types from the concrete instantiated shell's
	// parameter declaration — per-instantiation re-typing for free (g++'s
	// INDIRECT_REF tsubst rebuilds the deref from the substituted operand the
	// same way). Class-object argument uses defer via the N_IGNORE binding
	// marker; anything genuinely unsupported errors into the self-detecting
	// pattern-build fallback below.
	if (tsubst_pattern_has_dependent_call(recipe))
		return bail("dependent system-header call (scan)");

	// Build the recipe body cir ONCE into an immutable Tree-1 pattern, memoized
	// on the source template; later instantiations only copy+substitute it. The
	// recipe is a method of the same owner class, so it lowers in this concrete
	// method's already-set-up context (same `__this`, same member layout).
	std::map<FuncDef *, cir_node *>::iterator pi =
		m_tsubst_body_patterns.find(source);
	if (pi == m_tsubst_body_patterns.end()) {
		// Build in pattern mode so a placeholder TYPE (a body cast `(U)x`, a local
		// `U tmp;`) becomes a deferred type-spec marker rather than an error;
		// tsubst expands the marker to the concrete type per instantiation. A
		// pattern that still carries a genuine error node (some other unsupported
		// construct) is rejected -> memo NULL -> fall back to re-parse.
		bool saved_mode = m_tsubst_pattern_mode;
		std::set<std::string> saved_refs = referenced_funcs;
		node_t pat = NULL;
		m_tsubst_pattern_mode = true;
		{
			TsubstSpeculativeDiagnostics diag(m_prog);
			try {
				pat = translate_block((TokenCpnd *)recipe);
			} catch (...) {
				pat = NULL;
			}
			diag.restore_public_state();
		}
		m_tsubst_pattern_mode = saved_mode;
		referenced_funcs = saved_refs;
		const char *pat_err = NULL;
		if (pat && cir_tree_has_error(pat)) {
			// The first error in the copied pattern names the exact
			// sub-call/type the recipe could not resolve (e.g. a
			// "tsubst: system-header dependent call" from a nested
			// member call). Surface it so the worklist is specific.
			pat_err = cir_first_error_msg(pat);
			pat = NULL;
		}
		m_tsubst_body_patterns[source] = pat ? CIR_NODE(pat) : NULL;
		pi = m_tsubst_body_patterns.find(source);
		if (!pat)
			return bail(pat_err ? pat_err : "pattern build error");
		// Phase-5 slice 1: build the mem-init pattern alongside the body
		// pattern. Admitted shape only (everything else keeps the shell
		// path — absent map entry): every ci is a single-arg (or empty)
		// ASSIGN-path member init (scalar/pointer/reference member), the
		// owner has no bases / vtable / NSDMIs and no class-instance
		// members (so the pattern inits are the ENTIRE ctor prologue and
		// body-head emission order is [class.base.init]-correct).
		FuncDef *recipe_fd = dynamic_cast<FuncDef *>(recipe->var.type);
		DataDefCLASS *pat_ocls =
			dynamic_cast<DataDefCLASS *>(source->member_template_owner);
		const FuncDef::CtorInitializer *pat_del =
			(recipe_fd && pat_ocls)
				? find_delegating_initializer(pat_ocls, recipe_fd)
				: NULL;
		if (pat_del && recipe_fd->ctor_initializers.size() == 1) {
			// DELEGATING ctor (the pair piecewise shape): the delegation
			// call is the ENTIRE construction prologue — no class-shape
			// admission needed (bases/members/vptr belong to the target
			// ctor). The target is selected per instantiation from the
			// substituted arg types, so keep the TOKEN args for the hit
			// path's relower.
			TsubstMemInitPattern p;
			p.name = pat_del->name;
			p.delegating = true;
			p.ci_args = pat_del->args;
			m_tsubst_meminit_patterns[source] =
				std::vector<TsubstMemInitPattern>(1, p);
			if (getenv("MADC_XTEST_PAT_MEMINIT_DEBUG"))
				fprintf(stderr, "[MEMINIT-MEMO] deleg owner=%s ci=%s nargs=%zu\n",
					pat_ocls->name.c_str(), pat_del->name.c_str(),
					pat_del->args.size());
		} else if (recipe_fd && pat_ocls && !pat_del
		    && !recipe_fd->ctor_initializers.empty()
		    && pat_ocls->bases.empty() && !pat_ocls->has_vtable
		    && pat_ocls->member_default_inits.empty()) {
			bool admit = true;
			for (const auto &m : pat_ocls->members)
				if (as_class_instance(m.second))
					admit = false;
			std::vector<TsubstMemInitPattern> mip;
			for (const FuncDef::CtorInitializer &ci :
			     recipe_fd->ctor_initializers) {
				if (!admit) break;
				DataDef *mem = NULL;
				std::string mem_name;
				for (const auto &m : pat_ocls->members)
					if (ci.name == m.first
					    || last_scope_part(ci.name) == m.first) {
						mem = m.second;
						mem_name = m.first;
						break;
					}
				if (!mem || ci.args.size() > 1
				    || (ci.args.size() == 1 && !ci.args[0])) {
					admit = false;	// delegating/base/multi-arg: shell path
					break;
				}
				TsubstMemInitPattern p;
				p.name = mem_name;
				if (ci.args.empty()) {
					// `member()` value-init: scalar/pointer only.
					if (!mem->is_numeric() && !mem->is_pointer()) {
						admit = false;
						break;
					}
					p.value_init = true;
				} else {
					bool saved_mode2 = m_tsubst_pattern_mode;
					std::set<std::string> saved_refs2 = referenced_funcs;
					node_t an = NULL;
					m_tsubst_pattern_mode = true;
					{
						TsubstSpeculativeDiagnostics diag2(m_prog);
						try {
							an = translate_expr(ci.args[0]);
						} catch (...) {
							an = NULL;
						}
						diag2.restore_public_state();
					}
					m_tsubst_pattern_mode = saved_mode2;
					referenced_funcs = saved_refs2;
					if (!an || cir_tree_has_error(an)) {
						admit = false;
						break;
					}
					p.arg = CIR_NODE(an);
				}
				mip.push_back(p);
			}
			if (admit && !mip.empty())
				m_tsubst_meminit_patterns[source] = mip;
		} else if (recipe_fd && pat_ocls && !pat_del
		    && !recipe_fd->ctor_initializers.empty()
		    && pat_ocls->member_default_inits.empty()) {
			// Member-CONSTRUCTION shape (the pair indexed ctor:
			// `first(std::forward<_Args1>(std::get<_Indexes1>(__t1))...)`).
			// Every ci must name a data member, and every class-instance
			// member must be covered by a ci — an uncovered one would
			// default-construct in the PROLOGUE, before these body-head
			// inits (declaration-order violation). Bases/vtable are fine:
			// they emit in the prologue, always before members. Args stay
			// TOKENS; the hit relowers them per instantiation (pack
			// expansions need the live window).
			bool admit2 = true;
			std::vector<TsubstMemInitPattern> mip2;
			std::set<std::string> ci_member_names;
			for (const FuncDef::CtorInitializer &ci :
			     recipe_fd->ctor_initializers) {
				std::string mem_name;
				for (const auto &m : pat_ocls->members)
					if (ci.name == m.first
					    || last_scope_part(ci.name) == m.first) {
						mem_name = m.first;
						break;
					}
				if (mem_name.empty()) {
					admit2 = false;	// base/unknown ci: shell path
					break;
				}
				ci_member_names.insert(mem_name);
				TsubstMemInitPattern p;
				p.name = mem_name;
				p.construct = true;
				p.ci_args = ci.args;
				mip2.push_back(p);
			}
			if (admit2)
				for (const auto &m : pat_ocls->members)
					if (as_class_instance(m.second)
					    && !ci_member_names.count(m.first)) {
						admit2 = false;
						break;
					}
			if (admit2 && !mip2.empty()) {
				m_tsubst_meminit_patterns[source] = mip2;
				if (getenv("MADC_XTEST_PAT_MEMINIT_DEBUG"))
					fprintf(stderr, "[MEMINIT-MEMO] construct owner=%s ncis=%zu\n",
						pat_ocls->name.c_str(), mip2.size());
			}
		}
	}
	cir_node *pattern = pi->second;
	if (!pattern)
		return bail("pattern build error (memoized)");

	size_t need = source ? source->template_param_names.size() : 0;
	// Bind {placeholder -> concrete} directly from the parser-owned template
	// argument vectors. Scalars live in tsubst_type_args in non-pack order; type
	// packs live in the parallel tsubst_type_arg_packs slots. This is the
	// g++/clang shape: deduction/defaulting computes args once, then tsubst
	// consumes them by parameter index. Reconstructing the binding from the
	// instantiated signature loses body-only params and pack arity.
	if (!m_prog || need == 0)
		return bail("no template params");
	std::map<DataDef *, DataDef *> binding;
	std::map<unsigned, DataDef *> pack_params;
	size_t scalar_pos = 0;
	bool has_pack = false;
	for (size_t i = 0; i < need; ++i) {
		DataDefTemplateParam *param =
			m_prog->intern_template_param(source->template_param_names[i],
						      (unsigned)i);
		if (!param)
			return bail("param intern failure");
		bool is_pack = i < source->template_param_is_pack.size()
			    && source->template_param_is_pack[i];
		if (is_pack) {
			if (fd->tsubst_type_arg_packs.size() <= i)
				return bail("missing pack type-args");
			pack_params[(unsigned)i] = param;
			has_pack = true;
			continue;
		}
		if (scalar_pos >= fd->tsubst_type_args.size()
		    || !fd->tsubst_type_args[scalar_pos])
			return bail("missing scalar type-args");
		binding[param] = fd->tsubst_type_args[scalar_pos++];
	}
	if (scalar_pos != fd->tsubst_type_args.size())
		return bail("type-arg arity mismatch");

	const std::vector<std::vector<DataDef *> > *saved_packs =
		m_tsubst_active_type_arg_packs;
	std::map<unsigned, DataDef *> saved_pack_params =
		m_tsubst_active_pack_params;
	int saved_copy_index = m_tsubst_copy_pack_index;
	size_t saved_copy_elem = m_tsubst_copy_pack_elem;
	const char *saved_copy_name = m_tsubst_copy_pack_value_name;
	bool saved_under_deref = m_tsubst_copy_under_deref;
	m_tsubst_active_type_arg_packs =
		has_pack ? &fd->tsubst_type_arg_packs : NULL;
	m_tsubst_active_pack_params = pack_params;
	m_tsubst_copy_pack_index = -1;
	m_tsubst_copy_pack_elem = 0;
	m_tsubst_copy_pack_value_name = NULL;
	m_tsubst_copy_under_deref = false;
	// Local-class-in-template (g++ TAG_DEFN): a class defined in this template
	// body (`_M_construct`'s `_Guard`, the reduced `Box::build`'s `Guard`) is
	// instantiated ALONG WITH the enclosing method — the parser already built the
	// concrete `<owner>__<local>` class + its ctor/dtor. The Tree-1 pattern still
	// names the pattern local class (its ctor/dtor calls are raw-copied), so map
	// pattern local class -> concrete (for the local var TYPE, so the type-driven
	// scope-exit dtor injection uses the concrete dtor) AND pattern method emit
	// symbol -> concrete method emit symbol (for the raw-copied ctor/dtor CALLS).
	std::map<std::string, std::string> saved_local_method_remap =
		m_tsubst_local_method_remap;
	m_tsubst_local_method_remap.clear();
	if (DataDefCLASS *own = m_cur_method ? m_cur_method->owner_class : NULL) {
		std::set<DataDef *> pat_dds;
		collect_cir_node_datadefs(pattern->as_node(), pat_dds);
		for (DataDef *pd : pat_dds) {
			DataDefCLASS *pc = dynamic_cast<DataDefCLASS *>(pd);
			if (!pc || pc->enclosing_class || binding.count(pd))
				continue;
			std::map<std::string, DataDef *>::iterator ci =
				m_prog->struct_map.find(own->name + "__" + pc->name);
			DataDefCLASS *cc = ci != m_prog->struct_map.end()
				? dynamic_cast<DataDefCLASS *>(ci->second) : NULL;
			if (!cc || cc == pc || cc->enclosing_class != own)
				continue;
			binding[pd] = cc;
			for (auto &pm : pc->method_map) {
				std::map<std::string, Variable *>::iterator cm =
					cc->method_map.find(pm.first);
				if (cm == cc->method_map.end() || !pm.second
				    || !cm->second)
					continue;
				FuncDef *pfd =
					dynamic_cast<FuncDef *>(pm.second->type);
				FuncDef *cfd =
					dynamic_cast<FuncDef *>(cm->second->type);
				if (!pfd || !cfd)
					continue;
				std::string ps = func_emit_name(*pm.second, pfd);
				std::string cs = func_emit_name(*cm->second, cfd);
				if (!ps.empty() && !cs.empty() && ps != cs)
					m_tsubst_local_method_remap[ps] = cs;
			}
		}
	}
	// Snapshot referenced_funcs: the instantiation copy below inserts callee
	// symbols as it builds. If this body ultimately FALLS BACK (a post-copy
	// bail), those inserts must be undone — otherwise a fallen-back body leaves
	// un-emittable symbols (e.g. __gnu_cxx::operator-) recorded as ODR-used, and
	// MIR-link reports an undefined import even though the body re-parsed
	// cleanly. On a hit we keep them. (bail_restore unwinds before falling back.)
	std::set<std::string> saved_ref_funcs = referenced_funcs;
	auto bail_restore = [&](const char *why) -> node_t {
		referenced_funcs = saved_ref_funcs;
		return bail(why);
	};
	node_t result = NULL;
	// Phase-5 slice 1: substituted mem-init statements for the admitted ctor
	// shape, copied under the SAME active binding/pack window as the body.
	std::vector<node_t> meminit_stmts;
	bool meminit_failed = false;
	{
		TsubstSpeculativeDiagnostics diag(m_prog);
		try {
			cir_node *copied = tsubst_cir(pattern, binding);
			result = copied ? copied->as_node() : NULL;
		} catch (...) {
			result = NULL;
		}
		std::map<FuncDef *, std::vector<TsubstMemInitPattern> >::iterator
			mi = result ? m_tsubst_meminit_patterns.find(source)
				    : m_tsubst_meminit_patterns.end();
		if (mi != m_tsubst_meminit_patterns.end()) {
			DataDefCLASS *ocls = m_cur_method
				? m_cur_method->owner_class : NULL;
			for (const TsubstMemInitPattern &p : mi->second) {
				if (p.delegating) {
					if (getenv("MADC_XTEST_PAT_MEMINIT_DEBUG")) {
						// Recon probe: dump the template-origin record
						// (pending_template_instantiations) behind each
						// dependent-shell ci arg type.
						for (TokenBase *a : p.ci_args) {
							DataDefCLASS *sc = a ? dynamic_cast<DataDefCLASS *>(a->datadef()) : NULL;
							fprintf(stderr, "[DELEG-ORIGIN] ci-arg dd=%s cls=%d dep=%d surf=%d canon=%s\n",
								(a && a->datadef()) ? a->datadef()->name.c_str() : "(null)",
								(int)(sc != NULL),
								sc ? (int)sc->is_dependent_placeholder : -1,
								sc ? (int)sc->has_dependent_surface : -1,
								sc ? sc->canonical_cpp_spelling.c_str() : "");
							if (!sc)
								continue;
							std::map<DataDef *, Program::DependentShellOrigin>::iterator
								oi = m_prog->dependent_shell_origin.find(sc);
							if (oi != m_prog->dependent_shell_origin.end()) {
								fprintf(stderr, "[DELEG-ORIGIN]   origin tmpl=%s ns=%s nargs=%zu\n",
									oi->second.tname.c_str(),
									oi->second.defining_namespace.c_str(),
									oi->second.raw_arg_tokens.size());
								for (size_t ai = 0; ai < oi->second.raw_arg_tokens.size(); ++ai) {
									std::string toks;
									for (TokenBase *t : oi->second.raw_arg_tokens[ai]) {
										if (!toks.empty()) toks += " ";
										TokenIdent *ti = dynamic_cast<TokenIdent *>(t);
										if (ti)
											toks += ti->spelling();
										else if (t)
											toks += "#" + std::to_string((int)t->id());
										else
											toks += "(null)";
									}
									fprintf(stderr, "[DELEG-ORIGIN]     arg[%zu] spelling=%s toks={%s}\n",
										ai, oi->second.arg_spellings[ai].c_str(), toks.c_str());
								}
							}
							for (auto &kv : m_prog->pending_template_instantiations)
								for (auto &pti : kv.second) {
									if (pti.mangled_name != sc->name)
										continue;
									fprintf(stderr, "[DELEG-ORIGIN] shell=%s tmpl=%s canon=%s nargs=%zu\n",
										sc->name.c_str(), kv.first.c_str(),
										pti.canonical_spelling.c_str(), pti.args.size());
									for (TokenDataType *at : pti.args)
										fprintf(stderr, "[DELEG-ORIGIN]   arg tok=%s dd=%s tp=%d\n",
											at ? at->spelling() : "(null)",
											(at && at->datadef()) ? at->datadef()->name.c_str() : "(null)",
											(at && at->datadef()) ? (int)at->datadef()->is_template_param() : -1);
								}
						}
					}
					// The delegation IS the whole prologue: relower
					// the target ctor call from the pattern's token
					// args under the active binding/pack window
					// (overload selection is per-instantiation).
					cir_node *dc = NULL;
					if (ocls) {
						try {
							dc = tsubst_relower_deferred_construction(
								p.ci_args, tf, ocls, &binding,
								[&]() -> node_t {
									return id("__this", tf);
								},
								/*yield_this_addr=*/false,
								/*relax_class_args=*/true,
								/*require_overload_match=*/true);
						} catch (...) {
							dc = NULL;
						}
					}
					if (!dc || cir_tree_has_error(dc->as_node())) {
						if (getenv("MADC_XTEST_PAT_MEMINIT_DEBUG")) {
							const char *m = dc ? cir_first_error_msg(dc->as_node()) : NULL;
							fprintf(stderr, "[MEMINIT-DELEG-FAIL] fn=%s why=%s\n",
								tf->var.name.c_str(),
								m ? m : "(relower returned null)");
						}
						meminit_failed = true;
						break;
					}
					meminit_stmts.push_back(dc->as_node());
					continue;
				}
				DataDef *mem = NULL;
				if (ocls)
					for (const auto &m : ocls->members)
						if (m.first == p.name) {
							mem = m.second;
							break;
						}
				if (!mem) {
					meminit_failed = true;
					break;
				}
				TokenBase *origin = tf;
				if (p.construct) {
					// Member-CONSTRUCTION ci (token args, possibly
					// pack expansions): relower per instantiation
					// under the live binding/pack window. A
					// NON-EMPTY pack element is a raw dependent
					// pattern token (`std::forward<_Args1>
					// (std::get<_Indexes1>(__t1))`): the relower
					// binds ALL its packs in lockstep per element
					// (incl. the non-type index pack) and the
					// per-element copy re-resolves each nested
					// dependent call to its concrete instantiation
					// (the tsubst finish_call_expr analogue). A
					// shape it can't ground clean-fails as an error
					// tree (object_arg_addr refuses dependent-typed
					// materialization) — caught below, shell
					// carrier emits the cis.
					if (DataDefCLASS *mc = as_class_instance(mem)) {
						cir_node *cc = NULL;
						try {
							cc = tsubst_relower_deferred_construction(
								p.ci_args, tf, mc, &binding,
								[&]() -> node_t {
									return node1(N_ADDR,
										node2(N_DEREF_FIELD,
										      id("__this", origin),
										      id(p.name.c_str(), origin)),
										origin);
								},
								/*yield_this_addr=*/false,
								/*relax_class_args=*/true,
								/*require_overload_match=*/true);
						} catch (...) {
							cc = NULL;
						}
						if (!cc || cir_tree_has_error(cc->as_node())) {
							if (getenv("MADC_XTEST_PAT_MEMINIT_DEBUG")) {
								const char *m2 = cc ? cir_first_error_msg(cc->as_node()) : NULL;
								fprintf(stderr, "[MEMINIT-CONSTRUCT-FAIL] fn=%s member=%s why=%s\n",
									tf->var.name.c_str(), p.name.c_str(),
									m2 ? m2 : "(relower returned null)");
							}
							meminit_failed = true;
							break;
						}
						meminit_stmts.push_back(cc->as_node());
						continue;
					}
					// Scalar/pointer member: only the EMPTY pack
					// expansion (or bare `member()`) is covered —
					// value-init (`__this->second = 0`). A non-empty
					// pack on a scalar keeps the shell path.
					bool zero = false;
					if (p.ci_args.empty())
						zero = true;
					else if (p.ci_args.size() == 1) {
						TokenPackExpansion *pe =
							dynamic_cast<TokenPackExpansion *>(p.ci_args[0]);
						DataDefTemplateParam *tp = (pe && pe->pattern)
							? template_param_in_pack_pattern(pe->pattern)
							: NULL;
						if (tp && m_tsubst_active_type_arg_packs
						    && tp->param_index
						       < m_tsubst_active_type_arg_packs->size()
						    && (*m_tsubst_active_type_arg_packs)
							       [tp->param_index].empty())
							zero = true;
					}
					if (!zero || (!mem->is_numeric()
						      && !mem->is_pointer())) {
						if (getenv("MADC_XTEST_PAT_MEMINIT_DEBUG"))
							fprintf(stderr, "[MEMINIT-CONSTRUCT-FAIL] fn=%s member=%s why=scalar shape not covered\n",
								tf->var.name.c_str(), p.name.c_str());
						meminit_failed = true;
						break;
					}
					node_t zfld = node2(N_DEREF_FIELD,
							    id("__this", origin),
							    id(p.name.c_str(), origin));
					node_t zasgn = node2(N_ASSIGN, zfld,
							     integer(0L, origin), origin);
					meminit_stmts.push_back(
						node2(N_EXPR, list(), zasgn, origin));
					continue;
				}
				node_t fld = node2(N_DEREF_FIELD,
						   id("__this", origin),
						   id(p.name.c_str(), origin));
				node_t init;
				if (p.value_init)
					init = integer(0L, origin);
				else {
					cir_node *ic = NULL;
					try {
						ic = copy_cir_subtree(p.arg, &binding);
					} catch (...) {
						ic = NULL;
					}
					if (!ic || cir_tree_has_error(ic->as_node())) {
						meminit_failed = true;
						break;
					}
					init = ic->as_node();
					// Reference member (pointer slot) BINDS: store the
					// referent's address (class_ctor_initializer_stmts model).
					if (mem->is_reference())
						init = node1(N_ADDR, init, origin);
				}
				node_t asgn = node2(N_ASSIGN, fld, init, origin);
				meminit_stmts.push_back(
					node2(N_EXPR, list(), asgn, origin));
			}
		}
		diag.restore_public_state();
	}
	m_tsubst_active_type_arg_packs = saved_packs;
	m_tsubst_active_pack_params = saved_pack_params;
	m_tsubst_copy_pack_index = saved_copy_index;
	m_tsubst_copy_pack_elem = saved_copy_elem;
	m_tsubst_copy_pack_value_name = saved_copy_name;
	m_tsubst_copy_under_deref = saved_under_deref;
	m_tsubst_local_method_remap = saved_local_method_remap;
	// A type-spec marker that tsubst could not expand (unbound, or a concrete
	// type append_type_specs can't render here) left an error node -> fall back.
	if (!result)
		return bail_restore("substitute produced no tree");
	if (cir_tree_has_error(result)) {
		const char *m = cir_first_error_msg(result);
		return bail_restore(m ? m : "substitute error");
	}
	// Record the tsubst body's concrete callees as ODR-used AND verify each is
	// emittable. The copy path doesn't re-run the normal call lowering that
	// records callees, so without this the translate_module drain never
	// materializes their deferred-lazy definitions (e.g. _Rb_tree::_M_get_node)
	// → MIR-link "undefined import". Recording an emittable callee fixes that.
	// But a callee with NO definition source (e.g. an un-instantiated free
	// operator __gnu_cxx::operator-) cannot be satisfied — emitting a dangling
	// reference would fail the link, so the body is not yet safely tsubst-able:
	// fall back to the parsed concrete body (the completeness check). A landed
	// operator/member re-resolve KIND will make such a callee emittable and the
	// body becomes a hit then.
	{
		std::set<std::string> callees;
		cir_collect_call_callees(result, callees);
		// Mem-init statements ride at the head of the returned body (below):
		// their callees (delegated ctor, member ctor calls, arg calls) need
		// the same ODR-record + emittable gate — but collected SEPARATELY:
		// an un-emittable mem-init callee fails only the mem-inits (shell
		// carrier emits the cis), never the body hit.
		std::set<std::string> meminit_callees;
		if (!meminit_failed)
			for (node_t s : meminit_stmts)
				cir_collect_call_callees(s, meminit_callees);
		// Pass 1.6 synthesizes destructors for synth-eligible classes and emits
		// them DIRECTLY into the module (not as pending_funcs FuncDefs), so the
		// pending-funcs scan below cannot see them. Pre-collect their base dtor
		// symbols using the SAME predicate Pass 1.6 uses (class_gets_synth_dtor),
		// so a tsubst'd pseudo-destructor call (e.g. __new_allocator::destroy<T>
		// -> T::~T) recognizes the dtor as emittable instead of falling back.
		// Lockstep: only symbols Pass 1.6 will actually emit are admitted here, so
		// no dangling reference can result.
		std::set<std::string> synth_dtor_syms;
		for (auto &kv : m_prog->struct_map) {
			DataDefCLASS *cdd = as_user_class(kv.second);
			if (class_gets_synth_dtor(cdd))
				synth_dtor_syms.insert(class_dtor_symbol(cdd));
		}
		auto emittable = [&](const std::string &s) -> bool {
			if (is_c2mir_builtin_call_name(s)) return true;
			if (m_prog->has_deferred_lazy_body(s)) return true;
			if (external_symbol_available(s)) return true;
			if (synth_dtor_syms.count(s)) return true;
			for (TokenBase *pb : m_prog->pending_funcs) {
				TokenFunc *tf = dynamic_cast<TokenFunc *>(pb);
				FuncDef *tfd = tf ? dynamic_cast<FuncDef *>(tf->var.type)
						  : NULL;
				// A declaration-only FuncDef in pending_funcs (e.g. an
				// instantiated free operator/std::move signature with no
				// body) is NOT a definition — matching it here is what
				// false-passed __gnu_cxx::operator-. Require a real body.
				if (tf && tfd && !tfd->declaration_only
				    && func_emit_name(tf->var, tfd) == s) return true;
			}
			return false;
		};
		for (const std::string &s : callees) {
			if (!emittable(s)) {
				if (getenv("MADC_XTEST_PAT_MEMINIT_DEBUG"))
					fprintf(stderr, "[TSUBST-UNEMITTABLE] fn=%s sym=%s\n",
						tf->var.name.c_str(), s.c_str());
				return bail_restore("tsubst body calls un-emittable symbol");
			}
			referenced_funcs.insert(s);
		}
		for (const std::string &s : meminit_callees) {
			if (meminit_failed)
				break;
			if (!emittable(s)) {
				if (getenv("MADC_XTEST_PAT_MEMINIT_DEBUG"))
					fprintf(stderr, "[TSUBST-UNEMITTABLE-MEMINIT] fn=%s sym=%s\n",
						tf->var.name.c_str(), s.c_str());
				meminit_failed = true;
			}
		}
		// ODR-record mem-init callees only when the mem-inits survive —
		// a dropped set must leave no dangling referenced symbols.
		if (!meminit_failed)
			for (const std::string &s : meminit_callees)
				referenced_funcs.insert(s);
	}
	// Phase-5 slice 1: a fully-substituted mem-init set rides at the head of
	// the hit body ([class.base.init]-correct for the admitted shape: the
	// inits ARE the entire prologue) and func_def suppresses its shell-side
	// emission (whole-ctor switch). A failed substitution just keeps the
	// shell path — mem-inits are not yet load-bearing under hybrid B.
	m_tsubst_body_carries_meminits = false;
	if (getenv("MADC_XTEST_PAT_MEMINIT_DEBUG") && (meminit_failed || !meminit_stmts.empty()))
		fprintf(stderr, "[MEMINIT-HIT] fn=%s stmts=%zu failed=%d\n",
			tf->var.name.c_str(), meminit_stmts.size(), (int)meminit_failed);
	if (!meminit_failed && !meminit_stmts.empty()) {
		node_t outer = list();
		for (node_t s : meminit_stmts)
			append(outer, s);
		append(outer, result);
		result = node2(N_BLOCK, list(), outer, tf);
		m_tsubst_body_carries_meminits = true;
	}
	return result;
}

static std::string tsubst_profile_template_key(FuncDef *source)
{
	if (!source)
		return "<unknown-template>";
	std::ostringstream out;
	if (source->member_template_owner) {
		std::string owner = source->member_template_owner->canonical_cpp_spelling.empty()
				  ? source->member_template_owner->name
				  : source->member_template_owner->canonical_cpp_spelling;
		size_t lt = owner.find('<');
		if (lt != std::string::npos)
			owner = owner.substr(0, lt);
		if (!owner.empty())
			out << owner << "::";
	}
	if (!source->method_display_name.empty())
		out << source->method_display_name;
	else if (!source->function_display_name.empty())
		out << source->function_display_name;
	else
		out << "<anonymous>";
	if (!source->template_param_names.empty()) {
		out << "<";
		for (size_t i = 0; i < source->template_param_names.size(); ++i) {
			if (i)
				out << ",";
			bool is_pack = i < source->template_param_is_pack.size()
				    && source->template_param_is_pack[i];
			out << source->template_param_names[i];
			if (is_pack)
				out << "...";
		}
		out << ">";
	}
	return out.str();
}

static std::string tsubst_profile_concrete_sample(TokenFunc *tf, FuncDef *fd)
{
	if (tf && !tf->var.name.empty())
		return tf->var.name;
	return "<unknown-instantiation>";
}

node_t CirBuilder::func_def(TokenFunc *tf)
{
	FuncDef *fd = dynamic_cast<FuncDef *>(tf->var.type);
	if (!fd) return NULL;
#ifdef MADC_DEBUG_CTORINIT
	fprintf(stderr, "[ctorinit] func-def %s\n", tf->var.name.c_str());
#endif
	Method *saved_cur_method = m_cur_method;
	m_cur_method = tf->method;
	// Fresh defer-scope context per function body (a nested/hoisted function
	// must not see the enclosing function's pending defers).
	std::vector<TokenCpnd *> saved_defer_scopes;
	saved_defer_scopes.swap(m_defer_scopes);

	DataDef *ret_dd = &fd->return_value_type();
	bool ret_is_ptr = ret_dd && ret_dd->is_pointer();
	bool ret_is_ref = fd->returns_reference();   // T& -> returned by address (one more *)
	int ret_star_depth = dd_peel_pointers(ret_dd);

	// A by-value non-trivial class return uses the struct-return (__retbuf) ABI:
	// the C return type is `void`, a hidden `struct <T> *__retbuf` is the first
	// parameter, and `return obj;` copy-constructs *__retbuf (see
	// translate_return). A trivial struct keeps c2mir's native struct return.
	DataDefCLASS *ret_obj = (!ret_is_ptr && !ret_is_ref && !fd->is_multi_return())
				? class_return_via_retbuf(ret_dd) : NULL;
	m_cur_func_returns_object = ret_obj;
	bool ret_via_retbuf = ret_obj != NULL;
	// A by-VALUE class return that uses c2mir's NATIVE struct return (trivially
	// copyable: no retbuf). translate_return must still apply an implicit
	// converting constructor when `return expr;`'s class differs from the return
	// class (return-value copy-initialization). The retbuf path handles the
	// non-trivial case; this covers the trivially-copyable one.
	m_cur_func_returns_value_class = (!ret_is_ptr && !ret_is_ref
					  && !fd->is_multi_return() && !ret_via_retbuf)
					 ? as_class_instance(ret_dd) : NULL;
	DataDef *retbuf_dd = (DataDef *)ret_obj;
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
	// Track a `Cls *` or `Cls&` return so translate_return can emit a
	// derived->base adjustment for returned class subobjects.
	m_cur_func_returns_class_ptr = ret_is_ptr ? pointee_user_class(&fd->return_value_type())
						  : (ret_is_ref ? as_user_class(&fd->return_value_type())
								: NULL);
	// Track the scalar C return type so a gcc-accepted bare `return;` in a
	// non-void function gets a typed zero (the returned value is indeterminate
	// per gnu89/c11, so zero is a conformant lowering). Skip ref (returns by
	// address) — a bare return there is already nonsensical and rare.
	m_cur_func_scalar_ret = (!ret_via_retbuf && !ret_is_ref && !m_cur_func_returns_void
				 && !ret_is_multi)
				? &fd->return_value_type() : NULL;

	// A function whose return type is a function pointer — `RET (*f(params))
	// (fp-params)` (e.g. an instantiated std::for_each returning its functor
	// when the functor is a fn-ptr). type_list renders a DataDefFPTR as a bare
	// `long`, which made `return <fn-ptr>;` warn ("returning pointer without
	// cast for integer result"). Emit the real declarator instead: the spec is
	// the pointed-to function's RETURN type and the `(*)(fp-params)` suffix wraps
	// the function declarator (built by fnptr_decl_pieces at the decl_list below).
	DataDefFPTR *ret_fnptr = (!ret_via_retbuf && !ret_is_multi && !ret_is_ref
				 && fd->return_typedef_name.empty())
				? dynamic_cast<DataDefFPTR *>(ret_dd) : NULL;

	// Retbuf-returning OR multi-return fn: C return type is `void`.
	int ret_decl_stars = ret_star_depth;
	node_t ret_type = NULL;
	m_cur_func_ret_spec_dd = NULL;
	m_cur_func_ret_spec_alias.clear();
	m_cur_func_ret_stars = 0;
	if (ret_via_retbuf || ret_is_multi) {
		ret_type = type_list(&ddVOID);
		ret_decl_stars = 0;
	} else if (ret_fnptr) {
		// Spec filled by fnptr_decl_pieces (with the suffix) at decl_list build.
		ret_type = list();
		ret_decl_stars = 0;
	} else if (!fd->return_typedef_name.empty()) {
		ret_type = type_list(&fd->return_value_type(), fd->return_typedef_name);
		ret_decl_stars = explicit_star_count(&fd->return_value_type(),
						     fd->return_typedef_name);
		if (!m_cur_func_returns_void) {
			m_cur_func_ret_spec_dd = &fd->return_value_type();
			m_cur_func_ret_spec_alias = fd->return_typedef_name;
		}
	} else {
		ret_type = type_list(ret_dd);
		if (!m_cur_func_returns_void)
			m_cur_func_ret_spec_dd = ret_dd;
	}
	// Record the C return type's pointer suffix count (a ref-return adds one
	// below) so translate_return can declare a matching return-value temp
	// when pending defers must run AFTER the return expression is evaluated.
	m_cur_func_ret_stars = ret_decl_stars + (ret_is_ref ? 1 : 0);

	// optimize("-fno-strict-aliasing") rides the FUNC_DEF specs as an N_ATTR
	// (the vector_size/cleanup convention); c2mir's sema marks the function and
	// generates its accesses with alias class 0, surviving MIR inlining.
	if (fd->no_strict_aliasing) {
		node_t attr_args = list();
		static const char no_sa[] = "-fno-strict-aliasing";
		append(attr_args, str(no_sa, sizeof (no_sa), tf));
		append(ret_type, node2(N_ATTR, id("optimize", tf), attr_args, tf));
	}

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
			if (ptypedef.empty() && i < fd->param_typedef_names.size())
				ptypedef = fd->param_typedef_names[i];
			append(param_list, param_decl(fd->parameters[i], pname, ptypedef));
		}
	}
	if (fd->is_varargs)
		append(param_list, simple(N_DOTS));

	node_t func_inner = node1(N_FUNC, param_list);

	node_t func_id = id(tf->var.name.c_str(), tf);
	node_t decl_list = list();
	append(decl_list, func_inner);
	if (ret_fnptr) {
		// Function returning a fn-ptr: append the `*(fp-params)` suffix after
		// the function declarator and fill ret_type with the pointed-to
		// function's return-type specs. `RET (*f(params))(fp-params)`.
		fnptr_decl_pieces(ret_fnptr->target, true, ret_type, decl_list,
				  std::vector<carray_dim_t>());
	} else {
		for (int rs = 0; rs < ret_decl_stars; rs++)
			append(decl_list, pointer());
		// A T&-returning method returns by address: one extra pointer level (so
		// `int&`->`int*`, `char*&`->`char**`). Matches g++'s reference ABI.
		if (ret_is_ref)
			append(decl_list, pointer());
	}

	node_t decl = node2(N_DECL, func_id, decl_list);
	// Parse-once (DEFAULT since the flip): a covered instantiated member-template
	// method builds its body by tsubst of the Tree-1 recipe (no body re-parse);
	// everything else falls back to lowering the parsed body. tsubst_method_body
	// returns NULL when uncovered or when MADC_XTEST_DEP_PARSE=0 opts out.
	const char *tsubst_reason = NULL;
	node_t body = tsubst_method_body(tf, fd, &tsubst_reason);
	if (m_prog && fd->tsubst_source) {
		if (body)
			++m_prog->_tsubst_body_hits;
		else {
			++m_prog->_tsubst_body_fallbacks;
			std::string key =
				tsubst_profile_template_key(fd->tsubst_source);
			Program::TsubstBodyProfile &entry =
				m_prog->_tsubst_body_fallback_profile[key];
			++entry.count;
			if (entry.sample.empty())
				entry.sample = tsubst_profile_concrete_sample(tf, fd);
			if (entry.reason.empty() && tsubst_reason)
				entry.reason = tsubst_reason;
		}
	}
	if (!body) {
		// Phase-5 slice 2: this instantiation's body parse was SKIPPED
		// (its source pattern covers every suite path today), so a
		// tsubst bail must first materialize the captured span into tf
		// — the re-parse fallback, deleted with the machinery at
		// slice 4 (slice 3 makes a covered-shape bail a hard error).
		if (m_prog && fd->tsubst_source
		    && !fd->tsubst_skipped_body_tokens.empty())
			m_prog->materialize_tsubst_skipped_body(tf);
		body = translate_block((TokenCpnd *)tf);
	}

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
			DataDef *capt_ptr = capture_param_type(cv);
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
		if (!is_ctor)
			for (Variable *cv : ocls->ctors)
				if (cv && (cv->name == fname || cv->type == fd)) {
					is_ctor = true;
					break;
				}
		bool is_dtor = (fname == ocls->name + "___dtor");
		// Cast __this to `Base *` (offset-adjusted to the base subobject) for a
		// chained base ctor/dtor call. Offset 0 (primary/single base) = a plain
		// cast, byte-identical to the previous single-base path.
		auto base_addr_at = [&](DataDefCLASS *b, size_t off) -> node_t {
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
				node1(N_LIST, class_tag_ref(b)),
				node2(N_DECL, ignore(), node1(N_LIST, pointer())));
			return node2(N_CAST, t, adj, tf);
		};
		auto base_call_at = [&](DataDefCLASS *b, size_t off, const std::string &bsym) -> node_t {
			referenced_funcs.insert(bsym);
			node_t a = list();
			append(a, base_addr_at(b, off));
			node_t call = node2(N_CALL, id(bsym.c_str(), tf), a, tf);
			return node2(N_EXPR, list(), call, tf);
		};
		// Prologue statements (run before the body), in order:
		//   1. base ctor call (ctor of a derived class with a base ctor)
		//   2. __this->__vptr = (void*)ClassName__vtable (ctor of a virtual
		//      class) — set after base construction, matching C++.
		// Epilogue (after the body): base dtor call (dtor with a base dtor).
		std::vector<node_t> prologue, epilogue;
		std::set<std::string> explicit_member_inits;
		// C++11 DELEGATING constructor ([class.base.init]p6): the targeted
		// same-class ctor performs the COMPLETE initialization (bases,
		// members, vptr) before this ctor's body runs — the entire
		// construction prologue is the delegation call; everything below
		// (base ctors, member inits, default member construction, vptr
		// stores) belongs to the target ctor, not this one.
		const FuncDef::CtorInitializer *delegating_ci =
			(is_ctor && fd) ? find_delegating_initializer(ocls, fd)
					: NULL;
		// Phase-5 slice 1 whole-ctor switch (delegating form): a tsubst hit
		// body already carrying the substituted delegation call owns the
		// entire prologue — the shell-token-side call would double-construct.
		if (delegating_ci && !m_tsubst_body_carries_meminits) {
#ifdef MADC_DEBUG_CTORINIT
			fprintf(stderr, "[ctorinit] DELEGATING-EMIT owner=%s fn=%s ci=%s nargs=%zu ninit=%zu\n",
				ocls->name.c_str(), tf->var.name.c_str(),
				delegating_ci->name.c_str(), delegating_ci->args.size(),
				fd->ctor_initializers.size());
#endif
			node_t stmt = class_ctor_call_addr(id("__this", tf), ocls,
							   delegating_ci->args, tf);
			flush_pending_stmts(prologue);
			if (stmt) prologue.push_back(stmt);
		}
		if (is_ctor && !delegating_ci && fd)
			for (const FuncDef::CtorInitializer &ci : fd->ctor_initializers)
				for (const auto &m : ocls->members)
					if (ci.name == m.first || last_scope_part(ci.name) == m.first)
						explicit_member_inits.insert(m.first);
		// Construct each base in declaration order, with `this` adjusted to that
		// base's subobject (MI). Single inheritance = one base @ offset 0.
		if (is_ctor && !delegating_ci)
			for (size_t bi = 0; bi < ocls->bases.size(); bi++) {
				if (ocls->bases[bi].is_virtual) continue; // vbases: complete-object site
				DataDefCLASS *b = ocls->bases[bi].base;
#ifdef MADC_DEBUG_CTORINIT
				fprintf(stderr, "[ctorinit] base-construct owner=%s [%zu]=%s (b==owner? %d, has_user_ctor=%d)\n",
					ocls->name.c_str(), bi, b ? b->name.c_str() : "(null)",
					(int)(b == ocls), (int)(b && b->has_user_ctor));
#endif
				if (const FuncDef::CtorInitializer *ci =
					find_base_initializer(ocls, b, fd)) {
					node_t stmt = class_ctor_call_addr(
						base_addr_at(b, ocls->base_offset_of(b)),
						b, ci->args, tf);
					flush_pending_stmts(prologue);
					if (stmt) prologue.push_back(stmt);
					continue;
				}
				if (b->has_user_ctor) {
					// Resolve the base's DEFAULT ctor through the
					// ctor list — a RENAMED nested class registers
					// its ctor as Class__SourceName, so the blind
					// Class__Class composition is an undefined
					// import there.
					FuncDef *bctor = select_ctor_overload(
						b, std::vector<TokenBase *>());
					prologue.push_back(base_call_at(b,
						ocls->base_offset_of(b),
						bctor ? ctor_call_symbol(b, bctor)
						      : b->name + "__" + b->name));
				}
			}
		// Phase-5 slice 1 whole-ctor switch: a tsubst hit body that already
		// carries the substituted mem-inits (at its head) owns them — the
		// shell-token-side emission would double-initialize.
		if (is_ctor && !delegating_ci && !m_tsubst_body_carries_meminits)
			class_ctor_initializer_stmts(ocls, fd, prologue, tf);
		// C++11 default member initializers (`int x = 5;`): applied for every
		// member NOT given an explicit ctor member-init, after the ctor-init
		// list (which overrides them via explicit_member_inits).
		if (is_ctor && !delegating_ci)
			emit_member_default_inits(ocls, "__this", true, prologue, tf,
						  &explicit_member_inits);
		// Construct embedded object members at ctor entry (after base ctor),
		// destruct them at dtor exit (before base dtor) — C++ member lifetime.
		if (is_ctor && !delegating_ci)
			class_member_construct(ocls, prologue, tf, &explicit_member_inits);
		if (is_dtor)
			class_member_destruct(ocls, epilogue, tf);
		if (is_ctor && !delegating_ci && ocls->has_vtable) {
			std::string vname = class_vtable_symbol(ocls);
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

	// File-scope class globals construct before any user code runs (C++
	// static init). The ctor calls live in ONE synthesized module function,
	// __madc_global_init (built in translate_module, guarded against
	// re-entry): main calls it here, and call-only embedding sessions
	// (libmadc call/get_global/set_global without main) invoke it at
	// runtime init — both paths share the one implementation. (Scope-exit
	// destruction of file-scope objects stays deferred: program teardown
	// reclaims the memory.)
	if (tf->var.name == "main" && !m_global_ctor_stmts.empty()) {
		node_t outer = list();
		node_t icall = node2(N_CALL, id("__madc_global_init", tf), list(), tf);
		CIR_NODE(icall)->synth_from_origin = true;
		append(outer, node2(N_EXPR, list(), icall, tf));
		append(outer, body);
		body = node2(N_BLOCK, list(), outer, tf);
		// Re-emit with the wrapped body; the prologue runs once at main entry.
		node_t out = node4(N_FUNC_DEF, ret_type, decl, list(), body, tf);
		m_cur_method = saved_cur_method;
		m_defer_scopes.swap(saved_defer_scopes);
		return out;
	}

	node_t out = node4(N_FUNC_DEF, ret_type, decl, list(), body, tf);
	m_cur_method = saved_cur_method;
	m_defer_scopes.swap(saved_defer_scopes);
	return out;
}

// Build the constructor-call statement for a file-scope class global `v` of
// class `cdd`. The initializer comes from the linked TokenDecl (user source:
// `string g = "hi";`) when present, else from the runtime backing object
// (built-ins like `version`, registered via addGlobal with a host-side string
// initializer). A class with no constructor (no has_user_ctor) yields NULL —
// nothing to run.
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

	// No source TokenDecl (built-in global): if the runtime backing object has
	// a stored string payload and the class declares a char* converting ctor,
	// construct from that literal; otherwise fall back to the default ctor.
	FuncDef *cstr_ctor = NULL;
	for (Variable *cv : cdd->ctors) {
		FuncDef *fd = cv ? dynamic_cast<FuncDef *>(cv->type) : NULL;
		if (!fd || fd->parameters.size() < 2) continue;
		DataDef *p1 = fd->parameters[1];
		if (p1 && p1->is_pointer()) { cstr_ctor = fd; break; }
	}
	if (!v->data || !cstr_ctor) {
		std::vector<TokenBase *> none;
		return class_ctor_call(v, cdd, none, NULL);
	}
	const std::string init = (v->data ? *(std::string *)v->data : std::string());
	node_t args = list();
	append(args, node1(N_ADDR, id(v->name.c_str(), NULL), NULL)); // &v (this)
	append(args, str(init.c_str(), init.size() + 1, NULL));       // const char*
	if (cstr_ctor->ctor_trailing_self)
		append(args, node1(N_ADDR, id(v->name.c_str(), NULL), NULL));
	std::string sym = ctor_call_symbol(cdd, cstr_ctor);
	std::vector<ExternParam> eparams;
	eparams.push_back({ {N_VOID}, true });
	eparams.push_back({ {N_CHAR}, true });
	if (cstr_ctor->ctor_trailing_self)
		eparams.push_back({ {N_VOID}, true });
	need_output_extern(sym.c_str(), false, eparams);
	referenced_funcs.insert(sym);
	node_t call = node2(N_CALL, id(sym.c_str(), NULL), args, NULL);
	CIR_NODE(call)->synth_from_origin = true;
	return node2(N_EXPR, list(), call, NULL);
}

// Emit storage for, and queue construction of, every file-scope class-instance
// global. Source-declared globals already had their
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
		for (node_t p : m_pending_stmts)
			m_global_ctor_stmts.push_back(p);
		m_pending_stmts.clear();
		if (cc) m_global_ctor_stmts.push_back(cc);
	}
}


// -----------------------------------------------------------------------
// Host-call shims — the embedding boundary's call surface.
// -----------------------------------------------------------------------
// For every host-callable user function F the module carries a synthesized
//   long __madc_shim_<sym>(char *__args, char *__out)
// adapter over the 32-byte madc_value ABI (madc_api.h): __args is a packed
// madc_value array, __out one madc_value slot. The adapter validates the
// argument type ids (returning 1+i on a mismatch at argument i, 100 on an
// allocation failure, 0 on success), converts arguments through the C value
// helpers, calls F through its NORMAL lowered ABI, and converts the result
// back. ALL class knowledge — constructors, destructor symbols, by-value /
// __retbuf classification, sizes, typeids — comes from the parsed headers
// through the same generic machinery script-side calls use; the HOST never
// touches a class ABI (the no-per-class-helpers principle). The value
// helpers are compiler-machinery extern-C symbols (the cpp-first-api.md
// exception category), resolved from the host process at MIR link.

namespace {

// One marshallable parameter/return position.
struct ShimSlot {
	enum Kind { K_BOOL, K_INT, K_REAL, K_CSTR, K_CLASS_TEXT, K_CLASS_INST } kind;
	DataDefCLASS *cdd = NULL;     // class kinds
	FuncDef *text_ctor = NULL;    // K_CLASS_TEXT: the const-char* converting ctor
};

// The const-char*-converting constructor of `cdd`, if any (the same loose
// shape global_ctor_call's built-in path matches: this + one pointer param).
// Must be CALLABLE with exactly one text argument: every parameter after the
// const-char* needs a default (real <string> declares basic_string(const
// char*, size_type, const allocator&) with NO size default ahead of the
// (const char*, const allocator& = ...) converting ctor — picking by shape
// alone emitted a 2-arg call against a 4-arg prototype).
FuncDef *class_text_ctor(DataDefCLASS *cdd)
{
	if (!cdd) return NULL;
	for (Variable *cv : cdd->ctors) {
		FuncDef *fd = cv ? dynamic_cast<FuncDef *>(cv->type) : NULL;
		if (!fd || fd->parameters.size() < 2) continue;
		DataDef *p1 = fd->parameters[1];
		if (p1 && p1->is_cstr()
		    && fd->required_param_count() <= 2)
			return fd;
	}
	return NULL;
}

} // namespace

node_t CirBuilder::synth_call_shim(Program *prog, TokenFunc *tf)
{
	return tf ? synth_call_shim_var(prog, &tf->var) : NULL;
}

// The shim core, keyed on the function VARIABLE so it serves both parsed
// functions (via the TokenFunc wrapper above) and host-callback trampolines
// (addFunction-declared, defined by synth_host_trampoline — no TokenFunc).
node_t CirBuilder::synth_call_shim_var(Program *prog, Variable *fvar)
{
	// Classify one parameter position; false = not marshallable.
	auto classify_param = [](DataDef *pt, bool is_ref, ShimSlot &out) -> bool {
		if (!pt || is_ref) return false;
		if (DataDefCLASS *cdd = as_class_instance(pt)) {
			out.cdd = cdd;
			if (FuncDef *tc = class_text_ctor(cdd)) {
				out.kind = ShimSlot::K_CLASS_TEXT;
				out.text_ctor = tc;
			} else {
				out.kind = ShimSlot::K_CLASS_INST;
			}
			return true;
		}
		if (pt->is_cstr()) { out.kind = ShimSlot::K_CSTR; return true; }
		// A function pointer is not host-marshallable through the madc_value
		// integer ABI (a host cannot hand a callable across the value boundary).
		// DataDefFPTR reports dtINT64/is_integer() and does NOT answer
		// is_pointer(), so without this it slipped past the pointer bail into
		// K_INT and the shim passed a raw `long` to the fn-ptr parameter
		// ("using integer without cast for pointer type parameter"). Treat it
		// like any other pointer param: unmarshallable -> no shim.
		if (pt->is_pointer() || pt->is_simd() || dynamic_cast<DataDefFPTR *>(pt))
			return false;
		if (pt->type() == DataType::dtBOOL)   { out.kind = ShimSlot::K_BOOL; return true; }
		if (pt->is_integer())                 { out.kind = ShimSlot::K_INT;  return true; }
		if (pt->is_real())                    { out.kind = ShimSlot::K_REAL; return true; }
		return false;
	};

	FuncDef *fd = fvar ? dynamic_cast<FuncDef *>(fvar->type) : NULL;
	if (!prog || !fvar || !fd) return NULL;
	if (fvar->name.empty() || fvar->name == "main") return NULL;
	if (fd->is_multi_return() || fd->is_varargs) return NULL;
	// Function-LOCAL entities (GNU nested fns, lambdas) hoist under a
	// local_emit_name and may take hidden capture parameters — they are
	// not host-callable by name.
	if (!fd->local_emit_name.empty() || !fd->captured_vars.empty()
	    || fd->has_captures)
		return NULL;
	// Host-callable = globally name-addressable (perform_call resolves via
	// findVariable); methods/lambdas/locals are not.
	{
		std::string lookup = fvar->name;
		Variable *gv = prog->tkProgram ? prog->tkProgram->findVariable(prog->strpool, lookup) : NULL;
		if (gv != fvar) return NULL;
	}

	// Classify the signature; bail (no shim) on anything unmarshallable.
	std::vector<ShimSlot> params;
	for (size_t i = 0; i < fd->parameters.size(); i++) {
		ShimSlot slot;
		bool is_ref = fd->is_ref_param(i);
		if (!classify_param(fd->parameters[i], is_ref, slot)) return NULL;
		params.push_back(slot);
	}
	DataDef *rt = &fd->return_value_type();
	DataDefCLASS *ret_cdd = class_return_via_retbuf(rt);
	enum { R_VOID, R_BOOL, R_INT, R_REAL, R_CSTR, R_TEXTOBJ, R_INST } rkind;
	FuncDef *ret_cstr_fd = NULL, *ret_len_fd = NULL;
	if (ret_cdd) {
		// A returned object with the c_str()/size() (or length()) text
		// protocol converts to a TEXT value; any other class returns as a
		// typed INSTANCE cell finalized by its own destructor. Generic:
		// protocol/identity questions only, never a concrete class name.
		ret_cstr_fd = class_method_def(ret_cdd, "c_str");
		ret_len_fd = class_method_def(ret_cdd, "size");
		if (!ret_len_fd) ret_len_fd = class_method_def(ret_cdd, "length");
		rkind = (ret_cstr_fd && ret_len_fd) ? R_TEXTOBJ : R_INST;
	} else if (!rt || (rt->type() == DataType::dtVOID && !rt->is_pointer())) {
		// Structural: a void* is a DataDefPTR whose type() is now dtVOID (the
		// derivation moved off the tag), so guard with !is_pointer() — a void*
		// return is not R_VOID; it falls to the pointer bail below (no shim).
		rkind = R_VOID;
	} else if (rt->is_cstr()) {
		rkind = R_CSTR;
	} else if (as_class_instance(rt) || rt->is_pointer() || rt->is_simd()
		   || dynamic_cast<DataDefFPTR *>(rt)) {
		// trivial-class / pointer / SIMD returns: not marshalled yet. A
		// function-pointer return is the same case as a fn-ptr parameter
		// (classify_param): DataDefFPTR reports dtINT64/is_integer() and not
		// is_pointer(), so without this bail a fn-ptr-returning function (e.g.
		// `DO_FUN *pick_void(int)`) fell to R_INT and the shim passed the
		// returned pointer to madc_value_set_integer ("using pointer without
		// cast for integer type parameter"). A callable cannot cross the value
		// ABI -> no shim.
		return NULL;
	} else if (rt->type() == DataType::dtBOOL) {
		rkind = R_BOOL;
	} else if (rt->is_integer()) {
		rkind = R_INT;
	} else if (rt->is_real()) {
		rkind = R_REAL;
	} else {
		return NULL;
	}

	std::string target_sym = call_emit_symbol(fd, fvar->name);
	std::string shim_name = "__madc_shim_" + target_sym;
	referenced_funcs.insert(target_sym);

	// Value-helper externs (compiler machinery; resolved from the host).
	std::vector<ExternParam> p1{ { {N_CHAR}, true } };
	need_output_extern("madc_value_get_type_id", false, p1, {N_UNSIGNED});
	std::vector<node_t> stmts;

	// `__args + i*sizeof(madc_value)` — the i-th packed value slot. The 32
	// comes from the REAL struct (ABI-pinned), never a hand-written byte count.
	auto arg_slot = [&](size_t i) -> node_t {
		if (i == 0) return id("__args");
		return node2(N_ADD, id("__args"),
			     integer((long)(i * sizeof(madc_value))));
	};
	auto helper_call = [&](const char *sym, std::initializer_list<node_t> args) {
		node_t a = list();
		for (node_t n : args) append(a, n);
		return node2(N_CALL, id(sym), a);
	};

	// 1. Argument type validation — `if (get_type_id(slot) != T [&& != T2]) return i+1;`
	for (size_t i = 0; i < params.size(); i++) {
		std::vector<uint32_t> ok_tids;
		switch (params[i].kind) {
		case ShimSlot::K_BOOL: ok_tids = {MADC_TYPEID_BOOL, MADC_TYPEID_INT64}; break;
		case ShimSlot::K_INT:  ok_tids = {MADC_TYPEID_INT64, MADC_TYPEID_BOOL}; break;
		case ShimSlot::K_REAL: ok_tids = {MADC_TYPEID_DOUBLE, MADC_TYPEID_INT64}; break;
		case ShimSlot::K_CSTR:
		case ShimSlot::K_CLASS_TEXT: ok_tids = {MADC_TYPEID_TEXT}; break;
		case ShimSlot::K_CLASS_INST:
			ok_tids = {prog->type_id_for(params[i].cdd)};
			break;
		}
		node_t cond = NULL;
		for (uint32_t t : ok_tids) {
			node_t ne = node2(N_NE,
					  helper_call("madc_value_get_type_id", {arg_slot(i)}),
					  integer((long)t));
			cond = cond ? node2(N_ANDAND, cond, ne) : ne;
		}
		stmts.push_back(node4(N_IF, list(), cond,
				      node2(N_RETURN, list(), integer((long)i + 1)),
				      ignore()));
	}

	// 2. Class-text parameter temporaries: `struct X __madc_shim_pN;` with the
	// scope-cleanup destructor, constructed from the slot's text via the
	// class's OWN converting constructor.
	std::vector<std::string> param_tmp(params.size());
	std::vector<ExternParam> text_params{ { {N_CHAR}, true }, { {N_VOID}, true } };
	for (size_t i = 0; i < params.size(); i++) {
		if (params[i].kind != ShimSlot::K_CLASS_TEXT) continue;
		char tname[40];
		snprintf(tname, sizeof(tname), "__madc_shim_p%u", (unsigned)i);
		param_tmp[i] = tname;
		Variable *tv = new Variable(tname, *params[i].cdd, 1, NULL, false);
		tv->flags |= vfLOCAL;
		node_t vd = var_decl(tv, NULL);
		if (!vd) return NULL;
		stmts.push_back(vd);
		need_output_extern("madc_value_text", true, text_params);
		std::vector<node_t> ctor_arg_nodes;
		ctor_arg_nodes.push_back(
			helper_call("madc_value_text", {arg_slot(i), integer(0)}));
		node_t cc = ctor_call_assemble(node1(N_ADDR, id(tname)),
					       params[i].cdd, params[i].text_ctor,
					       ctor_arg_nodes, NULL);
		if (!cc) return NULL;
		// Default-argument completion can materialize temporaries
		// (e.g. the allocator object) via m_pending_stmts — drain them
		// ahead of the ctor call, as block translation would.
		for (node_t pstmt : m_pending_stmts)
			stmts.push_back(pstmt);
		m_pending_stmts.clear();
		stmts.push_back(cc);
	}

	// 3. Return landing: an instance cell or a protocol temp, before the call.
	std::string ret_tmp;
	if (rkind == R_INST) {
		// `char *__cell = 0; __cell = make_instance(__out, TID, SIZE, &dtor);`
		// The callee then constructs the result DIRECTLY into the cell;
		// the cell's finalizer is the class's own complete-object dtor.
		std::string dtor_sym = class_complete_dtor_symbol(ret_cdd);
		std::vector<ExternParam> dparams{ { {N_VOID}, true } };
		need_output_extern(dtor_sym.c_str(), false, dparams);
		referenced_funcs.insert(dtor_sym);
		std::vector<ExternParam> mi_params{
			{ {N_CHAR}, true }, { {N_UNSIGNED}, false },
			{ {N_LONG}, false }, { {N_VOID}, true } };
		need_output_extern("madc_value_make_instance", true, mi_params);
		node_t cdecl = simple(N_SPEC_DECL);
		append(cdecl, node1(N_SHARE, node1(N_LIST, simple(N_CHAR))));
		append(cdecl, node2(N_DECL, id("__cell"), node1(N_LIST, pointer())));
		append(cdecl, ignore());
		append(cdecl, ignore());
		append(cdecl, ignore());
		stmts.push_back(cdecl);
		node_t mi = helper_call("madc_value_make_instance",
					{id("__out"),
					 integer((long)prog->type_id_for(ret_cdd)),
					 integer((long)ret_cdd->size),
					 id(dtor_sym.c_str())});
		stmts.push_back(node2(N_EXPR, list(), node2(N_ASSIGN, id("__cell"), mi)));
		stmts.push_back(node4(N_IF, list(),
				      node2(N_EQ, id("__cell"), integer(0)),
				      node2(N_RETURN, list(), integer(100)),
				      ignore()));
	} else if (rkind == R_TEXTOBJ) {
		ret_tmp = "__madc_shim_ret";
		Variable *rv = new Variable(ret_tmp, *ret_cdd, 1, NULL, false);
		rv->flags |= vfLOCAL;
		node_t vd = var_decl(rv, NULL);
		if (!vd) return NULL;
		stmts.push_back(vd);
	}

	// 4. The call to F through its normal lowered ABI.
	node_t cargs = list();
	if (rkind == R_INST)
		append(cargs, node2(N_CAST, class_ptr_type(ret_cdd), id("__cell")));
	else if (rkind == R_TEXTOBJ)
		append(cargs, node1(N_ADDR, id(ret_tmp.c_str())));
	for (size_t i = 0; i < params.size(); i++) {
		switch (params[i].kind) {
		case ShimSlot::K_BOOL:
			need_output_extern("madc_value_get_bool", false, p1, {N_INT});
			append(cargs, helper_call("madc_value_get_bool", {arg_slot(i)}));
			break;
		case ShimSlot::K_INT:
			need_output_extern("madc_value_get_integer", false, p1, {N_LONG});
			append(cargs, helper_call("madc_value_get_integer", {arg_slot(i)}));
			break;
		case ShimSlot::K_REAL:
			need_output_extern("madc_value_get_real", false, p1, {N_DOUBLE});
			append(cargs, helper_call("madc_value_get_real", {arg_slot(i)}));
			break;
		case ShimSlot::K_CSTR:
			need_output_extern("madc_value_text", true, text_params);
			append(cargs, helper_call("madc_value_text", {arg_slot(i), integer(0)}));
			break;
		case ShimSlot::K_CLASS_TEXT:
			append(cargs, id(param_tmp[i].c_str()));
			break;
		case ShimSlot::K_CLASS_INST: {
			std::vector<ExternParam> dp{ { {N_CHAR}, true }, { {N_VOID}, true } };
			need_output_extern("madc_value_data", true, dp);
			append(cargs, node1(N_DEREF,
					    node2(N_CAST, class_ptr_type(params[i].cdd),
						  helper_call("madc_value_data",
							      {arg_slot(i), integer(0)}))));
			break;
		}
		}
	}
	node_t call = node2(N_CALL, id(target_sym.c_str()), cargs);

	// 5. Result conversion (set helpers; scalar calls nest in the setter).
	std::vector<ExternParam> set_scalar{ { {N_CHAR}, true }, { {N_LONG}, false } };
	std::vector<ExternParam> set_real_p{ { {N_CHAR}, true }, { {N_DOUBLE}, false } };
	std::vector<ExternParam> set_str{ { {N_CHAR}, true }, { {N_CHAR}, true } };
	std::vector<ExternParam> set_str_n{ { {N_CHAR}, true }, { {N_CHAR}, true },
					    { {N_LONG}, false } };
	switch (rkind) {
	case R_VOID:
	case R_INST:
		stmts.push_back(node2(N_EXPR, list(), call));
		break;
	case R_TEXTOBJ: {
		stmts.push_back(node2(N_EXPR, list(), call));
		std::string cs = class_method_symbol(ret_cdd, "c_str");
		std::string ls = ret_len_fd == class_method_def(ret_cdd, "size")
				 ? class_method_symbol(ret_cdd, "size")
				 : class_method_symbol(ret_cdd, "length");
		std::vector<ExternParam> mp{ { {N_VOID}, true } };
		need_output_extern(cs.c_str(), true, mp);
		need_output_extern(ls.c_str(), false, mp, {N_LONG});
		referenced_funcs.insert(cs);
		referenced_funcs.insert(ls);
		need_output_extern("madc_value_set_string_n", false, set_str_n, {N_INT});
		node_t addr1 = node1(N_ADDR, id(ret_tmp.c_str()));
		node_t addr2 = node1(N_ADDR, id(ret_tmp.c_str()));
		stmts.push_back(node2(N_EXPR, list(),
			helper_call("madc_value_set_string_n",
				    {id("__out"),
				     helper_call(cs.c_str(), {addr1}),
				     helper_call(ls.c_str(), {addr2})})));
		break;
	}
	case R_BOOL:
		need_output_extern("madc_value_set_bool", false, set_scalar, {N_INT});
		stmts.push_back(node2(N_EXPR, list(),
			helper_call("madc_value_set_bool", {id("__out"), call})));
		break;
	case R_INT:
		need_output_extern("madc_value_set_integer", false, set_scalar, {N_INT});
		stmts.push_back(node2(N_EXPR, list(),
			helper_call("madc_value_set_integer", {id("__out"), call})));
		break;
	case R_REAL:
		need_output_extern("madc_value_set_real", false, set_real_p, {N_INT});
		stmts.push_back(node2(N_EXPR, list(),
			helper_call("madc_value_set_real", {id("__out"), call})));
		break;
	case R_CSTR:
		need_output_extern("madc_value_set_string", false, set_str, {N_INT});
		stmts.push_back(node2(N_EXPR, list(),
			helper_call("madc_value_set_string", {id("__out"), call})));
		break;
	}
	stmts.push_back(node2(N_RETURN, list(), integer(0)));

	// Assemble: long __madc_shim_<sym>(char *__args, char *__out) { ... }
	auto char_ptr_param = [&](const char *pname) {
		node_t pp = simple(N_SPEC_DECL);
		append(pp, node1(N_LIST, simple(N_CHAR)));
		append(pp, node2(N_DECL, id(pname), node1(N_LIST, pointer())));
		append(pp, ignore());
		append(pp, ignore());
		append(pp, ignore());
		return pp;
	};
	node_t param_list = list();
	append(param_list, char_ptr_param("__args"));
	append(param_list, char_ptr_param("__out"));
	node_t decl = node2(N_DECL, id(shim_name.c_str()),
			    node1(N_LIST, node1(N_FUNC, param_list)));
	node_t items = list();
	for (node_t st : stmts) append(items, st);
	node_t body = node2(N_BLOCK, list(), items);
	node_t out = node4(N_FUNC_DEF, node1(N_LIST, simple(N_LONG)), decl,
			   list(), body);
	CIR_NODE(out)->synth_from_origin = true;
	return out;
}

void CirBuilder::synth_call_shims(Program *prog,
				  const std::vector<TokenFunc *> &roots,
				  std::vector<node_t> &func_def_nodes)
{
	for (TokenFunc *tf : roots) {
		node_t shim = synth_call_shim(prog, tf);
		if (shim) func_def_nodes.push_back(shim);
	}
	// Host-callback trampolines (synth_host_trampoline) are name-addressable
	// module functions too: give each one a shim so program::call works on
	// registered names through the same ONE call surface.
	for (const Program::HostCallbackReg &reg : prog->host_callback_regs) {
		std::string lookup = reg.name;
		Variable *fv = prog->tkProgram
			       ? prog->tkProgram->findVariable(prog->strpool, lookup) : NULL;
		if (!fv) continue;
		node_t shim = synth_call_shim_var(prog, fv);
		if (shim) func_def_nodes.push_back(shim);
	}
}

// -----------------------------------------------------------------------
// Host-callback trampolines — register_function's module half.
// -----------------------------------------------------------------------
// The reverse of synth_call_shim: the embedding host registered a native
// function; scripts call it by name through an ordinary prototype
// (Program::add_host_callbacks). Here the module gains the DEFINITION
//   RET name(p0..pn) { return __madc_host_cb_<k>([bound,] p0..pn); }
// — a typed pass-through call the compiler emits with the registration's
// real low types, so there is NO runtime type dispatch anywhere (the
// lesson of the deleted value_as/call_targetN pyramid). The import symbol
// is bound to the host entry (the deduced form's typed adapter, or the
// explicit form's callback itself) by the JIT session's import resolver
// at MIR link; `bound` carries the deduced form's user callback as the
// adapter's hidden first argument.
node_t CirBuilder::synth_host_trampoline(Program *prog,
					 const Program::HostCallbackReg &reg)
{
	typedef Program::HostCallbackReg HCR;
	if (!prog || reg.name.empty() || reg.import_sym.empty() || !reg.entry)
		return NULL;

	// The kind's DataDef — same vocabulary add_host_callbacks declared the
	// prototype with, so the definition matches the Pass-0.75 extern proto.
	auto kind_dd = [&](int k) -> DataDef * {
		switch (k) {
		case HCR::K_BOOL: return &ddBOOL;
		case HCR::K_INT:  return &ddINT64;
		case HCR::K_REAL: return &ddDOUBLE;
		case HCR::K_CSTR: return prog->getPointerType(&ddCHAR);
		default:          return &ddVOID;
		}
	};

	// Extern proto for the import the session binds to the host entry.
	std::vector<ExternParam> eps;
	if (reg.bound)
		eps.push_back({ {N_LONG}, false });
	for (int k : reg.params) {
		switch (k) {
		case HCR::K_BOOL: eps.push_back({ {N_CHAR}, false });   break;
		case HCR::K_INT:  eps.push_back({ {N_LONG}, false });   break;
		case HCR::K_REAL: eps.push_back({ {N_DOUBLE}, false }); break;
		case HCR::K_CSTR: eps.push_back({ {N_CHAR}, true });    break;
		default: return NULL;	// void parameter: not registrable
		}
	}
	bool ret_cstr = reg.returns == HCR::K_CSTR;
	std::vector<c2mir_node_code_t> ret_specs;
	switch (reg.returns) {
	case HCR::K_VOID: case HCR::K_CSTR: break;
	case HCR::K_BOOL: ret_specs = {N_CHAR};   break;
	case HCR::K_INT:  ret_specs = {N_LONG};   break;
	case HCR::K_REAL: ret_specs = {N_DOUBLE}; break;
	default: return NULL;
	}
	need_output_extern(reg.import_sym.c_str(), ret_cstr, eps, ret_specs);
	referenced_funcs.insert(reg.import_sym);

	// Body: [return] __madc_host_cb_<k>([bound,] __p0..__pn);
	node_t cargs = list();
	if (reg.bound)
		append(cargs, integer((long)reg.bound));
	char pn[16];
	for (size_t i = 0; i < reg.params.size(); i++) {
		snprintf(pn, sizeof(pn), "__p%u", (unsigned)i);
		append(cargs, id(pn));
	}
	node_t call = node2(N_CALL, id(reg.import_sym.c_str()), cargs);
	node_t items = list();
	if (reg.returns == HCR::K_VOID)
		append(items, node2(N_EXPR, list(), call));
	else
		append(items, node2(N_RETURN, list(), call));
	node_t body = node2(N_BLOCK, list(), items);

	// Definition signature — param_decl/type_list keep it byte-compatible
	// with the prototype func_proto emits for the same FuncDef types.
	node_t param_list = list();
	if (reg.params.empty()) {
		node_t void_spec = node1(N_LIST, simple(N_VOID));
		node_t void_decl = node2(N_DECL, ignore(), list());
		append(param_list, node2(N_TYPE, void_spec, void_decl));
	}
	for (size_t i = 0; i < reg.params.size(); i++) {
		snprintf(pn, sizeof(pn), "__p%u", (unsigned)i);
		node_t pd = param_decl(kind_dd(reg.params[i]), pn);
		if (!pd) return NULL;
		append(param_list, pd);
	}
	node_t decl_list = list();
	append(decl_list, node1(N_FUNC, param_list));
	node_t ret_type;
	if (ret_cstr) {
		ret_type = type_list(&ddCHAR);
		append(decl_list, pointer());
	} else {
		ret_type = type_list(kind_dd(reg.returns));
	}
	node_t decl = node2(N_DECL, id(reg.name.c_str()), decl_list);
	node_t out = node4(N_FUNC_DEF, ret_type, decl, list(), body);
	CIR_NODE(out)->synth_from_origin = true;
	return out;
}

void CirBuilder::synth_host_trampolines(Program *prog,
					std::vector<node_t> &func_def_nodes)
{
	if (!prog) return;
	for (const Program::HostCallbackReg &reg : prog->host_callback_regs) {
		node_t t = synth_host_trampoline(prog, reg);
		if (t) func_def_nodes.push_back(t);
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

	// Collect all TokenFunc entries
	std::vector<TokenFunc *> funcs;
	for (auto it = prog->pending_funcs.begin();
	     it != prog->pending_funcs.end(); ++it) {
		TokenFunc *tf = dynamic_cast<TokenFunc *>(*it);
		if (tf && !tf->is_overridden)
			funcs.push_back(tf);
	}

	// (struct_behind is now a static member — resolves the struct a typedef
	// ultimately names, peeling array/pointer layers.)

	// Pre-scan: structs that have a bare standalone definition entry
	// (`struct X {...};`). A typedef that references such a struct is a
	// forward reference and must emit STRUCT(tag, IGNORE) — the full body is
	// emitted once at the bare dkStruct, matching c2m's source structure.
	// The same pass detects COLLIDING typedef aliases: one bare alias mapped
	// to >1 distinct underlying struct tag (e.g. std::string vs std::pmr::string
	// both registering `string`). Those emit their unique tag (typedef_emit_name)
	// instead of the bare alias so flat C has no `typedef X string;` conflict.
	std::set<std::string> struct_def_points;
	std::map<std::string, std::set<std::string> > alias_tags;
	m_combined_typedef_alias.clear();
	m_hoisted_combined_aliases.clear();
	for (auto &td : prog->top_decls) {
		if (td.kind == Program::DeclKind::dkTypedef) {
			DataDefSTRUCT *asdd = struct_behind(td.dd);
			if (asdd && !td.name.empty())
				alias_tags[td.name].insert(asdd->name);
		}
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
			if (sdd && sdd->is_complete) {
				struct_def_points.insert(sdd->name);
				// Remember the combined typedef so a hoist of this struct emits
				// the body AND the alias together (first def-point wins).
				m_combined_typedef_alias.emplace(sdd->name, &td);
			}
		}
	}

	// An alias backing >1 distinct struct tag collides at flat-C module scope.
	m_ambiguous_typedef_aliases.clear();
	for (auto &kv : alias_tags)
		if (kv.second.size() > 1)
			m_ambiguous_typedef_aliases.insert(kv.first);

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
			cn->origin_id = madc_slot_id_for(td.origin);
			set_pos(cn, td.origin->file, td.origin->line, td.origin->column);
		} else if (td.file) {
			// No origin token captured (rare typedef variants): feed c2mir's
			// store the recorded file/line; node carries no +madc origin.
			set_pos(cn, td.file, td.line, 0);
		}
	};
	std::set<std::string> emitted_structs;
	std::set<std::string> emitted_globals;
	std::set<DataDefCLASS *> emitted_classes;
	std::set<DataDefCLASS *> emitting_classes;

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
			// Already emitted EARLY by emit_struct_with_deps as part of a combined
			// `typedef struct X {...} Y;` it hoisted (a by-value member needed X
			// complete before this source position). Skip the duplicate.
			if (m_hoisted_combined_aliases.count(td.name))
				break;
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
			if (sdd && sdd->is_complete)
				emit_class_member_deps(sdd, top_list, emitted_structs,
						       emitted_classes, emitting_classes);
			node_t n = typedef_decl(typedef_emit_name(td.name, td.dd),
						td.dd, emitted_structs, forward);
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
			// A generic dependent local struct (placeholder members) is a Tree-1
			// pattern artifact — skip global emission; the concrete per-
			// instantiation clone is emitted instead.
			if (sdd && sdd->is_complete && !emitted_structs.count(sdd->name)
			    && !struct_has_dependent_member(sdd)) {
				emit_class_member_deps(sdd, top_list, emitted_structs,
						       emitted_classes, emitting_classes);
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
			if (td.var && m_prog) {
				Variable *target =
					m_prog->resolve_global_storage_variable(td.var);
				if (target && target != td.var
				    && !(target->type && target->type->is_function()))
					break;
			}
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
	// to `struct ClassName { ... }`. By-value class members require the
	// member class's struct to be complete first, so emission walks those
	// dependencies before appending the owner.
	for (auto &kv : prog->struct_map) {
#ifdef MADC_DEBUG_TUPLE
		if (getenv("MADC_DEBUG_TUPLE")
		 && (kv.first.find("tuple") != std::string::npos
		  || kv.first.find("Tuple_impl") != std::string::npos
		  || kv.first.find("Head_base") != std::string::npos)) {
			DataDefCLASS *c2 = dynamic_cast<DataDefCLASS *>(kv.second);
			fprintf(stderr, "[EMIT] key='%s' as_user=%d base=%d raw=%d ref=%d members=%zu size=%ld dep_ph=%d\n",
				kv.first.c_str(), (int)(as_user_class(kv.second) != NULL),
				c2 ? (int)c2->basetype() : -1, c2 ? (int)c2->rawtype() : -1,
				c2 ? (int)c2->is_reference() : -1, c2 ? c2->members.size() : (size_t)0,
				c2 ? (long)c2->size : -1L, c2 ? (int)c2->is_dependent_placeholder : -1);
		}
#endif
		DataDefCLASS *cdd = as_user_class(kv.second);
		if (!cdd) continue;
		emit_class_struct_with_deps(cdd, top_list, emitted_structs,
					    emitted_classes, emitting_classes);
	}
	// Anchor: the last EARLY struct definition. Late-instantiated struct defs
	// (Pass 1.97) are spliced in right AFTER this point — i.e. after all early
	// structs (so their by-value deps on early structs stay defined-before-use)
	// but BEFORE the function prototypes (Pass 0.75/1/1.95) that take those late
	// structs by value (else c2mir sees an incomplete by-value param in a proto
	// emitted before the late definition — map<int,int>'s `_Index_tuple<0>`).
	node_t late_struct_anchor = c2mir_op_tail(c2m, top_list);

	// Collect user function names. Stored as a member too, so the body
	// translation (below) can tell a madc-compiled function (whose by-value
	// non-trivial object return madc lowers via the __retbuf ABI) from an
	// external / native function with its own ABI.
	std::set<std::string> user_func_names;
	for (TokenFunc *tf : funcs)
		user_func_names.insert(tf->var.name);
	m_user_func_names = &user_func_names;
	m_materialized_lib_syms.clear();   // per-module, like m_user_func_names

	// Bind the ctors + dtor of EXTERNALLY-DEFINED (libstdc++-owned) classes to
	// their real Itanium C1 / D1 symbols, BEFORE any construction is lowered (the
	// global-ctor pass + the function bodies below). libstdc++'s explicit
	// instantiation provides these out-of-line — exactly like the vtable/typeinfo
	// (Pass 1.5) and the gated dtor synthesis (Pass 1.6). madc must NOT synthesize a
	// member-wise ctor for such a class: it cannot reproduce the exact
	// base/vbase-construction + vptr-install + layout ABI (the synthesized
	// basic_ofstream ctor constructed basic_ios — a virtual base — with the wrong
	// overload -> c2mir "too few arguments"; g++ emits a single C1 call and lets
	// libstdc++ build the whole hierarchy, vbases and vptrs included). Setting
	// emit_symbol routes construction (ctor_call_symbol) and destruction
	// (class_dtor_symbol) to the real symbol AND suppresses the synthesized body +
	// vbase chaining (external = !emit_symbol.empty()). Data-driven
	// (is_externally_defined), never a namespace==std test (Rule #7).
	{
		std::set<DataDefCLASS *> bound_ext;
		for (auto &kv : prog->struct_map) {
			DataDefCLASS *cdd = as_user_class(kv.second);
			// Bind to libstdc++'s available exported symbols for (a) polymorphic
			// library classes (is_externally_defined, vtable-owned), and (b)
			// NON-polymorphic instantiations named by an `extern template`
			// decl (basic_string<char>) — libstdc++ exports those members
			// out-of-line too, but not every extern-template class exports every
			// inline member (allocator<char>::deallocate is header-only). Probe
			// the live symbol table before suppressing a header body.
			if (!cdd || !(cdd->is_externally_defined()
				   || cdd->is_extern_template_instantiated)) continue;
			if (cdd->canonical_cpp_spelling.empty()) continue;
			if (bound_ext.count(cdd)) continue;
			bound_ext.insert(cdd);
			const std::string &cls = cdd->canonical_cpp_spelling;
			// CTOR binding only for TEMPLATE-INSTANTIATION classes (canonical
			// spelling contains '<'): libstdc++ EXPLICITLY INSTANTIATES exactly
			// the polymorphic template families (basic_ios/basic_ofstream/...,
			// the locale facets), so their C1 ctors are exported out-of-line. A
			// concrete polymorphic class like std::bad_alloc has an inline/defaulted
			// ctor with NO exported _ZNSt9bad_allocC1Ev — binding it would dangle at
			// link. (A wrong bind fails loudly at MIR-link, never silently.) Those
			// concrete classes keep madc's vptr-init construction, which already
			// works. The dtor is virtual (in the vtable) so it's exported for both.
			bool ctor_exported = cls.find('<') != std::string::npos;
			for (Variable *cv : ctor_exported ? cdd->ctors
							  : std::vector<Variable *>()) {
				FuncDef *fd = cv ? dynamic_cast<FuncDef *>(cv->type) : NULL;
				if (!fd || !fd->emit_symbol.empty() || fd->is_member_template) continue;
				// Param spellings EXCLUDING the hidden __this (slot 0), same as
				// bind_declared_cpp_symbol — so PKc / St13_Ios_Openmode match the ABI.
				std::vector<std::string> psp;
				for (size_t i = 1; i < fd->param_cpp_spellings.size(); ++i)
					psp.push_back(fd->param_cpp_spellings[i]);
				std::string sym = itanium_mangle_ctor_sub(cls, psp);
				if (external_symbol_available(sym))
					fd->emit_symbol = sym;
			}
			if (Variable *dv = class_own_dtor(cdd)) {
				FuncDef *dt = dynamic_cast<FuncDef *>(dv->type);
				if (dt && dt->emit_symbol.empty()) {
					std::string sym = itanium_mangle_dtor_sub(cls);
					if (external_symbol_available(sym))
						dt->emit_symbol = sym;
				}
			}
			// Non-template methods/operators of an EXPLICITLY-INSTANTIATED class
			// (cls has '<') are candidates for out-of-line libstdc++ exports. The
			// explicit instantiation `extern template class basic_ostream<char>;`
			// emits many non-template members, INCLUDING ones written `inline` in the
			// header (e.g. operator<<(unsigned long) at <ostream>:172
			// `{return _M_insert(__n);}`), but other extern-template classes still
			// leave inline bodies only. Bind only symbols that dlsym can resolve.
			// bind_declared_cpp_symbol only catches DECLARED-ONLY members
			// (operator<<(int)); the inline ones reach here unbound, and emitting
			// their bodies forwards into _M_insert<T> — a member TEMPLATE that the
			// explicit instantiation does NOT export — which fails to lower. Bind them
			// to the real member symbol so the call goes external and no body is
			// emitted, exactly like the declared-only members. (member TEMPLATEs are
			// skipped: not exported by the class instantiation.)
			for (Variable *mv : ctor_exported ? cdd->methods
							  : std::vector<Variable *>()) {
				FuncDef *fd = mv ? dynamic_cast<FuncDef *>(mv->type) : NULL;
				if (!fd || !fd->emit_symbol.empty() || fd->is_member_template)
					continue;
				if (fd->defaulted_or_deleted || fd->pure_virtual)
					continue;
				const std::string &dn = fd->method_display_name;
				if (dn.empty() || dn[0] == '~')   // dtor handled above
					continue;
				std::vector<std::string> psp;
				for (size_t i = 1; i < fd->param_cpp_spellings.size(); ++i)
					psp.push_back(fd->param_cpp_spellings[i]);
				std::string sym;
				if (dn.compare(0, 8, "operator") == 0)
					sym = itanium_mangle_operator_sub(
						cls, dn.substr(8), psp, fd->is_const_method);
				else
					sym = itanium_mangle_member_sub(
						cls, dn, psp, fd->is_const_method);
				if (external_symbol_available(sym))
					fd->emit_symbol = sym;
			}
		}
	}

	// File-scope class-instance globals: emit storage for any not already
	// covered by the dkGlobalVar pass (built-ins like
	// `version`) and queue their ctor calls for main's prologue. Done before the
	// body loop so func_def(main) sees m_global_ctor_stmts; the storage rides in
	// deferred_globals (emitted after the prototypes, Pass 1a). referenced_funcs
	// it populates (the ctor symbol) is captured by the later proto pass.
	collect_global_ctors(prog, deferred_globals, emitted_globals);

	// Translate function bodies first, into a temp list, so referenced_funcs
	// (populated as N_CALL nodes are built) is complete before the prototype
	// pass — letting us emit extern protos for ONLY referenced functions.
	//
	// Reachability DCE for LIBRARY (system-header) function bodies: madc emits
	// a *definition* only for entities it defines and that are actually used.
	// Consuming a real libstdc++ header drags in that header's whole inline-
	// method web (basic_string, basic_ios, ctype/num_put facets, …), almost all
	// of it dead for a given program — and dead bodies are never exercised so
	// their lowering is never correct. g++ never emits an inline function unless
	// it is ODR-used (weak/COMDAT); this mirrors that model. We translate the
	// user's own functions (roots) first, which seeds referenced_funcs with what
	// user code actually calls, then translate a library function ONLY if it is
	// reachable (its emit symbol or name is in referenced_funcs), to a fixpoint
	// (a reached library fn may in turn call more). A function whose origin file
	// is NOT a system header is always a root, so a pure-C / non-real-header
	// build (where every function is user code) is byte-for-byte unchanged — the
	// gate fires only for functions parsed from a system include directory.
	std::vector<node_t> func_def_nodes;
	std::map<std::string, TokenFunc *> lib_funcs;   // emit-symbol -> library fn
	std::vector<TokenFunc *> roots;
	for (TokenFunc *tf : funcs) {
		FuncDef *tfd = dynamic_cast<FuncDef *>(tf->var.type);
		if (tfd && prog->is_system_header_path(tf->file))
			lib_funcs[func_emit_name(tf->var, tfd)] = tf;
		else
			roots.push_back(tf);
	}
	for (TokenFunc *tf : roots) {
		node_t fd = func_def(tf);
		if (fd) func_def_nodes.push_back(fd);
	}
	std::set<std::string> lib_emitted;
	// TokenFuncs parsed by the fixpoint below (parse_deferred_lazy_body). They
	// are NOT in the `funcs` snapshot taken at entry, so Pass 1's forward-proto
	// loop never sees them — they get their prototypes in the late proto pass
	// after the LAST fixpoint run (a body materialized in the Pass-1.9 re-run
	// would otherwise be defined module-tail with no forward declaration, and
	// every call to it from an earlier definition becomes an implicit-int K&R
	// call: pointer returns truncate, struct args mis-wire — the
	// __madc_shim string-ctor segfault).
	std::vector<TokenFunc *> materialized_funcs;
	// Reachability fixpoint: materialize ODR-used deferred member bodies + lower
	// reachable library fns until referenced_funcs stops growing. Lazy
	// member-function-body instantiation ([temp.inst]): a system-header body
	// deferred at parse time (Program::deferred_lazy_bodies) is parsed NOW, the
	// first time its emit symbol is ODR-used; parsing pushes a TokenFunc, we fold
	// it into lib_funcs so it lowers, and its own calls grow referenced_funcs →
	// fixpoint. g++'s "instantiate a definition only on ODR-use". Run as a lambda
	// so it can be re-run AFTER the synth-dtor passes, which add member/base dtor
	// symbols to referenced_funcs (e.g. a deferred allocator<int>::~allocator
	// reached only through a synthesized aggregate dtor).
	// Functions instantiated DURING a lazy re-parse (a materialized dtor body that
	// calls a free-fn template like std::_Destroy) append to prog->pending_funcs
	// but are NOT in the entry `funcs` snapshot — without folding them in they get
	// an extern proto (referenced) but no definition -> MIR "import of undefined
	// item". Drain anything appended past this boundary into lib_funcs each round.
	size_t pf_drained = prog->pending_funcs.size();
	auto materialize_and_lower = [&]() {
		for (bool grew = true; grew; ) {
			grew = false;
			if (!prog->deferred_lazy_bodies.empty()) {
				std::vector<std::string> ready;
				for (auto &db : prog->deferred_lazy_bodies)
					if (!lib_funcs.count(db.first)
					    && referenced_funcs.count(db.first))
						ready.push_back(db.first);
				for (auto &sym : ready) {
					TokenFunc *tf = prog->parse_deferred_lazy_body(sym);
					if (!tf) continue;
					FuncDef *tfd = dynamic_cast<FuncDef *>(tf->var.type);
					lib_funcs[tfd ? func_emit_name(tf->var, tfd) : tf->var.name] = tf;
					materialized_funcs.push_back(tf);
					// The materialized body is madc-emitted: record its
					// symbols so by-value class returns classify as the
					// retbuf ABI at every later call site. (Pass 0.75's
					// extern stays — it is the declaration call sites in
					// earlier-emitted bodies rely on — but now in the
					// retbuf shape for such returns.)
					m_materialized_lib_syms.insert(tf->var.name);
					if (tfd)
						m_materialized_lib_syms.insert(func_emit_name(tf->var, tfd));
					grew = true;
				}
			}
			// Fold functions newly instantiated by the re-parses above (e.g. a
			// std::_Destroy free-fn-template reached only through a lazily-
			// materialized dtor) so the reachability loop below can define them.
			while (pf_drained < prog->pending_funcs.size()) {
				TokenFunc *ntf = dynamic_cast<TokenFunc *>(prog->pending_funcs[pf_drained++]);
				if (!ntf || ntf->is_overridden) continue;
				FuncDef *nfd = dynamic_cast<FuncDef *>(ntf->var.type);
				std::string key = nfd ? func_emit_name(ntf->var, nfd) : ntf->var.name;
				if (lib_funcs.count(key)) continue;
				lib_funcs[key] = ntf;
				// Forward prototype: a free-fn-template instantiation reached
				// ONLY during a late re-parse (e.g. _Node_handle::release →
				// std::move → __ns_std_move__o2) gets a DEFINITION below (Pass 2)
				// but, unlike the deferred_lazy_bodies path (~11978), was never
				// recorded for the Pass-1.95(a) proto loop — so an earlier-emitted
				// body that calls it (release, line 909) precedes the definition
				// (line 938) with no declaration, and c2mir applies K&R implicit-int
				// (`int()`), truncating the pointer return ("conflicting types").
				// Record it here so Pass 1.95(a) emits its forward proto. (The
				// lib_funcs.count(key) guard above makes this push at most once per
				// key; the deferred path already routes its own funcs to
				// materialized_funcs, and its keys are in lib_funcs so they short-
				// circuit before here — no double-push.)
				materialized_funcs.push_back(ntf);
				m_materialized_lib_syms.insert(ntf->var.name);
				if (nfd) m_materialized_lib_syms.insert(key);
				grew = true;
			}
			// A concrete function-template instantiation can be parsed during a
			// speculative Tree-1 pattern build, then removed from pending_funcs
			// when that speculative parse rolls back. The Variable/FuncDef entry
			// still records `tsubst_source`, and calls may ODR-use the concrete
			// symbol later. Fold those already-parsed AST bodies back into the
			// same reachable-library pipeline instead of leaving only an extern.
			for (auto &kv : prog->funcdef_map) {
				const std::string &fname = kv.first;
				FuncDef *nfd = kv.second;
				if (!nfd || !nfd->tsubst_source)
					continue;
				std::string lookup = fname;
				Variable *nvar = prog->tkProgram
					? prog->tkProgram->findVariable(prog->strpool, lookup)
					: NULL;
				if (!nvar)
					nvar = prog->findVariable(lookup);
				std::string sym = nvar ? func_emit_name(*nvar, nfd) : fname;
				if (!referenced_funcs.count(sym)
				    && !referenced_funcs.count(fname))
					continue;
				TokenFunc *ntf = find_ast_function_body(this, prog, sym);
				if (!ntf && sym != fname)
					ntf = find_ast_function_body(this, prog, fname);
				if (!ntf || ntf->is_overridden)
					continue;
				if (!prog->is_system_header_path(ntf->file))
					continue;
				bool already_queued = false;
				for (auto &lf : lib_funcs)
					if (lf.second == ntf) {
						already_queued = true;
						break;
					}
				if (already_queued)
					continue;
				FuncDef *tf_fd = dynamic_cast<FuncDef *>(ntf->var.type);
				std::string key = tf_fd ? func_emit_name(ntf->var, tf_fd)
							: ntf->var.name;
				if (key.empty())
					key = sym;
				if (lib_funcs.count(key))
					continue;
				lib_funcs[key] = ntf;
				materialized_funcs.push_back(ntf);
				m_materialized_lib_syms.insert(ntf->var.name);
				if (tf_fd)
					m_materialized_lib_syms.insert(key);
				grew = true;
			}
			for (auto &kv : lib_funcs) {
				if (lib_emitted.count(kv.first)) continue;
				TokenFunc *tf = kv.second;
				if (!referenced_funcs.count(kv.first)
				    && !referenced_funcs.count(tf->var.name))
					continue;
				lib_emitted.insert(kv.first);
				node_t fd = func_def(tf);
				if (fd) func_def_nodes.push_back(fd);
				grew = true;
			}
		}
	};
	materialize_and_lower();

	// Pass 0.73: host-callback trampolines — one module definition per
	// libmadc register_function registration (see synth_host_trampoline);
	// scripts call the host through these typed pass-throughs.
	synth_host_trampolines(prog, func_def_nodes);

	// Pass 0.74: host-call shims — one __madc_shim_<sym> adapter per
	// host-callable user function (see synth_call_shim). Synthesized here so
	// the value-helper externs flow into Pass 0.8 and the referenced member
	// symbols (c_str/size protocol, dtors) are seen by the Pass-1.9 fixpoint.
	synth_call_shims(prog, roots, func_def_nodes);

	// Symbols that receive a TYPED forward prototype (Pass 0.75 below, Pass 1,
	// and the materialized-func protos). A void* extern from need_output_extern
	// for the SAME symbol — e.g. a mangled-direct `_ZNSaIcEC1ERKS_(void*,void*)`
	// ctor call emitted inside an instantiated body, while allocator<char>'s
	// copy-ctor is also a referenced FuncDef getting a typed
	// `(struct allocator_char*,...)` proto — is a CONFLICTING redeclaration that
	// c2mir/gcc reject ("incompatible argument type" / "conflicting types"). The
	// typed proto is canonical; the m_output_externs flushes skip these symbols.
	std::set<std::string> typed_proto_syms;

	// Pass 0.75: Extern function prototypes — referenced-only (matches c2m,
	// which only declares what #include pulled in).
	for (auto &kv : prog->funcdef_map) {
		const std::string &fname = kv.first;
		FuncDef *fd = kv.second;
		if (!fd || user_func_names.count(fname)) continue;
		// A declaration-only member-template PLACEHOLDER never needs a
		// module-level extern here: when instantiated locally the definition
		// carries its own prototype, and an EXPORTED member template is
		// called through its Itanium-mangled symbol (member_template_method_call
		// + need_output_extern_unprototyped), not the placeholder's name. Its
		// varargs placeholder signature (e.g. `(struct C *, ...)` for an
		// instance member with the hidden __this) otherwise CONFLICTS with the
		// instantiated definition ("incompatible types of ... declarations").
		if (fd->is_member_template) continue;
		std::string lookup_name = fname;
		Variable *fvar = prog->tkProgram ? prog->tkProgram->findVariable(prog->strpool, lookup_name) : NULL;
		std::string symbol = fvar ? func_emit_name(*fvar, fd) : fname;
		std::map<std::string, TokenFunc *>::iterator lfi = lib_funcs.find(symbol);
		if (lfi != lib_funcs.end() && lfi->second) {
			TokenFunc *ltf = lfi->second;
			FuncDef *lfd = dynamic_cast<FuncDef *>(ltf->var.type);
			if (lfd) {
				fd = lfd;
				fvar = &ltf->var;
				symbol = func_emit_name(*fvar, fd);
			}
		}
		if (!referenced_funcs.count(symbol) && !referenced_funcs.count(fname))
			continue;

		DataDef *ret_dd = &fd->return_value_type();
		bool ret_is_ptr = ret_dd && ret_dd->is_pointer();
		bool ret_is_ref = fd->returns_reference();
		int ret_decl_stars = dd_peel_pointers(ret_dd);

		// A madc-emitted by-value non-trivial class return uses the
		// __retbuf ABI — the extern must mirror func_proto/func_def
		// (void return + hidden `struct T *__retbuf` first param), or a
		// lazily-materialized definition conflicts with this declaration
		// ("incompatible types of ... declarations").
		DataDefCLASS *ret_obj = (!ret_is_ptr && !ret_is_ref
					 && !fd->is_multi_return())
					? class_return_via_retbuf(ret_dd) : NULL;

		// Build: EXTERN + type spec
		node_t ext_list = list();
		append(ext_list, simple(N_EXTERN));
		if (ret_obj) {
			append_type_specs(ext_list, &ddVOID);
			ret_decl_stars = 0;
		} else if (!fd->return_typedef_name.empty()) {
			// Route through the alias chokepoint: a cross-namespace-
			// colliding alias (std::string vs std::pmr::string) must
			// emit its unique struct tag here exactly like every other
			// type-spec site, or the extern references an alias the
			// module never defines ("unknown type string").
			append(ext_list, id(typedef_emit_name(fd->return_typedef_name,
							      &fd->return_value_type()).c_str()));
			ret_decl_stars = explicit_star_count(&fd->return_value_type(),
							     fd->return_typedef_name);
		} else if (ret_dd && (ret_dd->is_struct() || as_class_instance(ret_dd))
			   && !ret_dd->is_complex()) {
			DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(unqualified_type(ret_dd));
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
		if (ret_obj)
			append(param_list, retbuf_param((DataDef *)ret_obj, NULL));
		if (fd->parameters.empty() && fd->is_void_params && !ret_obj) {
			node_t void_spec = node1(N_LIST, simple(N_VOID));
			node_t void_decl = node2(N_DECL, ignore(), list());
			append(param_list, node2(N_TYPE, void_spec, void_decl));
		} else {
			size_t nparam = fd->parameters.size();
			if (fd->is_varargs && nparam > 0) nparam--;
			Method *fmethod = fvar ? (Method *)fvar->data : NULL;
			for (size_t i = 0; i < nparam; i++) {
				// 'p' + up to 20 digits (size_t max) + NUL.
				char pname[24];
				snprintf(pname, sizeof(pname), "p%zu", i);
				std::string ptypedef;
				if (i < fd->param_typedef_names.size())
					ptypedef = fd->param_typedef_names[i];
				if (ptypedef.empty()
				    && fmethod && i < fmethod->parameters.size()
				    && fmethod->parameters[i])
					ptypedef = fmethod->parameters[i]->typedef_name;
				append(param_list, param_decl(fd->parameters[i], pname,
							      ptypedef));
			}
		}
		if (fd->is_varargs)
			append(param_list, simple(N_DOTS));

		node_t func_inner = node1(N_FUNC, param_list);
		node_t func_id_node = id(symbol.c_str());
		node_t decl_list = list();
		append(decl_list, func_inner);
		for (int rs = 0; rs < ret_decl_stars; rs++)
			append(decl_list, pointer());
		if (ret_is_ref)
			append(decl_list, pointer());

		node_t decl = node2(N_DECL, func_id_node, decl_list);

		node_t proto = simple(N_SPEC_DECL);
		append(proto, share);
		append(proto, decl);
		append(proto, ignore());
		append(proto, ignore());
		append(proto, ignore());
		append(top_list, proto);
		typed_proto_syms.insert(symbol);
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

	// Pass 0.8: output externs for runtime and external symbols referenced by
	// lowering SO FAR. Bodies translated by the Pass-1.9 fixpoint re-run
	// register more entries after this point; the late declaration pass below
	// Pass 1.9 flushes those (emitted_extern_syms records this batch).
	// Fold in the Pass 1 user-function proto symbols (keyed by tf->var.name —
	// exactly what func_proto declares below) so the extern flush also skips a
	// void* duplicate of a symbol that Pass 1 will type.
	for (TokenFunc *tf : funcs)
		if (tf->var.name != "main"
		 && dynamic_cast<FuncDef *>(tf->var.type))
			typed_proto_syms.insert(tf->var.name);

	std::set<std::string> emitted_extern_syms;
	for (auto &kv : m_output_externs) {
		if (typed_proto_syms.count(kv.first)) continue;
		append(top_list, kv.second);
		emitted_extern_syms.insert(kv.first);
	}

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
		// An externally-defined class (a std:: library polymorphic class madc
		// has no body for) is OWNED by libstdc++: its vtable, typeinfo and
		// implicit ctor/dtor live in the .so. madc must not synthesize a
		// parallel set under wrong (un-namespaced) symbols; consumers reference
		// the real _ZTVSt.../_ZTISt... instead (see class_vtable_symbol /
		// class_typeinfo_symbol + the construction/RTTI sites). Suppressing here
		// also drops the now-unneeded method prototypes (the vtable initializer
		// is what registered them in referenced_funcs). We DO emit extern decls
		// for the library's real vtable/typeinfo so those references resolve.
		if (cdd->is_externally_defined()) {
			node_t ve = data_extern_decl(class_vtable_symbol(cdd));
			if (ve) append(top_list, ve);
			node_t te = data_extern_decl(class_typeinfo_symbol(cdd));
			if (te) append(top_list, te);
			continue;
		}
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
		// class_gets_synth_dtor folds: non-NULL, no user dtor, needs a dtor, and
		// NOT externally-defined. An externally-defined (libstdc++-owned) class's
		// dtor lives in the .so; synthesizing a parallel Cls___dtor here both
		// duplicates it and injects member-dtor cleanup for the library's internal
		// members (e.g. the __cow_string _M_msg of the std exception classes, whose
		// dtor libstdc++ does not export) -> a dangling reference. Skipping it means
		// complete-object destruction references the real dtor symbol instead.
		if (!class_gets_synth_dtor(cdd)) continue;
		if (emitted_synth_dtors.count(cdd)) continue;
		emitted_synth_dtors.insert(cdd);
		node_t dd = synth_dtor_def(cdd);
		if (dd) {
			// The synth dtor body calls its members' dtors. Those calls need a
			// forward prototype BEFORE this definition, or c2mir treats them as
			// implicit-int K&R variadic functions — mis-wiring the `this` arg
			// (ABI mismatch) → null-`this` deref in the member dtor (the
			// std::map/std::set `_Rb_tree` empty-container dtor SIGSEGV@0x8).
			// class_member_destruct queued typed `void d(struct M *)` protos into
			// m_output_externs (added AFTER the Pass-0.8 flush, so not yet
			// emitted); flush the new ones here, ahead of the definition that
			// references them.
			for (auto &kv : m_output_externs) {
				if (emitted_extern_syms.count(kv.first)) continue;
				if (typed_proto_syms.count(kv.first)) continue;
				append(top_list, kv.second);
				emitted_extern_syms.insert(kv.first);
			}
			append(top_list, dd);
		}
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
		if (cdd->is_externally_defined()) continue;   // libstdc++ owns it
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
		if (cdd->is_externally_defined()) continue;   // libstdc++ owns it
		if (emitted_deleting_dtors.count(cdd)) continue;
		emitted_deleting_dtors.insert(cdd);
		node_t dd0 = synth_deleting_dtor_def(cdd);
		if (dd0) append(top_list, dd0);
	}

	// Pass 1.9: re-run the reachability fixpoint. The synth-dtor passes (1.6/1.7/
	// 1.8) and member/base-dtor cleanup insert member/base dtor symbols into
	// referenced_funcs AFTER the first fixpoint ran — including deferred library
	// dtors (e.g. allocator<int>::~allocator reached only through a synthesized
	// aggregate dtor). Materialize + lower any now-ODR-used deferred body so it is
	// DEFINED, not just referenced (else MIR-link "import of undefined item").
	materialize_and_lower();
	m_user_func_names = NULL;   // backing set is a translate_module local; don't dangle

	// Pass 1.95: late declarations — everything the fixpoint runs added after
	// the early declaration passes had already emitted. Both lists land in
	// top_list here, still ahead of every definition (Pass 2), so each call
	// compiles against a real signature instead of a C implicit-int default
	// (which truncates pointer returns and mis-wires struct args — the
	// __madc_shim string-ctor segfault).
	// (a) Forward prototypes for fixpoint-materialized bodies: they are not in
	//     the `funcs` snapshot Pass 1 iterates. Record their symbols so the
	//     extern flush below skips any conflicting void* duplicate (same reason
	//     as proto_func_syms above — a typed proto is canonical).
	for (TokenFunc *tf : materialized_funcs) {
		node_t proto = func_proto(tf);
		if (proto) {
			append(top_list, proto);
			if (dynamic_cast<FuncDef *>(tf->var.type))
				typed_proto_syms.insert(tf->var.name);
		}
	}
	// (b) Externs registered (need_output_extern) during fixpoint body
	//     translation after Pass 0.8 ran. Skip any symbol that ALSO got a typed
	//     forward proto (Pass 0.75 / Pass 1 / materialized) — emitting both is a
	//     conflicting redeclaration c2mir rejects.
	for (auto &kv : m_output_externs)
		if (!emitted_extern_syms.count(kv.first)
		 && !typed_proto_syms.count(kv.first))
			append(top_list, kv.second);

	// Synthesize `void __madc_global_init(void)` — the ONE home for the
	// file-scope class-global ctor calls (collected by collect_global_ctors).
	// main's prologue calls it (translate_func), and main-less embedding
	// sessions invoke it at runtime init via CirJitSession::function_code.
	// A static once-guard makes a second invocation (host init + run_main)
	// a no-op. Emitted FIRST among definitions so main's call (and any
	// earlier-in-source function) sees the definition.
	if (!m_global_ctor_stmts.empty()) {
		// declaration: N_SPEC_DECL(N_SHARE(specs), declarator, attrs,
		// asm, initializer) — c2mir.c:538.
		node_t gspec = list();
		append(gspec, simple(N_STATIC));
		append(gspec, simple(N_INT));
		node_t guard_decl = simple(N_SPEC_DECL);
		append(guard_decl, node1(N_SHARE, gspec));
		append(guard_decl, node2(N_DECL, id("__madc_gi_done"), list()));
		append(guard_decl, ignore());
		append(guard_decl, ignore());
		append(guard_decl, ignore());

		node_t items = list();
		append(items, guard_decl);
		append(items, node4(N_IF, list(), id("__madc_gi_done"),
				    node2(N_RETURN, list(), ignore()), ignore()));
		append(items, node2(N_EXPR, list(),
				    node2(N_ASSIGN, id("__madc_gi_done"), integer(1))));
		for (node_t s : m_global_ctor_stmts)
			append(items, s);
		node_t ibody = node2(N_BLOCK, list(), items);
		node_t iret = node1(N_LIST, simple(N_VOID));
		node_t idecl = node2(N_DECL, id("__madc_global_init"),
				     node1(N_LIST, node1(N_FUNC, list())));
		func_def_nodes.insert(func_def_nodes.begin(),
				      node4(N_FUNC_DEF, iret, idecl, list(), ibody));
	}

	// Pass 1.97: struct definitions for classes instantiated LATE — during the
	// lazy-body reachability fixpoint (Pass 0.7x / 1.9). A deferred member body
	// re-parsed there (map::operator[], _M_emplace_hint_unique) can instantiate
	// NEW class templates (std::tuple<const int&> -> _Tuple_impl<...> ->
	// _Head_base<...>) that did NOT exist at Pass 0.5's struct sweep (12074), so
	// their definitions were never emitted and a by-value local of one is an
	// "incomplete struct". Sweep struct_map once more, emitting any class not yet
	// emitted (deps-first, deduped via the same sets). Emit into a TEMP list, then
	// splice it in right after the EARLY struct defs (late_struct_anchor) — BEFORE
	// the function prototypes (Pass 0.75/1/1.95) that take these late structs BY
	// VALUE. A plain append-to-tail placed the definition AFTER such a proto, so
	// c2mir saw an incomplete by-value param (map<int,int>'s `_Index_tuple<0>` param
	// of pair's indexed ctor → "incomplete struct or union" + "incompatible argument
	// type for struct/union type parameter"). Splicing after the early structs keeps
	// deps-first order (late structs' by-value deps on early structs stay
	// defined-before-use).
	node_t late_struct_list = list();
	for (auto &kv : prog->struct_map) {
		DataDefSTRUCT *sdd = dynamic_cast<DataDefSTRUCT *>(kv.second);
		if (!sdd) continue;
		if (DataDefCLASS *cdd = as_user_class(sdd))
			emit_class_struct_with_deps(cdd, late_struct_list,
						    emitted_structs, emitted_classes,
						    emitting_classes);
		else
			emit_struct_with_deps(sdd, late_struct_list,
					      emitted_structs, emitted_classes,
					      emitting_classes);
	}
	c2mir_op_splice_after(c2m, top_list, late_struct_anchor, late_struct_list);

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

static void cir_dump_node(FILE *f, node_t n, int indent, std::set<node_t> *seen)
{
	if (!n) return;
	// Cycle / shared-subtree guard: a CIR tree is a DAG (SHARE nodes point at
	// shared subtrees, and some library types — e.g. __max_size_type — form
	// genuine cycles, sometimes via lazily-generated distinct nodes). Re-descending
	// loops forever, so (a) print a node only once and mark later occurrences, and
	// (b) hard-cap recursion depth as a backstop against lazily-generated chains.
	if (indent > 800) {
		for (int i = 0; i < 80 && i < indent; i++) fputc(' ', f);
		fprintf(f, "<max depth>\n");
		return;
	}
	bool already = seen && !seen->insert(n).second;
	for (int i = 0; i < indent; i++) fputc(' ', f);
	fprintf(f, "%s", c2mir_node_code_name((c2mir_node_code_t)n->code));
	if (already) { fprintf(f, " <shared, dumped above>\n"); return; }

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
			cir_dump_node(f, op, indent + 2, seen);
		}
	}
}

void cir_dump_nodes(FILE *f, node_t tree)
{
	std::set<node_t> seen;
	fprintf(f, "=== CIR NODE TREE (+madc) ===\n");
	cir_dump_node(f, tree, 0, &seen);
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

// First error_msg in the tree (pre-order), or NULL. Used to surface WHY a
// tsubst pattern copy was rejected (the per-call resolve failure string) in the
// --show-stats fallback profile, so the worklist is self-diagnosing.
const char *cir_first_error_msg(node_t n)
{
	if (!n) return NULL;
	cir_node *cn = CIR_NODE(n);
	if (cn->error_msg) return cn->error_msg;
	if (n->code > N_ID) {
		for (int i = 0; ; i++) {
			node_t op = c2mir_node_op(n, i);
			if (!op) break;
			if (const char *m = cir_first_error_msg(op)) return m;
		}
	}
	return NULL;
}

// Collect the callee symbol of every N_CALL in the tree (child 0 = the callee
// N_ID). A tsubst-copied body references concrete callees (e.g. _M_get_node)
// without re-running the normal call lowering that records them as ODR-used, so
// the translate_module drain — which only materializes a deferred-lazy body when
// its symbol is in referenced_funcs — never emits them, and MIR-link reports an
// undefined import. Re-record them here.
void cir_collect_call_callees(node_t n, std::set<std::string> &out)
{
	if (!n) return;
	if (n->code == N_CALL) {
		node_t callee = c2mir_node_op(n, 0);
		if (callee && callee->code == N_ID && callee->u.s.s)
			out.insert(callee->u.s.s);
	}
	if (n->code > N_ID)
		for (int i = 0; ; i++) {
			node_t op = c2mir_node_op(n, i);
			if (!op) break;
			cir_collect_call_callees(op, out);
		}
}
