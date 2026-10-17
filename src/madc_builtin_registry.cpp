// The dialect is builtin — the registration owner, its differential and its
// generator (docs/plans/2026-10-17-dialect-builtin-registration.md, R1). The
// generated table lives in the object tree (obj/<mode>/dialect_registry.cpp);
// this file holds what the generator, the builders and the differential share.

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <iostream>
#include <sstream>
#include <memory>
#include <typeinfo>
#include <initializer_list>
#include <algorithm>
#include <stdint.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_builtin_registry.h"
#include "madc_module.h"		// MadcModuleDescription: an imported module's table

// A FuncDef's parameter vectors from generated code, as a parse fills them —
// a typedef name only where the parse recorded one (NULL: none recorded). A
// DECLARATION (parseFunction) also records, per parameter, whether a template
// parameter spelled it and its default (a builtin has neither); a function
// TYPE (parseFnPtrParams) records neither.
static void fill_builtin_params(FuncDef *fd, const BuiltinParam *params, size_t nparams,
				bool declaration)
{
    for ( size_t i = 0; i < nparams; ++i )
    {
	fd->parameters.push_back(params[i].type);
	fd->const_params.push_back(params[i].is_const);
	fd->param_cpp_spellings.push_back(madc::dis::istring(params[i].cpp_spelling));
	if ( params[i].typedef_name )
	    fd->param_typedef_names.push_back(madc::dis::istring(params[i].typedef_name));
	if ( declaration )
	{
	    fd->param_template_param_spelled_directly.push_back(false);
	    fd->param_defaults.push_back(NULL);
	}
    }
}

Variable *declare_builtin_function(Program &pgm, const BuiltinFunctionDecl &d)
{
    FuncDef *fd = new FuncDef(*d.returns);
    fill_builtin_params(fd, d.params, d.nparams, true);
    fd->namespace_name = madc::dis::istring(d.ns);
    fd->function_display_name = madc::dis::istring(d.name);
    fd->overload_spelling = madc::dis::istring(d.overload_spelling);
    fd->declaration_only = true;
    fd->is_varargs = (d.flags & bfVarargs) != 0;
    fd->is_void_params = (d.flags & bfVoidParams) != 0;
    fd->c_linkage = (d.flags & bfCLinkage) != 0;
    fd->noexcept_spec = (d.flags & bfNoexcept) ? FuncDef::NxTrue
		      : (d.flags & bfNoexceptUnknown) ? FuncDef::NxUnknown
		      : FuncDef::NxNone;
    fd->vague_linkage = (d.flags & bfVagueLinkage) != 0;
    fd->internal_linkage = (d.flags & bfInternalLinkage) != 0;
    fd->weak_binding = (d.flags & bfWeakBinding) != 0;
    Program::PendingForestFunc pf;
    pf.name = madc::dis::istring(d.key);
    pf.fd = fd;
    pf.mvar = NULL;
    for ( size_t i = 0; i < d.nparam_names && i < d.nparams; ++i )
	pf.mparams.push_back(std::make_pair(d.param_names[i], d.params[i].type));
    Variable *var = pgm.register_forest_func(pf);
    if ( var && (d.flags & bfEmitIsAlias) )
	fd->emit_symbol = var->storage_alias_name;
    // An imported module's function lives in the module: a call resolves its
    // own symbol there (the slot binding — CirBuilder::dyn_module_callee) —
    // the emit symbol, or a declaration's external one (register_forest_func's
    // storage alias: Itanium, or the bare name under C linkage) — in the
    // module its reference finds where the program runs.
    if ( var && pgm.registering_module )
	pgm.stamp_module_member(fd, pgm.registering_module->reference(),
				     fd->emit_symbol.empty() ? var->storage_alias_name
							     : fd->emit_symbol);
    return var;
}

// The type `pgm` registered as ns::name (namespace_datatype_map), or NULL.
static DataDef *registered_namespace_type(Program &pgm, const madc::dis::istring &ns,
					  const madc::dis::istring &name)
{
    namespace_datatype_map_t::iterator nti = pgm.namespace_datatype_map.find(ns);
    if ( nti == pgm.namespace_datatype_map.end() )
	return NULL;
    datatype_map_iter dti = nti->find(name);
    if ( dti == nti->end() || !dti->second )
	return NULL;
    return &dti->second->definition;
}

DataDefENUM *declare_builtin_enum(Program &pgm, const BuiltinEnumDecl &d)
{
    madc::dis::istring ns(d.ns), tag(d.name);
    if ( DataDef *had = registered_namespace_type(pgm, ns, tag) )
	return had->as_enum_dd();
    DataDefENUM *e = new DataDefENUM(tag);
    e->underlying = d.underlying;
    e->fixed_base = d.fixed_base;
    e->c_compatible = false;
    e->restore_layout(d.size, d.rawtype);
    e->set_canonical_spelling(ns + "::" + tag);
    TokenDataType *tdt = new TokenDataType(tag.c_str(), *e);
    pgm.datatype_map[tag] = tdt;
    pgm.namespace_datatype_map[ns][tag] = tdt;
    variable_map_t &scope_ns = pgm.namespace_variables_for_write(ns + "::" + tag);
    for ( size_t i = 0; i < d.nenumerators; ++i )
    {
	madc::dis::istring name(d.enumerators[i].name);
	Variable *v = new Variable(name, *e, 1, NULL, true);
	v->set(d.enumerators[i].value);
	v->make_enumerator();
	if ( !d.scoped )
	{
	    // Unscoped: the namespace's constant too (addVariable's program-scope
	    // Variable and namespace binding, without its parse state).
	    v->storage_alias_name = madc::dis::istring(d.enumerators[i].alias);
	    pgm.tkProgram->variables.push_back(v);
	    pgm.namespace_variables_for_write(ns)[name] = v;
	}
	scope_ns[name] = v;
	e->enumerators.push_back(std::make_pair(name, d.enumerators[i].value));
	e->enumerator_toks.push_back(NULL);
    }
    return e;
}

DataDef *builtin_function_pointer(Program &pgm, DataDef *returns,
				  std::initializer_list<BuiltinParam> params, unsigned flags)
{
    FuncDef *fd = new FuncDef(*returns);
    fill_builtin_params(fd, params.begin(), params.size(), false);
    fd->is_varargs = (flags & bfVarargs) != 0;
    fd->is_void_params = (flags & bfVoidParams) != 0;
    DataDefFPTR *fn = new DataDefFPTR(fd);
    fn->ptr_syntax = false;		// the function type, until its `*` folds it
    return pgm.getPointerType(fn);
}

DataDef *declare_builtin_typedef(Program &pgm, const char *ns, const char *name,
				 DataDef *aliased)
{
    madc::dis::istring n(ns), alias(name);
    if ( DataDef *had = registered_namespace_type(pgm, n, alias) )
	return had;
    TokenDataType *tdt = new TokenDataType(alias.c_str(), *aliased);
    pgm.datatype_map[alias] = tdt;	// register_scoped_typedef at namespace scope
    pgm.namespace_datatype_map[n][alias] = tdt;
    pgm.user_typedef_names.insert(alias);
    Program::TopDecl td;
    td.kind = Program::DeclKind::dkTypedef;
    td.name = alias;
    td.dd = aliased;
    td.tdt = tdt;
    td.forest_system = true;		// no parse position: a dialect header's
    pgm.top_decls.push_back(td);
    return aliased;
}

// One data member as the aggregate parsers add it: laid out by the
// aggregate's own addMember, its spelled alias and access recorded.
static void add_builtin_member(DataDefSTRUCT *s, const BuiltinMember &m)
{
    s->addMember(madc::dis::istring(m.name), *m.type, 1);
    s->members.back().typedef_name = madc::dis::istring(m.typedef_name);
    s->member_access.back() = m.access;
}

DataDefSTRUCT *declare_builtin_struct(Program &pgm, const BuiltinStructDecl &d)
{
    madc::dis::istring ns(d.ns), tag(d.name);
    if ( DataDef *had = registered_namespace_type(pgm, ns, tag) )
	return had->as_struct_dd();
    DataDefSTRUCT *s = new DataDefSTRUCT(tag, 0);
    s->definition_origin = AggregateDefinitionOrigin::Included;	// a header's; no file
    pgm.struct_map.set(tag, s);
    for ( size_t i = 0; i < d.nmembers; ++i )
	add_builtin_member(s, d.members[i]);
    s->is_complete = true;
    s->finalize(true);
    if ( d.object_class )
    {
	// TokenSTRUCT's promotion: the class takes the struct's state, the tag
	// is repointed, and the class layout is computed.
	DataDefCLASS *c = new DataDefCLASS(s->name, s->size, s->rawtype());
	static_cast<DataDefSTRUCT &>(*c) = *s;
	pgm.struct_map.set(tag, c);
	c->compute_layout();
	s = c;
    }
    s->set_canonical_spelling(ns + "::" + tag);
    TokenDataType *tdt = new TokenDataType(tag.c_str(), *s);
    pgm.datatype_map[tag] = tdt;	// register_cpp_aggregate_name
    pgm.namespace_datatype_map[ns][tag] = tdt;
    Program::TopDecl td;
    td.kind = Program::DeclKind::dkStruct;
    td.name = tag;
    td.dd = s;
    td.forest_system = true;		// no parse position: a dialect header's
    pgm.top_decls.push_back(td);
    return s;
}

DataDefCLASS *declare_builtin_class(Program &pgm, const char *ns, const char *name,
				    bool from_system_header)
{
    madc::dis::istring n(ns), tag(name);
    if ( registered_namespace_type(pgm, n, tag) )
	return NULL;
    DataDefCLASS *c = new DataDefCLASS(tag, 0, DataType::dtRESERVED);
    c->definition_origin = AggregateDefinitionOrigin::Included;	// a header's; no file
    c->set_canonical_spelling(n + "::" + tag);
    c->from_system_header = from_system_header;
    // register_class_shell, under the class's own namespace.
    pgm.struct_map.set(tag, c);
    TokenDataType *tdt = new TokenDataType(tag.c_str(), *c);
    pgm.datatype_map[tag] = tdt;
    pgm.namespace_datatype_map[n][tag] = tdt;
    pgm.set_class_type_alias(c, tag, c);
    return c;
}

void declare_builtin_class_member(DataDefCLASS *c, const BuiltinMember &m)
{
    add_builtin_member(c, m);
}

