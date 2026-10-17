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
#include <stdint.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "madc_builtin_registry.h"
#include "madc_modules.h"

Variable *declare_builtin_function(Program &pgm, const BuiltinFunctionDecl &d)
{
    FuncDef *fd = new FuncDef(*d.returns);
    for ( size_t i = 0; i < d.nparams; ++i )
    {
	fd->parameters.push_back(d.params[i].type);
	fd->const_params.push_back(d.params[i].is_const);
	fd->param_cpp_spellings.push_back(madc::dis::istring(d.params[i].cpp_spelling));
    }
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
    if ( d.module && *d.module )
	pgm.stamp_lazy_module_member(fd,
	    madc::dis::istring(madc_module_library_spelling(d.module).c_str()),
	    madc::dis::istring(d.key));
    Program::PendingForestFunc pf;
    pf.name = madc::dis::istring(d.key);
    pf.fd = fd;
    pf.mvar = NULL;
    for ( size_t i = 0; i < d.nparam_names && i < d.nparams; ++i )
	pf.mparams.push_back(std::make_pair(d.param_names[i], d.params[i].type));
    Variable *var = pgm.register_forest_func(pf);
    if ( var && (d.flags & bfFileScopeCppAlias) )
	var->storage_alias_name = pgm.declaration_cpp_symbol(madc::dis::istring(),
							     fd->function_display_name, fd);
    if ( var && (d.flags & bfEmitIsAlias) )
	fd->emit_symbol = var->storage_alias_name;
    return var;
}

// A type as the C++ expression that yields it in a builder's Program `pgm`:
// a singleton by identity, a layered type through its one minting owner
// (getQualifiedType / getReferenceType / getPointerType) over its base. The
// differential reads types through it too, so "same type" means "built the
// same way". False with `err` naming a type the generator does not yet express.
static bool type_expr(DataDef *dd, std::string &expr, std::string &err)
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
	if ( !type_expr(q->base_type, base, err) )
	    return false;
	expr = "pgm.getQualifiedType(" + base + ", " + std::to_string(q->quals) + "u)";
	return true;
    }
    if ( DataDefREF *r = dd->as_reference_dd() )
    {
	if ( !type_expr(r->base_type, base, err) )
	    return false;
	expr = "pgm.getReferenceType(" + base + (r->rvalue ? ", true)" : ", false)");
	return true;
    }
    if ( DataDefPTR *p = dd->as_pointer_dd() )
    {
	if ( !type_expr(p->base_type, base, err) )
	    return false;
	expr = "pgm.getPointerType(" + base + ")";
	return true;
    }
    err = std::string("type '") + dd->name.c_str() + "' (" + typeid(*dd).name() + ")";
    return false;
}

static std::string type_field(DataDef *dd)
{
    std::string expr, err;
    return type_expr(dd, expr, err) ? expr : "?" + err;
}

