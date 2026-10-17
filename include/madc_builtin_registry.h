#ifndef __MADC_BUILTIN_REGISTRY_H
#define __MADC_BUILTIN_REGISTRY_H 1

// The dialect is builtin (docs/plans/2026-10-17-dialect-builtin-registration.md):
// the madc dialect's namespaces (madc:: php:: perl:: python:: ruby:: js:: rust::
// ui:: git:: markdown::) and bits/* are registered by generated initialization
// code, never parsed per compile and never unfrozen. The build reads the
// include/madc/ns_* and bits/* files once (`madc --emit-builtin-registry=`) and
// writes one builder per name plus the sorted table below into the object tree
// (obj/<mode>/dialect_registry.cpp); a --std=madc lookup that misses the
// program's own scopes runs that name's builder.
//
// Thread contract: the table and the builders are constant code and data; a
// builder writes only the Program it is handed (registration is per Program and
// single-threaded, as every registration).

// Includers bring datadef.h / tokens.h / datatokens.h / madc.h first (the
// project's include order).

// One (namespace, name)'s builder: registers every declaration of that name the
// dialect files make (an overload set registers whole, in declaration order).
typedef void (*madc_builtin_builder_fn)(Program &pgm);

struct MadcBuiltinEntry
{
    const char *ns;		// "" = global scope (a fragment's extern "C" entries)
    const char *name;
    madc_builtin_builder_fn build;
};

// The generated table, sorted by (ns, name) in byte order; empty in stage one
// of the build (src/madc_builtin_registry_stage1.cpp). C++ names, never an
// extern "C" export (docs/adr/0003-c-abi-stance.md).
namespace madc { namespace builtins {
extern const MadcBuiltinEntry dialect_table[];
extern const size_t dialect_table_count;
} }

// A builtin function declaration as generated code spells it: what a live
// parse of the declaration registers (FuncDef + the registrar's Variable /
// Method), field for field.
enum BuiltinFnFlag : unsigned
{
    bfVarargs		= 1u << 0,	// FuncDef::is_varargs
    bfVoidParams	= 1u << 1,	// FuncDef::is_void_params — `(void)`
    bfCLinkage		= 1u << 2,	// FuncDef::c_linkage — extern "C"
    bfNoexcept		= 1u << 3,	// FuncDef::NxTrue
    bfNoexceptUnknown	= 1u << 4,	// FuncDef::NxUnknown
    bfVagueLinkage	= 1u << 5,	// FuncDef::vague_linkage
    bfInternalLinkage	= 1u << 6,	// FuncDef::internal_linkage
    bfWeakBinding	= 1u << 7,	// FuncDef::weak_binding
    bfEmitIsAlias	= 1u << 8,	// emit_symbol = the registrar's storage alias
					// (the target's mangling, never a host string)
};

struct BuiltinParam
{
    DataDef *type;
    const char *cpp_spelling;	// FuncDef::param_cpp_spellings
    bool is_const;		// FuncDef::const_params
};

struct BuiltinFunctionDecl
{
    const char *key;		// the funcdef_map key (the program-scope name)
    const char *ns;		// FuncDef::namespace_name
    const char *name;		// FuncDef::function_display_name
    DataDef *returns;
    const BuiltinParam *params;
    size_t nparams;
    const char *const *param_names;	// the Method's parameters, by name
    size_t nparam_names;
    const char *overload_spelling;
    unsigned flags;		// BuiltinFnFlag bits
};

// The one owner of a builtin function declaration: builds the FuncDef and
// registers it through the program's restored-function registrar
// (Program::register_forest_func — namespace member, overload set, storage
// alias), as a live parse's declaration registers.
Variable *declare_builtin_function(Program &pgm, const BuiltinFunctionDecl &d);

// A registered free function's identity, field by field, tab-separated: what
// the generated registration must reproduce of a live parse (R1's
// differential). `key` is its funcdef_map key. Empty when no function is
// registered under the key.
madc::dis::istring builtin_function_line(Program &pgm, const madc::dis::istring &key);

// The dialect as a live parse of its files declares it — the source of truth
// the generator reads and the differential compares against. `live` parses
// every dialect file as text (Program::dialect_image_roots, the auto-include's
// order; no pack bind), `base` the same empty program without them;
// `function_keys` are the funcdef_map keys `live` registered beyond `base`,
// less the builtin registry's own (a libc builtin a dialect body called) and
// an import's (a module row's interface member — the import serves it, never
// the builtin table).
struct DialectTextParse
{
    std::shared_ptr<Program> live;
    std::shared_ptr<Program> base;
    std::vector<madc::dis::istring> function_keys;
};
bool parse_dialect_as_text(DialectTextParse &out, std::string &err);

// The generator's plan: the dialect's functions grouped by (namespace, name),
// sorted as the table is, each group's keys in registration order. `skip` names
// why a group gets no builder yet (empty: it gets one) — a body, a template, a
// type the generator does not yet express.
struct BuiltinGroup
{
    madc::dis::istring ns;
    madc::dis::istring name;
    std::vector<madc::dis::istring> keys;
    std::string skip;
};
void plan_builtin_registry(DialectTextParse &dp, std::vector<BuiltinGroup> &groups);

// --emit-builtin-registry=<out.cpp>: the dialect's generated registration —
// one builder per planned group and the sorted table — written to `out`; a
// failure is reported on `diag`.
bool emit_builtin_registry(std::ostream &out, std::ostream &diag);

#endif