Variable *declare_builtin_method(Program &pgm, DataDefCLASS *c, const BuiltinMethodDecl &d)
{
    FuncDef *fd = new FuncDef(*d.returns);
    fill_builtin_params(fd, d.params, d.nparams, true);
    fd->declaration_only = true;
    fd->is_varargs = (d.flags & bfVarargs) != 0;
    fd->is_void_params = (d.flags & bfVoidParams) != 0;
    fd->c_linkage = (d.flags & bfCLinkage) != 0;
    fd->noexcept_spec = (d.flags & bfNoexcept) ? FuncDef::NxTrue
		      : (d.flags & bfNoexceptUnknown) ? FuncDef::NxUnknown
		      : FuncDef::NxNone;
    fd->vague_linkage = (d.flags & bfVagueLinkage) != 0;
    fd->internal_linkage = (d.flags & bfInternalLinkage) != 0;
    fd->weak_binding = (d.flags & bfWeakBinding) != 0;
    fd->is_const_method = (d.method_flags & bmConst) != 0;
    fd->is_explicit = (d.method_flags & bmExplicit) != 0;
    fd->implicit_dtor_noexcept = (d.method_flags & bmImplicitDtorNx) != 0;
    // The prototype's Variable and Method (addVariable + parseFunction's
    // prototype arm), bound in the class's namespace under its key.
    madc::dis::istring key(d.key);
    std::string canon = c->canonical_cpp_spelling().c_str();
    madc::dis::istring ns(canon.substr(0, canon.rfind("::")).c_str());
    pgm.funcdef_map[key] = fd;
    Variable *v = new Variable(key, *fd);
    pgm.tkProgram->variables.push_back(v);
    pgm.namespace_variables_for_write(ns)[key] = v;
    Method *m = new Method(*v);
    v->data = (void *)m;
    m->owner_class = c;
    Program::ClassMethodRegistration spec;
    spec.kind = d.kind;
    spec.display_name = madc::dis::istring(d.display);
    spec.access_flags = d.access;
    spec.is_static = (d.method_flags & bmStatic) != 0;
    spec.is_operator = (d.method_flags & bmOperator) != 0;
    spec.local_emit_name = madc::dis::istring(d.local_emit);
    pgm.register_class_method_signature(c, v, spec);
    return v;
}

void complete_builtin_class(Program &pgm, DataDefCLASS *c)
{
    pgm.complete_class_aggregate(c);
}

Variable *declare_builtin_extern(Program &pgm, const BuiltinExternDecl &d)
{
    madc::dis::istring ns(d.ns), name(d.name);
    variable_map_t &scope = pgm.namespace_variables_for_write(ns);
    variable_map_iter had = scope.find(name);
    if ( had != scope.end() && had->second )
	return had->second;
    Variable *v = new Variable(name, *d.type, 1, NULL, /*alloc=*/false);
    v->flags = d.flags;
    v->storage_alias_name = namespace_cpp_variable_symbol(ns, name);
    scope[name] = v;
    pgm.tkProgram->variables.push_back(v);
    Program::TopDecl td;		// the storage pass renders `extern T <alias>`
    td.kind = Program::DeclKind::dkGlobalVar;
    td.name = name;
    td.var = v;
    td.dd = d.type;
    td.forest_system = true;		// no parse position: a dialect header's
    pgm.top_decls.push_back(td);
    return v;
}

// A table's entry for ns::name (the table sorted by (ns, name) in byte
// order), or NULL — the dialect's table and an imported module's alike.
static const MadcBuiltinEntry *find_table_entry(const MadcBuiltinEntry *table, size_t count,
						const char *ns, const char *name)
{
    size_t lo = 0, hi = count;
    while ( lo < hi )
    {
	size_t mid = lo + (hi - lo) / 2;
	const MadcBuiltinEntry &e = table[mid];
	int c = strcmp(e.ns, ns);
	if ( !c )
	    c = strcmp(e.name, name);
	if ( !c )
	    return &e;
	if ( c < 0 )
	    lo = mid + 1;
	else
	    hi = mid;
    }
    return NULL;
}

static const MadcBuiltinEntry *find_builtin_entry(const char *ns, const char *name)
{
    return find_table_entry(madc::builtins::dialect_table, madc::builtins::dialect_table_count,
			    ns, name);
}

// The C++ string literal of `s` (the one C-string escape rule).
static std::string cpp_literal(const char *s)
{
    return std::string("\"") + madc_c_escape_string(s, strlen(s)).c_str() + "\"";
}

DataDef *builtin_type(Program &pgm, const char *ns, const char *name)
{
    madc::dis::istring n(ns), m(name);
    if ( DataDef *dd = registered_namespace_type(pgm, n, m) )
	return dd;
    if ( const MadcBuiltinEntry *e = find_builtin_entry(ns, name) )
	e->build(pgm);
    else
	// A type an imported module declares (a builder names its module's own
	// type before the type's builder ran: the table's order is the name's).
	for ( size_t i = 0; i < pgm.imported_modules.size(); ++i )
	{
	    const MadcModuleDescription *d = pgm.imported_modules[i].desc;
	    if ( const MadcBuiltinEntry *me = find_table_entry(d->declarations, d->ndeclarations,
								ns, name) )
	    {
		me->build(pgm);
		break;
	    }
	}
    return registered_namespace_type(pgm, n, m);
}

// The one namespace type whose TokenDataType holds `dd` — a function pointer's
// identity, which carries no canonical spelling (finish_alias keeps its shape:
// the alias IS the type). False when none, or more than one name, holds it.
static bool typedef_type_identity(Program &pgm, DataDef *dd, madc::dis::istring &ns,
				  madc::dis::istring &name)
{
    // Names copied out first (a lookup interns; the enumeration borrows keys).
    std::vector<std::pair<std::string, std::string> > hits;
    pgm.namespace_datatype_map.for_each_readonly(
	[dd, &hits](const char *n, const datatype_map_t &types) {
	    for ( datatype_map_t::const_iterator t = types.begin(); t != types.end(); ++t )
		if ( t->second && &t->second->definition == dd )
		    hits.push_back(std::make_pair(std::string(n), std::string(t->first.c_str())));
	    return false;
	});
    if ( hits.size() != 1 )
	return false;
    ns = madc::dis::istring(hits[0].first.c_str());
    name = madc::dis::istring(hits[0].second.c_str());
    return true;
}

// `dd` as the namespace type it is registered as: its canonical spelling
// `ns::name`, registered under that name — a function pointer by its typedef.
static bool namespace_type_identity(Program &pgm, DataDef *dd, madc::dis::istring &ns,
				    madc::dis::istring &name)
{
    if ( dd->as_fptr_dd() )
	return typedef_type_identity(pgm, dd, ns, name);
    std::string spelled = dd->canonical_cpp_spelling().c_str();
    size_t cut = spelled.rfind("::");
    if ( cut == std::string::npos || cut == 0 )
	return false;
    ns = madc::dis::istring(spelled.substr(0, cut).c_str());
    name = madc::dis::istring(spelled.substr(cut + 2).c_str());
    return registered_namespace_type(pgm, ns, name) == dd;
}

static bool function_pointer_expr(Program &pgm, DataDefFPTR *fp, std::string &expr,
				  std::string &err);

// A type as the C++ expression that yields it in a builder's Program `pgm`:
// a singleton by identity, a layered type through its one minting owner
// (getQualifiedType / getReferenceType / getPointerType) over its base. The
// differential reads types through it too, so "same type" means "built the
// same way". A dialect type is named through builtin_type (its own line
// proves its content). False with `err` naming a type the generator does not
// yet express.
static bool type_expr(Program &pgm, DataDef *dd, std::string &expr, std::string &err)
{
    static const struct { DataDef *dd; const char *spelled; } singletons[] = {
	{ &ddVOID, "&ddVOID" }, { &ddBOOL, "&ddBOOL" }, { &ddCHAR, "&ddCHAR" },
	{ &ddINT, "&ddINT" }, { &ddINT8, "&ddINT8" }, { &ddINT16, "&ddINT16" },
	{ &ddINT24, "&ddINT24" }, { &ddINT32, "&ddINT32" }, { &ddINT64, "&ddINT64" },
	{ &ddUINT8, "&ddUINT8" }, { &ddUINT16, "&ddUINT16" }, { &ddUINT24, "&ddUINT24" },
	{ &ddUINT32, "&ddUINT32" }, { &ddUINT64, "&ddUINT64" },
	{ &ddINT128, "&ddINT128" }, { &ddUINT128, "&ddUINT128" },
	{ &ddFLOAT, "&ddFLOAT" }, { &ddDOUBLE, "&ddDOUBLE" }, { &ddLDOUBLE, "&ddLDOUBLE" },
	{ &ddLPSTR, "&ddLPSTR" }, { &ddVOIDptr, "&ddVOIDptr" }, { &ddCHARptr, "&ddCHARptr" },
	{ &ddINTptr, "&ddINTptr" }, { &ddINT32ptr, "&ddINT32ptr" }, { &ddARRAY, "&ddARRAY" },
	{ &ddAUTO, "&ddAUTO" },
    };
    if ( !dd )
    {
	err = "a NULL type";
	return false;
    }
    for ( size_t i = 0; i < sizeof(singletons) / sizeof(singletons[0]); ++i )
	if ( singletons[i].dd == dd )
	{
	    expr = singletons[i].spelled;
	    return true;
	}
    std::string base;
    if ( DataDefQUAL *q = dd->as_qualified_dd() )
    {
	if ( !type_expr(pgm, q->base_type, base, err) )
	    return false;
	expr = "pgm.getQualifiedType(" + base + ", " + std::to_string(q->quals) + "u)";
	return true;
    }
    if ( DataDefREF *r = dd->as_reference_dd() )
    {
	if ( !type_expr(pgm, r->base_type, base, err) )
	    return false;
	expr = "pgm.getReferenceType(" + base + (r->rvalue ? ", true)" : ", false)");
	return true;
    }
    if ( DataDefPTR *p = dd->as_pointer_dd() )
    {
	if ( !type_expr(pgm, p->base_type, base, err) )
	    return false;
	expr = "pgm.getPointerType(" + base + ")";
	return true;
    }
    madc::dis::istring ns, name;
    if ( namespace_type_identity(pgm, dd, ns, name) )
    {
	expr = std::string("builtin_type(pgm, ") + cpp_literal(ns.c_str()) + ", "
	     + cpp_literal(name.c_str()) + ")";
	return true;
    }
    if ( DataDefFPTR *fp = dd->as_fptr_dd() )
	return function_pointer_expr(pgm, fp, expr, err);	// no typedef names it
    err = std::string("type '") + dd->name.c_str() + "' (" + typeid(*dd).name() + ")";
    return false;
}