// The fields a free function's registration carries — the record the forest
// restore proves sufficient (cir_freeze.cpp's DK_FUNC restore + restore_param)
// plus the registration site's own state (register_forest_func: the Variable's
// storage_alias_name, its Method's named parameters, its place in the
// namespace overload set). A default by its token run's spelling.
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
    line += "\tret=" + type_field(&fd->returns);
    line += "\trtd=" + std::string(fd->return_typedef_name.c_str());
    line += "\tparams=";
    for ( size_t i = 0; i < fd->parameters.size(); ++i )
    {
	if ( i )
	    line += ";";
	line += type_field(fd->parameters[i]);
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
    line += "\tmparams=";
    for ( size_t i = 0; m && i < m->parameters.size(); ++i )
    {
	if ( i )
	    line += ";";
	if ( m->parameters[i] )
	    line += std::string(m->parameters[i]->name.c_str()) + ":"
		  + type_field(m->parameters[i]->type);
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

bool parse_dialect_as_text(DialectTextParse &out, std::string &err)
{
    const std::string empty = "int main() { return 0; }\n";
    out.base = parse_text_program(empty, "<dialect-base>", err);
    if ( !out.base )
	return false;
    std::vector<madc::dis::istring> roots;
    if ( !out.base->dialect_image_roots(roots) )
    {
	err = "the dialect's roots exist only under --std=madc";
	return false;
    }
    std::string src;
    for ( size_t i = 0; i < roots.size(); ++i )
	src += std::string("#include <") + roots[i].c_str() + ">\n";
    src += empty;
    out.live = parse_text_program(src, "<dialect-live>", err);
    if ( !out.live )
	return false;
    out.function_keys.clear();
    for ( funcdef_map_iter it = out.live->funcdef_map.begin();
	  it != out.live->funcdef_map.end(); ++it )
	if ( out.base->funcdef_map.find(it->first) == out.base->funcdef_map.end()
	  && it->second && !it->second->builtin_registration )
	    out.function_keys.push_back(it->first);
    return true;
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
    std::string expr, err;
    if ( !type_expr(&fd->returns, expr, err) )
	return "return " + err;
    for ( size_t i = 0; i < fd->parameters.size(); ++i )
	if ( !type_expr(fd->parameters[i], expr, err) )
	    return "parameter " + err;
    for ( size_t i = 0; m && i < m->parameters.size(); ++i )
	if ( i >= fd->parameters.size() || !m->parameters[i]
	  || m->parameters[i]->type != fd->parameters[i] )
	    return "a Method parameter that is not the FuncDef's";
    if ( !fd->emit_symbol.empty() && (!var || fd->emit_symbol != var->storage_alias_name) )
	return "an emit symbol that is not the storage alias";
    if ( fd->namespace_name.empty() && var && !var->storage_alias_name.empty()
      && (fd->c_linkage || var->storage_alias_name
	  != live.declaration_cpp_symbol(madc::dis::istring(), fd->function_display_name, fd)) )
	return "a file-scope alias that is not its C++ symbol";
    if ( !fd->return_typedef_name.empty() )
	return "a return typedef name";
    for ( size_t i = 0; i < fd->param_typedef_names.size(); ++i )
	if ( !fd->param_typedef_names[i].empty() )
	    return "a parameter typedef name";
    if ( !fd->dyn_module_library.empty() )
    {
	const MadcModuleSpec *ms =
	    madc_module_find_spelled(std::string(fd->dyn_module_library.c_str()));
	if ( !ms || fd->dyn_module_member != key )
	    return "a module member that is not its row's";
    }
    if ( !fd->inline_builtin_kind.empty() )
	return "a builtin kind";
    if ( !fd->overload_template_args.empty() || !fd->template_return_param_name.empty() )
	return "a template field";
    return std::string();
}

void plan_builtin_registry(DialectTextParse &dp, std::vector<BuiltinGroup> &groups)
{
    Program &live = *dp.live;
    std::map<std::pair<std::string, std::string>, std::vector<madc::dis::istring> > by_name;
    for ( size_t i = 0; i < dp.function_keys.size(); ++i )
    {
	const FuncDef *fd = live.funcdef_map[dp.function_keys[i]];
	std::string name = fd->function_display_name.empty()
	    ? std::string(dp.function_keys[i].c_str())
	    : std::string(fd->function_display_name.c_str());
	by_name[std::make_pair(std::string(fd->namespace_name.c_str()), name)]
	    .push_back(dp.function_keys[i]);
    }
    groups.clear();
    for ( std::map<std::pair<std::string, std::string>,
		   std::vector<madc::dis::istring> >::const_iterator g = by_name.begin();
	  g != by_name.end(); ++g )
    {
	BuiltinGroup bg;
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
	groups.push_back(bg);
    }
}

// The C++ string literal of `s` (the one C-string escape rule).
static std::string cpp_literal(const char *s)
{
    return std::string("\"") + madc_c_escape_string(s, strlen(s)).c_str() + "\"";
}

// One declaration's registration, as generated code.
static void emit_declaration(std::ostream &out, Program &live, const madc::dis::istring &key)
{
    FuncDef *fd = live.funcdef_map[key];
    Variable *var = live.tkProgram->findVariable(live.strpool, key);
    const Method *m = var ? (const Method *)var->data : NULL;
    std::string expr, err;
    out << "    {\n";
    if ( !fd->parameters.empty() )
    {
	out << "\tconst BuiltinParam p[] = {\n";
	for ( size_t i = 0; i < fd->parameters.size(); ++i )
	{
	    type_expr(fd->parameters[i], expr, err);
	    out << "\t    { " << expr << ", "
		<< cpp_literal(i < fd->param_cpp_spellings.size()
			       ? fd->param_cpp_spellings[i].c_str() : "") << ", "
		<< (( i < fd->const_params.size() && fd->const_params[i] ) ? "true" : "false")
		<< " },\n";
	}
	out << "\t};\n";
    }
    size_t nnames = m ? m->parameters.size() : 0;
    if ( nnames )
    {
	out << "\tstatic const char *const n[] = {";
	for ( size_t i = 0; i < nnames; ++i )
	    out << ( i ? ", " : " " ) << cpp_literal(m->parameters[i]->name.c_str());
	out << " };\n";
    }
    std::string flags;
    if ( fd->is_varargs ) flags += " | bfVarargs";
    if ( fd->is_void_params ) flags += " | bfVoidParams";
    if ( fd->c_linkage ) flags += " | bfCLinkage";
    if ( fd->noexcept_spec == FuncDef::NxTrue ) flags += " | bfNoexcept";
    if ( fd->noexcept_spec == FuncDef::NxUnknown ) flags += " | bfNoexceptUnknown";
    if ( fd->vague_linkage ) flags += " | bfVagueLinkage";
    if ( fd->internal_linkage ) flags += " | bfInternalLinkage";
    if ( fd->weak_binding ) flags += " | bfWeakBinding";
    if ( !fd->emit_symbol.empty() ) flags += " | bfEmitIsAlias";
    if ( fd->namespace_name.empty() && var && !var->storage_alias_name.empty() )
	flags += " | bfFileScopeCppAlias";
    const MadcModuleSpec *module = fd->dyn_module_library.empty() ? NULL
	: madc_module_find_spelled(std::string(fd->dyn_module_library.c_str()));
    type_expr(&fd->returns, expr, err);
    out << "\tBuiltinFunctionDecl d = { " << cpp_literal(key.c_str()) << ", "
	<< cpp_literal(fd->namespace_name.c_str()) << ", "
	<< cpp_literal(fd->function_display_name.c_str()) << ", " << expr << ", "
	<< ( fd->parameters.empty() ? "NULL" : "p" ) << ", " << fd->parameters.size() << ", "
	<< ( nnames ? "n" : "NULL" ) << ", " << nnames << ", "
	<< cpp_literal(fd->overload_spelling.c_str()) << ", "
	<< ( flags.empty() ? "0u" : "0u" + flags ) << ", "
	<< cpp_literal(module ? module->name : "") << " };\n"
	<< "\tdeclare_builtin_function(pgm, d);\n"
	<< "    }\n";
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
    Program &live = *dp.live;
    std::vector<BuiltinGroup> groups;
    plan_builtin_registry(dp, groups);

    out << "// GENERATED by `madc --emit-builtin-registry` from include/madc/ns_* and\n"
	<< "// bits/* — never edit; see include/madc_builtin_registry.h.\n\n"
	<< "#include <cstdio>\n#include <cstring>\n#include <string>\n#include <vector>\n"
	<< "#include <map>\n#include <set>\n#include <stack>\n#include <list>\n"
	<< "#include <queue>\n#include <iostream>\n#include <sstream>\n#include <memory>\n"
	<< "#include <stdint.h>\n\n"
	<< "#define DBG(x) do { if(madc_verbose){x;} } while(0)\n\n"
	<< "#include \"datadef.h\"\n#include \"tokens.h\"\n#include \"datatokens.h\"\n"
	<< "#include \"madc.h\"\n#include \"madc_builtin_registry.h\"\n\n";

    std::ostringstream table;
    size_t built = 0;
    for ( size_t g = 0; g < groups.size(); ++g )
    {
	const BuiltinGroup &bg = groups[g];
	if ( !bg.skip.empty() )
	{
	    out << "// not yet built: " << bg.ns << "::" << bg.name << " — " << bg.skip << "\n";
	    for ( size_t k = 0; k < bg.keys.size(); ++k )
		out << "//   " << builtin_function_line(live, bg.keys[k]).c_str() << "\n";
	    continue;
	}
	out << "static void b" << built << "(Program &pgm)\n{\n";
	for ( size_t k = 0; k < bg.keys.size(); ++k )
	    emit_declaration(out, live, bg.keys[k]);
	out << "}\n\n";
	table << "    { " << cpp_literal(bg.ns.c_str()) << ", "
	      << cpp_literal(bg.name.c_str()) << ", b" << built << " },\n";
	++built;
    }
    out << "\nnamespace madc { namespace builtins {\n"
	<< "const MadcBuiltinEntry dialect_table[] = {\n" << table.str()
	<< ( built ? "" : "    { \"\", \"\", NULL },\n" )
	<< "};\nconst size_t dialect_table_count = " << built << ";\n} }\n";
    return true;
}
