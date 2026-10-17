#ifndef __MADC_BUILTIN_REGISTRY_H
#define __MADC_BUILTIN_REGISTRY_H 1

// The dialect is builtin (docs/plans/2026-10-17-dialect-builtin-registration.md):
// the madc dialect's namespaces (madc:: php:: perl:: python:: ruby:: js:: rust::
// ui:: git::) and bits/* are registered by generated initialization
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
    const char *typedef_name;	// FuncDef::param_typedef_names (a function type's)
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

// A builtin enumeration as generated code spells it: what TokenENUM::parse
// registers for a tagged namespace-scope C++ enum (parser.cpp) — the
// DataDefENUM (its base and storage as the close left them), one
// TokenDataType under the bare tag and under the namespace, every enumerator
// a constant in the tag's pseudo-namespace `ns::Tag`, and an unscoped enum's
// ALSO bound in the namespace and the program scope.
struct BuiltinEnumerator
{
    const char *name;
    int64_t value;
    const char *alias;		// an unscoped enumerator's storage alias ("" for scoped)
};

struct BuiltinEnumDecl
{
    const char *ns;
    const char *name;
    bool scoped;		// `enum class`: enumerators only under ns::Tag
    bool fixed_base;		// DataDefENUM::fixed_base
    DataDef *underlying;	// DataDefENUM::underlying
    size_t size;		// the storage (DataDefENUM::restore_layout)
    uint32_t rawtype;
    const BuiltinEnumerator *enumerators;
    size_t nenumerators;
};

DataDefENUM *declare_builtin_enum(Program &pgm, const BuiltinEnumDecl &d);

// A pointer to a builtin function TYPE as generated code spells it — one
// expression: the function type parseFnPtrParams builds for a declarator's
// `(params)` (every parameter's type, C++ spelling, referent const and typedef
// name; `(void)` and `...` as the bfVoidParams / bfVarargs flags), folded by
// getPointerType as a `(*NAME)` or `(*)` declarator folds it. A fresh type per
// call, as each declarator makes its own.
DataDef *builtin_function_pointer(Program &pgm, DataDef *returns,
				  std::initializer_list<BuiltinParam> params, unsigned flags);

// A namespace-scope typedef whose aliased type keeps its shape (a function
// pointer): what TokenTYPEDEF::parse's finish_alias + record_typedef register —
// one TokenDataType under the bare alias and under the namespace, the alias
// among the typedef names emitted verbatim, and its file-scope typedef
// declaration (system-origin: a dialect header's).
DataDef *declare_builtin_typedef(Program &pgm, const char *ns, const char *name,
				 DataDef *aliased);

// A builtin data-only struct as generated code spells it: what TokenSTRUCT::parse
// registers for a namespace-scope C++ struct whose members are all data (a
// DataDefSTRUCT, or the DataDefCLASS an object member promotes it to; no
// method, base or nested type) — the struct laid out member by
// member by its own addMember and finalize (the TARGET's sizes and
// alignments, never a recorded offset), complete; the bare tag in struct_map
// and datatype_map, the namespace's type, its canonical spelling ns::Tag, and
// its file-scope struct TopDecl (system-origin). A member's typedef_name is
// the alias it was spelled with ("" for a raw type).
struct BuiltinMember
{
    const char *name;
    DataDef *type;
    const char *typedef_name;
    uint32_t access;		// DataDefSTRUCT::member_access (a class's private
				// member: vfPRIVATE; a struct's: 0)
};

struct BuiltinStructDecl
{
    const char *ns;
    const char *name;
    const BuiltinMember *members;
    size_t nmembers;
    bool object_class;		// the parse promoted it — an object member made it
				// a non-trivial class (TokenSTRUCT's promotion):
				// a DataDefCLASS, its class layout computed
};

DataDefSTRUCT *declare_builtin_struct(Program &pgm, const BuiltinStructDecl &d);

// A builtin class as generated code spells it — what TokenCLASS::parse
// registers for a namespace-scope class whose member functions are all
// declaration-only (the host implements them; their symbols are the TARGET's
// mangling, bound by the class registrar): three steps, in one builder.
//
// The shell (register_class_shell's writes, under the class's own namespace):
// the DataDefCLASS spelled ns::Tag, the bare tag in struct_map and
// datatype_map, the namespace's type, the tag its own scope alias. NULL when
// ns::Tag is already registered — a builder that finds its shell stops.
DataDefCLASS *declare_builtin_class(Program &pgm, const char *ns, const char *name,
				    bool from_system_header);

// One data member, laid out by the class's addMember, with its access.
void declare_builtin_class_member(DataDefCLASS *c, const BuiltinMember &m);