// A function pointer spelled structurally — `builtin_function_pointer(pgm,
// RET, { {T, "spelling", const, "typedef"}, ... }, FLAGS)` — when its
// signature holds only what that spelling carries.
static bool function_pointer_expr(Program &pgm, DataDefFPTR *fp, std::string &expr,
				  std::string &err)
{
    FuncDef *fd = fp->target;
    if ( !fd || !fp->ptr_syntax )
    {
	err = "a function type";
	return false;
    }
    size_t n = fd->parameters.size();
    if ( fd->is_const_method || fd->noexcept_spec != FuncDef::NxNone
      || !fd->return_typedef_name.empty() || !fd->function_display_name.empty()
      || !fd->namespace_name.empty() || fd->const_params.size() != n
      || fd->param_cpp_spellings.size() != n || fd->param_typedef_names.size() > n
      || !fd->param_default_tokens.empty() || !fd->param_defaults.empty()
      || !fd->param_template_param_spelled_directly.empty() )
    {
	err = "a function pointer whose signature carries more than its parameters";
	return false;
    }
    std::string t;
    if ( !type_expr(pgm, &fd->returns, t, err) )
	return false;
    expr = "builtin_function_pointer(pgm, " + t + ", {";
    for ( size_t i = 0; i < n; ++i )
    {
	if ( !type_expr(pgm, fd->parameters[i], t, err) )
	    return false;
	expr += std::string(i ? ", " : " ") + "{ " + t + ", "
	      + cpp_literal(fd->param_cpp_spellings[i].c_str()) + ", "
	      + (fd->const_params[i] ? "true" : "false") + ", "
	      + (i < fd->param_typedef_names.size()
		 ? cpp_literal(fd->param_typedef_names[i].c_str()) : std::string("NULL"))
	      + " }";
    }
    expr += n ? " }, 0u" : "}, 0u";
    if ( fd->is_varargs )
	expr += " | bfVarargs";
    if ( fd->is_void_params )
	expr += " | bfVoidParams";
    expr += ")";
    return true;
}

static std::string type_field(Program &pgm, DataDef *dd)
{
    std::string expr, err;
    return type_expr(pgm, dd, expr, err) ? expr : "?" + err;
}

// A signature's fields — what a function and a function TYPE share: the
// return, each parameter (type | C++ spelling | referent const | typedef name
// | a default by its token run's spelling) and the parameter vectors' lengths.
static std::string signature_fields(Program &pgm, FuncDef *fd)
{
    std::string line = "\tret=" + type_field(pgm, &fd->returns);
    line += "\trtd=" + std::string(fd->return_typedef_name.c_str());
    line += "\tparams=";
    for ( size_t i = 0; i < fd->parameters.size(); ++i )
    {
	if ( i )
	    line += ";";
	line += type_field(pgm, fd->parameters[i]);
	line += "|";
	if ( i < fd->param_cpp_spellings.size() )
	    line += fd->param_cpp_spellings[i].c_str();
	line += "|";
	line += ( i < fd->const_params.size() && fd->const_params[i] ) ? "c" : "-";
	line += "|";
	if ( i < fd->param_typedef_names.size() )
	    line += fd->param_typedef_names[i].c_str();
	line += "|";
	if ( i < fd->param_default_tokens.size() && !fd->param_default_tokens[i].empty() )
	    line += template_tokens_spelling(fd->param_default_tokens[i]).c_str();
    }
    line += "\tvecs=" + std::to_string(fd->parameters.size()) + ","
	  + std::to_string(fd->const_params.size()) + ","
	  + std::to_string(fd->param_cpp_spellings.size()) + ","
	  + std::to_string(fd->param_typedef_names.size()) + ","
	  + std::to_string(fd->param_default_tokens.size()) + ","
	  + std::to_string(fd->param_template_param_spelled_directly.size()) + ","
	  + std::to_string(fd->param_defaults.size());
    for ( size_t i = 0; i < fd->param_template_param_spelled_directly.size(); ++i )
	if ( fd->param_template_param_spelled_directly[i] )
	    line += ":tp" + std::to_string(i);
    for ( size_t i = 0; i < fd->param_defaults.size(); ++i )
	if ( fd->param_defaults[i] )
	    line += ":def" + std::to_string(i);
    return line;
}

// The fields a free function's registration carries — the record the forest
// restore proves sufficient (cir_freeze.cpp's DK_FUNC restore + restore_param)
// plus the registration site's own state (register_forest_func: the Variable's
// storage_alias_name, its Method's named parameters, its place in the
// namespace overload set).
madc::dis::istring builtin_function_line(Program &pgm, const madc::dis::istring &key)
{
    funcdef_map_iter fit = pgm.funcdef_map.find(key);
    if ( fit == pgm.funcdef_map.end() || !fit->second )
	return madc::dis::istring();
    FuncDef *fd = fit->second;
    Variable *var = pgm.tkProgram
	? pgm.tkProgram->findVariable(pgm.strpool, key) : NULL;
    const Method *m = var ? (const Method *)var->data : NULL;

    std::string line = std::string(key.c_str());
    line += "\tdisp=" + std::string(fd->function_display_name.c_str());
    line += "\tns=" + std::string(fd->namespace_name.c_str());
    line += signature_fields(pgm, fd);
    line += "\tmparams=";
    for ( size_t i = 0; m && i < m->parameters.size(); ++i )
    {
	if ( i )
	    line += ";";
	if ( m->parameters[i] )
	    line += std::string(m->parameters[i]->name.c_str()) + ":"
		  + type_field(pgm, m->parameters[i]->type);
    }
    line += "\temit=" + std::string(fd->emit_symbol.c_str());
    line += "\talias=" + std::string(var ? var->storage_alias_name.c_str() : "");
    line += "\tflags=";
    line += fd->is_varargs ? "v" : "-";
    line += fd->is_void_params ? "0" : "-";
    line += fd->c_linkage ? "C" : "-";
    line += fd->declaration_only ? "d" : "-";
    line += fd->defaulted_or_deleted ? "=" : "-";
    line += fd->is_deleted ? "x" : "-";
    line += fd->noexcept_spec == FuncDef::NxTrue ? "n"
	  : fd->noexcept_spec == FuncDef::NxUnknown ? "?" : "-";
    line += fd->has_forest_body ? "b" : "-";
    line += fd->internal_linkage ? "i" : "-";
    line += fd->vague_linkage ? "g" : "-";
    line += fd->weak_binding ? "w" : "-";
    line += "\tbk=" + std::string(fd->inline_builtin_kind.c_str());
    line += "\tovl=" + std::string(fd->overload_spelling.c_str());
    line += "\ttargs=";
    for ( size_t i = 0; i < fd->overload_template_args.size(); ++i )
    {
	if ( i )
	    line += ",";
	line += fd->overload_template_args[i].c_str();
    }
    line += "\ttret=" + std::string(fd->template_return_param_name.c_str());
    line += "\tdyn=" + std::string(fd->dyn_module_library.c_str()) + ":"
	  + fd->dyn_module_member.c_str();
    // A member function's: its class, its names, its qualifiers; every
    // function's Variable flags and the namespaces that bind it by its key.
    line += "\towner=" + std::string(m && m->owner_class
				     ? m->owner_class->canonical_cpp_spelling().c_str() : "");
    line += "\tmdisp=" + std::string(fd->method_display_name.c_str())
	  + "\tlemit=" + fd->local_emit_name.c_str() + "\tmflags=";
    line += fd->is_const_method ? "k" : "-";
    line += fd->is_volatile_method ? "V" : "-";
    line += fd->is_explicit ? "e" : "-";
    line += fd->pure_virtual ? "p" : "-";
    line += fd->implicit_dtor_noexcept ? "i" : "-";
    line += fd->ctor_trailing_self ? "t" : "-";
    line += fd->is_member_template ? "T" : "-";
    line += fd->borrows_receiver_text ? "b" : "-";
    line += fd->is_constexpr ? "c" : "-";
    line += "\tvflags=" + std::to_string(var ? var->flags : 0) + "\tnsbind=";
    for ( namespace_map_t::const_iterator nsi = pgm.namespace_map.begin();
	  var && nsi != pgm.namespace_map.end(); ++nsi )
    {
	variable_map_t::const_iterator vi = nsi->second.find(key);
	if ( vi != nsi->second.end() && vi->second == var )
	    line += std::string(nsi->first.c_str()) + ",";
    }
    // Its place in the namespace overload set (registration order).
    line += "\tovidx=";
    if ( var && !fd->function_display_name.empty() )
    {
	madc::dis::istring ovkey = fd->namespace_name + "::" + fd->function_display_name;
	if ( pgm.namespace_fn_overload_sets.find(ovkey) != pgm.namespace_fn_overload_sets.end() )
	{
	    const std::vector<Program::NamespaceFnOverload> &ovset =
		pgm.namespace_fn_overload_sets[ovkey];
	    for ( size_t i = 0; i < ovset.size(); ++i )
		if ( ovset[i].var == var )
		    line += std::to_string(i);
	}
    }
    return line;
}

// Is `v` in the program scope (tkProgram's own variables)?
static bool in_program_scope(Program &pgm, const Variable *v)
{
    if ( !pgm.tkProgram )
	return false;
    for ( size_t i = 0; i < pgm.tkProgram->variables.size(); ++i )
	if ( pgm.tkProgram->variables[i] == v )
	    return true;
    return false;
}

// The variable `pgm` binds as ns::name (namespace_map), or NULL.
static Variable *namespace_variable(Program &pgm, const madc::dis::istring &ns,
				    const madc::dis::istring &name)
{
    namespace_map_t::iterator nsi = pgm.namespace_map.find(ns);
    if ( nsi == pgm.namespace_map.end() )
	return NULL;
    variable_map_iter vi = nsi->second.find(name);
    return vi == nsi->second.end() ? NULL : vi->second;
}

// An enum's enumerators bound in its namespace too: the unscoped form.
static bool enum_is_unscoped(Program &pgm, const madc::dis::istring &ns, DataDefENUM *e)
{
    if ( e->enumerators.empty() )
	return false;
    madc::dis::istring tag = e->enum_name;
    Variable *in_tag = namespace_variable(pgm, ns + "::" + tag, e->enumerators[0].first);
    return in_tag && namespace_variable(pgm, ns, e->enumerators[0].first) == in_tag;
}

// A class's own fields: its class layout (compute_layout's), its special
// members and methods by key, its scope (aliases, statics, friends), its
// bases and its virtual machinery.
static std::string class_fields(DataDefCLASS *c)
{
    std::string line = "\tnvsize=" + std::to_string(c->nvsize) + "\town=" + std::to_string(c->own_block_off)
	+ "\tcalign=" + std::to_string(c->class_align) + "\tcflags=";
    line += c->has_vptr_slot ? "p" : "-";
    line += c->has_vtable ? "v" : "-";
    line += c->from_system_header ? "s" : "-";
    line += c->has_user_ctor ? "c" : "-";
    line += c->has_user_dtor ? "d" : "-";
    line += c->is_dependent_placeholder ? "D" : "-";
    line += c->has_dependent_surface ? "S" : "-";
    line += c->opaque_concrete_tag ? "o" : "-";
    line += c->is_extern_template_instantiated ? "x" : "-";
    line += c->has_deleted_copy_assign ? "=" : "-";
    line += c->has_deleted_default_ctor ? "0" : "-";
    line += c->has_defaulted_default_ctor ? "+" : "-";
    line += c->has_deleted_dtor ? "~" : "-";
    line += "\tencl=" + std::string(c->enclosing_class ? c->enclosing_class->name.c_str() : "")
	  + "\tbase=" + std::string(c->base_class ? c->base_class->name.c_str() : "");
    line += "\tmethods=";
    for ( size_t i = 0; i < c->methods.size(); ++i )
	line += std::string(i ? "," : "") + (c->methods[i] ? c->methods[i]->name.c_str() : "?")
	      + ":" + std::to_string(c->methods[i] ? c->methods[i]->flags : 0);
    line += "\tctors=";
    for ( size_t i = 0; i < c->ctors.size(); ++i )
	line += std::string(i ? "," : "") + (c->ctors[i] ? c->ctors[i]->name.c_str() : "?");
    line += "\tmmap=";
    for ( std::map<madc::dis::istring, Variable *>::const_iterator mi = c->method_map.begin();
	  mi != c->method_map.end(); ++mi )
	line += std::string(mi->first.c_str()) + ">" + (mi->second ? mi->second->name.c_str() : "?") + ",";
    line += "\taliases=";
    for ( std::map<madc::dis::istring, DataDef *>::const_iterator ai = c->type_aliases.begin();
	  ai != c->type_aliases.end(); ++ai )
	line += std::string(ai->first.c_str()) + (ai->second == c ? ">self," : ">other,");
    line += "\tcvecs=" + std::to_string(c->staticconst.size()) + ","
	  + std::to_string(c->friend_class_names.size()) + ","
	  + std::to_string(c->friend_function_names.size()) + ","
	  + std::to_string(c->static_member_types.size()) + ","
	  + std::to_string(c->static_member_const_values.size()) + ","
	  + std::to_string(c->bases.size()) + ","
	  + std::to_string(c->vbase_offset.size()) + ","
	  + std::to_string(c->secondary_vptr_owners.size()) + ","
	  + std::to_string(c->vtable_slots.size()) + ","
	  + std::to_string(c->virtual_methods.size()) + ","
	  + std::to_string(c->vtable_groups.size());
    return line;
}

// An aggregate's fields: its layout (the target's), its flags, its tag binding
// and every member record with the per-member vectors' lengths.
static std::string aggregate_fields(Program &pgm, const madc::dis::istring &name,
				    DataDefSTRUCT *s)
{
    std::string line = std::string("\tkind=") + typeid(*s).name();
    line += s->is_complete ? "\tcomplete" : "\tincomplete";
    line += "\talign=" + std::to_string(s->max_align) + "\tpack=" + std::to_string(s->pack)
	  + "/" + std::to_string(s->pragma_pack) + "/" + std::to_string(s->tag_explicit_align);
    line += "\torigin=" + std::to_string((int)s->definition_origin) + "\tsflags=";
    line += s->union_layout ? "u" : "-";
    line += s->struct_is_final ? "f" : "-";
    line += s->forest_restored ? "r" : "-";
    line += s->has_anon_aggregate ? "a" : "-";
    line += s->is_anonymous ? "n" : "-";
    line += s->reverse_scalar_storage ? "s" : "-";
    StructRegistry::const_iterator si = pgm.struct_map.find(name);
    line += "\tsmap=";
    line += si == pgm.struct_map.end() ? "none" : si->second == s ? "self" : "other";
    line += "\tmembers=";
    for ( size_t i = 0; i < s->members.size(); ++i )
    {
	const memberpair_t &m = s->members[i];
	line += std::string(i ? ";" : "") + m.first.c_str() + ":" + type_field(pgm, m.second)
	      + ":" + m.typedef_name.c_str();
	if ( i < s->member_offsets.size() )
	    line += ":@" + std::to_string(s->member_offsets[i]);
	if ( i < s->member_counts.size() )
	    line += ":x" + std::to_string(s->member_counts[i]);
	if ( i < s->member_array_flags.size() && s->member_array_flags[i] )
	    line += ":arr";
	if ( i < s->member_access.size() )
	    line += ":a" + std::to_string(s->member_access[i]);
	if ( i < s->member_bitfields.size() && s->member_bitfields[i].is_bitfield )
	    line += ":bits";
	if ( i < s->member_dims.size() && !s->member_dims[i].empty() )
	    line += ":dims";
	if ( i < s->member_count_exprs.size() && s->member_count_exprs[i] )
	    line += ":vla";
    }
    line += "\tmvecs=" + std::to_string(s->members.size()) + ","
	  + std::to_string(s->member_counts.size()) + ","
	  + std::to_string(s->member_array_flags.size()) + ","
	  + std::to_string(s->member_offsets.size()) + ","
	  + std::to_string(s->member_bitfields.size()) + ","
	  + std::to_string(s->member_dims.size()) + ","
	  + std::to_string(s->member_count_exprs.size()) + ","
	  + std::to_string(s->member_access.size()) + ","
	  + std::to_string(s->member_origin.size()) + ","
	  + std::to_string(s->anonymous_aggregates.size()) + ","
	  + std::to_string(s->member_vbase.size()) + ","
	  + std::to_string(s->member_explicit_align.size()) + ","
	  + std::to_string(s->member_packed.size()) + ","
	  + std::to_string(s->member_default_inits.size()) + ","
	  + std::to_string(s->member_nsdmi_braces.size());
    if ( DataDefCLASS *c = s->as_class_dd() )
	line += class_fields(c);
    return line;
}

madc::dis::istring builtin_type_line(Program &pgm, const madc::dis::istring &ns,
				     const madc::dis::istring &name)
{
    DataDef *dd = registered_namespace_type(pgm, ns, name);
    if ( !dd )
	return madc::dis::istring();
    std::string line = std::string(ns.c_str()) + "::" + name.c_str();
    line += "\tcanon=" + std::string(dd->canonical_cpp_spelling().c_str());
    line += "\tsize=" + std::to_string(dd->size);
    line += "\traw=" + std::to_string((unsigned)dd->type());
    flat_datatype_map_iter fi = pgm.datatype_map.find(name);
    line += "\tflat=";
    line += fi == pgm.datatype_map.end() ? "none" : &(*fi)->definition == dd ? "self" : "other";
    line += pgm.user_typedef_names.count(name) ? "\tudt" : "\t-";
    // Its file-scope declarations: kind, system-origin verdict (the emitter's
    // td_system), a combined typedef's body.
    line += "\ttop=";
    for ( size_t i = 0; i < pgm.top_decls.size(); ++i )
    {
	const Program::TopDecl &td = pgm.top_decls[i];
	if ( td.name != name || td.dd != dd )
	    continue;
	int at = 0;
	const char *f = pgm.top_decl_position(td, at);
	bool sys = td.forest_system || (f && pgm.is_system_header_path(f));
	line += std::to_string((int)td.kind) + (sys ? "s" : "u") + (td.struct_body ? "b" : "") + ",";
    }
    if ( DataDefFPTR *fp = dd->as_fptr_dd() )
    {
	line += "\tkind=fptr";
	line += fp->ptr_syntax ? "\tptr" : "\tfn";
	if ( !fp->target )
	    return line + "\tno-target";
	FuncDef *fd = fp->target;
	line += signature_fields(pgm, fd) + "\tfflags=";
	line += fd->is_varargs ? "v" : "-";
	line += fd->is_void_params ? "0" : "-";
	line += fd->is_const_method ? "k" : "-";
	line += fd->noexcept_spec == FuncDef::NxTrue ? "n"
	      : fd->noexcept_spec == FuncDef::NxUnknown ? "?" : "-";
	line += "\tdisp=" + std::string(fd->function_display_name.c_str())
	      + "\tfns=" + fd->namespace_name.c_str();
	return line;
    }
    if ( DataDefSTRUCT *s = dd->as_struct_dd() )
	return line + aggregate_fields(pgm, name, s);
    DataDefENUM *e = dd->as_enum_dd();
    if ( !e )
	return line + "\tkind=" + typeid(*dd).name();
    line += "\tkind=enum\tunder=" + (e->underlying ? type_field(pgm, e->underlying) : "-");
    line += e->fixed_base ? "\tfixed" : "\tcomputed";
    line += e->c_compatible ? "\tc" : "\tcxx";
    bool unscoped = enum_is_unscoped(pgm, ns, e);
    line += unscoped ? "\tunscoped" : "\tscoped";
    madc::dis::istring tag_ns = ns + "::" + name;
    line += "\tenumerators=";
    for ( size_t i = 0; i < e->enumerators.size(); ++i )
    {
	const madc::dis::istring &en = e->enumerators[i].first;
	line += std::string(i ? ";" : "") + en.c_str() + "=" + std::to_string(e->enumerators[i].second);
	Variable *v = namespace_variable(pgm, tag_ns, en);
	if ( !v )
	{
	    line += ":missing";
	    continue;
	}
	line += ":" + std::to_string((long long)v->get<int64_t>()) + ":" + std::to_string(v->flags)
	      + ":" + type_field(pgm, v->type);
	if ( unscoped )
	    line += std::string(":") + (namespace_variable(pgm, ns, en) == v ? "ns" : "-")
		  + (in_program_scope(pgm, v) ? "+prog" : "") + ":" + v->storage_alias_name.c_str();
    }
    return line;
}