// One declaration-only member function as parseFunction's prototype arm
// leaves it — the FuncDef (the hidden `__this` its first parameter), its
// program-scope Variable bound in the class's namespace under its key, a
// Method owned by the class — then registered by the class registrar
// (register_class_method_signature: display name, access, method_map /
// ctors, the emitted symbol).
enum BuiltinMethodFlag : unsigned
{
    bmConst		= 1u << 0,	// FuncDef::is_const_method
    bmExplicit		= 1u << 1,	// FuncDef::is_explicit
    bmImplicitDtorNx	= 1u << 2,	// FuncDef::implicit_dtor_noexcept
    bmOperator		= 1u << 3,	// an operator-function-id (its Itanium name)
    bmStatic		= 1u << 4,	// a static member function
};

struct BuiltinMethodDecl
{
    const char *key;		// the funcdef_map key (`channel__ok`, `..._o2`)
    const char *display;	// the registrar's display name (`channel`, `~channel`, `ok`)
    Program::ClassMethodKind kind;
    DataDef *returns;
    const BuiltinParam *params;	// the hidden `__this` first
    size_t nparams;
    unsigned flags;		// BuiltinFnFlag bits (linkage, noexcept)
    unsigned method_flags;	// BuiltinMethodFlag bits
    uint32_t access;		// vfPRIVATE / vfPROTECTED / 0
    const char *local_emit;	// FuncDef::local_emit_name ("" for none)
};

Variable *declare_builtin_method(Program &pgm, DataDefCLASS *c, const BuiltinMethodDecl &d);

// The class completed as complete_class_aggregate completes it (its layout,
// noexcept resolution).
void complete_builtin_class(Program &pgm, DataDefCLASS *c);

// A builtin namespace object as generated code spells it: what parseDeclaration
// registers for a namespace-scope `extern T name;` (the extern-reference arm the
// forest restore reproduces too) — the Variable with no storage, its flags as
// the parse left them, its storage alias the TARGET's mangling of ns::name
// (namespace_cpp_variable_symbol, never a host string), bound in its namespace
// and the program scope, and its file-scope global TopDecl (system-origin, no
// initializer).
struct BuiltinExternDecl
{
    const char *ns;
    const char *name;
    DataDef *type;
    unsigned flags;		// Variable::flags (vfEXTERN, ...)
};

Variable *declare_builtin_extern(Program &pgm, const BuiltinExternDecl &d);

// A dialect type by its namespace and name: the program's registration, or —
// the first time — its builder's (the table's entry for ns::name). NULL when
// the dialect has no such type. The generated code names a dialect type
// through this, so a builder registers what it needs first.
DataDef *builtin_type(Program &pgm, const char *ns, const char *name);

// A registered namespace type's identity, field by field, tab-separated (the
// types' half of the differential). Empty when ns::name names no type.
madc::dis::istring builtin_type_line(Program &pgm, const madc::dis::istring &ns,
				     const madc::dis::istring &name);

// A registered namespace object's identity, field by field, tab-separated (the
// objects' half of the differential). Empty when ns::name binds no object.
madc::dis::istring builtin_variable_line(Program &pgm, const madc::dis::istring &ns,
					 const madc::dis::istring &name);

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
    // The namespace types `live` registered beyond `base`, as (ns, name).
    std::vector<std::pair<madc::dis::istring, madc::dis::istring> > type_keys;
    // The namespace objects (not a function, not an enumerator) beyond `base`.
    std::vector<std::pair<madc::dis::istring, madc::dis::istring> > variable_keys;
};
bool parse_dialect_as_text(DialectTextParse &out, std::string &err);
// A module's face (madc_module.h): `base` the dialect parsed as text, `live`
// the dialect and the face file at `face` — the keys are the face's own.
bool parse_module_as_text(DialectTextParse &out, const std::string &face, std::string &err);

// The generator's plan: the dialect's functions grouped by (namespace, name),
// sorted as the table is, each group's keys in registration order. `skip` names
// why a group gets no builder yet (empty: it gets one) — a body, a template, a
// type the generator does not yet express.
enum BuiltinGroupKind
{
    bgFunctions,	// an overload set: `keys` are its funcdef_map keys
    bgType,		// a namespace type (keys empty)
    bgVariable		// a namespace object (keys empty)
};

struct BuiltinGroup
{
    madc::dis::istring ns;
    madc::dis::istring name;
    std::vector<madc::dis::istring> keys;
    BuiltinGroupKind kind;
    std::string skip;
};
void plan_builtin_registry(DialectTextParse &dp, std::vector<BuiltinGroup> &groups);

// --emit-builtin-registry=<out.cpp>: the dialect's generated registration —
// one builder per planned group and the sorted table — written to `out`; a
// failure is reported on `diag`.
bool emit_builtin_registry(std::ostream &out, std::ostream &diag);
// --emit-module-table=<out.inc> <face>: a module's declarations as builders
// and a static table (madc_module_declarations / madc_module_ndeclarations)
// the module's source includes; every declaration must build (a refused one
// fails the generation, naming it).
bool emit_module_table(std::ostream &out, std::ostream &diag, const std::string &face);

#endif