madc::dis::istring builtin_variable_line(Program &pgm, const madc::dis::istring &ns,
					 const madc::dis::istring &name)
{
    Variable *v = namespace_variable(pgm, ns, name);
    if ( !v )
	return madc::dis::istring();
    std::string line = std::string(ns.c_str()) + "::" + name.c_str();
    line += "\ttype=" + type_field(pgm, v->type);
    line += "\tflags=" + std::to_string(v->flags) + "\tcount=" + std::to_string(v->count);
    line += "\talias=" + std::string(v->storage_alias_name.c_str());
    line += in_program_scope(pgm, v) ? "\tprog" : "\t-";
    line += v->data ? "\tstorage" : "\t-";
    // Its file-scope declarations: kind, system-origin verdict, initializer.
    line += "\ttop=";
    for ( size_t i = 0; i < pgm.top_decls.size(); ++i )
    {
	const Program::TopDecl &td = pgm.top_decls[i];
	if ( td.var != v )
	    continue;
	int at = 0;
	const char *f = pgm.top_decl_position(td, at);
	bool sys = td.forest_system || (f && pgm.is_system_header_path(f));
	bool init = td.decl && (td.decl->initialize || !td.decl->init_list.empty()
				|| !td.decl->ctor_args.empty());
	line += std::to_string((int)td.kind) + (sys ? "s" : "u") + (init ? "i" : "")
	      + (td.dd == v->type ? "" : "?") + ",";
    }
    return line;
}

// One program parsed as text — the pack bind off (--no-forest-bind's
// setting), so a declaration is what the parser makes of the file's text.
static std::shared_ptr<Program> parse_text_program(const std::string &src,
						   const char *display_name,
						   std::string &err)
{
    std::shared_ptr<Program> p = std::make_shared<Program>();
    p->registration_policy.enable_forest_bind = false;
    p->forest_bind_path.clear();
    TokenProgram *tp = p->tokenize_buffer(madc::dis::istring(src.c_str()),
					  madc::dis::istring(display_name));
    if ( !tp || !p->parse(tp) )
    {
	err = std::string("the dialect's text parse failed (") + display_name + ")";
	return std::shared_ptr<Program>();
    }
    return p;
}

// The dialect's roots as include lines (the auto-include order), or false
// outside --std=madc.
static bool dialect_root_includes(Program &pgm, std::string &src, std::string &err)
{
    std::vector<madc::dis::istring> roots;
    if ( !pgm.dialect_image_roots(roots) )
    {
	err = "the dialect's roots exist only under --std=madc";
	return false;
    }
    for ( size_t i = 0; i < roots.size(); ++i )
	src += std::string("#include <") + roots[i].c_str() + ">\n";
    return true;
}

static void collect_delta_keys(DialectTextParse &out);

bool parse_dialect_as_text(DialectTextParse &out, std::string &err)
{
    const std::string empty = "int main() { return 0; }\n";
    out.base = parse_text_program(empty, "<dialect-base>", err);
    if ( !out.base )
	return false;
    std::string src;
    if ( !dialect_root_includes(*out.base, src, err) )
	return false;
    src += empty;
    out.live = parse_text_program(src, "<dialect-live>", err);
    if ( !out.live )
	return false;
    collect_delta_keys(out);
    return true;
}

bool parse_module_as_text(DialectTextParse &out, const std::string &face, std::string &err)
{
    const std::string empty = "int main() { return 0; }\n";
    std::shared_ptr<Program> probe = parse_text_program(empty, "<module-probe>", err);
    if ( !probe )
	return false;
    std::string dialect;
    if ( !dialect_root_includes(*probe, dialect, err) )
	return false;
    out.base = parse_text_program(dialect + empty, "<module-base>", err);
    if ( !out.base )
	return false;
    out.live = parse_text_program(dialect + "#include \"" + face + "\"\n" + empty,
				  "<module-live>", err);
    if ( !out.live )
	return false;
    collect_delta_keys(out);
    return true;
}

// What the live parse declared that the base did not: its functions, its
// namespace types and its namespace objects (DialectTextParse's keys).
static void collect_delta_keys(DialectTextParse &out)
{
    out.function_keys.clear();
    for ( funcdef_map_iter it = out.live->funcdef_map.begin();
	  it != out.live->funcdef_map.end(); ++it )
	if ( out.base->funcdef_map.find(it->first) == out.base->funcdef_map.end()
	  && it->second && !it->second->builtin_registration
	  && it->second->dyn_module_library.empty() )
	    out.function_keys.push_back(it->first);
    // The namespace types: names copied out first (a lookup interns, and the
    // enumeration's keys are borrowed from the pool), then filtered.
    std::vector<std::pair<std::string, std::string> > live_types;
    out.live->namespace_datatype_map.for_each_readonly(
	[&live_types](const char *ns, const datatype_map_t &types) {
	    for ( datatype_map_t::const_iterator t = types.begin(); t != types.end(); ++t )
		live_types.push_back(std::make_pair(std::string(ns), std::string(t->first.c_str())));
	    return false;
	});
    out.type_keys.clear();
    for ( size_t i = 0; i < live_types.size(); ++i )
    {
	madc::dis::istring ns(live_types[i].first.c_str()), name(live_types[i].second.c_str());
	if ( !registered_namespace_type(*out.base, ns, name) )
	    out.type_keys.push_back(std::make_pair(ns, name));
    }
    // The namespace objects: a function's Variable (its type a FuncDef — a
    // free function's and a method's) and an enumerator are their own
    // declarations' (the function and enum builders).
    out.variable_keys.clear();
    for ( namespace_map_t::const_iterator nsi = out.live->namespace_map.begin();
	  nsi != out.live->namespace_map.end(); ++nsi )
	for ( variable_map_t::const_iterator vi = nsi->second.begin(); vi != nsi->second.end(); ++vi )
	{
	    Variable *v = vi->second;
	    if ( !v || v->is_enumerator() || (v->type && v->type->as_funcdef_dd())
	      || namespace_variable(*out.base, nsi->first, vi->first) )
		continue;
	    out.variable_keys.push_back(std::make_pair(nsi->first, vi->first));
	}
}

// The types one skip judgement is judging. A type met again while it is
// being judged is answered by its own builder (a class registers its shell
// before its members name it); the plan's closing pass refuses any built
// group that names a type that got no builder.
typedef std::set<DataDef *> TypeVisit;

static std::string named_type_reason(Program &live, DataDef *dd, TypeVisit &seen);

// A signature that names a dialect type the generator does not build.
static std::string signature_type_reason(Program &live, FuncDef *fd, TypeVisit &seen)
{
    std::string why = named_type_reason(live, &fd->returns, seen);
    for ( size_t i = 0; why.empty() && i < fd->parameters.size(); ++i )
	why = named_type_reason(live, fd->parameters[i], seen);
    return why;
}

// An aggregate's registration as the builders make it: spelled and bound
// under its own tag (struct_map and the flat type map hold it), complete and
// attribute-free, every member plain (one element; no bit-field, array or
// initializer) naming no aggregate (a struct registers no shell first: an
// aggregate member could close a cycle) and no type the generator does not
// build.
static std::string aggregate_skip_reason(Program &live, const madc::dis::istring &ns,
					 const madc::dis::istring &name, DataDefSTRUCT *s,
					 TypeVisit &seen)
{
    if ( s->canonical_cpp_spelling() != ns + "::" + name || s->name != name )
	return "an aggregate registered under another name";
    flat_datatype_map_iter fi = live.datatype_map.find(name);
    StructRegistry::const_iterator si = live.struct_map.find(name);
    if ( fi == live.datatype_map.end() || &(*fi)->definition != s
      || si == live.struct_map.end() || si->second != s )
	return "a bare tag another type holds";
    if ( !s->is_complete || s->union_layout || s->pack || s->pragma_pack
      || s->tag_explicit_align || s->struct_is_final || s->has_anon_aggregate
      || s->is_anonymous || s->reverse_scalar_storage
      || !s->anonymous_aggregates.empty() || !s->member_vbase.empty()
      || !s->member_explicit_align.empty() || !s->member_packed.empty()
      || !s->member_default_inits.empty() || !s->member_nsdmi_braces.empty() )
	return "an aggregate with layout attributes or member initializers";
    for ( size_t i = 0; i < s->members.size(); ++i )
    {
	if ( s->member_counts[i] != 1 || s->member_array_flags[i]
	  || s->member_bitfields[i].is_bitfield || !s->member_dims[i].empty()
	  || s->member_count_exprs[i] )
	    return "a member the builder does not lay out";
	std::string expr, err;
	if ( !type_expr(live, s->members[i].second, expr, err) )
	    return "member " + err;
	DataDef *leaf = s->members[i].second;
	while ( leaf && (leaf->as_qualified_dd() || leaf->as_pointer_dd()) )
	    leaf = leaf->as_qualified_dd() ? leaf->as_qualified_dd()->base_type
					   : leaf->as_pointer_dd()->base_type;
	if ( leaf && leaf->as_struct_dd() && expr.compare(0, 3, "&dd") != 0 )
	    return "a member naming an aggregate";
	std::string why = named_type_reason(live, s->members[i].second, seen);
	if ( !why.empty() )
	    return why;
    }
    return std::string();
}

// A class TokenCLASS registered whose member functions the builder registers:
// its tag its one scope alias, no base, virtual, static, friend or special-
// member state, every member function a declaration-only prototype (the host
// implements it) the class registrar registered, carrying only what
// declare_builtin_method sets. A constructor, a destructor, a method or an
// operator — not a conversion function.
static std::string class_skip_reason(Program &live, DataDefCLASS *c, TypeVisit &seen)
{
    if ( typeid(*c) != typeid(DataDefCLASS) )
	return std::string("a ") + typeid(*c).name();
    if ( c->type_aliases.size() != 1 || c->type_aliases.begin()->first != c->name
      || c->type_aliases.begin()->second != c )
	return "a class with a scope of its own (nested types, aliases)";
    if ( c->base_class || !c->bases.empty() || c->enclosing_class || c->has_vtable
      || c->has_vptr_slot || !c->virtual_methods.empty() || !c->vtable_slots.empty() )
	return "a class with bases or virtual functions";
    if ( !c->staticconst.empty() || !c->static_member_types.empty()
      || !c->static_member_const_values.empty() || !c->friend_class_names.empty()
      || !c->friend_function_names.empty() || c->has_deleted_copy_assign
      || c->has_deleted_default_ctor || c->has_defaulted_default_ctor || c->has_deleted_dtor
      || c->is_dependent_placeholder || c->has_dependent_surface || c->opaque_concrete_tag
      || c->is_extern_template_instantiated )
	return "a class with static members, friends or special-member state";
    for ( size_t i = 0; i < c->methods.size(); ++i )
    {
	Variable *v = c->methods[i];
	const Method *m = v ? (const Method *)v->data : NULL;
	FuncDef *fd = v && v->type ? v->type->as_funcdef_dd() : NULL;
	if ( !fd || !m || m->owner_class != c || !m->parameters.empty() )
	    return "a member function the builder does not register";
	if ( !fd->declaration_only )
	    return "a member function with a body (R3)";
	bool ctor = std::find(c->ctors.begin(), c->ctors.end(), v) != c->ctors.end();
	if ( !ctor && fd->method_display_name.c_str()[0] != '~'
	  && std::string(fd->method_display_name.c_str()).compare(0, 9, "operator ") == 0 )
	    return "a conversion function";
	if ( fd->pure_virtual || fd->is_member_template || fd->ctor_trailing_self
	  || fd->borrows_receiver_text || fd->is_constexpr || fd->is_volatile_method
	  || fd->defaulted_or_deleted || fd->is_deleted || fd->has_forest_body
	  || !fd->inline_builtin_kind.empty() || !fd->return_typedef_name.empty()
	  || !fd->overload_template_args.empty() || !fd->template_return_param_name.empty()
	  || !fd->dyn_module_library.empty() || !fd->overload_spelling.empty()
	  || !fd->namespace_name.empty() || !fd->function_display_name.empty() )
	    return "a member function with state the builder does not set";
	if ( (v->flags & ~(uint32_t)(vfPRIVATE | vfPROTECTED | vfSTATIC)) != 0 )
	    return "a member function whose Variable carries flags the builder does not set";
	for ( size_t k = 0; k < fd->param_defaults.size(); ++k )
	    if ( fd->param_defaults[k] )
		return "a member function with a default argument";
	for ( size_t k = 0; k < fd->param_default_tokens.size(); ++k )
	    if ( !fd->param_default_tokens[k].empty() )
		return "a member function with a default argument";
	for ( size_t k = 0; k < fd->param_template_param_spelled_directly.size(); ++k )
	    if ( fd->param_template_param_spelled_directly[k] )
		return "a member function a template parameter spells";
	std::string expr, err;
	if ( !type_expr(live, &fd->returns, expr, err) )
	    return "return " + err;
	for ( size_t k = 0; k < fd->parameters.size(); ++k )
	    if ( !type_expr(live, fd->parameters[k], expr, err) )
		return "parameter " + err;
	std::string why = signature_type_reason(live, fd, seen);
	if ( !why.empty() )
	    return why;
    }
    return std::string();
}

// Why a namespace aggregate gets no builder yet: a data-only DataDefSTRUCT
// (declare_builtin_struct), the DataDefCLASS an object member promoted one to
// (no scope, member functions, bases, statics, friends or virtuals of its
// own), or a class TokenCLASS registered (declare_builtin_class).
static std::string struct_skip_reason(Program &live, const madc::dis::istring &ns,
				      const madc::dis::istring &name, DataDefSTRUCT *s,
				      TypeVisit &seen)
{
    if ( DataDefCLASS *c = s->as_class_dd() )
    {
	if ( !c->type_aliases.empty() )
	{
	    std::string why = class_skip_reason(live, c, seen);
	    return why.empty() ? aggregate_skip_reason(live, ns, name, s, seen) : why;
	}
	if ( typeid(*c) != typeid(DataDefCLASS)
	  || !c->methods.empty() || !c->ctors.empty() || !c->method_map.empty()
	  || c->has_user_ctor || c->has_user_dtor || c->base_class || !c->bases.empty()
	  || c->enclosing_class || c->has_vtable || c->has_vptr_slot || c->from_system_header
	  || !c->staticconst.empty() || !c->static_member_types.empty()
	  || !c->friend_class_names.empty() || !c->friend_function_names.empty() )
	    return std::string("a ") + typeid(*s).name() + " (R2)";
    }
    else if ( typeid(*s) != typeid(DataDefSTRUCT) )
	return std::string("a ") + typeid(*s).name() + " (R2)";
    for ( size_t i = 0; i < s->members.size(); ++i )
	if ( s->member_access[i] )
	    return "a struct member with an access level";
    return aggregate_skip_reason(live, ns, name, s, seen);
}

// Why one namespace type gets no builder yet; empty when it gets one.
static std::string type_skip_reason(Program &live, const madc::dis::istring &ns,
				    const madc::dis::istring &name, TypeVisit &seen)
{
    DataDef *dd = registered_namespace_type(live, ns, name);
    if ( !dd )
	return "not a registered type";
    if ( !seen.insert(dd).second )
	return std::string();		// being judged: its own builder's
    if ( DataDefFPTR *fp = dd->as_fptr_dd() )
    {
	madc::dis::istring ins, iname;
	if ( !typedef_type_identity(live, fp, ins, iname) || ins != ns || iname != name )
	    return "a function pointer another name holds too";
	flat_datatype_map_iter fi = live.datatype_map.find(name);
	if ( fi == live.datatype_map.end() || &(*fi)->definition != dd )
	    return "a bare alias another type holds";
	std::string expr, err;
	if ( !function_pointer_expr(live, fp, expr, err) )
	    return err;
	return signature_type_reason(live, fp->target, seen);
    }
    if ( DataDefSTRUCT *s = dd->as_struct_dd() )
	return struct_skip_reason(live, ns, name, s, seen);
    DataDefENUM *e = dd->as_enum_dd();
    if ( !e )
	return std::string("a ") + typeid(*dd).name() + " (R2)";
    if ( e->canonical_cpp_spelling() != ns + "::" + name || e->enum_name != name )
	return "an enum registered under another name";
    flat_datatype_map_iter fi = live.datatype_map.find(name);
    if ( fi == live.datatype_map.end() || &(*fi)->definition != dd )
	return "a bare tag another type holds";
    if ( e->underlying )
    {
	std::string expr, err;
	if ( !type_expr(live, e->underlying, expr, err) )
	    return "underlying " + err;
    }
    return std::string();
}

// A function's type that names a dialect type the generator does not build.
static std::string named_type_reason(Program &live, DataDef *dd, TypeVisit &seen)
{
    for ( int guard = 0; dd && guard < 8; ++guard )
    {
	if ( DataDefQUAL *q = dd->as_qualified_dd() )
	    dd = q->base_type;
	else if ( DataDefPTR *pd = dd->as_pointer_dd() )
	    dd = pd->base_type;
	else
	    break;
    }
    madc::dis::istring ns, name;
    std::string expr, err;
    // A singleton (ddARRAY is madc::value) is every program's, never a builder's.
    if ( dd && type_expr(live, dd, expr, err) && expr.compare(0, 3, "&dd") == 0 )
	return std::string();
    if ( dd && namespace_type_identity(live, dd, ns, name) )
    {
	std::string why = type_skip_reason(live, ns, name, seen);
	if ( !why.empty() )
	    return std::string("type ") + ns.c_str() + "::" + name.c_str() + ": " + why;
    }
    else if ( DataDefFPTR *fp = dd ? dd->as_fptr_dd() : NULL )
    {
	if ( fp->target )
	    return signature_type_reason(live, fp->target, seen);	// spelled in place
    }
    return std::string();
}

// Why one function gets no builder yet; empty when it gets one.
static std::string builtin_skip_reason(Program &live, const madc::dis::istring &key)
{
    FuncDef *fd = live.funcdef_map[key];
    Variable *var = live.tkProgram->findVariable(live.strpool, key);
    const Method *m = var ? (const Method *)var->data : NULL;
    if ( m && m->owner_class )
	return "a class member (R2)";
    if ( !fd->declaration_only )
	return "a body (R3 / the polyglot wrappers' move into the host)";
    madc::dis::istring tkey = fd->namespace_name + "::" + fd->function_display_name;
    if ( !fd->function_display_name.empty()
      && live.fn_template_map.find(tkey) != live.fn_template_map.end() )	/* identity-read: existence */
	return "a template";
    for ( size_t i = 0; i < fd->param_default_tokens.size(); ++i )
	if ( !fd->param_default_tokens[i].empty() )
	    return "a default argument";
    for ( size_t i = 0; i < fd->param_defaults.size(); ++i )
	if ( fd->param_defaults[i] )
	    return "a default argument";
    for ( size_t i = 0; i < fd->param_template_param_spelled_directly.size(); ++i )
	if ( fd->param_template_param_spelled_directly[i] )
	    return "a parameter a template parameter spells";
    std::string expr, err;
    if ( !type_expr(live, &fd->returns, expr, err) )
	return "return " + err;
    for ( size_t i = 0; i < fd->parameters.size(); ++i )
	if ( !type_expr(live, fd->parameters[i], expr, err) )
	    return "parameter " + err;
    TypeVisit seen;
    std::string why = signature_type_reason(live, fd, seen);
    if ( !why.empty() )
	return why;
    for ( size_t i = 0; m && i < m->parameters.size(); ++i )
	if ( i >= fd->parameters.size() || !m->parameters[i]
	  || m->parameters[i]->type != fd->parameters[i] )
	    return "a Method parameter that is not the FuncDef's";
    if ( !fd->emit_symbol.empty() && (!var || fd->emit_symbol != var->storage_alias_name) )
	return "an emit symbol that is not the storage alias";
    if ( !fd->return_typedef_name.empty() )
	return "a return typedef name";
    if ( !fd->inline_builtin_kind.empty() )
	return "a builtin kind";
    if ( !fd->overload_template_args.empty() || !fd->template_return_param_name.empty() )
	return "a template field";
    return std::string();
}

// Why one namespace object gets no builder yet: declare_builtin_extern builds
// an `extern T name;` reference — no storage, no initializer, one element,
// bound under its own name with the target's mangled alias.
static std::string variable_skip_reason(Program &live, const madc::dis::istring &ns,
					const madc::dis::istring &name)
{
    Variable *v = namespace_variable(live, ns, name);
    if ( !v || v->name != name )
	return "an object bound under another name";
    if ( !(v->flags & vfEXTERN) || v->data || v->count != 1 )
	return "an object with storage (a definition)";
    if ( v->storage_alias_name != namespace_cpp_variable_symbol(ns, name) )
	return "an object whose alias is not its namespace mangling";
    if ( !in_program_scope(live, v) )
	return "an object outside the program scope";
    std::string expr, err;
    if ( !type_expr(live, v->type, expr, err) )
	return "type " + err;
    TypeVisit seen;
    return named_type_reason(live, v->type, seen);
}

void plan_builtin_registry(DialectTextParse &dp, std::vector<BuiltinGroup> &groups)
{
    Program &live = *dp.live;
    std::map<std::pair<std::string, std::string>, std::vector<madc::dis::istring> > by_name;
    for ( size_t i = 0; i < dp.function_keys.size(); ++i )
    {
	// A member function is its class's (the class group carries its key).
	Variable *var = live.tkProgram->findVariable(live.strpool, dp.function_keys[i]);
	const Method *m = var ? (const Method *)var->data : NULL;
	if ( m && m->owner_class )
	    continue;
	const FuncDef *fd = live.funcdef_map[dp.function_keys[i]];
	std::string name = fd->function_display_name.empty()
	    ? std::string(dp.function_keys[i].c_str())
	    : std::string(fd->function_display_name.c_str());
	by_name[std::make_pair(std::string(fd->namespace_name.c_str()), name)]
	    .push_back(dp.function_keys[i]);
    }
    // One table, sorted by (ns, name): the types, the objects and the function
    // groups. A name two of them claim gets no builder.
    std::map<std::pair<std::string, std::string>, BuiltinGroup> sorted;
    auto place = [&sorted](BuiltinGroup &bg) {
	std::pair<std::string, std::string> at(bg.ns.c_str(), bg.name.c_str());
	std::map<std::pair<std::string, std::string>, BuiltinGroup>::iterator had = sorted.find(at);
	if ( had == sorted.end() )
	    sorted.insert(std::make_pair(at, bg));
	else
	    had->second.skip = "a name two kinds of declaration claim";
    };
    for ( size_t i = 0; i < dp.type_keys.size(); ++i )
    {
	BuiltinGroup bg;
	bg.ns = dp.type_keys[i].first;
	bg.name = dp.type_keys[i].second;
	bg.kind = bgType;
	TypeVisit seen;
	bg.skip = type_skip_reason(live, bg.ns, bg.name, seen);
	// A class's member functions, in registration order.
	DataDef *dd = registered_namespace_type(live, bg.ns, bg.name);
	if ( DataDefCLASS *c = dd ? dd->as_class_dd() : NULL )
	    for ( size_t k = 0; k < c->methods.size(); ++k )
		if ( c->methods[k] )
		    bg.keys.push_back(c->methods[k]->name);
	place(bg);
    }
    for ( size_t i = 0; i < dp.variable_keys.size(); ++i )
    {
	BuiltinGroup bg;
	bg.ns = dp.variable_keys[i].first;
	bg.name = dp.variable_keys[i].second;
	bg.kind = bgVariable;
	bg.skip = variable_skip_reason(live, bg.ns, bg.name);
	place(bg);
    }
    for ( std::map<std::pair<std::string, std::string>,
		   std::vector<madc::dis::istring> >::const_iterator g = by_name.begin();
	  g != by_name.end(); ++g )
    {
	BuiltinGroup bg;
	bg.kind = bgFunctions;
	bg.ns = madc::dis::istring(g->first.first.c_str());
	bg.name = madc::dis::istring(g->first.second.c_str());
	// Registration order: the namespace overload set's, as declared.
	std::set<madc::dis::istring> pending(g->second.begin(), g->second.end());
	madc::dis::istring ovkey = bg.ns + "::" + bg.name;
	if ( !bg.ns.empty()
	  && live.namespace_fn_overload_sets.find(ovkey) != live.namespace_fn_overload_sets.end() )
	{
	    const std::vector<Program::NamespaceFnOverload> &ovset =
		live.namespace_fn_overload_sets[ovkey];
	    for ( size_t i = 0; i < ovset.size(); ++i )
		if ( ovset[i].var && pending.erase(ovset[i].var->name) )
		    bg.keys.push_back(ovset[i].var->name);
	}
	for ( size_t i = 0; i < g->second.size(); ++i )
	    if ( pending.count(g->second[i]) )
		bg.keys.push_back(g->second[i]);
	for ( size_t i = 0; i < bg.keys.size() && bg.skip.empty(); ++i )
	    bg.skip = builtin_skip_reason(live, bg.keys[i]);
	place(bg);
    }
    groups.clear();
    for ( std::map<std::pair<std::string, std::string>, BuiltinGroup>::const_iterator g =
	      sorted.begin(); g != sorted.end(); ++g )
	groups.push_back(g->second);
    // The closing pass: a built group whose generated code names a dialect
    // type that got no builder (a judgement answered "its own builder's"
    // while that type was later refused) is refused too, until none is — a
    // builder never meets a NULL type.
    for ( bool changed = true; changed; )
    {
	changed = false;
	std::vector<std::string> unbuilt;
	for ( size_t g = 0; g < groups.size(); ++g )
	    if ( groups[g].kind == bgType && !groups[g].skip.empty() )
		unbuilt.push_back(std::string("builtin_type(pgm, ") + cpp_literal(groups[g].ns.c_str())
				  + ", " + cpp_literal(groups[g].name.c_str()) + ")");
	for ( size_t g = 0; g < groups.size(); ++g )
	{
	    BuiltinGroup &bg = groups[g];
	    if ( !bg.skip.empty() )
		continue;
	    std::string text = bg.kind == bgType ? builtin_type_line(live, bg.ns, bg.name).c_str()
			     : bg.kind == bgVariable ? builtin_variable_line(live, bg.ns, bg.name).c_str()
			     : "";
	    for ( size_t k = 0; k < bg.keys.size(); ++k )
		text += builtin_function_line(live, bg.keys[k]).c_str();
	    for ( size_t u = 0; u < unbuilt.size(); ++u )
		if ( text.find(unbuilt[u]) != std::string::npos )
		{
		    bg.skip = "names a type that got no builder: " + unbuilt[u];
		    changed = true;
		    break;
		}
	}
    }
}


// A function's BuiltinFnFlag bits as generated code: ` | bfX | bfY` (appended
// to `0u`).
static std::string function_flags_expr(FuncDef *fd)
{
    std::string flags;
    if ( fd->is_varargs ) flags += " | bfVarargs";
    if ( fd->is_void_params ) flags += " | bfVoidParams";
    if ( fd->c_linkage ) flags += " | bfCLinkage";
    if ( fd->noexcept_spec == FuncDef::NxTrue ) flags += " | bfNoexcept";
    if ( fd->noexcept_spec == FuncDef::NxUnknown ) flags += " | bfNoexceptUnknown";
    if ( fd->vague_linkage ) flags += " | bfVagueLinkage";
    if ( fd->internal_linkage ) flags += " | bfInternalLinkage";
    if ( fd->weak_binding ) flags += " | bfWeakBinding";
    return flags;
}

// A signature's parameters as generated code: `const BuiltinParam p[]` (none
// written for an empty list).
static void emit_params(std::ostream &out, Program &live, FuncDef *fd)
{
    if ( fd->parameters.empty() )
	return;
    std::string expr, err;
    out << "\tconst BuiltinParam p[] = {\n";
    for ( size_t i = 0; i < fd->parameters.size(); ++i )
    {
	type_expr(live, fd->parameters[i], expr, err);
	out << "\t    { " << expr << ", "
	    << cpp_literal(i < fd->param_cpp_spellings.size()
			   ? fd->param_cpp_spellings[i].c_str() : "") << ", "
	    << (( i < fd->const_params.size() && fd->const_params[i] ) ? "true" : "false") << ", "
	    << ( i < fd->param_typedef_names.size()
		 ? cpp_literal(fd->param_typedef_names[i].c_str()) : std::string("NULL") )
	    << " },\n";
    }
    out << "\t};\n";
}

// One declaration's registration, as generated code.
static void emit_declaration(std::ostream &out, Program &live, const madc::dis::istring &key)
{
    FuncDef *fd = live.funcdef_map[key];
    Variable *var = live.tkProgram->findVariable(live.strpool, key);
    const Method *m = var ? (const Method *)var->data : NULL;
    std::string expr, err;
    out << "    {\n";
    emit_params(out, live, fd);
    size_t nnames = m ? m->parameters.size() : 0;
    if ( nnames )
    {
	out << "\tstatic const char *const n[] = {";
	for ( size_t i = 0; i < nnames; ++i )
	    out << ( i ? ", " : " " ) << cpp_literal(m->parameters[i]->name.c_str());
	out << " };\n";
    }
    std::string flags = function_flags_expr(fd);
    if ( !fd->emit_symbol.empty() ) flags += " | bfEmitIsAlias";
    type_expr(live, &fd->returns, expr, err);
    out << "\tBuiltinFunctionDecl d = { " << cpp_literal(key.c_str()) << ", "
	<< cpp_literal(fd->namespace_name.c_str()) << ", "
	<< cpp_literal(fd->function_display_name.c_str()) << ", " << expr << ", "
	<< ( fd->parameters.empty() ? "NULL" : "p" ) << ", " << fd->parameters.size() << ", "
	<< ( nnames ? "n" : "NULL" ) << ", " << nnames << ", "
	<< cpp_literal(fd->overload_spelling.c_str()) << ", "
	<< ( flags.empty() ? "0u" : "0u" + flags ) << " };\n"
	<< "\tdeclare_builtin_function(pgm, d);\n"
	<< "    }\n";
}

// One enum's registration, as generated code.
static void emit_enum(std::ostream &out, Program &live, const madc::dis::istring &ns,
		      const madc::dis::istring &name)
{
    DataDefENUM *e = registered_namespace_type(live, ns, name)->as_enum_dd();
    bool unscoped = enum_is_unscoped(live, ns, e);
    if ( !e->enumerators.empty() )
    {
	out << "    static const BuiltinEnumerator e[] = {\n";
	for ( size_t i = 0; i < e->enumerators.size(); ++i )
	{
	    Variable *v = unscoped ? namespace_variable(live, ns, e->enumerators[i].first) : NULL;
	    out << "\t{ " << cpp_literal(e->enumerators[i].first.c_str()) << ", "
		<< (long long)e->enumerators[i].second << "LL, "
		<< cpp_literal(v ? v->storage_alias_name.c_str() : "") << " },\n";
	}
	out << "    };\n";
    }
    std::string under = "NULL", err;
    if ( e->underlying )
	type_expr(live, e->underlying, under, err);
    out << "    BuiltinEnumDecl d = { " << cpp_literal(ns.c_str()) << ", "
	<< cpp_literal(name.c_str()) << ", " << (unscoped ? "false" : "true") << ", "
	<< (e->fixed_base ? "true" : "false") << ", " << under << ", " << e->size << "u, "
	<< (unsigned)e->type() << "u, " << (e->enumerators.empty() ? "NULL" : "e") << ", "
	<< e->enumerators.size() << " };\n"
	<< "    declare_builtin_enum(pgm, d);\n";
}

// One function-pointer typedef's registration, as generated code: the
// signature spelled out (never the alias's own builtin_type — it is what this
// builds).
static void emit_fptr_typedef(std::ostream &out, Program &live, const madc::dis::istring &ns,
			      const madc::dis::istring &name)
{
    std::string expr, err;
    function_pointer_expr(live, registered_namespace_type(live, ns, name)->as_fptr_dd(),
			  expr, err);
    out << "    declare_builtin_typedef(pgm, " << cpp_literal(ns.c_str()) << ", "
	<< cpp_literal(name.c_str()) << ",\n\t" << expr << ");\n";
}

// One data member as generated code: `{ "name", TYPE, "alias", ACCESS }`.
static std::string member_expr(Program &live, DataDefSTRUCT *s, size_t i)
{
    std::string expr, err;
    type_expr(live, s->members[i].second, expr, err);
    return std::string("{ ") + cpp_literal(s->members[i].first.c_str()) + ", " + expr + ", "
	 + cpp_literal(s->members[i].typedef_name.c_str()) + ", "
	 + std::to_string(s->member_access[i]) + "u }";
}

// One data-only struct's registration, as generated code. The members' types
// are expressions over this builder's Program: an automatic array, never a
// static one.
static void emit_struct(std::ostream &out, Program &live, const madc::dis::istring &ns,
			const madc::dis::istring &name)
{
    DataDefSTRUCT *s = registered_namespace_type(live, ns, name)->as_struct_dd();
    if ( !s->members.empty() )
    {
	out << "    const BuiltinMember m[] = {\n";
	for ( size_t i = 0; i < s->members.size(); ++i )
	    out << "\t" << member_expr(live, s, i) << ",\n";
	out << "    };\n";
    }
    out << "    BuiltinStructDecl d = { " << cpp_literal(ns.c_str()) << ", "
	<< cpp_literal(name.c_str()) << ", " << (s->members.empty() ? "NULL" : "m") << ", "
	<< s->members.size() << ", " << (s->as_class_dd() ? "true" : "false") << " };\n"
	<< "    declare_builtin_struct(pgm, d);\n";
}

// One class's registration, as generated code: its shell (a builder that
// finds it registered stops), its data members, its member functions in
// registration order (the registrar's kind read from the class: a
// constructor is in its ctors, a destructor's display name is `~`), then its
// completion.
static void emit_class(std::ostream &out, Program &live, const madc::dis::istring &ns,
		       const madc::dis::istring &name)
{
    DataDefCLASS *c = registered_namespace_type(live, ns, name)->as_class_dd();
    out << "    DataDefCLASS *c = declare_builtin_class(pgm, " << cpp_literal(ns.c_str()) << ", "
	<< cpp_literal(name.c_str()) << ", " << (c->from_system_header ? "true" : "false") << ");\n"
	<< "    if ( !c )\n\treturn;\n";
    for ( size_t i = 0; i < c->members.size(); ++i )
	out << "    {\n\tconst BuiltinMember mb = " << member_expr(live, c, i) << ";\n"
	    << "\tdeclare_builtin_class_member(c, mb);\n    }\n";
    std::string ret, err;
    for ( size_t i = 0; i < c->methods.size(); ++i )
    {
	Variable *v = c->methods[i];
	FuncDef *fd = v->type->as_funcdef_dd();
	bool ctor = std::find(c->ctors.begin(), c->ctors.end(), v) != c->ctors.end();
	const char *kind = ctor ? "Program::ClassMethodKind::Constructor"
	    : fd->method_display_name.c_str()[0] == '~' ? "Program::ClassMethodKind::Destructor"
	    : "Program::ClassMethodKind::Method";
	std::string mflags;
	if ( fd->is_const_method ) mflags += " | bmConst";
	if ( fd->is_explicit ) mflags += " | bmExplicit";
	if ( fd->implicit_dtor_noexcept ) mflags += " | bmImplicitDtorNx";
	if ( std::string(fd->method_display_name.c_str()).compare(0, 8, "operator") == 0 )
	    mflags += " | bmOperator";
	if ( v->flags & vfSTATIC ) mflags += " | bmStatic";
	type_expr(live, &fd->returns, ret, err);
	out << "    {\n";
	emit_params(out, live, fd);
	out << "\tBuiltinMethodDecl d = { " << cpp_literal(v->name.c_str()) << ", "
	    << cpp_literal(fd->method_display_name.c_str()) << ", " << kind << ", " << ret << ", "
	    << ( fd->parameters.empty() ? "NULL" : "p" ) << ", " << fd->parameters.size() << ", "
	    << "0u" << function_flags_expr(fd) << ", 0u" << mflags << ", "
	    << (v->flags & (vfPRIVATE | vfPROTECTED)) << "u, "
	    << cpp_literal(fd->local_emit_name.c_str()) << " };\n"
	    << "\tdeclare_builtin_method(pgm, c, d);\n    }\n";
    }
    out << "    complete_builtin_class(pgm, c);\n";
}

// One namespace object's registration, as generated code.
static void emit_extern(std::ostream &out, Program &live, const madc::dis::istring &ns,
			const madc::dis::istring &name)
{
    Variable *v = namespace_variable(live, ns, name);
    std::string expr, err;
    type_expr(live, v->type, expr, err);
    out << "    BuiltinExternDecl d = { " << cpp_literal(ns.c_str()) << ", "
	<< cpp_literal(name.c_str()) << ", " << expr << ", " << v->flags << "u };\n"
	<< "    declare_builtin_extern(pgm, d);\n";
}

// The generated file's head: what every builder needs in scope.
static void emit_builder_prologue(std::ostream &out)
{
    out << "#include <cstdio>\n#include <cstring>\n#include <string>\n#include <vector>\n"
	<< "#include <map>\n#include <set>\n#include <stack>\n#include <list>\n"
	<< "#include <queue>\n#include <iostream>\n#include <sstream>\n#include <memory>\n"
	<< "#include <initializer_list>\n#include <stdint.h>\n\n"
	<< "#define DBG(x) do { if(madc_verbose){x;} } while(0)\n\n"
	<< "#include \"datadef.h\"\n#include \"tokens.h\"\n#include \"datatokens.h\"\n"
	<< "#include \"madc.h\"\n#include \"madc_builtin_registry.h\"\n\n";
}

// One builder per group the plan built (a refused group is written as a
// comment with its reason), and each built group's table row into `table`.
// The count of builders written.
static size_t emit_builders(std::ostream &out, Program &live,
			    const std::vector<BuiltinGroup> &groups, std::ostream &table)
{
    size_t built = 0;
    for ( size_t g = 0; g < groups.size(); ++g )
    {
	const BuiltinGroup &bg = groups[g];
	if ( !bg.skip.empty() )
	{
	    out << "// not yet built: " << bg.ns << "::" << bg.name << " — " << bg.skip << "\n";
	    if ( bg.kind == bgType )
		out << "//   " << builtin_type_line(live, bg.ns, bg.name).c_str() << "\n";
	    else if ( bg.kind == bgVariable )
		out << "//   " << builtin_variable_line(live, bg.ns, bg.name).c_str() << "\n";
	    for ( size_t k = 0; k < bg.keys.size(); ++k )
		out << "//   " << builtin_function_line(live, bg.keys[k]).c_str() << "\n";
	    continue;
	}
	out << "static void b" << built << "(Program &pgm)\n{\n";
	DataDef *tdd = bg.kind == bgType ? registered_namespace_type(live, bg.ns, bg.name) : NULL;
	if ( bg.kind == bgVariable )
	    emit_extern(out, live, bg.ns, bg.name);
	else if ( tdd && tdd->as_fptr_dd() )
	    emit_fptr_typedef(out, live, bg.ns, bg.name);
	else if ( tdd && tdd->as_class_dd() && !tdd->as_class_dd()->type_aliases.empty() )
	    emit_class(out, live, bg.ns, bg.name);
	else if ( tdd && tdd->as_struct_dd() )
	    emit_struct(out, live, bg.ns, bg.name);
	else if ( tdd )
	    emit_enum(out, live, bg.ns, bg.name);
	for ( size_t k = 0; bg.kind == bgFunctions && k < bg.keys.size(); ++k )
	    emit_declaration(out, live, bg.keys[k]);
	out << "}\n\n";
	table << "    { " << cpp_literal(bg.ns.c_str()) << ", "
	      << cpp_literal(bg.name.c_str()) << ", b" << built << " },\n";
	++built;
    }
    return built;
}

bool emit_builtin_registry(std::ostream &out, std::ostream &diag)
{
    DialectTextParse dp;
    std::string err;
    if ( !parse_dialect_as_text(dp, err) )
    {
	diag << "madc: --emit-builtin-registry: " << err << std::endl;
	return false;
    }
    std::vector<BuiltinGroup> groups;
    plan_builtin_registry(dp, groups);
    out << "// GENERATED by `madc --emit-builtin-registry` from include/madc/ns_* and\n"
	<< "// bits/* — never edit; see include/madc_builtin_registry.h.\n\n";
    emit_builder_prologue(out);
    std::ostringstream table;
    size_t built = emit_builders(out, *dp.live, groups, table);
    out << "\nnamespace madc { namespace builtins {\n"
	<< "const MadcBuiltinEntry dialect_table[] = {\n" << table.str()
	<< ( built ? "" : "    { \"\", \"\", NULL },\n" )
	<< "};\nconst size_t dialect_table_count = " << built << ";\n} }\n";
    return true;
}

bool emit_module_table(std::ostream &out, std::ostream &diag, const std::string &face)
{
    DialectTextParse dp;
    std::string err;
    if ( !parse_module_as_text(dp, face, err) )
    {
	diag << "madc: --emit-module-table: " << err << std::endl;
	return false;
    }
    std::vector<BuiltinGroup> groups;
    plan_builtin_registry(dp, groups);
    // A module's declarations are all built, or the module is not: a refused
    // group would be a name the import never registers.
    for ( size_t g = 0; g < groups.size(); ++g )
	if ( !groups[g].skip.empty() )
	{
	    diag << "madc: --emit-module-table: " << face << ": " << groups[g].ns << "::"
		 << groups[g].name << " cannot be built — " << groups[g].skip << std::endl;
	    return false;
	}
    out << "// GENERATED by `madc --emit-module-table` from " << face << " — never\n"
	<< "// edit. The module's source includes it and returns the table in its\n"
	<< "// description (include/madc_module.h).\n\n";
    emit_builder_prologue(out);
    std::ostringstream table;
    size_t built = emit_builders(out, *dp.live, groups, table);
    out << "\nstatic const MadcBuiltinEntry madc_module_declarations[] = {\n" << table.str()
	<< ( built ? "" : "    { \"\", \"\", NULL },\n" )
	<< "};\nstatic const size_t madc_module_ndeclarations = " << built << ";\n";
    return true;
}
