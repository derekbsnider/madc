#ifndef __MADC_H
//////////////////////////////////////////////////////////////////////////
//									//
// madc main header file			2019 Derek Snider	//
//									//
//////////////////////////////////////////////////////////////////////////
#define __MADC_H 1

#include <cstdint>
#include <cctype>
#include <cassert>
#include <fstream>
#include <functional>
#include <istream>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <memory>
#include <ostream>
#include <sstream>
#include <deque>
#include <iterator>
#include <initializer_list>
#include <set>
#include <stack>
#include <utility>
#include <vector>

#include "libmadc/value.h"
#include "madcdis/intern_table.h"
#include "madcdis/id_table.h"		// madc::dis::id_table — segmented stable-id registry
#include "madcdis/value_pool.h"		// madc::dis::value_pool — >64-bit value handles
#include "madc_typeid.h"		// MADC_TYPEID_PROJECT_BASE (the project segment base)
#include "cir_arena.h"		// madc::dis::DefArena — B3 arena-native DataDef storage

class Method;
class Program;
class CirFrozenForest;	// forest grove binding (cir_freeze.h); pointer member only
struct madc_stdlib_flavor;	// generated stdlib flavor table (madc_sys_includes.h); pointer member only
class MadcEngine;
class TokenBase;
class TokenSWITCH;
struct DelimDepth;	// parser-internal balanced-delimiter depth (parser.cpp)
class TokenCASE;
class TokenFunc;
class TokenCpnd;
class DataDefTemplateParam;	// typed template-parameter placeholder (datadef.h)

// The semantic measure of a C/C++ type query. References measure their
// referent, not madc's pointer-sized reference storage. Both eager parsing and
// parse-once tsubst use this owner so dependent and concrete queries agree.
size_t query_datadef_measure(const DataDef *dd, bool want_alignof);

class MadcTeeBuf : public std::streambuf
{
public:
    std::streambuf *primary;
    std::streambuf *secondary;

    MadcTeeBuf(std::streambuf *p=NULL, std::streambuf *s=NULL)
	: primary(p), secondary(s) {}

protected:
    virtual int overflow(int ch = EOF) override;
    virtual std::streamsize xsputn(const char *s, std::streamsize n) override;
    virtual int sync() override;
};

class FuncDef: public DataDef
{
public:
    // The function's return TYPE. With first-class references a reference return
    // is a DataDefREF (is_reference() true); use returns_reference() to test it
    // and return_value_type() to get the referent. Read through those accessors,
    // not `returns` directly, so the reference identity lives in ONE place (the
    // type) — first-class refs Phase 2 retired the old parallel returns_ref flag.
    DataDef &returns;
    // True when the return type is a reference (T& / T&&). The reference lives in
    // the type (returns is a DataDefREF). A T& return is lowered to a by-address
    // (T*) return at codegen; the call site is an lvalue (assign stores through
    // it, read derefs it), matching g++.
    bool returns_reference() const { return returns.is_reference(); }
    // The VALUE type a (possibly reference) return denotes: the referent for a
    // reference return, else the return type itself. Flip-transparent — correct
    // whether `returns` holds the DataDefREF or (legacy) the bare referent.
    DataDef &return_value_type() const {
	if ( returns.is_reference() )
	    return *static_cast<DataDefPTR &>(returns).base_type;
	return returns;
    }
    std::vector<DataDef *> parameters;
    size_t explicit_alignment;
    // [&] capture support
    bool has_captures;
    struct CaptureEntry { std::string name; DataDef *type; };
    std::vector<Variable *> potential_captures; // outer-scope vars at lambda creation time
    std::vector<CaptureEntry> captures;         // populated during lambda body compilation
    // GNU nested-function / [&]-lambda capture-by-reference lowering: the
    // enclosing variables the body actually USES, in first-reference order.
    // Filled by the CIR builder while translating the body (pointer identity
    // against potential_captures). Each becomes a hidden `T *name` parameter and
    // every call site forwards `&var`. Empty for a non-capturing function.
    std::vector<Variable *> captured_vars;
    // The C symbol a madc-EMITTED function's body is DEFINED-as and CALLED-as,
    // when it differs from the default scheme. Two disjoint sources feed it
    // (a FuncDef is never both a hoisted free fn and a class method):
    //   - a GNU nested function / [&]-lambda hoisted to a unique top-level C
    //     symbol (`enclosing__name__N`); its in-scope alias keeps the source
    //     name, but every call site must reference this hoisted symbol.
    //   - a madc-emitted class method/operator whose default
    //     `ClassName__method` scheme collides with another overload of
    //     DIFFERENT arity (unary vs binary `operator-`, prefix vs postfix
    //     `operator++(int)`); this is the arity-disambiguated symbol.
    // Empty for the common case (call sites use the default scheme). DISTINCT
    // from emit_symbol: this still takes the normal madc-emitted-body path; it
    // is NOT the extern-binding (no-body) path that emit_symbol triggers.
    std::string local_emit_name;
    // multiple return values (empty = single return via `returns`)
    std::vector<DataDef *> return_types;
    // Reference-ness of parameter i, derived from its type: a reference parameter
    // is a DataDefREF (is_reference() true). This is the SINGLE source of truth —
    // the old parallel `ref_params` flag vector was retired (first-class-references
    // Phase 2). A DataDefREF renders its name as `T*`, so the emitted ABI is
    // unchanged; class_behind() unwraps it for dispatch.
    bool is_ref_param(size_t i) const {
	return i < parameters.size() && parameters[i]
	    && parameters[i]->is_reference();
    }
    // const parameter tracking: const_params[i] == true when parameter i is const T&
    std::vector<bool> const_params;
    // Canonical C++ spelling of each parameter, captured from the SOURCE TOKENS
    // at parse time (where top-level pointee-const / `&` / template spelling are
    // all visible — the DataDef alone loses pointee-const, e.g. it stores
    // "char*" for a `const char*` param). Index-aligned with `parameters`; the
    // hidden `__this` slot (param 0 of a method) holds an empty string. Fed to
    // the Itanium mangler so a header-declared C++ method binds to its real
    // external symbol.
    std::vector<std::string> param_cpp_spellings;
    // Source typedef alias used for each parameter, when the declaration named
    // one. Index-aligned with `parameters`; empty means render from DataDef.
    std::vector<std::string> param_typedef_names;
    // True when a class-template method parameter spelled its base type as the
    // template parameter itself, rather than through a class-scope alias that
    // resolves to the same DataDef. Definition-capture provenance only; the
    // normalized ClassPattern persists the corresponding bit.
    std::vector<bool> param_template_param_spelled_directly;
    // Default ARGUMENT expression for each parameter (C++ `T x = expr`), captured
    // at parse time, index-aligned with `parameters`; NULL when the parameter has
    // no default. A call that omits a trailing argument fills it from here, and
    // arity matching treats the function as callable with [required..total] args
    // (required = count of params with no default).
    std::vector<class TokenBase *> param_defaults;
    // RAW SOURCE TOKENS of each default expression — the exact token range
    // parseExpression consumed to build param_defaults[i], cloned at capture.
    // Forest SAVE state (a parsed TokenBase tree cannot serialize; the tokens
    // can, and the load re-runs the ONE live derivation — parseExpression —
    // over them at the pending-funcs flush). Captured ONLY when
    // forest_arena_enabled (a --freeze parse); index-aligned with
    // param_defaults when non-empty, and possibly SHORTER (bounds-check reads).
    std::vector<std::vector<class TokenBase *>> param_default_tokens;
    // RAW SOURCE TOKENS of a FREE function's parsed body (`stmts... }`,
    // the parseCompound span incl. the closing brace — the exact shape
    // parse_deferred_function_body::body_tokens re-parses). Forest SAVE state
    // (v26 piece a): the frozen AST holds only TRANSLATED defs, so a bodied
    // include-origin free fn the producer never called (std::abs) serializes
    // its body as an ownerless DK_DEFBODY token run instead. Captured ONLY
    // when forest_arena_enabled and owner_class == NULL.
    std::vector<class TokenBase *> forest_body_tokens;
    // A ctor's MEM-INITIALIZER-LIST tokens, captured beside the body span
    // when the body came through parse_deferred_function_body (in-class
    // inline bodies parse at class close — they never reach parseFunction's
    // parseCompound). Rides DK_DEFBODY run slot 3 (ctor_init_tokens).
    std::vector<class TokenBase *> forest_ctor_init_tokens;
    // The captured body's PARSE CONTEXT: true when it was parsed inside a
    // function-template instantiation (fn_template_instantiation_depth > 0
    // at capture — an instantiated __oN definition like __stoa__o2). The
    // ownerless DEFBODY re-run reproduces that context so instantiation
    // allowances (the local-class reuse at TokenCLASS::parse) apply as live.
    bool forest_body_in_instantiation = false;
    // Number of leading parameters that have NO default — the minimum arg count a
    // call must supply. Equals parameters.size() when no parameter has a default.
    size_t required_param_count() const
    {
	size_t req = parameters.size();
	for ( size_t i = parameters.size(); i-- > 0; )
	{
	    if ( i < param_defaults.size() && param_defaults[i] )
		req = i;
	    else
		break;
	}
	return req;
    }
    std::string template_return_param_name;
    int template_return_deduce_arg_index;
    bool template_return_deduce_from_pointer;
    bool template_return_ref;
    std::string return_typedef_name;
    // When non-empty, the C symbol this function is CALLED as / DEFINED as,
    // instead of the default ClassName__method scheme. Used to bind a class
    // method directly to an externally-provided C++ ABI symbol. madc emits no
    // body for such methods.
    std::string emit_symbol;
    // The UNMANGLED display name of a class method (`take`, `find`, `operator=`),
    // independent of the mangled call symbol stored as the Variable's name
    // (`Box__take`, `Box__take__o2`). Lets overload resolution enumerate the
    // same-named overloads in DataDefCLASS::methods (whose entries are keyed by
    // their mangled names). Empty for non-method FuncDefs.
    std::string method_display_name;
    // Namespace/free-function source identity for overloaded C++ functions.
    // These let call-site resolution enumerate overloads registered under unique
    // internal symbols while preserving the source name.
    std::string function_display_name;
    std::string namespace_name;
    std::string inline_builtin_kind;
    // TRUE when this FuncDef is a BUILTIN-STYLE registration (the
    // builtin_registry core/process/dlfcn loops — the caller passes the
    // intent into Program::addFunction). An explicit source (re)declaration
    // REPLACES such an entry wholesale (gcc canon: an explicit prototype
    // replaces a builtin) — parameters, return type, and any wrapper
    // binding (local_emit_name) all come from the source declaration.
    // parseFunction consumes this at its already-declared check.
    // NOT stamped on addFunction's OTHER mints — namespace fn-template
    // placeholders, member-template instantiation registrations, __dl_
    // dynamic symbols, host embedding — a later source declaration of
    // those ids is a definition/refinement, not a builtin override
    // (clobbering _M_construct instantiation entries mid-header broke the
    // freeze producer's .tcc body parses).
    bool builtin_registration = false;
    // For an externally-bound ctor (emit_symbol set) whose real ABI takes a
    // trailing reference argument that madc has no value for, pass the object's
    // own address (&this) as that trailing arg.
    bool ctor_trailing_self;
    // Member function/constructor template specializations are emitted in the
    // current translation unit unless an explicit specialization says otherwise;
    // an extern-template class instantiation does not export arbitrary member
    // template specializations.
    bool is_member_template;
    std::vector<std::string> template_param_names;
    // Pack-ness per template_param_names entry (a `typename... _Args` parameter
    // pack vs a plain `typename _Up`). Needed so a variadic member template
    // (allocator_traits::construct) instantiates its `_Args...` correctly.
    // Empty == none-are-packs (back-compat for older registrations).
    std::vector<bool> template_param_is_pack;
    // Type-ness per template_param_names entry (false = a NON-TYPE param such as
    // `size_t... _Indexes`). Empty == all-are-type (back-compat). Needed so a
    // member-template ctor with non-type packs (std::pair's indexed ctor)
    // instantiates with the correct typeparam_is_type classification.
    std::vector<bool> template_param_is_type;
    std::string template_return_spelling;
    std::vector<std::string> template_param_spellings;
    // For a STATIC member function template of a madc-LOCAL (monomorphized,
    // not libstdc++-exported) class, the retained body declaration tokens
    // (declarator + params + `{ ... }`, WITHOUT the `template<...>` header) and
    // the owning class. A call site instantiates the body on ODR-use
    // (instantiate_member_fn_template_for_call) instead of leaving a bare
    // undefined extern. Empty unless the body was retained.
    std::vector<TokenBase *> member_template_decl;
    DataDef *member_template_owner;
    // The DEPENDENT return-type token range of a member function template
    // (the tokens before the declarator name, still naming the template
    // parameters — e.g. `Succ < T >`). Retained for BOTH body-bearing AND
    // body-less member templates so a call site can resolve the CONCRETE
    // return type by substituting the deduced/explicit args into these tokens
    // WITHOUT instantiating a body (the clang SubstDecl model — see
    // resolve_member_template_call_return_type). Empty == not a member tmpl.
    std::vector<TokenBase *> member_template_return_tokens;
    // Per-param DEFAULT token runs (parallel to template_param_names; an empty
    // run = no default). [temp.deduct]/8: an unbound defaulted param must
    // substitute-and-resolve at the call, or the candidate is not viable —
    // gcc13's __is_destructible_impl::__test carries its ENTIRE SFINAE in
    // `typename = decltype(declval<_Tp1&>().~_Tp1())` (plain true_type return).
    std::vector<std::vector<TokenBase *> > member_template_param_defaults;
    // Per-param CONSTRAINT-type token runs (parallel to template_param_names;
    // empty run = unconstrained). A NON-TYPE param's SFINAE lives in its
    // declared TYPE (`typename enable_if<C, bool>::type = true`) — the
    // template-head parse captures that compound type spelling here (the same
    // capture the namespace-fn lane stores in FnTemplateDef::
    // typeparam_constraints). The instantiation twins fill a non-type
    // param's default ONLY when this run is empty (`bool _Dummy = true` —
    // nothing to evaluate, nothing asserted); a captured run still clears
    // the default until the run is evaluated at instantiation.
    std::vector<std::vector<TokenBase *> > member_template_param_constraints;
    // Per FUNCTION-parameter TYPE token runs (declaration order, default
    // value stripped past a top-level `=`). [temp.deduct]/8's OTHER half:
    // the SFINAE may live in the parameter type itself — libc++'s
    // __has_iterator_category pair puts it in
    // `typename _Up::iterator_category* = nullptr`. A run naming a bound
    // template param must substitute-and-resolve at the call
    // (resolve_member_template_call_return_type), or the candidate is not
    // viable and overload resolution falls to the next same-name member.
    std::vector<std::vector<TokenBase *> > member_template_param_type_tokens;
    // Two-tree Phase 2: the DEPENDENT parse-tree pattern for this template's body
    // — a TokenFunc parsed ONCE with the template params bound to
    // DataDefTemplateParam placeholders (via build_dependent_pattern), with eager
    // nested instantiation SUPPRESSED so dependent calls stay dependent. NULL until
    // produced; the immutable Tree-1 RECIPE that cir-build lowers to a cir pattern
    // and Phase 3 tsubst copies+substitutes per instantiation. Removed from
    // pending_funcs at capture so it never reaches cir-build/emit on its own.
    TokenFunc *dependent_pattern;
    // Two-tree Phase 3: on a CONCRETE instantiated member-template method, the
    // SOURCE member-template FuncDef this instance was produced from (the one
    // carrying dependent_pattern, the Tree-1 recipe). NULL for an ordinary
    // function. Set in instantiate_member_fn_template_for_call so the cir-build
    // seam can find the recipe.
    FuncDef *tsubst_source;
    // Concrete TYPE template arguments for the instantiated member-template
    // method, in tsubst_source->template_param_names order. These are captured
    // where deduction/defaulting actually happens (the parser's fn-template
    // instantiation path) and consumed by CIR tsubst directly, matching g++'s
    // "saved tree + args" model. Empty means not recorded / not tsubst-covered.
    std::vector<DataDef *> tsubst_type_args;
    // Concrete TYPE parameter-pack arguments for the instantiated
    // member-template method, parallel to tsubst_source->template_param_names:
    // non-pack slots are empty; a pack slot contains its deduced element types
    // in expansion order. This is the argument-pack half of the g++ saved-tree
    // + args model, captured now so CIR pack expansion can consume real arity
    // and element types instead of re-deriving them from rewritten tokens.
    std::vector<std::vector<DataDef *> > tsubst_type_arg_packs;
    // Phase-5 slice 4b: on a CONCRETE instantiated member-template method whose
    // source carries a dependent_pattern, the instantiation's body parse was
    // SKIPPED — tsubst supplies the body at lowering. A tsubst bail on a
    // skipped body is a loud pre-c2mir error (the re-parse fallback and its
    // captured-span machinery are deleted; the burndown/ratchet gates keep
    // that path unreachable). False == body parsed eagerly (auto-return
    // deduction, or a source with no pattern).
    bool tsubst_body_skipped;
    struct CtorInitializer {
	std::string name;
	std::vector<TokenBase *> args;
	// `m{...}` (list-init) vs `m(...)` (direct-init) — [dcl.init]. For an
	// aggregate member the brace form initializes its FIELDS in order, so
	// the two cannot be collapsed: `p{1,2}` fills p.a and p.b, while
	// `p(other)` copies one value. Nested braces are flattened at parse
	// time to one scalar sequence, matching the declaration path.
	bool braced = false;
    };
    std::vector<CtorInitializer> ctor_initializers;
    // Initializer order matches member declaration order (avoids -Wreorder).
    FuncDef(DataDef &d) : returns(d), explicit_alignment(0), has_captures(false), template_return_param_name(), template_return_deduce_arg_index(-1), template_return_deduce_from_pointer(false), template_return_ref(false), return_typedef_name(), emit_symbol(), method_display_name(), function_display_name(), namespace_name(), inline_builtin_kind(), ctor_trailing_self(false), is_member_template(false), template_param_names(), template_param_is_pack(), template_param_is_type(), template_return_spelling(), template_param_spellings(), member_template_decl(), member_template_owner(NULL), member_template_return_tokens(), member_template_param_type_tokens(), dependent_pattern(NULL), tsubst_source(NULL), tsubst_type_args(), tsubst_type_arg_packs(), tsubst_body_skipped(false), ctor_initializers(), is_varargs(false), is_void_params(false), no_instrument_function(false), no_strict_aliasing(false), has_large_struct_retbuf(false), declaration_only(false), defaulted_or_deleted(false), is_deleted(false), noexcept_spec(0), pure_virtual(false), is_const_method(false), ref_qualifier(0), vague_linkage(false) {}
    DataDef *findParameter(const std::string &);
    virtual BaseType basetype() const { return BaseType::btFunct; }
    virtual size_t alignment() const { return explicit_alignment ? explicit_alignment : DataDef::alignment(); }
    bool is_varargs;  // function declared with ... (variadic)
    // A parsed trailing `...` rides the param arrays as a pseudo-param with an
    // empty captured spelling; give a mangle param list its "..." entry (the
    // Itanium encoders spell it `z`). No-op when the tail slot isn't the
    // parsed pseudo-param.
    void spell_varargs_tail(std::vector<std::string> &psp) const
    {
	if ( is_varargs && !psp.empty() && psp.back().empty() )
	    psp.back() = "...";
    }
    // Mangle-ready spelling of parameter i: the captured source spelling,
    // except a BARE scalar-typedef core (std::streamoff, std::streamsize)
    // desugars to its builtin C spelling via DataDef::mangle_scalar_spelling
    // — Itanium encodes canonical types, never typedef names. Decorated
    // spellings (*, &, <...>) keep the captured form: the alias dd behind
    // them is not the parameter's own DataDef.
    std::string mangle_param_spelling(size_t i) const
    {
	std::string sp = i < param_cpp_spellings.size() ? param_cpp_spellings[i]
							: std::string();
	if ( sp.empty() || i >= parameters.size() || !parameters[i] )
	    return sp;
	if ( sp.find('<') != std::string::npos
	  || sp.back() == '*' || sp.back() == '&' )
	    return sp;
	std::string scalar = parameters[i]->mangle_scalar_spelling();
	if ( scalar.empty() || scalar == sp )
	    return sp;
	return scalar;
    }
    bool is_void_params; // f(void) — explicitly zero params (vs f() which is K&R unspecified)
    bool no_instrument_function;
    // __attribute__((optimize("-fno-strict-aliasing"))): the CIR builder forwards
    // this as an N_ATTR in the FUNC_DEF specs; c2mir suppresses TBAA per-function.
    bool no_strict_aliasing;
    bool has_large_struct_retbuf; // __retbuf was injected for struct return > 16 bytes
    // True when DECLARED with no body (prototype ended in ';' / ',' not '{').
    // For a C++ class method whose class carries canonical C++ spelling, this
    // can bind emit_symbol to the mangled external symbol. Stays false for any
    // madc-compiled (bodied) function.
    bool declaration_only;
    // The source file of a declaration_only prototype (token ->file pointer;
    // NULL when not recorded). Lets the compile-stage registration-policy
    // gate distinguish a USER-SOURCE extern prototype from curated header
    // declarations.
    const char *decl_file = nullptr;
    // Forest INLINE-method body: when a bound method was reconstructed from a
    // frozen container (forest save/load) AND the container held the method's
    // func-def in its AST (an INLINE method, body only in the header — no .so),
    // this locates that Tree-1 func-def subtree by (unit, record idx). On ODR-use
    // the consumer materializes it from prog->bind_forest and appends it to its
    // own module — the "copy the saved body into Tree-2" step. False/0 for a
    // normally-parsed method or a LIBRARY method (body in a .so; declaration_only).
    bool     has_forest_body = false;
    uint32_t forest_body_unit = 0;
    uint32_t forest_body_idx = 0;
    // True for C++ special declarations like `= default` or `= delete`.
    // These are not bodyless shared-library declarations and must not be bound
    // as external symbols just because the class has canonical C++ spelling.
    bool defaulted_or_deleted;
    // True ONLY for `= delete` (a SUBSET of defaulted_or_deleted, which also
    // covers `= default`). Distinguished because faithful `__is_assignable` /
    // `__is_constructible` must treat a deleted special member as "not
    // assignable / not constructible" while a defaulted one is available.
    bool is_deleted;
    // Parsed noexcept-ness of the exception specification, three-state:
    // NxNone = no specifier (may throw), NxTrue = `noexcept` / `noexcept(true)` /
    // `throw()` (non-throwing), NxUnknown = `noexcept(expr)` whose condition did
    // not constant-fold at parse. Consumed by __is_nothrow_constructible; NOT a
    // codegen contract (madc emits no std::terminate fence).
    enum NoexceptSpec : uint8_t { NxNone = 0, NxTrue = 1, NxUnknown = 2 };
    uint8_t noexcept_spec;
    // True for C++ pure virtual declarations (`= 0`). They have no body but
    // still participate in method lookup and vtable layout.
    bool pure_virtual;
    // True when the method was declared with a trailing `const` (e.g.
    // `bool good() const;`). The const-qualified `this` mangles with the Itanium
    // 'K' (e.g. _ZNKSt9basic_ios...4goodEv). Set by TokenCLASS::parse / parseFunction
    // when a trailing const follows the parameter list. Default false.
    bool is_const_method;
    // C++11 ref-qualifier ([dcl.fct]p6) on the method: 0 = none, 1 = `&`,
    // 2 = `&&`. Overloads may differ ONLY in cv+ref qualification (libc++'s
    // __optional_storage_base::__get declares all four combinations), so this
    // participates in signature identity beside is_const_method. Itanium
    // mangling: 'R' (&) / 'O' (&&) before the CV qualifiers.
    uint8_t ref_qualifier;
    // True when the body was defined in-class or arrived through the
    // deferred-body machinery (parse_deferred_function_body): the C++
    // implicit-inline set — every TU that sees the defining class/header can
    // emit an identical copy. Set by the parser; consumed by is_linkonce().
    bool vague_linkage;
    // A definition multiple TUs can each emit identically (C++ vague
    // linkage): a template instantiation or an in-class/deferred body. The
    // CIR builder emits these with the `linkonce` attribute so the native
    // object capture binds them STB_WEAK — identical per-TU copies merge at
    // a multi-.o link (first wins) instead of colliding as duplicate strong
    // definitions [ELF-completion S4].
    bool is_linkonce() const { return tsubst_source != NULL || vague_linkage; }
    bool is_multi_return() const { return return_types.size() > 1; }
};

// Generic overload-resolution ranking — NO type is special-cased. Ranks binding
// an argument of type `adc` to a parameter of type `pdc`: higher is a better
// match; -1 means it cannot bind. Scalars, pointers, and ANY class object all
// go through the same category logic; a class parameter additionally accepts an
// argument through ONE user-defined conversion (a single-argument converting
// constructor of that class). That converting-ctor path is what lets, say, a
// `const char*` bind a class parameter that has a `(const char*)` constructor
// WITHOUT the resolver knowing or caring which class it is. `allow_udc` is
// cleared on the recursive converting-ctor check so an implicit conversion
// sequence uses at most one user-defined conversion (the C++ rule), which also
// bounds the recursion to depth 1. Defined in cir_builder.cpp.
// `arg_is_zero_literal`: the argument EXPRESSION is an integer literal of value
// zero — a C++ null-pointer constant ([conv.ptr]), so it binds a pointer
// parameter as a standard conversion. Only callers that can see the argument
// token set it; the DataDef alone cannot carry "literal zero".
int score_arg_to_param(const DataDef *adc, const DataDef *pdc,
		       bool param_is_ref = false, bool allow_udc = true,
		       bool arg_is_zero_literal = false);

class DataStruct: public DataDef
{
public:
    std::vector<DataDef *> elements;
    virtual BaseType basetype() const { return BaseType::btStruct; }
};

class DataClass: public DataDef
{
public:
    std::vector<DataDef *> elements;
    std::vector<FuncDef *> methods;
    virtual BaseType basetype() const { return BaseType::btClass; }
};

enum class HoistedDeclKind
{
    LocalClass,
    NestedFunction,
    PatternFunction
};

struct HoistedDeclIdentity
{
    HoistedDeclKind kind;
    std::string owner_symbol;
    std::string source_name;
    size_t ordinal;
    std::string symbol;

    HoistedDeclIdentity()
	: kind(HoistedDeclKind::LocalClass), ordinal(0) {}
};

class Method
{
public:
    Variable &returns;
    std::vector<Variable *> parameters;
    std::vector<Variable *> variables;
    void *x86code;
    Variable *env_param; // hidden void** param for [&] lambdas (nullptr if no capture)
    class DataDefCLASS *owner_class; // non-null when this is a class method
    // Block-local classes and GNU nested functions share one source-ordered
    // ordinal stream per enclosing function body. The per-scope map makes a
    // forward declaration and its definition reuse one identity while equal
    // source names in distinct lexical blocks remain distinct.
    size_t next_hoisted_decl_ordinal;
    typedef std::pair<HoistedDeclKind, std::string> hoisted_decl_key_t;
    std::map<const TokenCpnd *,
	     std::map<hoisted_decl_key_t, HoistedDeclIdentity> > hoisted_decls;
    Method(Variable &v) : returns(v), x86code(NULL), env_param(NULL),
	owner_class(NULL), next_hoisted_decl_ordinal(0) {}
    void reset_hoisted_declarations()
    {
	next_hoisted_decl_ordinal = 0;
	hoisted_decls.clear();
    }
    Variable *getParameter(unsigned int i) { if ( i >= parameters.size() ) return NULL; return parameters[i]; }
    Variable *findParameter(const std::string &);
    Variable *findVariable(const std::string &);
};


// Program tokens


// placeholder class
class TokenStmt: public TokenBase
{
public:
    TokenStmt() : TokenBase() {}
};


// Token for containing a "Compound Statement" built up of multiple "Statement" tokens
// Used to to extend the "Function" token and the "Program" token, described below
//
// Used to keep track of local variables, and nesting of brackets { }
// As well as managing registers, and stack management (constructors, destructors)
class TokenCpnd: public virtual TokenBase
{
public:
    Method *method;
    TokenCpnd *parent;
    TokenCpnd *child;
    std::vector<Variable *> variables;
    // O(1) name -> Variable* index over `variables`, built incrementally. The old
    // linear scan in findVariable was the dominant parse cost on large real system
    // headers (thousands of symbols per scope -> O(n^2)). `var_indexed` = how many
    // of `variables` have been absorbed; findVariable absorbs any appended since,
    // first-wins (emplace) to match the old front-to-back scan. `variables` is
    // append-only during parse; the one erase site (foreach-catch lowering) resets
    // the index, and a shrink is detected defensively below.
    // Keyed by interned spelling_id (madc::dis::intern_table), NOT std::string: the query name
    // is interned ONCE at the scope-walk entry and the integer sid is probed at
    // every scope level, instead of re-hashing the std::string per level up the
    // parent chain (C2 — "intern the name into the lookup"; findVariable was
    // ~12% of -O2 front-end time). Same sid <=> same string (intern dedups), so
    // the mapping is exact; first-wins via emplace, matching the old scan.
    std::unordered_map<uint32_t, Variable *> var_index;
    size_t var_indexed = 0;
    std::vector<TokenStmt *> statements;
    std::vector<TokenBase *> deferred;   // defer statements (compiled in LIFO at scope exit)
    std::vector<Variable *> destruct_order; // class-typed vars in declaration order (for LIFO dtor)
    int end_line;			// line of closing } (set by parseCompound)
    bool is_stmt_expr = false;		// true: a GNU statement-expression `({...})`, not a plain `{...}` block
    TokenCpnd() : TokenBase() { method = NULL; parent = NULL; child = NULL; end_line = 0; }
    virtual TokenType type() const override { return TokenType::ttCompound; }
    virtual DataDef *datadef() const override {
	if ( statements.empty() ) return &ddVOID;
	DataDef *dd = statements.back()->datadef();
	return dd ? dd : &ddVOID;
    }
    Variable *getParameter(unsigned int);
    Variable *findParameter(const std::string &s);
    // Walk this scope chain for `id`. The (intern_table&, std::string&) entry interns
    // the query ONCE; the inner overload takes the pre-interned sid and recurses up
    // `parent` probing the sid-keyed index at each level (no per-level re-hash).
    Variable *findVariable(const madc::dis::intern_table &sp, const std::string &id);
    Variable *findVariable(const madc::dis::intern_table &sp, uint32_t qsid, const std::string &id);
    // Single-level lookup in THIS scope only (no parent walk).
    Variable *findVariableThisScope(const madc::dis::intern_table &sp, uint32_t qsid,
				    const std::string &id);
    // Function-local lookup: walk THIS scope chain up to (but not into) the
    // program/global scope. A name found here is a parameter or block local,
    // which in C++ unqualified lookup shadows any same-named namespace member.
    Variable *findVariableLocal(const madc::dis::intern_table &sp, const std::string &id);
};

class TokenFunc: public TokenVar, public TokenCpnd
{
public:
    // True when a later definition of the same function overrides this
    // one. Set during compile pre-pass by walking pending_funcs in
    // reverse and marking earlier duplicates. Overridden TokenFuncs skip
    // both prepareFuncNode and body emission, so exactly one definition
    // is emitted per function.
    // (Still required, but no longer for the reason originally recorded
    // here: this was written for an asmjit Compiler defect where a second
    // addFunc on the same FuncNode unbound the labels of every funcnode
    // added in between. asmjit is gone; emitting one body twice is now
    // simply a duplicate definition to c2mir. Same behaviour, different
    // reason — see the long-double case for one where the behaviour did
    // NOT survive the backend swap.)
    bool is_overridden = false;
    TokenFunc(Variable &v) : TokenVar(v), TokenCpnd() {}
    virtual size_t argc() const { if (var.type->basetype() != BaseType::btFunct) return 0; return ((FuncDef *)var.type)->parameters.size(); }
    virtual TokenType type() const { return TokenType::ttFunction; }
};

class TokenDecl: public TokenVar
{
public:
    TokenBase *initialize;
    std::vector<TokenBase *> init_list; // brace-enclosed initializer for fixed-size arrays
    std::vector<TokenBase *> ctor_args; // constructor arguments for class-typed vars
    // v25 forest SAVE state: the ctor-args list's RAW SOURCE TOKEN run (cloned,
    // commas included), captured only during a --freeze parse (the cursor tap,
    // like FuncDef::param_default_tokens). The parsed ctor_args trees cannot
    // serialize; the flush re-runs the args-list parse over these tokens.
    std::vector<TokenBase *> ctor_arg_src;
    bool has_brace_init;               // true when `= { ... }` syntax was used
    bool is_const_decl;                // true when declared with `const` qualifier
    // True when this declaration carried a constant initializer that the parser
    // baked into Variable::data and then cleared init_list (a static/global
    // fixed array — brace `{...}` OR string-literal `"..."` char array). The CIR
    // builder uses this to reconstruct the INIT list from v->data. Crucially it
    // distinguishes the DEFINING declaration from a shared `extern` placeholder
    // (which never had an initializer), so the extern is not turned into a
    // second definition ("Repeated item declaration").
    bool baked_static_init = false;
    // True when this is a FUNCTION-BLOCK-scope `extern T name;` that refers to a
    // file-scope global of the same name. The CIR backend must emit an actual
    // `extern T name;` inside that block so c2mir's scope resolution rebinds the
    // name to the file-scope object — otherwise an enclosing local of the same
    // name would shadow it (C: a block `extern` re-exposes the file-scope
    // object). Distinct from baked_static_init: this NEVER defines storage.
    bool block_extern_redecl = false;
    TokenDecl(Variable &v) : TokenVar(v) { initialize = NULL; has_brace_init = false; is_const_decl = false; }
    virtual TokenType type() const { return TokenType::ttDeclare; }
};

// AST node for a typedef declaration. Returned by TokenTYPEDEF::parse()
// so typedefs appear in the AST in source order (consumed by the CIR
// layer via Program::top_decls). The JIT compiler ignores it: compile()
// is a no-op that returns the inherited _operand.
class TokenTypedefDecl: public TokenBase
{
public:
    std::string alias;       // typedef alias name (e.g. "EXT_BV")
    DataDef *target_type;    // what the typedef resolves to
    TokenTypedefDecl(const std::string &a, DataDef *t) : alias(a), target_type(t) {}
    virtual TokenType type() const { return TokenType::ttTypedefDecl; }
};

// AST node for a standalone struct/union definition (no variable
// declarator). Returned by TokenSTRUCT::parse() so the definition keeps
// its source-order position. The JIT compiler ignores it.
class TokenStructDef: public TokenBase
{
public:
    DataDefSTRUCT *sdd;
    bool is_union;
    TokenStructDef(DataDefSTRUCT *s, bool u = false) : sdd(s), is_union(u) {}
    virtual TokenType type() const { return TokenType::ttStructDef; }
};

// { v0, v1, ... } — a nested brace initializer, used for elements of an
// array-of-structs or for nested struct members. Not a value by itself.
class TokenStructLit: public TokenBase
{
public:
    std::vector<TokenBase *> inits;
    // When the compound-literal type was named via a typedef (e.g. `(T){...}`),
    // this holds the alias so the CIR builder can render the type-name spec as
    // ID("T") instead of re-deriving an anonymous struct/union layout. Empty for
    // a bare `struct Tag` / `union Tag` / builtin element type.
    std::string typedef_name;
    // Non-null for a C99 ARRAY compound literal `(T[]){...}` / `(T[N]){...}`.
    // The parser models the literal's storage as a synthetic `__compound_array`
    // struct (datadef()), but that loses array semantics: c2mir would see a
    // forward-ref struct (incomplete type) that cannot be subscripted. When this
    // is set the CIR builder instead emits a real array type-name
    // `T name[]` so c2mir sizes the array from the initializer count — the
    // faithful C99 lowering. Holds the ELEMENT type T.
    DataDef *array_elem_dd = nullptr;
    TokenStructLit() {}
    virtual TokenType type() const { return TokenType::ttStructLit; }
};

// Tree-1 marker for a C++ pack expansion pattern (`expr...`) captured during a
// dependent template-body parse. Normal concrete instantiation paths rewrite
// packs before parsing; this wrapper exists only so CIR tsubst can fan out the
// saved pattern by the already-deduced pack arity.
class TokenPackExpansion: public TokenBase
{
public:
    TokenBase *pattern;
    TokenPackExpansion(TokenBase *p = NULL) : TokenBase(), pattern(p)
    {
	if (p) _datatype = p->datadef();
    }
    virtual DataDef *datadef() const override
    {
	return pattern ? pattern->datadef() : TokenBase::datadef();
    }
    virtual TokenBase *clone() override
    {
	TokenPackExpansion *t = new TokenPackExpansion(pattern ? pattern->clone() : NULL);
	t->file = file;
	t->line = line;
	t->column = column;
	return t;
    }
};

class TokenCallFunc: public TokenVar
{
public:
    std::vector<TokenBase *> parameters;
    DataDef *return_override = nullptr;
    bool returns_ref_override = false;
    // Explicit template arguments captured at the call site
    // (`__stoa<long, int>(...)`): leading template parameters bound
    // left-to-right by instantiate_namespace_fn_template_for_call.
    // Empty for ordinary calls.
    std::vector<DataDef *> explicit_template_args;
    // Number of USER-WRITTEN arguments, set when parseCallFunc appends
    // C++ default arguments. Overload ranking must consider only these:
    // the defaults were copied from ONE overload's declaration, and scoring
    // them (e.g. the literal 0 of `size_t* __idx = 0` against the pointer
    // param) would veto the very overload that supplied them.
    // (size_t)-1 = no defaults appended; every argument is user-written.
    size_t user_argc = (size_t)-1;
    // Non-null when the function-pointer value comes from a sub-expression
    // (e.g. a struct member access c.fn or arr[i].fn) rather than a variable.
    // When set, TokenCallFunc::compile loads the fn-ptr by compiling src_node
    // instead of calling voperand(var). var.type must still be DataDefFPTR.
    TokenBase *src_node = nullptr;
    // The concrete member-template instantiation THIS call binds to, set by
    // instantiate_member_fn_template_for_call (per type-shape — a member
    // template used at two types has two instances; `var` stays the
    // non-rebindable placeholder). Consumers: call_target_variable (the one
    // TokenCallFunc-lane callee resolver) and the parse-side call rebuilds.
    // NULL = not a madc-instantiated member-template call (the common case).
    Variable *mti_instance = nullptr;
    bool auto_scope_context = false;
    TokenCallFunc(Variable &v) : TokenVar(v) { if (v.type->is_function()) _datatype = returns(); }
    virtual DataDef *returns()  const {
	if ( return_override )
	    return return_override;
	if ( DataDefFPTR *fptr = dynamic_cast<DataDefFPTR *>(var.type) )
	    return (fptr->target != NULL) ? &fptr->target->returns : &ddVOID;
	return &((FuncDef *)var.type)->returns;
    }
    // Does this call yield a REFERENCE (an lvalue of the referent)? The
    // reference-ness lives in EITHER the callee's declared return
    // (FuncDef::returns_reference) OR the parse-time substituted return of a
    // template call (returns_ref_override — set when explicit template args
    // form the return type without a body). THE one test every "is this call
    // an lvalue / may a reference bind to it" consumer must use — checking
    // only the FuncDef half sent a placeholder-bound
    // `T &r = std::use_facet<F>(loc)` down the address-of-the-callee arm.
    bool call_returns_reference() const {
	if ( returns_ref_override )
	    return true;
	FuncDef *fd = dynamic_cast<FuncDef *>(var.type);
	return fd && fd->returns_reference();
    }
    virtual DataDef *datadef()  const override {
        if ( var.type->is_function() )
            return returns();
        return _datatype;
    }
    virtual bool is_real() const override { return datadef() && datadef()->is_real(); }
    virtual size_t argc() const override { return parameters.size(); }
    virtual TokenType type() const override { return TokenType::ttCallFunc; }
};

class TokenScopeContext: public TokenBase
{
public:
    Variable &context_var;
    std::vector<Variable *> scope_vars;
    TokenScopeContext(Variable &ctx) : TokenBase(), context_var(ctx) { _datatype = &ddARRAY; }
    virtual TokenType type() const override { return TokenType::ttVariable; }
    virtual DataDef *datadef() const override { return &ddARRAY; }
};

class TokenMember: public TokenCallFunc
{
public:
    Variable &object;
    size_t offset;
    TokenBase *parent_expr;  // non-null for chained -> (e.g. ch->in_room->name)
    TokenMember(Variable &o, Variable &m, size_t ofs)
        : TokenCallFunc(m), object(o), offset(ofs), parent_expr(nullptr) { _datatype = m.type; }
    TokenMember(Variable &o, Variable &m, size_t ofs, TokenBase *parent)
        : TokenCallFunc(m), object(o), offset(ofs), parent_expr(parent) { _datatype = m.type; }
    virtual TokenType type() const override { return TokenType::ttMember; }
    virtual DataDef *datadef() const override { return _datatype; }
    virtual bool is_real() const override { return _datatype->is_real(); }
    // Member is declared as a fixed array (e.g. `SKILLTYPE *arr[N]`).
    // Such a member's datadef reports the element type but the storage
    // is in-place, so subscripting needs LEA on the member's Mem and
    // the parser must not unwrap pointer-typed elements.
    bool is_fixed_array_member() const
    {
	// For `obj->member` access, object.type is the pointer-to-struct,
	// not the struct itself. Walk through any pointer wrapper.
	DataDefSTRUCT *sdd = owner_struct_type();
	if ( !sdd ) return false;
	std::string mname = var.name;
	return sdd->m_is_array_decl(mname);
    }
    DataDefSTRUCT *owner_struct_type() const
    {
	DataDef *otype = object.type;
	if ( DataDefPTR *opt = dynamic_cast<DataDefPTR *>(otype) )
	    otype = opt->base_type;
	return dynamic_cast<DataDefSTRUCT *>(otype);
    }
    const DataDefSTRUCT::BitFieldInfo *bitfield_info() const
    {
	DataDefSTRUCT *sdd = owner_struct_type();
	if ( !sdd ) return NULL;
	return sdd->m_bitfield(var.name);
    }
    bool is_bitfield_member() const
    {
	return bitfield_info() != NULL;
    }
};

// & address-of operator — emits LEA to get address of a variable
class TokenAddrOf: public TokenBase
{
public:
    Variable &var;
    DataDef *ptr_type;  // pointer-to-var type
    TokenAddrOf(Variable &v, DataDef *pt) : var(v), ptr_type(pt) {}
    virtual TokenType type() const override { return TokenType::ttBase; }
    virtual DataDef *datadef() const override { return ptr_type ? ptr_type : &ddVOID; }
};

// &(expr) address-of operator for member/subscript/deref lvalues
class TokenAddrExpr: public TokenBase
{
public:
    TokenBase *expr;
    DataDef *ptr_type;
    TokenAddrExpr(TokenBase *e, DataDef *pt) : expr(e), ptr_type(pt) {}
    virtual TokenType type() const override { return TokenType::ttBase; }
    virtual DataDef *datadef() const override { return ptr_type ? ptr_type : &ddVOID; }
};

// GNU computed-goto label address: `&&label`
class TokenLabelAddr: public TokenBase
{
public:
    std::string name;
    DataDef *ptr_type;
    TokenLabelAddr(const std::string &n, DataDef *pt) : name(n), ptr_type(pt) {}
    virtual TokenType type() const override { return TokenType::ttBase; }
    virtual TokenBase *clone() override { return new TokenLabelAddr(name, ptr_type); }
    virtual DataDef *datadef() const override { return ptr_type ? ptr_type : &ddVOID; }
};

class TokenExprContextObject: public TokenBase
{
public:
    std::string path;
    const madc::value *context_value;
    TokenExprContextObject(const std::string &p, const madc::value *v)
	: path(p), context_value(v) {}
    virtual TokenType type() const { return TokenType::ttBase; }
    virtual TokenBase *clone() { return new TokenExprContextObject(path, context_value); }
};

// *ptr dereference — reads/writes the value at the address held by a pointer
class TokenDeref: public TokenBase
{
public:
    Variable &var;
    DataDef *deref_type;  // pointed-to type
    TokenDeref(Variable &v, DataDef *dt) : var(v), deref_type(dt) { _datatype = dt; }
    virtual TokenType type() const override { return TokenType::ttMember; }  // reuse member type for assignment compat
    virtual DataDef *datadef() const override { return deref_type; }
};

// *(expr) dereference for cast/member/subscript pointer expressions
class TokenDerefExpr: public TokenBase
{
public:
    TokenBase *expr;
    DataDef *deref_type;
    TokenDerefExpr(TokenBase *e, DataDef *dt) : expr(e), deref_type(dt) { _datatype = dt; }
    virtual TokenType type() const override { return TokenType::ttMember; }
    virtual DataDef *datadef() const override { return deref_type; }
};

class TokenComplexPart: public TokenBase
{
public:
    TokenBase *expr;
    bool imag_part;

    TokenComplexPart(TokenBase *e, bool imag)
	: expr(e), imag_part(imag) {}

    virtual TokenType type() const override { return TokenType::ttMember; }
    virtual DataDef *datadef() const override
    {
	DataDef *expr_dd = expr ? expr->datadef() : NULL;
	if ( expr_dd && expr_dd->is_complex() )
	{
	    DataDefCOMPLEX *cdd = dynamic_cast<DataDefCOMPLEX *>(expr_dd);
	    return (cdd && cdd->element_type) ? cdd->element_type : expr_dd;
	}
	if ( imag_part )
	    return (expr_dd && expr_dd->is_real()) ? expr_dd : &ddINT64;
	return expr_dd ? expr_dd : &ddINT64;
    }
};

// *ptr++ / *ptr-- — dereference the current pointer value, then advance
// or rewind the pointer variable itself.
class TokenDerefStep: public TokenBase
{
public:
    Variable &var;
    DataDef *deref_type;
    bool increment;
    TokenDerefStep(Variable &v, DataDef *dt, bool inc)
        : var(v), deref_type(dt), increment(inc) { _datatype = dt; }
    virtual TokenType type() const override { return TokenType::ttBase; }
    virtual DataDef *datadef() const override { return deref_type; }
};

// (TYPE *) cast expression — type annotation, no codegen for pointer casts
class TokenCast: public TokenBase
{
public:
    DataDef *cast_type;   // target type
    TokenBase *expr;      // expression being cast
    TokenCast(DataDef *ct, TokenBase *e) : cast_type(ct), expr(e) {}
    virtual TokenType type() const override { return TokenType::ttBase; }
    virtual DataDef *datadef() const override { return cast_type; }
};

class TokenCallMethod: public TokenMember
{
public:
    TokenCallMethod(Variable &o, Variable &m) : TokenMember(o, m, 0) { _datatype = returns(); }
    virtual TokenType type() const { return TokenType::ttCallMethod; }
};

// subscript access: container[index]
class TokenSubscript: public TokenBase
{
public:
    Variable &object;    // the container variable
    TokenBase *index;    // the primary (first) index expression
    std::vector<TokenBase *> extra_indices; // additional indices for multi-dim fixed arrays
    Variable *tmp_var;   // temp string variable for string-returning subscripts (or NULL)

    TokenSubscript(Variable &o, TokenBase *idx, Variable *tmp = nullptr)
        : object(o), index(idx), tmp_var(tmp)
    {
        if ( o.is_fixed_array() )
            _datatype = o.type; // C fixed array: subscript yields element of base type
        else if ( o.type->is_pointer() )
        {
            // Raw pointer: ptr[i] == *(ptr + i). Element type = pointed-to type.
            DataDefPTR *pdd = dynamic_cast<DataDefPTR *>(o.type);
            _datatype = (pdd && pdd->base_type) ? pdd->base_type : &ddINT64;
        }
        else if ( o.type->type() == DataType::dtSIMD )
            _datatype = static_cast<DataDefSIMD *>(o.type)->element_type;
        else if ( DataDef *e = subscript_operator_element_type(o.type) )
            // A class with `T& operator[](...)` (a real madc template container
            // like vector<T>/map<K,V>/set<T>): the element type is the operator[]
            // return VALUE type (the base T — return_value_type() yields the
            // referent of a reference return). This is what lets `v[i].method()`
            // see a structured element.
            _datatype = e;
        else
            _datatype = &ddINT64; // madc array (madc::value): default to int
    }

    // The element type produced by a class object's `operator[]`, or NULL when
    // `dd` is not a class declaring operator[]. Static so the ctor can use it.
    static DataDef *subscript_operator_element_type(DataDef *dd)
    {
        if ( !dd || !dd->is_object() )
            return NULL;
        DataDefCLASS *cls = dynamic_cast<DataDefCLASS *>(dd);
        if ( !cls )
            return NULL;
        std::string opname = "operator[]";
        Variable *mv = cls->findMethod(opname);
        if ( !mv )
            return NULL;
	FuncDef *fd = dynamic_cast<FuncDef *>(mv->type);
	if ( !fd )
	    return NULL;
	return &fd->return_value_type();
    }
    virtual TokenType type() const override { return TokenType::ttSubscript; }
    virtual bool is_real() const override { return _datatype->is_real(); }
    // The element type is computed in the constructor for every container
    // kind (fixed array, pointer, string, SIMD, vector, map, madc array).
    // The asmjit codegen path used a helper to refine multi-dimensional
    // fixed-array indexing; that path is gone, so return the stored
    // element type directly. The CIR backend derives types independently.
    virtual DataDef *datadef() const override { return _datatype; }
};

class TokenSubscriptExpr: public TokenBase
{
public:
    TokenBase *base_expr;
    TokenBase *index;

    TokenSubscriptExpr(TokenBase *base, TokenBase *idx, DataDef *elem_type)
        : base_expr(base), index(idx)
    {
        _datatype = elem_type ? elem_type : &ddINT64;
    }
    virtual TokenType type() const override { return TokenType::ttSubscript; }
    virtual bool is_real() const override { return _datatype->is_real(); }
    // operand() must return an *lvalue* (Mem) — i.e. the address of
    // the element — not the loaded value. Callers (TokenAssign LHS,
    // outer TokenSubscriptExpr for chained 2D indexing, TokenMember
    // for s[i].field, etc.) need to compute offsets from the
    // element's address, and the default `compile()` path emits
    // emit_ir_value which loads through Mem and yields a Gp holding
    // the value — chaining through that returned a numeric value as
    // if it were an address and crashed at NULL/garbage.
};

class TokenProgram: public TokenCpnd
{
public:
    std::string source;
    std::istream *is;
    uint32_t lines;
    size_t bytes;
    TokenProgram() : TokenCpnd() { lines = 0; bytes = 0; is = NULL; }
    virtual TokenType type() const { return TokenType::ttProgram; }
};


// generic void function pointer
typedef void (*fVOIDFUNC)(void);

// maps
typedef madc::dis::intern_keyed_map<TokenKeyword *> keyword_map_t;
// The flat (current-scope) type-name map is interned: O(1) id-keyed lookup over
// a DEDICATED dense type-name pool (Program::type_name_pool), not the global
// strpool — type names are a tiny domain, so a dense pool keeps the
// intern_keyed_map _slot array small (sharing strpool would size it to the
// global spelling-id max). The namespace-owned inner maps keep std::string keys
// (datatype_map_t below) because they are enumerated by key; only the flat map
// is hot enough to intern. See docs/plans/2026-06-12-type-table-value-abi-design.md.
typedef madc::dis::intern_keyed_map<TokenDataType *> flat_datatype_map_t;
typedef std::map<std::string, TokenDataType *> datatype_map_t;
typedef std::map<std::string, DataDef *> datadef_map_t;

template<class Key, class Value>
class registration_map : public std::map<Key, Value>
{
    typedef std::map<Key, Value> base_type;
    struct SavedValue {
	Key key;
	bool existed;
	Value value;
	SavedValue(const Key &k, bool e, const Value &v)
	    : key(k), existed(e), value(v) {}
    };
public:
    struct transaction_state {
	std::vector<SavedValue> saved;
	std::set<Key> touched;
    };
private:
    transaction_state *transaction;

    void save(const Key &key)
    {
	if ( !transaction )
	    return;
	if ( !transaction->touched.insert(key).second )
	    return;
	typename base_type::const_iterator found = base_type::find(key);
	transaction->saved.push_back(SavedValue(
	    key, found != base_type::end(),
	    found == base_type::end() ? Value() : found->second));
    }
public:
    typedef typename base_type::iterator iterator;
    typedef typename base_type::const_iterator const_iterator;
    typedef typename base_type::value_type value_type;
    typedef typename base_type::size_type size_type;

    registration_map() : transaction(NULL) {}
    registration_map(const registration_map &other)
	: base_type(other), transaction(NULL) {}
    registration_map(const base_type &other)
	: base_type(other), transaction(NULL) {}
    registration_map &operator=(const registration_map &other)
    {
	if ( this != &other )
	{
	    assert(!transaction);
	    base_type::operator=(other);
	}
	return *this;
    }
    registration_map &operator=(const base_type &other)
    {
	assert(!transaction);
	base_type::operator=(other);
	return *this;
    }
    Value &operator[](const Key &key)
    {
	save(key);
	return base_type::operator[](key);
    }
    std::pair<iterator, bool> insert(const value_type &value)
    {
	save(value.first);
	return base_type::insert(value);
    }
    size_type erase(const Key &key)
    {
	save(key);
	return base_type::erase(key);
    }
    iterator erase(const_iterator position)
    {
	if ( position != base_type::end() )
	    save(position->first);
	return base_type::erase(position);
    }
    void clear()
    {
	if ( transaction )
	    for ( const_iterator it = base_type::begin();
		  it != base_type::end(); ++it )
		save(it->first);
	base_type::clear();
    }
    void begin_transaction(transaction_state &state)
    {
	assert(!transaction);
	state.saved.clear();
	state.touched.clear();
	transaction = &state;
    }
    void commit_transaction(transaction_state &state)
    {
	assert(transaction == &state);
	transaction = NULL;
	state.saved.clear();
	state.touched.clear();
    }
    void rollback_transaction(transaction_state &state)
    {
	assert(transaction == &state);
	transaction = NULL;
	for ( size_t i = state.saved.size(); i-- > 0; )
	{
	    const SavedValue &saved = state.saved[i];
	    if ( saved.existed )
		base_type::operator[](saved.key) = saved.value;
	    else
		base_type::erase(saved.key);
	}
	state.saved.clear();
	state.touched.clear();
    }
    bool transaction_active() const { return transaction != NULL; }
};

template<class Key>
class registration_set : public std::set<Key>
{
    typedef std::set<Key> base_type;
public:
    struct transaction_state {
	std::vector<Key> changed;
	std::set<Key> existed;
	std::set<Key> touched;
    };
private:
    transaction_state *transaction;

    void save(const Key &key)
    {
	if ( !transaction || !transaction->touched.insert(key).second )
	    return;
	transaction->changed.push_back(key);
	if ( base_type::count(key) )
	    transaction->existed.insert(key);
    }
public:
    typedef typename base_type::iterator iterator;
    typedef typename base_type::const_iterator const_iterator;
    typedef typename base_type::value_type value_type;
    typedef typename base_type::size_type size_type;

    registration_set() : transaction(NULL) {}
    registration_set(const registration_set &other)
	: base_type(other), transaction(NULL) {}
    registration_set &operator=(const registration_set &other)
    {
	if ( this != &other )
	{
	    assert(!transaction);
	    base_type::operator=(other);
	}
	return *this;
    }
    std::pair<iterator, bool> insert(const Key &key)
    {
	save(key);
	return base_type::insert(key);
    }
    std::pair<iterator, bool> insert(Key &&key)
    {
	save(key);
	return base_type::insert(std::move(key));
    }
    iterator insert(const_iterator hint, const Key &key)
    {
	save(key);
	return base_type::insert(hint, key);
    }
    iterator insert(const_iterator hint, Key &&key)
    {
	save(key);
	return base_type::insert(hint, std::move(key));
    }
    template<class InputIterator>
    void insert(InputIterator first, InputIterator last)
    {
	for ( ; first != last; ++first )
	    insert(*first);
    }
    void insert(std::initializer_list<value_type> values)
    {
	insert(values.begin(), values.end());
    }
    template<class... Args>
    std::pair<iterator, bool> emplace(Args&&... args)
    {
	Key key(std::forward<Args>(args)...);
	return insert(std::move(key));
    }
    template<class... Args>
    iterator emplace_hint(const_iterator hint, Args&&... args)
    {
	Key key(std::forward<Args>(args)...);
	return insert(hint, std::move(key));
    }
    size_type erase(const Key &key)
    {
	save(key);
	return base_type::erase(key);
    }
    iterator erase(const_iterator position)
    {
	if ( position != base_type::end() )
	    save(*position);
	return base_type::erase(position);
    }
    iterator erase(const_iterator first, const_iterator last)
    {
	for ( const_iterator it = first; it != last; ++it )
	    save(*it);
	return base_type::erase(first, last);
    }
    void clear()
    {
	if ( transaction )
	    for ( const_iterator it = base_type::begin();
		  it != base_type::end(); ++it )
		save(*it);
	base_type::clear();
    }
    void swap(registration_set &other)
    {
	if ( this == &other )
	    return;
	for ( const_iterator it = base_type::begin();
	      it != base_type::end(); ++it )
	{
	    save(*it);
	    other.save(*it);
	}
	for ( const_iterator it = other.begin(); it != other.end(); ++it )
	{
	    save(*it);
	    other.save(*it);
	}
	base_type::swap(other);
    }
    friend void swap(registration_set &left, registration_set &right)
    {
	left.swap(right);
    }
    void begin_transaction(transaction_state &state)
    {
	assert(!transaction);
	state.changed.clear();
	state.existed.clear();
	state.touched.clear();
	transaction = &state;
    }
    void commit_transaction(transaction_state &state)
    {
	assert(transaction == &state);
	transaction = NULL;
	state.changed.clear();
	state.existed.clear();
	state.touched.clear();
    }
    void rollback_transaction(transaction_state &state)
    {
	assert(transaction == &state);
	transaction = NULL;
	for ( size_t i = state.changed.size(); i-- > 0; )
	    if ( state.existed.count(state.changed[i]) )
		base_type::insert(state.changed[i]);
	    else
		base_type::erase(state.changed[i]);
	state.changed.clear();
	state.existed.clear();
	state.touched.clear();
    }
};

typedef registration_map<std::string, FuncDef *> funcdef_map_t;
typedef registration_map<std::string, Variable *> variable_map_t;
typedef std::map<std::string, variable_map_t> namespace_map_t;
// Outer map (namespace -> inner type-name map) on the substrate primitive,
// keyed via Program::namespace_name_pool. `::iterator` is datatype_map_t* now
// (find() returns a pointer to the inner map; end()==nullptr). The INNER
// datatype_map_t stays std::map because it is enumerated by key. NOTE: the
// inner maps live in the primitive's value VECTOR, so an outer operator[]
// insert can relocate them — a held outer pointer must be re-fetched after an
// insert; inner iterators survive (std::map move-construction keeps nodes).
typedef madc::dis::intern_keyed_map<datatype_map_t> namespace_datatype_map_t;

// map-iterators
typedef keyword_map_t::iterator keyword_map_iter;
typedef flat_datatype_map_t::iterator flat_datatype_map_iter;	// TokenDataType ** (NULL = absent)
typedef std::map<std::string, TokenDataType *>::iterator datatype_map_iter;
typedef std::map<std::string, DataDef *>::iterator datadef_map_iter;
typedef std::map<std::string, DataDef *>::const_iterator datadef_map_citer;
typedef funcdef_map_t::iterator funcdef_map_iter;
typedef variable_map_t::iterator variable_map_iter;

// struct_map + its despaced-canonical lookup index as ONE object: set() is the
// map's only write channel, so the index cannot silently desync from the map.
// resolve_arg_spelling_datadef's fallback (template-id spellings are keyed in
// struct_map by MANGLED name, so they resolve by matching
// despace(strip_ns(canonical_cpp_spelling))) used to linearly re-despace every
// entry per query; find_despaced serves the same first-hit-in-key-order answer
// from an incrementally maintained index. Invalidation channels:
//   growth                          -> size stamp; unseen nodes topped up
//                                      (the map never erases)
//   value repoint at existing key   -> set() bumps DataDef::canonical_spelling_gen
//   spelling rewrite on a swept dd  -> DataDef::set_canonical_spelling bumps it
// A gen mismatch rebuilds from scratch (the size-stamp reset forces the resweep).
class StructRegistry
{
public:
    typedef datadef_map_t::const_iterator const_iterator;
    struct transaction_state {
	struct SavedValue {
	    std::string key;
	    bool existed;
	    DataDef *value;
	    SavedValue(const std::string &k, bool e, DataDef *v)
		: key(k), existed(e), value(v) {}
	};
	std::vector<SavedValue> saved;
	std::set<std::string> touched;
    };
    ~StructRegistry();				// MADC_DESPACE_PROBE counter dump
    const_iterator begin() const { return map_.begin(); }
    const_iterator end() const { return map_.end(); }
    const_iterator find(const std::string &k) const { return map_.find(k); }
    size_t count(const std::string &k) const { return map_.count(k); }
    size_t size() const { return map_.size(); }
    bool empty() const { return map_.empty(); }
    void set(const std::string &key, DataDef *dd);
    datadef_map_t snapshot() const { return map_; }
    void restore(const datadef_map_t &entries);
    void begin_transaction(transaction_state &state);
    void commit_transaction(transaction_state &state);
    void rollback_transaction(transaction_state &state);
    // First entry (in key order) whose despaced, namespace-stripped canonical
    // spelling equals `want` — exactly the old linear scan's answer.
    DataDef *find_despaced(const std::string &want);
private:
    struct Hit { const std::string *key; DataDef *dd; };
    datadef_map_t map_;
    transaction_state *transaction_ = NULL;
    std::unordered_map<std::string, Hit> index_;
    std::unordered_set<const void *> seen_;	// map-node key addrs (map never erases)
    size_t size_stamp_ = 0;
    uint64_t gen_stamp_ = 0;
    // MADC_DESPACE_PROBE counters (always maintained — increments are noise)
    uint64_t probe_lookups_ = 0, probe_rebuilds_ = 0, probe_swept_ = 0;
};

// A builtin-registration type selector: names a parameter/return type for
// addFunction() either by a DataType enum word or directly by a DataDef*. Both
// constructors are implicit so existing `datatype_vec_t{dtX,...}` call sites are
// unchanged, while class types can be named by their actual DataDef. When `dd`
// is set it wins; otherwise `dt` is resolved by the existing DataType_to_dd
// switch.
struct typespec_t
{
    DataType  dt;
    DataDef  *dd;
    RefType   ref;   // when dd!=NULL: rtValue, rtPointer (T*), or rtReference (T&)
    typespec_t(DataType t) : dt(t), dd(nullptr), ref(RefType::rtValue) {}
    typespec_t(DataDef *d) : dt(DataType::dtVOID), dd(d), ref(RefType::rtValue) {}
    typespec_t(DataDef &d) : dt(DataType::dtVOID), dd(&d), ref(RefType::rtValue) {}
    typespec_t(DataDef *d, RefType r) : dt(DataType::dtVOID), dd(d), ref(r) {}
};

inline typespec_t ptr_of(DataDef &d) { return typespec_t(&d, RefType::rtPointer); }
inline typespec_t ref_of(DataDef &d) { return typespec_t(&d, RefType::rtReference); }

// vectors
typedef std::vector<typespec_t> datatype_vec_t;
typedef std::vector<Variable *> variable_vec_t;

// vector iterators
typedef std::vector<DataDef *>::iterator datadef_vec_iter;
typedef std::vector<Variable *>::iterator variable_vec_iter;
//typedef std::vector<CodeBlock *>::iterator codeblock_vec_iter;
typedef std::vector<TokenBase *>::iterator tokenbase_vec_iter;


// class to hold source for lexing
// embedded header lookup (generated by scripts/gen_embedded_headers.sh)
const std::string *find_embedded_header(const std::string &name);

// Is `name` one of the compiler type-trait intrinsics madc implements
// (__is_class, __has_trivial_destructor, …)? The registry lives in parser.cpp
// beside the traits' sema; it is declared here because __has_builtin must
// answer from the SAME registry — a standard library that guards a trait with
// `#if __has_builtin(__has_trivial_destructor)` must be told the truth about
// what madc actually implements. One registry, two consumers.
bool is_type_trait_builtin(const std::string &name);

class Source
{
protected:
    struct PushbackFrame
    {
	size_t remaining;
	std::string disabled_macro;
	bool recount = true;   // false: text was already read once; re-reading
			       // it must not re-advance the column counter
    };
    // Flat in-memory source buffer + read cursor. The entire input (file or
    // string) is slurped ONCE into _buf; get()/peek()/good()/eof() are then
    // plain index ops — no per-char std::istream sentry/locale overhead. The old
    // std::stringstream backing made istream::sentry/get/peek + Source::get/peek
    // the dominant lex self-cost in callgrind (P2 perf lever, 2026-06-23).
    std::string _buf;			// entire source text
    size_t _gpos = 0;			// read cursor into _buf
    std::string _pushback;		// pushback buffer for #define substitution
    std::deque<PushbackFrame> _pushback_frames;
    int _lf, _cr, _column;
    std::string _fname;
    void add_pushback_frame(const std::string &s, const std::string &disabled_macro,
			    bool recount = true)
    {
	if ( s.empty() )
	    return;
	PushbackFrame frame;
	frame.remaining = s.size();
	frame.disabled_macro = disabled_macro;
	frame.recount = recount;
	_pushback_frames.push_front(frame);
    }
public:
    Source() { _lf = 0; _cr = 0; _column = 0; }
    const char *fname() const { return _fname.c_str(); }
    const char *fname(const char *s)  { _fname = s; return _fname.c_str(); }
    const char *fname(std::string &s) { _fname = s; return _fname.c_str(); }
    void copybuf(std::streambuf *sb)  { std::ostringstream tmp; tmp << sb; _buf = tmp.str(); _gpos = 0; }
    void str(const std::string &s) { _buf = s; _gpos = 0; }
    void pushback(const std::string &s) { _pushback = s + _pushback; add_pushback_frame(s, ""); }
    // Push back text that was ALREADY read (lexer lookahead/backtrack). Those
    // source characters were already counted on the first read, so re-reading
    // them must not re-advance the column counter (contrast pushback(), used
    // for synthesized text like macro expansions, which does count).
    void pushback_reread(const std::string &s) { _pushback = s + _pushback; add_pushback_frame(s, "", false); }
    void pushback_macro(const std::string &s, const std::string &disabled_macro)
    {
	_pushback = s + _pushback;
	add_pushback_frame(s, disabled_macro);
    }
    bool macro_disabled(const std::string &name) const
    {
	for ( const PushbackFrame &frame : _pushback_frames )
	    if ( frame.disabled_macro == name )
		return true;
	return false;
    }
    // Active macro-expansion nesting depth (one frame per live expansion).
    // A runaway recursive macro grows this without bound; the lexer guards on
    // it to fail with a clean diagnostic instead of crashing the stack.
    size_t pushback_depth() const { return _pushback_frames.size(); }
    bool good() { return !_pushback.empty() || _gpos < _buf.size(); }
    bool eof()  { return _pushback.empty() && _gpos >= _buf.size(); }
    int line()  { if ( _lf > _cr ) return _lf+1; return _cr+1; }
    int column(){ return _column ? _column : 1; }
    int get()
    {
	if ( !_pushback.empty() )
	{
	    // Delayed-pop blue paint. A macro-expansion frame whose text was fully
	    // consumed on a PRIOR get() is dropped HERE, on the next read — NOT the
	    // instant its last char was taken. That keeps the macro "disabled"
	    // (macro_disabled()) while the token its expansion produced is re-lexed
	    // and macro-checked, so a self-referential `#define A A` (or mutual
	    // `A`<->`B`) expands once and stops instead of recursing to a crash.
	    while ( !_pushback_frames.empty() && _pushback_frames.front().remaining == 0 )
		_pushback_frames.pop_front();
	    int ch = (unsigned char)_pushback[0];
	    _pushback.erase(0, 1);
	    bool recount = true;
	    if ( !_pushback_frames.empty() )
	    {
		recount = _pushback_frames.front().recount;
		if ( _pushback_frames.front().remaining > 0 )
		    --_pushback_frames.front().remaining;
	    }
	    if ( recount )
		++_column;
	    return ch;
	}
	// Pushback drained: any frames left are fully-consumed expansions — drop
	// them so a later, independent use of the same macro can expand again.
	if ( !_pushback_frames.empty() )
	    _pushback_frames.clear();
	if ( _gpos >= _buf.size() ) return -1;
	int ch = (unsigned char)_buf[_gpos++];
	// C line splice: backslash + optional trailing whitespace + newline
	if ( ch == '\\' )
	{
	    size_t splice_start = _gpos;
	    // skip optional trailing spaces/tabs after backslash
	    while ( _gpos < _buf.size() && (_buf[_gpos] == ' ' || _buf[_gpos] == '\t') )
		++_gpos;
	    int next = _gpos < _buf.size() ? (unsigned char)_buf[_gpos] : -1;
	    if ( next == '\n' || next == '\r' )
	    {
		++_gpos;
		if ( next == '\r' && _gpos < _buf.size() && _buf[_gpos] == '\n' )
		    ++_gpos;
		++_lf; _column = 0;
		return get(); // recurse to get next real char
	    }
	    // not a line splice — rewind
	    _gpos = splice_start;
	}
	/**/ if ( ch == '\n' ) { ++_lf; _column = 0; }
	else if ( ch == '\r' ) { ++_cr; _column = 0; }
	else { ++_column; }
	return ch;
    }
    int peek()
    {
	if ( !_pushback.empty() )
	    return (unsigned char)_pushback[0];
	if ( _gpos >= _buf.size() ) return -1;
	// C line splice: skip backslash + optional whitespace + newline in peek
	int ch = (unsigned char)_buf[_gpos];
	if ( ch == '\\' )
	{
	    size_t saved = _gpos;
	    ++_gpos; // consume '\'
	    // skip optional trailing spaces/tabs
	    while ( _gpos < _buf.size() && (_buf[_gpos] == ' ' || _buf[_gpos] == '\t') )
		++_gpos;
	    int next = _gpos < _buf.size() ? (unsigned char)_buf[_gpos] : -1;
	    if ( next == '\n' || next == '\r' )
	    {
		// There IS a line splice — consume it and peek the real char
		++_gpos;
		if ( next == '\r' && _gpos < _buf.size() && _buf[_gpos] == '\n' )
		    ++_gpos;
		++_lf; _column = 0;
		ch = _gpos < _buf.size() ? (unsigned char)_buf[_gpos] : -1;
		return ch;
	    }
	    // Not a line splice — rewind
	    _gpos = saved;
	}
	return ch;
    }
    // Fast-path identifier-continuation scan (perf lever, 2026-06-23). When NOT
    // inside a pushback/macro expansion, scan the maximal identifier-continuation
    // run directly off the flat buffer and hand it back as a [base,len) span,
    // advancing the cursor + column in ONE step — replacing the per-char
    // get()/peek() (with their pushback/line-splice branches) and the char-by-char
    // std::string growth on the hot path. Returns false when a pushback frame is
    // active (the caller's char loop handles macro-expanded text). A line-splice
    // mid-run stops the span at the '\\' (not an identifier char); the caller's
    // char loop then reads the spliced remainder (peek()/get() resolve the splice).
    // The classification is the SAME `isalnum||'_'` as the slow loop, so the two
    // paths are byte-identical. `len` may be 0 (1-char identifier, or an immediate
    // splice) — still a valid fast-path result; the caller's loop continues.
    bool fast_ident_span(const char *&base, size_t &len)
    {
	if ( !_pushback.empty() ) return false;
	size_t start = _gpos;
	while ( _gpos < _buf.size() )
	{
	    unsigned char c = (unsigned char)_buf[_gpos];
	    if ( c == '_' || isalnum(c) ) ++_gpos;
	    else break;
	}
	len = _gpos - start;
	base = _buf.data() + start;
	_column += (int)len;
	return true;
    }
    bool getline(std::string &s)
    {
	int ch = -1;
	s.clear();
	while ( _gpos < _buf.size() && (ch=(unsigned char)_buf[_gpos]) != '\r' && ch != '\n' )
	{ s += (char)ch; ++_gpos; }
	if ( _gpos >= _buf.size() ) { return !s.empty(); }
	++_gpos; // consume the \r or \n terminator
	/**/ if ( ch == '\n' ) { ++_lf; _column = 0; }
	else if ( ch == '\r' ) { ++_cr; _column = 0; }
	if ( _gpos < _buf.size() && _buf[_gpos] == '\n' )
	{
	    ++_gpos;
	    ++_lf;
	}
	return !s.empty();
    }
    void setpos(int row, int col) { _lf = _cr = (row-1); _column = col; }
    void showerror(int row=0, int col=0);
};

// Diagnostic source-echo helpers (lexer.cpp). show_error_source_line is the
// one formatter for the offending-line + caret display; madc_show_file_error
// rereads a non-live file (an #included header) from disk on the cold
// diagnostic path and returns false when it cannot echo faithfully.
void show_error_source_line(const std::string &ln, int col);
bool madc_show_file_error(const char *fname, int row, int col);

// very simple exception container
class Exception: public std::exception
{
protected:
    std::string _msg;
public:
    explicit Exception(const std::string& message): _msg(message) {}
    virtual ~Exception() throw() {}
    virtual const char *what() const throw () { return _msg.c_str(); }
};

// streambuf class to throw an exception at sync
class throwbuf: public std::stringbuf
{
protected:
    TokenBase *_tb;
    Source *_src;
public:
    throwbuf() : std::stringbuf() { _tb = NULL; _src = NULL; }
    virtual int sync();
    TokenBase *token() { return _tb; }
    TokenBase *token(TokenBase *t) { return (_tb=t); }
    Source *source() { return _src; }
    Source *source(Source *s) { return (_src=s); }
};

class throwstream: public std::ostream
{
protected:
    throwbuf _tbuf;
public:
    throwstream() : std::ostream(&_tbuf) { exceptions(std::ios_base::badbit); }
    TokenBase *token() { return _tbuf.token(); };
    Source *source() { return _tbuf.source(); }
    Source *source(Source *s) { return _tbuf.source(s); }
    Source *source(Source &s) { return _tbuf.source(&s); }
    std::string str() const { return _tbuf.str(); }
    // Reset state + buffer at the START of each error. The stream has
    // exceptions(badbit); a prior `Throw(..) << .. << flush` leaves badbit set
    // after it throws, so WITHOUT this clear() the next `Throw(t) << "msg"`
    // re-throws ios_failure AT THE `<<` (before the message is built) — turning
    // every error after the first into a spurious "basic_ios::clear: iostream
    // error" and discarding the real message. str("") drops any stale message.
    throwstream& operator()(TokenBase *t)
    { clear(); _tbuf.str(std::string()); _tbuf.token(t); return *this; }
};


// Classification of a declared C++ class member for Itanium symbol emission.
enum class CppSymKind { Method, Ctor, Dtor };

// A parsed parameter signature (resolved base type + ref/const/pointer-depth),
// used when matching an upcoming parameter list against candidate overloads.
struct ParsedParamSig
{
    DataDef *base;
    bool is_ref;
    bool is_const;
    int pointer_depth;
    ParsedParamSig() : base(NULL), is_ref(false), is_const(false), pointer_depth(0) {}
};

// ---------------------------------------------------------------------------
// TokenStream — the parser's token feed.
//
// P1 of the front-end-performance plan
// (docs/plans/2026-06-22-front-end-performance-plan.md). Replaces the old
// `std::deque<TokenBase *>` with a flat, contiguous buffer + an integer read
// cursor, so the hot operations are O(1) index moves with no allocation:
//   - consume (nextToken)    -> ++_cursor
//   - peek    (peekToken)    -> _buf[_cursor]
//   - backtrack save/restore -> copy {cursor, pushback}, NOT the whole stream.
//        This is the ~43%-inclusive `std::deque` copy-ctor the profile flagged
//        (`saved = tokens` / `tokens = saved`); it is now an int + a tiny vec.
//   - inject  (pushToken)    -> a small LIFO pushback stack, read before _buf.
//   - sub-stream (tokens=body) -> swap_in/swap_back, by MOVE (no copy).
//
// The logical token sequence is:   reverse(_pushback) ++ _buf[_cursor .. end].
// Every accessor honours that, so a pushed-back token is seen at the front and
// by index exactly as `push_front` into the old deque was.
//
// Copy is DELETED on purpose: every old `deque` copy site must be rewritten to
// one of the cheap primitives above; the compiler enumerates them.
//
// PULL-BASED-LEXER (P2) COMPATIBILITY — this IS the arena P2 appends into:
//   * Production is `push_back`, valid at ANY cursor position. P2 makes the
//     lexer produce on demand and `push_back` into this same `_buf` while the
//     parser cursors forward; consumed tokens stay in `_buf` for re-read, so
//     backtrack remains index-rewind. P1 and P2 are complementary by design.
//
// STEP 2.2b: `_buf` now stores uint32 SLOT-IDS (not TokenBase*). The public API
// is unchanged — it materializes the shell via the arena slot registry at the
// boundary (front/operator[]/back/iterators), and push_back/swap_in register the
// slot-id (madc_slot_id_for). `_pushback` stays TokenBase* (a small transient
// injection LIFO). Shells persist (transitional, plan §1e); the registry is the
// cache. See docs/plans/2026-06-23-p1-token-arena-implementation-plan.md Phase 2.
//   * `nextToken()` (in Program) is the single consume chokepoint; P2 replaces
//     its end-of-stream throw with "pull one token from the lexer and append".
//   * The ONE place that assumes a fully-pre-lexed buffer is the auto-include
//     reorder (begin/end/rbegin/erase/insert/swap over the whole `_buf` while
//     `_cursor==0`). That is P2's revisit point — under pull-based it must only
//     touch un-consumed tokens. P1 keeps it as-is (full-tokenize, cursor==0).
// ---------------------------------------------------------------------------
// Token-arena slot registry bridge (defined in parser.cpp; also declared in
// token_arena.h). Forward-declared here so TokenStream's inline accessors can
// map slot-id <-> TokenBase* without pulling the whole arena header.
uint32_t   madc_slot_id_for(TokenBase *t);
TokenBase *madc_token_for_slot(uint32_t id);

class TokenStream
{
    // STEP 2: the flat arena + cursor. _buf is the contiguous token table;
    // _cursor is the read position; _pushback is a small LIFO of injected
    // tokens. Logical sequence = reverse(_pushback) ++ _buf[_cursor..]. Backtrack
    // is a cursor rewind (savepos/restore copies {cursor, pushback}, never the
    // buffer) — that is the ~43%-inclusive deque copy the profile flagged.
    // (Step 1 proved the call sites behavior-neutral with a deque underneath;
    // this step changes ONLY these method bodies.)
    std::vector<uint32_t>	_buf;	  // slot-ids (2.2b) — materialized at boundary
    size_t			_cursor;
    std::vector<TokenBase *> _pushback;	  // transient injection LIFO (stays pointers)
public:
    TokenStream() : _cursor(0) {}
    TokenStream(const TokenStream &) = delete;
    TokenStream &operator=(const TokenStream &) = delete;

    // -- lexer production (append; valid at any cursor — see P2 note above) --
    void push_back(TokenBase *t) { _buf.push_back(madc_slot_id_for(t)); }
    TokenBase *back() const { return madc_token_for_slot(_buf.back()); }

    // -- size / emptiness over the logical remaining sequence --
    bool empty() const { return _pushback.empty() && _cursor >= _buf.size(); }
    size_t size() const { return _pushback.size() + (_buf.size() - _cursor); }

    // -- front / consume / inject (parser hot path) --
    TokenBase *front() const
    {
	if ( !_pushback.empty() ) return _pushback.back();
	return _cursor < _buf.size() ? madc_token_for_slot(_buf[_cursor]) : NULL;
    }
    void pop_front()
    {
	if ( !_pushback.empty() ) { _pushback.pop_back(); return; }
	++_cursor;
    }
    void push_front(TokenBase *t) { _pushback.push_back(t); }

    // -- random access from the logical front (lookahead / introspection) --
    TokenBase *operator[](size_t i) const
    {
	size_t pb = _pushback.size();
	if ( i < pb ) return _pushback[pb - 1 - i];
	return madc_token_for_slot(_buf[_cursor + (i - pb)]);
    }

    // -- consumed-range introspection (forest default-arg capture): consumed
    //    buffer tokens stay in _buf, so [cursor-before, cursor-after) of a
    //    sub-parse IS the raw source token range it consumed — provided the
    //    pushback LIFO is empty at both ends (an injected token is not a
    //    buffer position). buf_at() indexes the BUFFER directly (not the
    //    logical front like operator[]). --
    size_t cursor() const { return _cursor; }
    size_t pushback_size() const { return _pushback.size(); }
    // Read-only view of the injection LIFO (bottom..top) — the v26 default-arg
    // capture snapshots it at begin so tokens consumed FROM the pushback (a
    // template-instantiation replay) can be reconstructed as popped entries.
    const std::vector<TokenBase *> &pushback_ref() const { return _pushback; }
    TokenBase *buf_at(size_t i) const
    {
	return i < _buf.size() ? madc_token_for_slot(_buf[i]) : NULL;
    }

    // -- backtrack: save/restore is {cursor, pushback} — NEVER the buffer --
    struct Pos { size_t cursor; std::vector<TokenBase *> pushback; };
    Pos savepos() const { Pos p; p.cursor = _cursor; p.pushback = _pushback; return p; }
    void restore(const Pos &p) { _cursor = p.cursor; _pushback = p.pushback; }
    // Sugar so the original `tokens = saved` restore idiom reads unchanged.
    TokenStream &operator=(const Pos &p) { restore(p); return *this; }

    // -- sub-stream: install `seq` as the active buffer (moved), returning the
    //    prior state (moved); swap_back restores it. No copy. Models the old
    //    `saved = tokens; tokens = body; ...; tokens = saved;` idiom. --
    struct State { std::vector<uint32_t> buf; size_t cursor; std::vector<TokenBase *> pushback; };
    State swap_in(std::vector<TokenBase *> seq)
    {
	State prev;
	prev.buf.swap(_buf); prev.cursor = _cursor; prev.pushback.swap(_pushback);
	_buf.clear(); _buf.reserve(seq.size());
	for ( TokenBase *t : seq ) _buf.push_back(madc_slot_id_for(t));
	_cursor = 0; _pushback.clear();
	return prev;
    }
    void swap_back(State prev)
    {
	_buf.swap(prev.buf); _cursor = prev.cursor; _pushback.swap(prev.pushback);
    }
    // Sugar so the body-replace restore `tokens = saved` reads unchanged too
    // (mirrors operator=(Pos)); consumes the saved State.
    TokenStream &operator=(State &s) { swap_back(std::move(s)); return *this; }

    // -- replace the next `remove` LOGICAL tokens at the front with `ins`:
    //    drain the pushback then advance the cursor, then inject `ins` reversed
    //    so ins[0] is read first. Cursor-aware front splice. --
    void splice_front(size_t remove, const std::vector<TokenBase *> &ins)
    {
	while ( remove && !_pushback.empty() ) { _pushback.pop_back(); --remove; }
	_cursor += remove;
	for ( size_t i = ins.size(); i > 0; --i )
	    _pushback.push_back(ins[i - 1]);
    }

    // -- lexer reorder support (auto-include; runs at _cursor==0, _pushback
    //    empty — P2's pull-based lexer revisits this so it only touches
    //    un-consumed tokens) --
    // Materializing READ iterator over the slot-id buffer: dereferences a slot-id
    // to its TokenBase* via the registry, so the read-only scans over the stream
    // (`for (TokenBase *t : tokens)`, reverse decl-head walks) are unchanged.
    // Buffer MUTATION goes through the explicit id-level helpers below — never
    // through iterators (2.2b removed the vector<TokenBase*> erase/insert/swap).
    class const_iterator
    {
	const uint32_t *_p;
    public:
	typedef std::ptrdiff_t difference_type;
	typedef TokenBase *value_type;
	typedef TokenBase *reference;
	typedef void pointer;
	typedef std::random_access_iterator_tag iterator_category;
	const_iterator(const uint32_t *p = NULL) : _p(p) {}
	TokenBase *operator*() const { return madc_token_for_slot(*_p); }
	const_iterator &operator++() { ++_p; return *this; }
	const_iterator &operator--() { --_p; return *this; }
	const_iterator operator++(int) { const_iterator t(*this); ++_p; return t; }
	const_iterator operator--(int) { const_iterator t(*this); --_p; return t; }
	const_iterator &operator+=(difference_type d) { _p += d; return *this; }
	const_iterator operator+(difference_type d) const { return const_iterator(_p + d); }
	const_iterator operator-(difference_type d) const { return const_iterator(_p - d); }
	difference_type operator-(const const_iterator &o) const { return _p - o._p; }
	bool operator==(const const_iterator &o) const { return _p == o._p; }
	bool operator!=(const const_iterator &o) const { return _p != o._p; }
    };
    typedef std::reverse_iterator<const_iterator> const_reverse_iterator;
    const_iterator begin() const { return const_iterator(_buf.data()); }
    const_iterator end()   const { return const_iterator(_buf.data() + _buf.size()); }
    const_reverse_iterator rbegin() const { return const_reverse_iterator(end()); }
    const_reverse_iterator rend()   const { return const_reverse_iterator(begin()); }

    // id-level buffer mutation (auto-include reorder; runs at cursor==0, pushback
    // empty). Replaces the former vector<TokenBase*> swap / erase+insert idiom.
    void assign_ids_from(std::vector<TokenBase *> &toks)
    {
	_buf.clear();
	_buf.reserve(toks.size());
	for ( TokenBase *t : toks )
	    _buf.push_back(madc_slot_id_for(t));
    }
    void move_tail_to(size_t from, size_t insert_at)
    {
	if ( from >= _buf.size() || insert_at > from )
	    return;
	std::vector<uint32_t> tail(_buf.begin() + from, _buf.end());
	_buf.erase(_buf.begin() + from, _buf.end());
	_buf.insert(_buf.begin() + insert_at, tail.begin(), tail.end());
    }

    // -- tokens popped from the logical FRONT since `before` (callers that only
    //    pop the front, e.g. skip_requires_clause), reconstructed WITHOUT copying
    //    the whole stream: the popped pushback entries (LIFO — logical front is
    //    the back of the vector) then the _buf range the cursor advanced over.
    //    Cost is O(consumed), not O(stream). Replaces a former full-stream
    //    logical_snapshot() copy that made per-`template<>` parsing O(n^2)
    //    (snapshot × every template declaration). --
    std::vector<TokenBase *> consumed_since(const Pos &before) const
    {
	std::vector<TokenBase *> out;
	for ( size_t i = before.pushback.size(); i > _pushback.size(); --i )
	    out.push_back(before.pushback[i - 1]);
	for ( size_t i = before.cursor; i < _cursor; ++i )
	    out.push_back(madc_token_for_slot(_buf[i]));
	return out;
    }
};

// program class, keep things somewhat contained
class Program
{
public:
    struct FunctionRegistrationSpec
    {
	std::string id;
	datatype_vec_t params;
	fVOIDFUNC extfunc;
	bool is_method;
    };

    struct BuiltinRegistry
    {
	bool defaults_loaded = false;
	std::vector<FunctionRegistrationSpec> core_functions;
	std::vector<FunctionRegistrationSpec> process_functions;
	std::vector<FunctionRegistrationSpec> dlfcn_functions;

	void add_core_function(const std::string &id, const datatype_vec_t &params, fVOIDFUNC extfunc, bool is_method=false);
	void add_process_function(const std::string &id, const datatype_vec_t &params, fVOIDFUNC extfunc, bool is_method=false);
	void add_dlfcn_function(const std::string &id, const datatype_vec_t &params, fVOIDFUNC extfunc, bool is_method=false);
    };

    typedef void (*namespace_init_fn_t)(Program &);

    struct NamespaceRegistrationSpec
    {
	std::string name;
	namespace_init_fn_t init;
    };

    struct NamespaceRegistry
    {
	bool defaults_loaded = false;
	std::vector<NamespaceRegistrationSpec> specs;

	void add_namespace(const std::string &name, namespace_init_fn_t init);
    };

    struct RegistrationPolicy
    {
	struct RuntimeEvalChildPolicy
	{
	    bool enable_core_functions = true;
	    bool enable_process_functions = true;
	    bool enable_dlfcn_functions = true;
	    bool enable_std_namespace = true;
	    bool enable_madc_namespace = true;
	    bool enable_php_namespace = true;
	    bool enable_perl_namespace = true;
	    bool enable_python_namespace = true;
	    bool enable_ruby_namespace = true;
	    bool enable_js_namespace = true;
	    bool enable_rust_namespace = true;
	    bool restrict_headers_to_allowlist = false;
	    bool restrict_dlfcn_symbols_to_allowlist = false;
	    std::vector<std::string> allowed_headers;
	    std::vector<std::string> allowed_dlfcn_symbols;
	};

	bool enable_core_functions = true;
	bool enable_process_functions = true;
	bool enable_dlfcn_functions = true;
	// When false, a #load directive does not dlopen its named library; the
	// namespace is bound to the global symbol scope instead, so the symbols
	// must be provided explicitly (e.g. via `madc -l<lib>` or the host). Lets
	// a build make all linking explicit. (enable_dlfcn_functions=false is the
	// stricter sandbox knob that forbids #load outright.)
	bool enable_auto_library_loading = true;
	bool enable_runtime_eval_source_scope_access = true;
	bool enable_runtime_eval_expression_scope_access = true;
	bool enable_std_namespace = true;
	bool enable_madc_namespace = true;
	bool enable_php_namespace = true;
	bool enable_perl_namespace = true;
	bool enable_python_namespace = true;
	bool enable_ruby_namespace = true;
	bool enable_js_namespace = true;
	bool enable_rust_namespace = true;
	bool restrict_headers_to_allowlist = false;
	bool restrict_dlfcn_symbols_to_allowlist = false;
	// Header partition (madc-header-partition-handoff.md): when true, an
	// embedded baked-in header is bypassed in favor of the REAL system header
	// IFF it is a system-library shim (glibc/libstdc++ twin). madc-own headers
	// (no real twin: ns_php/__madc__/…) and compiler-owned freestanding headers
	// (stddef/limits/float, resolved in the gcc-internal dir) stay embedded.
	// This is the incremental shim-retirement lever — bypass system shims while
	// keeping madc's own headers. Data-driven: see embedded_header_is_system_library_shim().
	bool bypass_system_library_headers = false;
	// Frozen-forest discovery + failure policy (forest-carriers S3).
	// enable_external_forest gates the probe-chain arms that read frozen
	// state from OUTSIDE the binary/library images (the <exe>.forest and
	// <lib>.forest sidecars and the MADC_FOREST environment variable): a
	// sandboxed embedding host turns it off so nothing external can
	// redirect where the compiler loads frozen state from. The IMAGE arms
	// (self-image, and libmadc's own image in the shared shape) stay
	// enabled — they are the installation the host already loaded, not a
	// redirection. forest_missing_policy decides
	// what happens when the discovery chain finds NO usable container:
	// silent_fallback live-parses (the dev default), loud_fallback
	// live-parses after ONE stderr notice (the packaged-CLI default,
	// baked via MADC_FOREST_EXPECT_*), strict_require hard-errors (an
	// embedding host that must never silently degrade). A producer-config
	// (std / -D) mismatch — a container was found but for a different
	// dialect — is the by-design multi-dialect fall-through: NEVER a
	// notice under loud_fallback (the packed CLI compiling C against its
	// C++-parsed corpus is the everyday case); strict_require still
	// hard-errors on it, naming the mismatch.
	enum class ForestPolicy { silent_fallback, loud_fallback, strict_require };
	ForestPolicy forest_missing_policy = ForestPolicy::silent_fallback;
	bool enable_external_forest = true;
	// Discovery arm 5 (forest-carriers S6): the container path a madc.ini
	// `forest = <file>` key configured. Empty = no config file said
	// anything, which is also the library default — libmadc never reads a
	// config file; the CLI parses one and puts the result here. It rides
	// the POLICY rather than being a plain Program field so every child (a
	// --project TU, a runtime-eval child) inherits it through the one
	// propagation point, exactly like enable_forest_bind. Gated by
	// enable_external_forest with the sidecar/env arms: a file that can
	// redirect where the compiler loads frozen state from is precisely
	// what a sandboxed host turns off.
	std::string forest_config_path;
	// May this compile bind frozen state at all (the --forest-bind /
	// --no-forest-bind switch, and the library's enable_forest_bind
	// compile_option)? OFF is the Program default because a FREEZE must
	// live-parse: the CLI turns it on for compiles, and every libmadc host
	// gets it on through compile_options. It rides the policy so a child
	// Program (runtime eval, --project TU) inherits it with everything else.
	bool enable_forest_bind = false;
	std::vector<std::string> allowed_headers;
	std::vector<std::string> allowed_dlfcn_symbols;
	RuntimeEvalChildPolicy runtime_eval_source_policy;
    };

    struct ErrorInfo
    {
	bool has_error = false;
	std::string message;
	std::string file;
	int line = 0;
	int column = 0;
    };

    enum class DiagnosticSeverity
    {
	warning,
	error
    };

    enum class DiagnosticPhase
    {
	unknown,
	lexer,
	parser,
	compiler,
	runtime
    };

    struct Diagnostic
    {
	DiagnosticSeverity severity = DiagnosticSeverity::error;
	DiagnosticPhase phase = DiagnosticPhase::unknown;
	std::string message;
	std::string file;
	int line = 0;
	int column = 0;
    };

protected:
    // === pop-1 lexer-token factory (token-arena Phase 2 seam, step 2.2a) ===
    // ONE construction point for every lexed (pop-1) token, so the lexer no
    // longer scatters `new TokenX` across ~130 sites. In 2.2a this is
    // behavior-identical: each call still heap-`new`s a TokenBase and returns
    // it (NO representation change). Step 2.2b makes the factory append a POD
    // TokenRec to the arena and hand back a slot-id-backed shell.
    // Payload-free kinds (operators/punctuation/keywords) delegate to the
    // shared madc_pch::token_from_id switch; payload kinds construct directly.
    // See docs/plans/2026-06-23-p1-token-arena-implementation-plan.md.
    TokenBase *make_token(TokenID kind);                       // payload-free
    TokenBase *make_ident(const std::string &spelling);        // TokenIdent
    TokenBase *make_int(int64_t value);                        // TokenInt
    TokenBase *make_int(int64_t value, const std::string &src);// TokenInt + text
    TokenBase *make_real(long double value);                   // TokenReal
    TokenBase *make_str(const std::string &bytes, bool wide = false); // TokenStr
    TokenBase *make_char(int code);                            // TokenChar
    TokenBase *make_datatype(const char *name, DataDef &dd);   // TokenDataType
    TokenBase *make_rem(const std::string &text);              // TokenREM
    TokenBase *make_space(int cnt);                            // TokenSpace
    TokenBase *make_tab(int cnt);                              // TokenTab
    TokenBase *make_eol(int cnt);                              // TokenEOL
    TokenBase *read_wide_literal(const std::string &prefix = "L");   // L/u/U/u8 string/char literal -> pop-1 token
    // Fill the immutable (ROM) TokenRec of a lexed pop-1 token from the formed
    // shell — kind/value/spelling/provenance — so a fresh mutable (RAM) shell
    // can later be rebuilt from the rec alone (the no-clone substitution split,
    // step 1). type_id is resolved lazily at materialize time, not here.
    void finalize_pop1_rec(TokenBase *tb);
    TokenBase *_getToken();
    TokenBase *skipConditionalBlock();
    bool evaluateIfCondition();
    // The clang `__has_*` preprocessor operators, evaluated inside an #if.
    // `pos` enters at the operator's '(' and leaves past its ')'. Every one
    // answers from madc's OWN state and answers TRUTHFULLY: a yes madc cannot
    // back turns a library's clean "#error not implemented" into a mystifying
    // failure deeper in its headers, so an unknown query answers 0.
    int64_t evaluateHasQuery(const std::string &op, const std::string &expr,
			     size_t &pos);
    // Does madc implement this builtin? (`__has_builtin`, and the same
    // question the parser answers when it sees the call.)
    // (not const: intern_keyed_map::count() is not const-qualified)
    bool has_builtin(const std::string &name);
    // Is this `__has_*` operator one madc ANSWERS from its own state? The ONE
    // list: evaluateHasQuery declines everything else, and macro_name_defined
    // makes exactly these visible to `#ifdef` — so an operator can never be
    // answerable but invisible, or visible but unanswerable.
    static bool has_query_operator_implemented(const std::string &op);
    // Is this name DEFINED for `#ifdef` / `#ifndef` / `defined()`? A macro in
    // either table, or an implemented `__has_*` operator (gcc and clang both
    // make those visible to #ifdef, and libstdc++ gates whole feature families
    // on it).
    // (not const: intern_keyed_map::count() is not const-qualified)
    bool macro_name_defined(const std::string &name);
    void popOperator(std::stack<TokenBase *> &, std::stack<TokenBase *> &);
//  inline int get(std::istream &is) { ++_column; return is.get(); }
    // initializers / finalizers
    void _tokenizer_init();
    void _parser_init();
    void _compiler_init();
    bool _compiler_finalize();
    // protected members
    int _braces;
//  std::streampos _pos;
    TokenBase *_prv_token;
    TokenBase *_cur_token;
    Source source;
public:
    MadcEngine *engine;
    Program *runtime_scope_prev;
    std::istream *input_stream;
    std::ostream *output_stream;
    std::ostream *error_stream;
    const madc::value *expression_context_root;
    RegistrationPolicy registration_policy;
    BuiltinRegistry builtin_registry;
    NamespaceRegistry namespace_registry;
    ErrorInfo last_error;
    std::vector<Diagnostic> diagnostics;
    keyword_map_t  keyword_map;		// reserved keywords
    // C++ alternative-token operators (and/or/not/bitand/...): word spellings
    // that are exact synonyms for symbolic operators ([lex.digraph]). Held
    // separately from keyword_map because the operator tokens are not
    // TokenKeyword subclasses. Populated (C++-gated) in add_keywords; looked up
    // and cloned in _getToken alongside keyword_map.
    madc::dis::intern_keyed_map<TokenBase *> cpp_operator_map;
    madc::dis::intern_table type_name_pool;	// dedicated dense pool for flat type-name keys
    flat_datatype_map_t datatype_map;	// TokenDataType map (interned, keyed via type_name_pool)
    // Block-scope typedef shadow frames — one per live compound depth. Each
    // entry records (alias, the flat datatype_map entry it shadowed, or NULL).
    // register_scoped_typedef() records; unwind_block_typedef_shadows() restores
    // at block exit so a local typedef's meaning ends with its block
    // ([basic.scope.block]) instead of leaking into the flat map forever.
    std::vector<std::vector<std::pair<std::string, TokenDataType *> > >
	block_typedef_shadows;
    void register_scoped_typedef(const std::string &alias, TokenDataType *tdt);
    void unwind_block_typedef_shadows(size_t depth, const char *site = "?");
    // Struct-tag first declaration — the ONE mint recipe (incomplete
    // DataDefSTRUCT + pack tap + struct_map + C++ bare-name registration).
    // Consumers: TokenSTRUCT::parse's two forward-declaration arms and
    // resolve_declared_type_token's elaborated-type-specifier miss
    // ([basic.scope.pdecl]/7 — `wp<struct nat>` first-declares `nat`).
    DataDefCLASS *nested_aggregate_owner() const;
    std::string scoped_struct_tag(const std::string &name);
    TokenDataType *register_cpp_aggregate_name(const std::string &name,
					       DataDefSTRUCT *sdd);
    DataDefSTRUCT *mint_incomplete_struct_tag(const std::string &name,
					      bool is_union);
    // Dedicated dense pool for the template-name domain (template/partial-spec/
    // alias/var-template/fn-template map keys). Separate from strpool so each
    // intern_keyed_map's _slot array stays sized to the small template-name set,
    // not the full identifier population (the type_name_pool discipline).
    madc::dis::intern_table template_name_pool;
    // Dedicated dense pool for namespace-name keys of namespace_datatype_map.
    madc::dis::intern_table namespace_name_pool;
    datadef_map_t  datadef_map;		// data definitions defined by typedef or class
    StructRegistry struct_map;		// data definitions defined by struct
					// (writes via .set() ONLY — despaced index)
    std::map<std::string, DataDefSTRUCT *> tsubst_local_aggregate_map;
    // Type table identity layer — project segment (madc_typeid.h; design
    // docs/plans/2026-06-12-type-table-value-abi-design.md §2). Holds every
    // non-primitive DataDef this Program has been asked an id for; index i
    // <=> typeid MADC_TYPEID_PROJECT_BASE + i. Lazy registration order is
    // ask order (deterministic per compilation). The madc::dis::id_table
    // primitive owns this segment's storage (stable ids over the project base,
    // pointers not values — DataDef is polymorphic and ids survive growth);
    // type_id_for/type_from_id own the id policy (the dd->type_id memo + the
    // primitive/system/project segment dispatch).
    madc::dis::id_table<DataDef> project_types{MADC_TYPEID_PROJECT_BASE};
    uint32_t type_id_for(DataDef *dd);	// THE lazy-stamp chokepoint
    DataDef *type_from_id(uint32_t id);	// segment-dispatching reverse lookup
    // B3 arena-native DataDef storage (docs/plans/2026-07-06-forest-b3-record-layout-DESIGN.md).
    // The write-through target for the DataDef migration: as a project type is created it also
    // populates a flat POD record here (keyed by its project-id slot), so SAVE becomes a dump
    // and LOAD an mmap. `forest_arena_enabled` gates population (default off = zero change; the
    // migration flips it on, tests flip it on) — the runtime realization of the design's
    // FEATURE_FOREST_ARENA guard, chosen over a #ifdef because parser.o is SHARED between
    // bin/madc and the unit tests (a compile guard could not be on-for-test / off-for-ship in
    // one build). Reads still go through the live DataDef fields; only writes dual-populate the
    // record, until a later slice flips reads onto it. See feedback_forest_load_never_reparse.
    madc::dis::DefArena forest_arena;
    bool forest_arena_enabled = false;
    // write-through: record a newly-created unary derived type (pointer / reference / const)
    // into forest_arena, keyed by its project-id slot. Dispatches on the actual type for the
    // record kind (DK_PTR / DK_REF / DK_CONST) and reads the operand from its base_type.
    void forest_arena_record_unary(DataDef *dd);
    // write-through: record a completed aggregate (struct / union / class) into forest_arena,
    // keyed by its project-id slot, at its parse-completion point. Per-aggregate + NON-recursive:
    // every cross-ref (member / base / method / vbase / vgroup-owner type) is a SERIALIZED
    // type-id (the referent records itself at ITS completion; the id-addressed arena tolerates a
    // forward slot ref). Reads stay on the live DataDef; only this write dual-populates the record.
    void forest_arena_record_aggregate(DataDefSTRUCT *sdd);
    // write-through: record a FuncDef (DK_FUNC) into forest_arena at its project-id slot —
    // ref0 = return type-id, a params run of paramrec (type-id + const/spelling), and the
    // is_varargs/is_void_params/declaration_only flags. Called for each class method from the
    // aggregate recorder (slice 1f-a, closing the methodrec.func_id forward-ref); free functions
    // route through it at their parse completion in a follow-on.
    void forest_arena_record_func(FuncDef *fd, class Method *mth = NULL);
    // write-through (v22): record a FUNCTION-POINTER type reached through a member / param /
    // return cross-ref. DataDefFPTR has no single birth funnel (born at ~10 declarator sites),
    // so the recording rides the cross-ref sites: walks the unary chain (ptr/ref/const
    // read-caches) to the FPTR, writes its DK_FPTR record (ref0 = the target FuncDef's DK_FUNC
    // record, encoded by forest_arena_record_func) at its own project slot. Idempotent via
    // has_def; the target's own fptr-typed params/return recurse, bounded by the same guard.
    void forest_arena_record_fptr(DataDef *dd);
    // Class-template parse-once representation. TemplateDef is copied across
    // lookup, merge, partial selection, and forest restore, so it owns only a
    // stable Program-lifetime arena id. Pattern nodes use ids and value fields;
    // no temporary aggregate pointer survives definition-time capture.
    typedef uint32_t ClassPatternId;
    typedef uint32_t ClassTypePatternId;
    enum class ClassParseReason : uint8_t {
	None,
	PatternNotCaptured,		// B0/B1 transition; removed when B2 admits patterns
	PatternParseError,
	PatternParsePoisoned,
	UnnormalizableType,
	DependentValueExpression,
	UnsupportedDeclKind,
	UnsupportedNestedDefinition,
	UnsupportedFriendDefinition,
	UnsupportedDefaultedComparison,
	UnsupportedOutOfLineNested,
	RequiresEagerBodyParse,
	RegistrationEscape,
	// Same-name member-function-template overloads (vector::_M_data_ptr):
	// the nested-recipe replay does not yet reproduce live overload
	// selection — an instantiated body can come from the wrong overload
	// (caught by the subbind gate as a drained-body check error).
	UnsupportedMemberTemplateOverloads
    };
    enum class ClassDeclKind : uint8_t {
	TypeAlias,
	NestedAggregate,
	NestedForward,
	NestedEnum,
	DataMember,
	BitField,
	AnonymousAggregate,
	StaticDataMember,
	Method,
	MemberTemplate,
	FriendType,
	FriendFunction,
	DefaultedComparison,
	UsingBaseMember,
	StaticAssert
    };
    enum class ClassTypePatternKind : uint8_t {
	ConcreteType,
	TemplateParam,
	SelfType,
	NestedType,
	Pointer,
	Reference,
	ConstType,
	CArray,
	FunctionPointer,
	TemplateId,
	DependentMember,
	PackExpansion
    };
    enum class ClassAggregateKind : uint8_t {
	Class,
	Struct,
	Union,
	Enum
    };
    enum class ClassMethodKind : uint8_t {
	Method,
	Constructor,
	Destructor,
	Conversion
    };
    enum class ClassNestedTemplateKind : uint8_t {
	ClassTemplate,
	AliasTemplate
    };
    struct ClassTypePattern {
	ClassTypePatternKind kind;
	uint32_t flags;
	uint32_t concrete_type_id;
	ClassTypePatternId operand;
	ClassTypePatternId secondary;
	uint32_t template_param_index;
	uint32_t nested_node_id;
	uint32_t pack_param_index;
	std::string name;
	std::vector<ClassTypePatternId> arguments;
	std::vector<uint64_t> dimensions;
	ClassTypePattern()
	    : kind(ClassTypePatternKind::ConcreteType), flags(0), concrete_type_id(0),
	      operand(0), secondary(0), template_param_index(0),
	      nested_node_id(0), pack_param_index(0) {}
    };
    struct ClassBasePattern {
	ClassTypePatternId type;
	uint32_t access;
	bool is_virtual;
	ClassBasePattern() : type(0), access(0), is_virtual(false) {}
    };
    struct ClassAliasPattern {
	std::string name;
	ClassTypePatternId type;
	ClassAliasPattern() : type(0) {}
    };
    struct ClassMemberPattern {
	std::string name;
	ClassTypePatternId type;
	uint64_t count;
	uint32_t access;
	bool is_array;
	bool is_bitfield;
	bool is_anonymous;
	uint64_t bit_width;
	std::vector<uint64_t> dimensions;
	ClassMemberPattern()
	    : type(0), count(1), access(0), is_array(false),
	      is_bitfield(false), is_anonymous(false), bit_width(0) {}
    };
    struct ClassMethodParamPattern {
	std::string name;
	ClassTypePatternId type;
	uint32_t flags;
	bool is_const;
	bool template_param_spelled_directly;
	std::string cpp_spelling;
	std::string typedef_name;
	std::vector<TokenBase *> default_tokens;
	ClassMethodParamPattern()
	    : type(0), flags(0), is_const(false),
	      template_param_spelled_directly(false) {}
    };
    struct ClassMethodPattern {
	ClassMethodKind kind;
	std::string variable_name;
	std::string display_name;
	std::string storage_alias_name;
	std::string local_emit_name;
	std::string emit_symbol;
	std::string return_typedef_name;
	ClassTypePatternId return_type;
	uint32_t flags;
	bool is_varargs;
	bool is_void_params;
	bool declaration_only;
	bool defaulted_or_deleted;
	bool is_deleted;
	uint8_t noexcept_spec;	// FuncDef::NoexceptSpec value
	bool pure_virtual;
	bool is_const_method;
	bool is_member_template;
	bool has_eager_body;
	std::vector<ClassMethodParamPattern> parameters;
	std::vector<std::string> template_param_names;
	std::vector<bool> template_param_is_type;
	std::vector<bool> template_param_is_pack;
	std::string template_return_spelling;
	std::vector<std::string> template_param_spellings;
	std::vector<TokenBase *> body_tokens;
	std::vector<TokenBase *> definition_tokens;
	std::vector<TokenBase *> trailing_ret_tokens;
	std::vector<TokenBase *> ctor_init_tokens;
	std::vector<TokenBase *> member_template_decl;
	std::vector<TokenBase *> member_template_return_tokens;
	// Per-param DEFAULT token runs (parallel to template_param_names;
	// empty run = no default) — the [temp.deduct]/8 SFINAE payload of a
	// member template (`typename = decltype(declval<_Tp1&>().~_Tp1())`).
	std::vector<std::vector<TokenBase *> > template_param_defaults;
	// Per-param CONSTRAINT-type runs (parallel; empty = unconstrained) —
	// mirrors FuncDef::member_template_param_constraints through the
	// pattern lane (gates which non-type defaults are fillable).
	std::vector<std::vector<TokenBase *> > template_param_constraints;
	// Per FUNCTION-parameter TYPE token runs — the [temp.deduct]/8 SFINAE
	// carried in a parameter type (`typename _Up::iterator_category* =
	// nullptr`, default value stripped). Mirrors
	// FuncDef::member_template_param_type_tokens through the pattern lane.
	std::vector<std::vector<TokenBase *> > param_type_token_runs;
	ClassMethodPattern()
	    : kind(ClassMethodKind::Method), return_type(0), flags(0),
	      is_varargs(false), is_void_params(false), declaration_only(false),
	      defaulted_or_deleted(false), is_deleted(false), noexcept_spec(0),
	      pure_virtual(false), is_const_method(false),
	      is_member_template(false), has_eager_body(false) {}
    };
    struct ClassUsingMemberPattern {
	ClassTypePatternId owner_type;
	std::string name;
	ClassUsingMemberPattern() : owner_type(0) {}
    };
    struct ClassNestedTemplatePattern {
	ClassNestedTemplateKind kind;
	std::vector<std::string> typeparams;
	std::vector<std::vector<TokenBase *> > typeparam_defaults;
	std::vector<bool> typeparam_is_type;
	std::vector<bool> typeparam_is_pack;
	bool has_non_type_params;
	std::string class_name;
	std::vector<TokenBase *> body;
	std::string defining_namespace;
	bool is_partial_specialization;
	std::vector<std::vector<TokenBase *> > spec_pattern;
	std::vector<TokenBase *> constraint;
	std::vector<TokenBase *> target;
	ClassNestedTemplatePattern()
	    : kind(ClassNestedTemplateKind::ClassTemplate),
	      has_non_type_params(false), is_partial_specialization(false) {}
    };
    struct ClassAggregatePatternNode {
	uint32_t local_id;
	uint32_t parent_id;
	ClassAggregateKind kind;
	std::string source_name;
	std::string canonical_spelling;
	bool complete;
	bool from_system_header;
	std::vector<ClassBasePattern> bases;
	std::vector<ClassDeclKind> declarations;
	std::vector<ClassAliasPattern> aliases;
	std::vector<ClassMemberPattern> members;
	std::vector<ClassMethodPattern> methods;
	std::vector<ClassUsingMemberPattern> using_members;
	std::vector<ClassNestedTemplatePattern> nested_templates;
	std::vector<std::pair<std::string, ClassTypePatternId> > static_members;
	std::vector<std::pair<std::string, int64_t> > static_values;
	std::vector<std::string> friend_classes;
	std::vector<std::string> friend_functions;
	ClassAggregatePatternNode()
	    : local_id(0), parent_id(0), kind(ClassAggregateKind::Class),
	      complete(false), from_system_header(false) {}
    };
    struct ClassPattern {
	std::string identity;
	std::string class_name;
	std::string defining_namespace;
	bool is_partial_specialization;
	ClassParseReason capture_reason;
	uint64_t semantic_fingerprint;
	std::vector<ClassTypePattern> types;
	std::vector<ClassAggregatePatternNode> nodes;
	ClassPattern()
	    : is_partial_specialization(false),
	      capture_reason(ClassParseReason::None), semantic_fingerprint(0)
	{
	    types.push_back(ClassTypePattern());
	}
    };
    struct ClassPatternResolverMemoEntry {
	uint8_t kind;
	uint32_t name_id;
	uint32_t namespace_id;
	DataDefCLASS *owner;
	std::vector<DataDef *> arguments;
	DataDef *result;
	ClassPatternResolverMemoEntry(
		uint8_t k = 0, uint32_t n = 0, uint32_t ns = 0,
		DataDefCLASS *o = NULL,
		const std::vector<DataDef *> &args = std::vector<DataDef *>(),
		DataDef *dd = NULL)
	    : kind(k), name_id(n), namespace_id(ns), owner(o),
	      arguments(args), result(dd) {}
    };
    typedef std::unordered_multimap<uint64_t, ClassPatternResolverMemoEntry>
	class_pattern_resolver_memo_t;
    class ClassPatternArena {
	std::deque<ClassPattern> patterns;
    public:
	ClassPatternArena() { patterns.push_back(ClassPattern()); }
	ClassPatternId add(const ClassPattern &pattern)
	{
	    patterns.push_back(pattern);
	    return (ClassPatternId)(patterns.size() - 1);
	}
	ClassPatternId add(ClassPattern &&pattern)
	{
	    patterns.push_back(std::move(pattern));
	    return (ClassPatternId)(patterns.size() - 1);
	}
	const ClassPattern *get(ClassPatternId id) const
	{
	    return id && id < patterns.size() ? &patterns[id] : NULL;
	}
	ClassPattern *get(ClassPatternId id)
	{
	    return id && id < patterns.size() ? &patterns[id] : NULL;
	}
	size_t size() const { return patterns.size() - 1; }
    };
    ClassPatternArena class_pattern_arena;
    class_pattern_resolver_memo_t class_pattern_resolver_memo;
    unsigned long long _class_pattern_resolver_memo_hits = 0;
    unsigned long long _class_pattern_resolver_memo_misses = 0;
    unsigned long long _class_pattern_resolver_memo_published = 0;
    std::map<const uint32_t *, ClassPatternId> restored_class_pattern_cache;
    unsigned long long _class_pattern_restore_deferred = 0;
    unsigned long long _class_pattern_restore_materialized = 0;
    bool class_pattern_capture_in_progress = false;
    // Temporary B2-B5 equivalence-test switch. The default remains the
    // structural lane; B6 deletes this when the parse-reason tally reaches zero.
    bool force_legacy_class_patterns = false;
    unsigned class_registration_journal_depth = 0;
    std::map<DataDefCLASS *, std::vector<ClassDeclKind> >
	*class_pattern_decl_capture = NULL;
    std::map<DataDefCLASS *,
	std::vector<std::pair<DataDefCLASS *, std::string> > >
	*class_pattern_using_capture = NULL;

    // Captured `template<typename T> class Name {...}` definitions for
    // Borland-model instantiation: name -> {type params, the class-body token
    // range}. `Name<ConcreteT>` clones+substitutes+re-parses it as a concrete
    // class. See docs/plans/2026-05-30-template-instantiation.md.
	struct TemplateDef {
	    std::vector<std::string> typeparams;   // e.g. ["T"]
	    std::vector<std::vector<TokenBase *>> typeparam_defaults;
	    std::vector<bool> typeparam_is_type;
	    std::vector<bool> typeparam_is_pack;
	    bool has_non_type_params;
	    std::string class_name;                // e.g. "Box"
	uint32_t registry_name_id;             // template_name_pool id for class_name
	    std::vector<TokenBase *> body;         // cloned tokens: `class Name { ... }`
	std::string defining_namespace;        // current_namespace at capture (e.g. "std")
	DataDefCLASS *owner_class;             // enclosing class for member templates
	bool is_partial_specialization;        // template<class T> struct X<T*> {...}
	// For a partial spec: the pattern token sequence per arg slot (e.g. ["T","*"]
	// for `X<T*>`). Empty for a primary template.
	std::vector<std::vector<TokenBase *>> spec_pattern;
	// C++20 requires-clause on a partial spec (`requires C<I>`): cloned
	// constraint tokens (the `requires` keyword excluded). Empty =
	// unconstrained. match_partial_specialization folds this (typeparams
	// substituted) and rejects the candidate when it is false.
	std::vector<TokenBase *> constraint;
	ClassPatternId class_pattern_id;
	ClassParseReason class_pattern_reason;
	// Live parse defers header-template capture to the SECOND concrete
	// instantiation (the same environment the parse lane sees): the first
	// demand goes to the parse lane and only repeat demand — the pattern
	// lane's only market — pays the one-time capture parse. Pack-time
	// (forest_arena_enabled) and TU-root definitions still capture
	// eagerly. Never serialized: a frozen forest always carries either a
	// real pattern or a real reason.
	bool class_pattern_capture_deferred;
	// Concrete-instantiation demand seen so far (saturating; only the
	// 0 -> 1 transition matters). Lives on the REGISTERED definition via
	// Program::note_class_pattern_use.
	uint16_t class_pattern_use_count;
	// A bound forest keeps the immutable payload mapped for the Program's
	// lifetime. Restore records this span and materializes the ClassPattern only
	// if an eligible specialization actually needs it.
	const uint32_t *frozen_class_pattern;
	uint32_t frozen_class_pattern_words;
	CirFrozenForest *frozen_class_pattern_forest;
	TemplateDef() : has_non_type_params(false), registry_name_id(0),
			owner_class(nullptr),
			is_partial_specialization(false), class_pattern_id(0),
			class_pattern_reason(ClassParseReason::None),
			class_pattern_capture_deferred(false),
			class_pattern_use_count(0),
			frozen_class_pattern(NULL), frozen_class_pattern_words(0),
			frozen_class_pattern_forest(NULL) {}
    };
	template<class Definition>
	struct TemplateRegistryEntry {
	    typedef std::vector<Definition> variants_t;
	    variants_t namespace_variants;
	    std::unordered_map<DataDefCLASS *, variants_t> member_variants;

	    variants_t *find(DataDefCLASS *owner)
	    {
		if ( !owner )
		    return namespace_variants.empty() ? NULL : &namespace_variants;
		typename std::unordered_map<DataDefCLASS *, variants_t>::iterator it =
		    member_variants.find(owner);
		return it == member_variants.end() ? NULL : &it->second;
	    }
	    const variants_t *find(DataDefCLASS *owner) const
	    {
		if ( !owner )
		    return namespace_variants.empty() ? NULL : &namespace_variants;
		typename std::unordered_map<DataDefCLASS *, variants_t>::const_iterator it =
		    member_variants.find(owner);
		return it == member_variants.end() ? NULL : &it->second;
	    }
	    variants_t &for_write(DataDefCLASS *owner)
	    {
		return owner ? member_variants[owner] : namespace_variants;
	    }
    };
    typedef TemplateRegistryEntry<TemplateDef> template_registry_entry_t;
    struct TemplateAliasDef;
    ClassPatternId capture_class_pattern(TemplateDef &td);
	const ClassPattern *materialize_class_pattern(const TemplateDef &td);
	void writeback_class_pattern_capture(const TemplateDef &td);
	void note_class_pattern_use(const TemplateDef &td);
	// Lazy capture demanded at journal depth > 0 (nested inside a pattern
	// serve) cannot run there — a nested isolate journal does not snapshot,
	// so its rollback could not undo the capture parse's registrations.
	// Queue the demand by registry key and drain at the next depth-0
	// checkpoint (the outermost serve's return), where a capture journal
	// is outermost again.
	struct PendingClassPatternCapture
	{
	    uint32_t name_id;
	    DataDefCLASS *owner;	// pointer-equality lookup key only
	    bool is_partial;
	};
	std::vector<PendingClassPatternCapture> pending_class_pattern_captures;
	void queue_class_pattern_capture(const TemplateDef &td);
	void drain_pending_class_pattern_captures();
	uint64_t class_pattern_fingerprint(const ClassPattern &pattern) const;
    // B0 class-KIND parse-once foundation. Capture uses the production class
    // parser once, so all temporary registrations must roll back as one unit.
    // The implementation journals first writes behind this small RAII handle;
    // B1 activates it around pattern capture.
    class ClassRegistrationJournal
    {
	struct State;
	Program &pgm;
	State *state;
	bool finished;
	bool outermost;
	ClassRegistrationJournal(const ClassRegistrationJournal &);
	ClassRegistrationJournal &operator=(const ClassRegistrationJournal &);
    public:
	explicit ClassRegistrationJournal(Program &p,
		bool isolate_registration_side_effects = true);
	~ClassRegistrationJournal();
	void commit();
	void rollback();
	void record_type_alias_write(DataDefCLASS *owner,
				     const std::string &name);
	variable_map_t &namespace_for_write(const std::string &name);
	std::vector<TemplateDef> &class_template_variants_for_write(
		uint32_t name_id, DataDefCLASS *owner, bool partial);
	std::vector<TemplateAliasDef> &alias_template_variants_for_write(
		uint32_t name_id, DataDefCLASS *owner);
	DataDef *find_class_pattern_resolution(uint64_t resolution_hash,
		uint8_t kind, uint32_t name_id, uint32_t namespace_id,
		DataDefCLASS *owner,
		const std::vector<DataDef *> &arguments) const;
	void record_class_pattern_resolution(uint64_t resolution_hash,
		uint8_t kind, uint32_t name_id, uint32_t namespace_id,
		DataDefCLASS *owner,
		const std::vector<DataDef *> &arguments, DataDef *result);
	void publish_class_pattern_resolutions();
    };
    ClassRegistrationJournal *active_class_registration_journal = NULL;
    variable_map_t &namespace_variables_for_write(const std::string &name);
    DataDef *find_class_pattern_resolution(uint64_t resolution_hash,
	uint8_t kind, uint32_t name_id, uint32_t namespace_id,
	DataDefCLASS *owner,
	const std::vector<DataDef *> &arguments) const;
    void record_class_pattern_resolution(uint64_t resolution_hash,
	uint8_t kind, uint32_t name_id, uint32_t namespace_id,
	DataDefCLASS *owner,
	const std::vector<DataDef *> &arguments, DataDef *result);
    void set_class_type_alias(DataDefCLASS *owner, const std::string &name,
			      DataDef *type);
    // Namespace/global templates are keyed by bare name; member templates are
    // partitioned by their concrete owner. Same-key namespace variants remain
    // disambiguated by TemplateDef::defining_namespace. All selection goes
    // through find_template().
    madc::dis::intern_keyed_map<template_registry_entry_t> template_map; // bare-name id -> namespace + owner variants
    // Select a template variant. owner_hint scopes nested member templates
    // (e.g. allocator<T>::rebind<U>); NULL selects namespace/global templates.
    // ns_hint != "" => exact defining_namespace match (or NULL); ns_hint == ""
    // => the sole matching-owner variant if unique, else prefer current_namespace,
    // then the global ("") variant, then the first matching-owner variant.
    TemplateDef *find_template(const std::string &name,
			       const std::string &ns_hint = std::string(),
			       DataDefCLASS *owner_hint = NULL);
    TemplateDef *find_template(uint32_t name_id,
			       const std::string &ns_hint,
			       DataDefCLASS *owner_hint);
    // The variant carrying a parsed body, if any (for completion gating).
    TemplateDef *template_with_body(const std::string &name);
    // Replace the same-namespace variant (merging template-default args from the
    // prior one) or append a new variant. only_if_absent => leave an existing
    // same-namespace variant untouched (first-wins, for bodyless forward decls).
    void register_template(const TemplateDef &td, bool only_if_absent);
    // Partial specializations (template<class T> struct X<T*> {...}), keyed by
    // bare-name id with concrete owners partitioned inside the value. Kept OUT
    // of template_map (its same-namespace merge would clobber the primary).
    // Selected at instantiation by most-specialized pattern unification.
    madc::dis::intern_keyed_map<template_registry_entry_t> partial_spec_map; // bare-name id -> namespace + owner variants
    std::vector<TemplateDef> &class_template_variants_for_write(
	uint32_t name_id, DataDefCLASS *owner, bool partial = false);
    // Choose the most-specialized partial spec of `name` whose pattern unifies with
    // the concrete arguments. Type slots deduce into out_subst; non-type slots must
    // fold to the same constant value. Returns NULL to fall back to the primary.
    TemplateDef *match_partial_specialization(const std::string &name,
	    const std::vector<TokenDataType *> &arg_types_by_slot,
	    const std::vector<std::string> &arg_spellings,
	    const std::vector<std::vector<TokenBase *>> &arg_tokens_by_slot,
	    const std::vector<bool> &param_is_type,
	    std::map<std::string, TokenDataType *> &out_subst,
	    std::map<std::string, std::string> &out_template_subst,
	    std::map<std::string, std::vector<std::string> > &out_pack_subst,
	    std::map<std::string, std::vector<TokenBase *> > &out_nontype_subst,
	    const std::string &ns_hint, DataDefCLASS *owner_hint = NULL,
	    uint32_t name_id = 0);
    // Unify a nested template-id pattern arg (e.g. `allocator<_Tp>`) against a
    // concrete type spelling (e.g. `std::allocator<char>`), deducing the spec's
    // type params. Fallback used by match_partial_specialization when the flat
    // unifier cannot match a template-id-shaped pattern slot.
    bool unify_nested_spec_pattern_arg(const std::string &pat_spelling,
	    const std::vector<std::string> &spec_params,
	    const std::string &concrete_spelling,
	    std::map<std::string, DataDef *> &ded, int &score,
	    std::map<std::string, std::string> *out_tmpl = NULL,
	    std::map<std::string, std::vector<std::string> > *out_pack = NULL);
    // Evaluate a `__void_t<Args...>` detection-idiom partial-spec slot: matches a
    // concrete void IFF every Arg (a `typename PARAM::member` dependent type) resolves
    // after substituting the already-deduced params. The SFINAE half of the std
    // detection idiom (__detected_or / __iterator_traits / allocator_traits).
    // `slot_tokens` (the spec pattern slot's token run) enables the EXPRESSION
    // probe arm — `__void_t<decltype(EXPR)>` (libc++ __common_type2_imp) —
    // which substitutes the deduced params into the decltype token run and
    // resolves it through the real decltype machinery.
    // `concrete_dd` is the concrete slot's resolved type, needed by the BARE
    // `decltype(EXPR)` arm — libc++'s spelling of the same idiom, which carries no
    // void_t wrapper to force void, so the probe's resolved type must be COMPARED
    // against the concrete arg rather than merely proven well-formed.
    bool eval_void_t_detection_slot(const std::string &slot_spelling,
	    const std::string &concrete_spelling,
	    const std::map<std::string, DataDef *> &ded, int &score,
	    const std::vector<TokenBase *> *slot_tokens = NULL,
	    DataDef *concrete_dd = NULL);
    // Evaluate the nth `decltype ( ... )` run inside a spec-pattern slot's
    // tokens under the deduced-param substitution; true iff it names a type.
    // `out_type` optionally receives the resolved type (the bare-decltype arm
    // needs the type itself, not just well-formedness).
    bool eval_decltype_probe_tokens(const std::vector<TokenBase *> &slot_tokens,
	    size_t nth, const std::map<std::string, DataDef *> &ded,
	    DataDef **out_type = NULL);
    // General non-deduced pattern slot ([temp.class.spec.match]/2): substitute
    // the deduced params into the WHOLE slot token run, resolve it as a type
    // under the SFINAE trap, and require identity with the concrete slot's
    // type. Covers the [meta.logical] `__enable_if_t<bool(_B1::value)>`
    // idiom, which is neither __void_t nor decltype. False when the slot
    // mentions no deduced name (the literal matchers own that case), when
    // resolution fails (SFINAE reject), or when it resolves to another type.
    bool eval_substituted_slot_type(const std::vector<TokenBase *> *slot_tokens,
	    const std::map<std::string, DataDef *> &ded,
	    DataDef *concrete_dd, int &score);
    // Confirm that a dependent member chain `BASE::seg1::seg2::...` (where some
    // seg names a TEMPLATE member, e.g. `rebind<_Up>`) resolves to a real type —
    // the part of the __void_t SFINAE probe the plain type-alias walk cannot do.
    // Builds the qualified-id tokens with deduced params substituted and resolves
    // via resolve_typename_type_token in an isolated stream. True iff it resolves.
    bool confirm_dependent_member_type(DataDef *base,
	    const std::vector<std::string> &segs,
	    const std::map<std::string, DataDef *> &ded);
    // Constant-fold ONE non-type template argument from its already-collected
    // token vector (+ spelling), WITHOUT touching the live token stream or
    // instantiating anything — a LOCAL cursor only. Recognizes manifest integer/
    // bool literals and the type-trait builtins (`__has_trivial_destructor(T)`,
    // `__is_same(A,B)`, …). Returns false (→ keep the spelling) for any form it
    // cannot fold with certainty (dependent / value-dependent / unsupported).
    bool fold_nontype_template_arg(const std::vector<TokenBase *> &argtoks,
				   const std::string &spelling, int64_t &out);
    // Evaluate a NON-dependent non-type template argument (a `<...>` slot that is
    // an integral/bool constant expression — `(1==1)`, `is_trivial<int>::value`,
    // `__is_bitwise_relocatable<T>::value` for concrete T) to its value via the
    // SAME constant-expression evaluator `static_assert` uses, parsing a clone of
    // the collected arg tokens in an ISOLATED stream. Returns false (keep the raw
    // tokens) for anything still dependent / unfoldable. This is the substitution-
    // time non-type-arg fold (g++ convert_nontype_argument / clang converted-
    // constant-expression) without which `enable_if<C,T>` never selects its
    // partial spec unless C is a literal.
    bool fold_nontype_arg_constant(const std::vector<TokenBase *> &argtoks,
				   int64_t &out);
    // The canonical, identifier-safe instantiation-key fragment for ONE template
    // argument: a foldable non-type arg renders as its normalized VALUE (so
    // `<true>`, `<1>`, and `<__has_trivial_destructor(int)>` all key identically
    // and select the matching specialization), else the sanitized spelling
    // (byte-identical to the legacy behavior). EVERY template-instantiation
    // key-build site routes through this so keys agree corpus-wide — the
    // canonical-forms discipline (docs/plans/2026-06-13-canonical-forms-on-mc11ir-sketch.md);
    // canonicalizing at only SOME sites shatters identity (the 202-regression).
    std::string canonical_arg_key_fragment(const std::vector<TokenBase *> &argtoks,
					   const std::string &spelling);
    // Canonicalize ONE template-argument SPELLING to the form a use site
    // produces for the RESOLVED type (template_type_arg_spelling reads
    // canonical_cpp_spelling()); template-ids canonicalize their inner args
    // recursively (`alloc9<int>` -> `alloc9<int32_t>`). The template-id arm
    // of canonical_arg_key_fragment's one-key rule. Returns EMPTY unless
    // every part resolves concretely — a dependent spelling must keep its
    // raw fragment, never a context-dependent rewrite.
    std::string canonical_template_arg_spelling(const std::string &spelling);
    struct TemplateAliasDef {
	    std::vector<std::string> typeparams;
	    std::vector<std::vector<TokenBase *>> typeparam_defaults;
	    std::vector<bool> typeparam_is_type;
	    std::vector<bool> typeparam_is_pack;
	    bool has_non_type_params;
	    std::string alias_name;
	uint32_t registry_name_id;
	std::vector<TokenBase *> target;
	std::string defining_namespace;
	DataDefCLASS *owner_class;
	TemplateAliasDef() : has_non_type_params(false), registry_name_id(0),
		owner_class(nullptr) {}
    };
    typedef TemplateRegistryEntry<TemplateAliasDef> template_alias_registry_entry_t;
    madc::dis::intern_keyed_map<template_alias_registry_entry_t> template_alias_map; // bare-name id -> namespace + owner variants
    std::vector<TemplateAliasDef> &alias_template_variants_for_write(
	uint32_t name_id, DataDefCLASS *owner);
    struct ClassNestedTemplateCaptureEntry {
	ClassNestedTemplateKind kind;
	bool partial_specialization;
	uint32_t name_id;
	ClassNestedTemplateCaptureEntry(ClassNestedTemplateKind k,
		bool partial, uint32_t registry_name_id)
	    : kind(k), partial_specialization(partial),
	      name_id(registry_name_id) {}
    };
    typedef std::map<DataDefCLASS *,
	std::vector<ClassNestedTemplateCaptureEntry> >
	class_nested_template_capture_t;
    class_nested_template_capture_t *class_pattern_nested_template_capture = NULL;
    void record_class_pattern_nested_template(DataDefCLASS *owner,
	ClassNestedTemplateKind kind, const std::string &key,
	bool partial_specialization = false);
    void record_class_pattern_nested_template(DataDefCLASS *owner,
	ClassNestedTemplateKind kind, uint32_t name_id,
	bool partial_specialization = false);
    // C++14 VARIABLE TEMPLATE: `template<...> [inline constexpr] T name = init;`
    // (std::numbers::e_v, pi_v, …). madc does not model these as first-class
    // values; it registers the name + typeparams + initializer tokens so a use
    // `name<Arg>` resolves to its arg-substituted initializer expression (parsed
    // inline at the use site). Keyed by simple name AND `ns::name`.
    struct VarTemplateDef {
	std::vector<std::string> typeparams;
	std::vector<bool> typeparam_is_pack;    // parallel to typeparams; last may be a pack
	std::vector<TokenBase *> init;          // tokens after '=' up to ';'
	std::string defining_namespace;
    };
    madc::dis::intern_keyed_map<VarTemplateDef> var_template_map; // keyed via template_name_pool
    // C++20 CONCEPT: `template<...> concept Name = <constraint-expr>;`. madc does
    // not yet EVALUATE concept satisfaction, but it captures the constraint tokens
    // (+ typeparams) here so a future evaluator can decide `Name<Args>` by
    // substituting Args and evaluating the constraint (nested concept-ids,
    // type-trait `::value`, `derived_from`, requires-expression well-formedness).
    // Until the evaluator lands this is storage-only (no behavior change). Keyed by
    // simple name AND `ns::name`.
    struct ConceptDef {
	std::vector<std::string> typeparams;
	std::vector<TokenBase *> constraint;    // tokens after '=' up to ';'
	std::string defining_namespace;
    };
    registration_map<std::string, ConceptDef> concept_map;
    std::vector<TokenBase *> last_skipped_template_decl;
    // The SOURCE name of the tracked free-function overload parseDeclaration
    // is about to hand to parseFunction ("operator<"), stamped on the FuncDef
    // BEFORE its body parses (the hidden-friend access grant compares
    // display names mid-body). Cleared after the parseFunction call.
    std::string pending_function_display_name;
    // The inst_key of the fn-template instantiation whose substituted decl is
    // being re-parsed (instantiate_fn_template_binding). parseFunction's
    // overload tracking folds it into the overload identity: two
    // specializations differing only in a non-type value (g<int,3> / g<int,7>)
    // share a parameter spelling but are distinct functions
    // ([temp.over.link]). Saved/restored around nested instantiations.
    std::string pending_fn_instantiation_identity;
    std::vector<std::string> last_skipped_template_typeparams;
    // Pack-ness of last_skipped_template_typeparams (parallel vector), so a
    // skipped member template's variadic typeparam (`typename... _Args`) is
    // preserved through register_skipped_class_template_function.
    std::vector<bool> last_skipped_template_typeparam_is_pack;
    // TYPE-ness of last_skipped_template_typeparams (parallel vector; true =
    // `typename T`, false = a NON-TYPE param such as `unsigned long __a`), so a
    // skipped member template's non-type params survive
    // register_skipped_class_template_function. Without it every param looked
    // like a type and instantiation failed against its explicit VALUE argument.
    std::vector<bool> last_skipped_template_typeparam_is_type;
    // Per-param DEFAULT token runs (parallel vector; empty run = no default).
    // A member template can carry its whole SFINAE in a defaulted param
    // (`template<typename _Tp1, typename = decltype(declval<_Tp1&>().~_Tp1())>
    // static true_type __test(int);`, gcc13 __is_destructible_impl) — the
    // resolver must substitute-and-resolve the default per [temp.deduct]/8.
    std::vector<std::vector<TokenBase *> > last_skipped_template_typeparam_defaults;
    // Per-param CONSTRAINT-type runs (parallel vector; empty run =
    // unconstrained) — the compound declared TYPE of a non-type param
    // (`typename enable_if<C, bool>::type`), captured by the template-head
    // parse. Rides beside the defaults so the member-template stamp can tell
    // a plain `bool _Dummy = true` (fillable) from a constrained default
    // (cleared until the run is evaluated).
    std::vector<std::vector<TokenBase *> > last_skipped_template_typeparam_constraints;
    // W2 (retire-std-hardcoding-design): non-member operator overload candidates
    // declared at namespace scope (e.g. std::operator<<). Member-operator
    // resolution already exists (class_operator_call); these let `obj << x`
    // ALSO consider a free `operator<<(ostream&, const char*)` and bind the
    // winner mangled-direct via itanium_mangle_std_free_template. Captured from
    // the declaration (signature as spellings + the ordered template params)
    // so deduction + mangling are data-driven, never a stream-specific picker.
    struct FreeOperatorOverload {
	std::string ns;                            // "std"
	std::string opname;                        // "operator<<"
	std::vector<std::string> template_params;  // ordered: $T0,$T1,…
	std::string return_spelling;               // "basic_ostream<char,_Traits>&"
	std::vector<std::string> param_spellings;  // {"basic_ostream<char,_Traits>&","const char*"}
    };
    std::vector<FreeOperatorOverload> free_operator_overloads;
    // Named (non-operator) free-function template overloads captured from REAL
    // system headers (libstdc++), e.g. std::getline. Reuses FreeOperatorOverload
    // (opname holds the function name). The call site (cir_builder) selects an
    // overload by arity + deduces template args from the call's arg types, then
    // binds MANGLED-DIRECT to the real libstdc++ Itanium symbol — NOT the
    // __ns_<ns>_<name> wrapper path (which is correct only for madc's own polyglot
    // namespaces). See itanium_mangle_std_free_template + project_cpp_mangled_direct.
    std::vector<FreeOperatorOverload> free_function_overloads;
    // NON-template C++ namespace free-function overload sets (e.g. the inline
    // standard-library conversion helpers).
    // Each overload parses into its OWN Variable/FuncDef under a unique
    // internal symbol (unique_overload_symbol); this registry — keyed
    // "ns::name" — lets the call site enumerate and rank them by arg types
    // (the same generic score_arg_to_param ranking methods use).
    // param_spelling is the normalized source spelling of the parameter list,
    // used to tell a re-declaration of the SAME overload (reuse its Variable)
    // from a NEW overload (mint a fresh symbol).
    struct NamespaceFnOverload {
	std::string param_spelling;
	std::vector<std::string> template_arg_names;
	Variable *var;
    };
    registration_map<std::string, std::vector<NamespaceFnOverload> >
	namespace_fn_overload_sets;
    // Rank ns::name's parsed overloads against the call's arg types; returns
    // the winning Variable, or NULL when no set (or no viable overload) exists.
    // `zero_args` (parallel to argtypes, optional) marks literal-0 arguments —
    // the null-pointer-constant rule ([conv.ptr]) must survive the re-rank
    // (global ::operator== sets rank here too: testfreeop's `a == 0`).
    Variable *find_namespace_function_overload(const std::string &ns,
					       const std::string &name,
					       const std::vector<const DataDef *> &argtypes,
					       const std::vector<bool> *zero_args = NULL,
					       const std::vector<DataDef *> *explicit_template_args = NULL);
    // A parsed CONCRETE free-operator function viable for the operand types:
    // ranks the union of every "::"+opname-suffixed overload set (all
    // namespaces + the global "" key). NULL when none binds. `zero_args`
    // (optional, index-aligned) marks integer-literal-zero arguments
    // (null-pointer constants).
    Variable *find_free_operator_function(const std::string &opname,
					  const std::vector<const DataDef *> &argtypes,
					  const std::vector<bool> *zero_args = NULL);
    // The DataDef an operand denotes for free-operator overload ranking
    // (reference-transparent via operand_object_class).
    DataDef *free_operator_arg_datadef(TokenBase *operand);
    // Mid-parse arity-check deferral: true when the call's namespace overload
    // set has a member accepting more than argc args (or a retained fn
    // template that could instantiate one) — the tail re-rank decides then.
    bool namespace_overload_set_accepts_more(TokenCallFunc *tc, size_t argc);
    // Namespace FUNCTION templates with retained declaration tokens (the
    // post-`template<...>` range: [specifiers] RET name(params) [noexcept]
    // { body }), keyed "ns::name". Bodies are not parsed at capture; a call
    // whose callee is the body-less placeholder instantiates on demand
    // (Borland monomorphize: substitute the deduced type args into the
    // retained tokens and re-parse as a concrete namespace function — it
    // registers in namespace_fn_overload_sets and call_target_funcdef ranks
    // it). Tokens are retained raw (the program owns its token stream; same
    // lifetime model as last_skipped_template_decl) and cloned per
    // instantiation.
    struct FnTemplateDef {
	std::vector<std::string> typeparams;
	std::vector<std::vector<TokenBase *>> typeparam_defaults;
	// Per-parameter constraint-TYPE token runs (parallel to typeparams;
	// an empty run = unconstrained). Captured for a NON-TYPE parameter
	// whose type is a compound type-specifier (`__enable_if_t<...>*`,
	// `typename enable_if<C,bool>::type`) — the SFINAE carrier evaluated
	// under the completed binding at instantiation, so overload selection
	// rejects the candidate on substitution failure ([temp.deduct]/8).
	std::vector<std::vector<TokenBase *>> typeparam_constraints;
	std::vector<bool> typeparam_is_type;
	std::vector<bool> typeparam_is_pack;
	std::vector<TokenBase *> decl;
	std::string ns;
	std::string inline_builtin_kind;
	// Owner class for a MEMBER function template being instantiated: pushed
	// on class_scope_stack during the body parse so the params/body resolve
	// class-scope members ([basic.scope.class]). NULL for free/namespace fn
	// templates. (No default initializer — keeps the aggregate-init shape.)
	DataDefCLASS *owner_class;
	// INSTANCE (non-static) member function template: the instantiated body
	// is parsed AS A METHOD of owner_class (hidden `__this`, member access),
	// not as a free function — the clang/gcc model (a member fn template is
	// instantiated as a CXXMethodDecl of the class; static-ness is just a
	// flag). False for free/namespace templates AND for STATIC member
	// templates (those stay on the free-function parse).
	bool instance_method;
	// Identity base for the instantiation memo (fn_template_instantiated):
	// when non-empty, the memo keys on THIS + "<binding>" instead of the
	// registration key. A member-template instantiation registers under a
	// per-request unique symbol (`__mti`, `__mti__oN`), so keying the memo
	// on that name lets two call SHAPES that resolve to the SAME binding
	// mint duplicate definitions — g++ has one instantiation per
	// (template, binding). Set to the placeholder's stable symbol.
	std::string inst_identity;
	FnTemplateDef() : typeparams(), typeparam_defaults(),
	    typeparam_constraints(), typeparam_is_type(),
	    typeparam_is_pack(), decl(), ns(), inline_builtin_kind(),
	    owner_class(NULL), instance_method(false), inst_identity() {}
    };
    madc::dis::intern_keyed_map<std::vector<FnTemplateDef>> fn_template_map; // keyed via template_name_pool (enumerated via for_each)
    // BODY-LESS free/namespace function template declarations (no `{ body }` to
    // instantiate — e.g. `template<class T> T declval();`), keyed "ns::name".
    // Kept OUT of fn_template_map (which drives body instantiation + the arity-
    // deferral signal) so instantiation behavior is unchanged; read ONLY to
    // form a call's return TYPE by substituting explicit template args into the
    // declared return (resolve_namespace_fn_template_call_return_type — the
    // clang deduction-forms-the-function-type-without-a-body model).
    madc::dis::intern_keyed_map<std::vector<FnTemplateDef>> fn_template_decl_map; // keyed via template_name_pool
    registration_set<std::string> fn_template_instantiated; // "ns::name<t1,t2,...>" memo
    // inst_key -> the overload Variable that instantiation registered, so an
    // operator USE site can call the instantiated definition directly.
    madc::dis::intern_keyed_map<Variable *> fn_template_instantiated_vars; // keyed via template_name_pool
    // inst_keys whose body parse is LIVE on the stack right now. The var memo
    // is written only at completion, so a memo hit during the same key's own
    // body parse must NOT treat the missing var as a stale memo and erase the
    // guard — that re-enters the instantiation unboundedly (same-set siblings:
    // variadic_inst_in_progress, member_fn_inst_in_progress).
    std::set<std::string> fn_template_inst_in_progress;
    // > 0 while re-parsing an instantiated function-template body (nesting =
    // instantiation triggering instantiation). static_asserts inside such a
    // body are consumed unevaluated, exactly like instantiated class-template
    // member bodies (parse_static_assert_statement).
    size_t fn_template_instantiation_depth = 0;
    // True only while resolving a qualified member-TYPE chain (`typename X<..>::type`):
    // a concrete-arg trailing-type-pack class template is then REALLY instantiated
    // (body parse + member eval) so its member types fold (e.g. `__construct_helper<
    // _Tp,_Args...>::type` for the vector<T*> SFINAE trait). Elsewhere — notably the
    // constant-fold path (`__and_<..>::value`) — variadic templates stay opaque, as a
    // recursive variadic body (`__and_`, `tuple`) would otherwise re-instantiate
    // unboundedly. Scoped-set around the member-type instantiate_template_id calls.
    bool allow_variadic_real_inst = false;
    // VALUE-pack (trailing NON-TYPE pack) real instantiation is gated by its OWN
    // demand flag (task #102). The expansion machinery (token_pack_subst splice /
    // replicate in the legacy body clone) is live, but the caller-side spellings
    // are not yet folded (`sizeof...(_Args)` in template-arg position, the
    // `__integer_pack` / `__make_integer_seq` index-pack builtins) — so admitting
    // the shape under the GENERAL vri arming types one entity through two routes
    // (params via the real instance, caller temps via a raw-spelling flatten:
    // the _Index_tuple by-value incompatibility that broke 8 default-lane tests).
    // Armed only by a demand site that also folds those spellings; consumed and
    // cleared alongside allow_variadic_real_inst at each instantiate entry.
    bool allow_valuepack_real_inst = false;
    // STICKY variant of the above: stays on through a real-instantiation SUBTREE
    // whose root template forwards its meaning through a dependent-member base
    // (`__invoke_result : __result_of_impl<...>::type`). Unlike allow_variadic_real_inst
    // (consumed once at each instantiate_template_use entry), this propagates into
    // nested arg-resolution + base re-parse so the whole forwarding chain
    // (__is_invocable -> __invoke_result -> __result_of_impl) really instantiates.
    // Bounded by variadic_inst_in_progress so a recursive body cannot re-enter.
    bool variadic_real_inst_sticky = false;
    // Class-template real-instantiations in flight, keyed by mangled name. Re-entering
    // the same key while variadic_real_inst_sticky is on returns the opaque placeholder
    // instead of recursing — the recursion bound for the sticky forwarding-trait path.
    std::set<std::string> variadic_inst_in_progress;
    // Class instantiations in flight — BOTH lanes (pattern serve AND legacy
    // body re-parse) — keyed by registered mangled name. A cyclic dependency
    // web, or a template whose base clause names its own specialization as a
    // template ARGUMENT (libc++'s
    // `allocator : __non_trivial_if<..., allocator<_Tp>>`), re-enters here
    // mid-flight; the re-entry returns the early-registered incomplete shell
    // instead of recursing. The re-parse lane going unguarded was a stack
    // overflow on `#include <string>` under -stdlib=libc++.
    std::set<std::string> class_inst_in_progress;
    // Alias-template uses currently being resolved (keyed tname + arg spellings).
    // Re-entering the SAME key is a resolution CYCLE (a self-referential trait, now
    // reachable because variadic members really instantiate) — it short-circuits to
    // the opaque placeholder instead of recursing into a stack overflow. A genuine
    // (acyclic) deep nest uses distinct keys and resolves normally.
    std::set<std::string> alias_resolve_in_progress;
    // Member-fn-template instantiations currently in flight, keyed by LOGICAL identity
    // (owner + fn name + arg-type spellings), NOT call site. Breaks the allocator
    // trait cycle construct -> _S_construct -> __has_construct -> __test -> construct:
    // re-entering the same logical instantiation returns early (the body completes in
    // the outer frame) instead of recursing into a stack overflow. Call-site keying
    // (tc->var.name) can't catch this — each cycle hop is a distinct call site.
    std::set<std::string> member_fn_inst_in_progress;
    // Memoize member-ctor-template instantiations (keyed class + arg-type
    // spellings) so a repeated construction shape instantiates the ctor once;
    // doubles as the recursion guard for the construct -> forward -> construct chain.
    std::set<std::string> member_ctor_inst_done;
    // Dependent-pattern builds currently in flight. A dependent parse triggered
    // FROM a pattern build (e.g. a delegating mem-init constructing the ctor's
    // own class) must not re-enter the SAME fd's pattern build —
    // fd->dependent_pattern is still NULL mid-build, so the parse would recurse
    // unboundedly (stack exhaustion). Re-entrant requests return no pattern.
    std::set<class FuncDef *> dependent_patterns_in_progress;
    // >0 while parsing the operand of an UNEVALUATED context (decltype(...),
    // sizeof, noexcept). C++ does not ODR-use entities named in an unevaluated
    // operand ([basic.def.odr]) — so a function-template call there forms only a
    // signature (its return type, via the resolver), never an instantiated body.
    // declval is the canonical case: libstdc++ DEFINES it (a static_assert body
    // "declval() must not be used!" that calls the declaration-only __declval) but
    // it is only ever named in unevaluated operands, so its body is never emitted.
    // Instantiating it would import the undefined __declval. Gate body
    // instantiation on this depth == 0.
    int unevaluated_operand_depth = 0;
    void instantiate_namespace_fn_template_for_call(TokenCallFunc *tc);
    // A STATIC member function template of a madc-LOCAL class (a monomorphized
    // template instance such as `_Destroy_aux<true>`) is registered
    // declaration-only with its body retained on the FuncDef
    // (register_skipped_class_template_function). When such a callee is called,
    // deduce its template params from the call's args, instantiate the retained
    // body via the shared free-fn-template machinery, and bind the call to the
    // instantiated definition. Instantiations are PER CALL SHAPE (arg types +
    // value categories + explicit template args, memoized in
    // member_fn_inst_names): each distinct shape reaches deduction independently;
    // the binding memo then gives equal deductions one shared specialization.
    // Returns the concrete instance Variable for THIS call's shape (NULL when
    // not a member-template call / instantiation failed) and records it on
    // tc->mti_instance; the placeholder's local_emit_name alias stays FIRST-wins
    // for consumers without a call token. libstdc++-EXPORTED member templates
    // keep the mangled-direct path (member_template_method_call).
    Variable *instantiate_member_fn_template_for_call(TokenCallFunc *tc);
    // Per-call-shape member-template instantiation memo: shape key (the
    // placeholder's unique var.name + arg type/value-category + explicit-arg
    // spellings — placeholder IDENTITY, unlike the display-name cycle key,
    // which deliberately blurs sibling placeholders) -> instance symbol. The
    // call-lane twin of member_ctor_inst_done (which memoizes but needs no
    // symbol map — ctor instances register into cdd->ctors and re-select per
    // construction).
    std::map<std::string, std::string> member_fn_inst_names;
    // A member-template CONSTRUCTOR whose parameters use the template pack
    // (`template<class... A> C(B&, A&&...)`, e.g. libstdc++'s _Rb_tree::_Auto_node)
    // is registered declaration-only as a ctor (register_skipped_class_template_function)
    // with its body retained. A construction site `C v(args...)` is an object
    // declaration, NOT a call, so the call-only instantiate_member_fn_template_for_call
    // hook never fires. Given the construction's arg tokens, deduce the ctor's
    // template params, instantiate the retained body under a CONCRETE ctor symbol
    // (the ClassName__ClassName__oN scheme — so the mem-initializer list parses and
    // it is recognized as a ctor), and register that instantiation as a real ctor of
    // `cdd` so select_ctor_overload binds it. Idempotent / memoized per (class, arg
    // types). No-op when `cdd` has no member-template ctor.
    void instantiate_member_ctor_template_for_construction(
		DataDefCLASS *cdd, const std::vector<TokenBase *> &ctor_args);
    bool instantiate_member_ctor_template_candidate(
		DataDefCLASS *cdd, const std::vector<TokenBase *> &ctor_args,
		size_t candidate_skip);
    // Resolve the CONCRETE return type of a member-function-template call by
    // substituting the call's explicit/deduced template args into the retained
    // DEPENDENT return-type tokens (FuncDef::member_template_return_tokens) and
    // resolving — WITHOUT instantiating a body (the clang SubstDecl model).
    // Returns the concrete DataDef, or NULL on substitution failure (SFINAE) /
    // missing data. Used for decltype/unevaluated calls to body-less member
    // templates whose normal (body) instantiation produced no __mti definition.
    DataDef *resolve_member_template_call_return_type(class FuncDef *fd,
		const std::vector<DataDef *> &explicit_targs);
    // Deduce + instantiate a retained free-OPERATOR template body for
    // `lhs <op> rhs` cases whose operator body must be instantiated because no
    // exported library symbol covers that operand shape.
    // Returns the deduced return class; *callee_out = the registered overload.
    DataDef *instantiate_free_operator_template(const std::string &opname,
						TokenBase *lhs, TokenBase *rhs,
						Variable **callee_out);
    // Cfront lowering: when a binary class-operand operator resolves ONLY via
    // a retained operator template (no member operator, no exported free
    // shape), return a call node to the instantiated overload. NULL = not
    // this path; the normal operator machinery proceeds.
    TokenBase *lower_free_operator_to_call(class TokenOperator *to,
					   bool no_rewrite = false);
    // C++20 rewritten candidates ([over.match.oper]): != via ==, reversed
    // ==, relationals via (x <=> y) @ 0. Consulted only after every direct
    // candidate set missed.
    TokenBase *rewritten_operator_candidate(class TokenOperator *to);
    // madc strict-equality === domain rule: lower `x === y` to `x == y`.
    TokenBase *lower_strict_equality_domain_rule(class TokenOperator *to);
    // The std comparison-category class a builtin <=> yields
    // ([expr.spaceship]); NULL when <compare> has not been parsed.
    DataDef *comparison_category_class(class TokenOperator *to);
    // Public seam for the CIR lowering: `Class::member` static data member
    // as a value token (storage-backed TokenVar for object-typed statics,
    // folded constant for scalars). NULL when no such static member.
    TokenBase *class_static_member_value_token(DataDefCLASS *scope,
						const std::string &member,
						TokenBase *at);
    // Namespaces named by `using namespace X;` directives (C++
    // [namespace.udir]). The single-Variable import model skips a member
    // whose name a global already claimed; unqualified CALL resolution
    // consults these to bind the namespace overload when the global's arity
    // rejects the call (using_namespace_call_fallback).
    std::vector<std::string> active_using_namespaces;
    Variable *using_namespace_call_fallback(Variable *var, size_t argc);
    // Parse an explicit template-argument list after a resolved function name
    // (`name<long, int>(...)`) into concrete DataDefs, consuming through the
    // closing '>'. Bails to opaque consumption (skip_template_id_suffix
    // semantics) and returns an EMPTY list when an argument is not a
    // resolvable type, so a value argument never mis-binds a type parameter.
    std::vector<DataDef *> capture_call_template_args();
    // Record (then push back) the upcoming balanced parameter-list tokens —
    // the parser sits just after the function declarator's '(' — returning a
    // normalized spelling used as the overload-identity key.
    std::string peek_param_list_spelling();
    struct PendingTemplateInstantiation {
	std::string mangled_name;
	std::string canonical_spelling;
	std::vector<TokenDataType *> args;
    };
    registration_map<std::string, std::vector<PendingTemplateInstantiation> >
	pending_template_instantiations;
    // Template-origin structure for a DEPENDENT template-id placeholder shell
    // (`tuple<_Args1...>`, `_Index_tuple<__integer_pack(sizeof...(_Args1))...>`),
    // recorded at every dependent-shell creation/reuse seam (both class-
    // template instantiation lanes) so tsubst can later rebuild the CONCRETE
    // instantiation structurally from the binding (g++ tsubst on a dependent
    // template-id) instead of being stuck with an opaque name. Keyed by the
    // shell DataDef; arg token runs are clones (structural type tokens, not
    // source text).
    struct DependentShellOrigin {
	std::string tname;			// class-template name (template map key)
	uint32_t registry_name_id = 0;
	std::string defining_namespace;
	DataDefCLASS *owner_class = NULL;
	std::vector<std::string> arg_spellings;
	std::vector<std::vector<TokenBase *>> raw_arg_tokens;
    };
    registration_map<DataDef *, DependentShellOrigin> dependent_shell_origin;
    // Provenance for DERIVED dependent placeholders — the `X__deref` /
    // `Owner__member` opaques minted by dependent_deref_result_type /
    // materialize_dependent_member_type: the SOURCE type + derivation kind,
    // so tsubst can RE-DERIVE the concrete type under an instance
    // substitution (deref of `_ForwardIterator` re-derives to the element
    // type once _ForwardIterator = elem*). Without it the opaque leaks into
    // instantiation bindings as if it were concrete — the
    // `_Destroy<_ForwardIterator__deref>` no-op husk that silently swallows
    // element destructors. In-memory only; opaques never freeze.
    struct DependentDerivedOrigin {
	DataDef *source = NULL;
	enum Kind { Deref, MemberType } kind = Deref;
	std::string member;			// MemberType only
	bool member_is_template = false;		// MemberType only; preserves member<>
	std::vector<std::vector<TokenBase *> > raw_arg_tokens;
    };
    registration_map<DataDef *, DependentDerivedOrigin> dependent_derived_origin;
    // Thin public accessor over the class-scope member-type lookup
    // (type_aliases + bases + enclosing walk) for the tsubst re-derivation.
    static DataDef *class_member_type(DataDefCLASS *cls, const std::string &name);
    registration_set<std::string> template_completion_requested; // mangled aliases awaiting completion
    std::set<std::string> template_instantiated;           // mangled names done
    std::vector<DataDefCLASS *> class_scope_stack;	// active C++ class scopes for nested type lookup
    // An isolated definition-context type resolve (member-template defaults /
    // constraints) must outrank the ambient method owner. Both remain live
    // while a callee's SFINAE is evaluated from inside a caller method, so a
    // dedicated stack distinguishes the explicit definition owner from the
    // ordinary class_scope_stack.
    std::vector<DataDefCLASS *> class_type_lookup_override_stack;
    class ClassTypeLookupScope
    {
	Program &pgm;
	bool pushed;
	ClassTypeLookupScope(const ClassTypeLookupScope &);
	ClassTypeLookupScope &operator=(const ClassTypeLookupScope &);
    public:
	ClassTypeLookupScope(Program &p, DataDefCLASS *owner)
	    : pgm(p), pushed(owner != NULL)
	{ if ( pushed ) pgm.class_type_lookup_override_stack.push_back(owner); }
	~ClassTypeLookupScope()
	{ if ( pushed ) pgm.class_type_lookup_override_stack.pop_back(); }
    };
    // Function-local class identities are parse-session metadata. The class's
    // emitted name carries the durable identity; this side table lets tsubst
    // map a Tree-1 pattern local class to the matching concrete instantiation
    // without encoding identity in a pattern-global counter or a new forest
    // record family.
    registration_map<DataDefCLASS *, HoistedDeclIdentity>
	function_local_class_identities;
    // materialize_pattern_local_class replays only the class-definition token
    // span, outside its original compound. It supplies the already-computed
    // concrete identity through this one-shot context so TokenCLASS takes the
    // same naming path as an eager body parse.
    bool forced_local_class_identity_active = false;
    HoistedDeclIdentity forced_local_class_identity;
    // Computed symbol -> full source identity. Repeated parses of the same
    // definition are allowed; two distinct definitions minting one symbol are
    // a hard collision rather than a counter-based silent uniquification.
    registration_map<std::string, std::string> hoisted_symbol_identity_keys;
    // Scoped template-parameter registry (two-tree Phase 2 / 2a). A stack of
    // {param-name -> DataDefTemplateParam*} frames, pushed (via TemplateParamScope)
    // for the duration of a DEPENDENT template-body parse so a bare `T` resolves to
    // its placeholder through the EXISTING scoped type path
    // (resolve_current_class_type_alias) — NO change to datatype_map, NO new branch
    // in the general type lookup. Empty in every non-dependent parse, so behavior is
    // byte-identical until 2a pushes a frame. Placeholders are owned by
    // template_param_pool (Program lifetime, same never-freed-on-exit convention as
    // ptr_type_cache) so a Tree-1 pattern referencing one outlives the per-TU frame.
    std::vector<std::map<std::string, DataDef *>> template_param_scopes;
    std::vector<DataDefTemplateParam *> template_param_pool;
    // get-or-create a placeholder for parameter `name` at 0-based `index`.
    DataDefTemplateParam *intern_template_param(const std::string &name, unsigned index);
    // resolve `name` to an active template-parameter placeholder, innermost frame
    // first; NULL if no frame is active or the name is not a parameter.
    DataDef *resolve_template_param(const std::string &name);
    // Two-tree Phase 2 — TRUE while build_dependent_pattern is parsing a dependent
    // body. The template-instantiation entry points bail when this is set, so a
    // param-dependent call/construction in the body is left DEPENDENT (bound to its
    // body-less placeholder) instead of being eagerly instantiated with the param
    // PLACEHOLDER as a concrete arg — the leak that an unguarded dependent parse
    // caused (PLAN §11.5c). The g++ defer-instantiation model, scoped to the parse.
    // In-class initialized (kept out of the ctor init lists to avoid -Wreorder).
    bool dependent_parse_in_progress = false;
    // Two-tree: set when a dependent-pattern parse hits a construct it can only
    // SWALLOW (the unresolved-dependent-call `0`-literal placeholder): the
    // resulting token tree is structurally valid but semantically WRONG, so
    // build_dependent_pattern must discard it (clean re-parse fallback) rather
    // than let tsubst bake the placeholder literal into every instantiation.
    bool dependent_parse_poisoned = false;
    // Phase-5 slice 1: set by build_dependent_pattern across the pattern
    // parseFunction of a CONSTRUCTOR member template (ctor-ness derived from
    // fd ∈ owner->ctors — no name surgery). parseFunction consumes it ONCE at
    // entry (nested parses never see it) and, when set, captures the ctor's
    // `: mem-init` span raw and REPLAYS it inside the body compound (the
    // parse_deferred_function_body ctor_init_tokens model), so the pattern fd
    // gets dependent ctor_initializers instead of the skip loop discarding
    // the span (the `__patN` rename defeats the owner-prefix ctor gate).
    bool dependent_pattern_ctor_inits = false;
    // Phase-5 slice 2: armed by the member-template instantiation entry points
    // (call + ctor-construction paths) with the instantiation's unique symbol,
    // across try_instantiate_namespace_fn_template, when the source fd carries
    // a dependent_pattern. parseFunction skips the BODY parse of exactly the
    // function with this id (marking fd->tsubst_body_skipped); name-keying
    // means parses nested under the signature / SFINAE / mem-init machinery
    // never misfire. Empty == no skip armed.
    std::string tsubst_skip_body_name;
    // Two-tree Phase 2: capability predicate — true only for the conservative
    // first-slice shape the dependent-parse / tsubst path handles (one TYPE param,
    // no pack, NON-DEPENDENT return, body uses `T` only in scalar positions). False
    // routes to the existing re-parse instantiation (no behavior change).
    bool tsubst_eligible(FuncDef *fd, const char **why = NULL);
    // [expr.sizeof]/5 — `sizeof ... ( identifier )` yields a parameter pack's
    // ARITY. The operator is parsed by evaluate_type_query; it needs the arity of
    // a pack NAME that is only known to whichever instantiation is in flight, so
    // each instantiation PUBLISHES its pack arities here for the duration of the
    // body parse and the operator resolves against them. Publishing data beats
    // every substitution path re-implementing the operator (which is what the
    // deleted token-level folds did, and one of them was missing, so `sizeof...`
    // silently mis-parsed at arity >= 2).
    // A STACK because instantiations nest; lookup runs innermost -> outermost so
    // an inner template's own pack shadows an enclosing one of the same name.
    std::vector<std::map<std::string, size_t> > pack_arity_scopes;
    void push_pack_arity_scope()
	{ pack_arity_scopes.push_back(std::map<std::string, size_t>()); }
    void pop_pack_arity_scope()
	{ if ( !pack_arity_scopes.empty() ) pack_arity_scopes.pop_back(); }
    void publish_pack_arity(const std::string &n, size_t c)
	{ if ( !pack_arity_scopes.empty() ) pack_arity_scopes.back()[n] = c; }
    bool lookup_pack_arity(const std::string &n, size_t &out) const;
    // RAII: an instantiation body parse can throw (SFINAE probes do it routinely),
    // and a leaked scope would make a later sizeof...(P) resolve against a dead
    // binding instead of failing.
    struct PackArityScope
    {
	Program &p;
	PackArityScope(Program &pp) : p(pp) { p.push_pack_arity_scope(); }
	~PackArityScope() { p.pop_pack_arity_scope(); }
    };
    // Two-tree Phase 2: parse a member function template's retained body ONCE with
    // its param bound to a DataDefTemplateParam placeholder (a TemplateParamScope
    // pushed for the parse + eager instantiation suppressed via
    // dependent_parse_in_progress), capture the resulting TokenFunc as
    // fd->dependent_pattern, and remove it from pending_funcs (a Tree-1 RECIPE, not a
    // real function). Fully isolated (the parse_deferred_lazy_body save/restore
    // model). Returns the captured pattern, or NULL if ineligible / nothing parsed.
    TokenFunc *build_dependent_pattern(FuncDef *fd);
    // Parse-once TAG_DEFN (g++ [temp.inst]): a LOCAL class defined in a
    // member-template body is instantiated WITH the enclosing method. On the
    // eager lane the first instantiation's body parse builds the concrete
    // `<owner>__<local>` class as a side effect; when tsubst skips that body
    // parse, THIS is the producer: replay just the local class's definition
    // token subspan (retained in the template's member_template_decl) under
    // the owner's class scope — the same TokenCLASS::parse context the eager
    // lane uses, so naming, enclosing_class, ctor/dtor registration, emit
    // symbols, and deferred method bodies come out identical. Returns the
    // concrete class (pre-existing or materialized), or NULL when the decl
    // span holds no definition of that name (a class merely REFERENCED by the
    // body is not defined in it and must not materialize).
    DataDefCLASS *materialize_pattern_local_class(FuncDef *source,
						  DataDefCLASS *pattern_class,
						  DataDefCLASS *owner,
						  const std::string &enclosing_symbol);
    registration_map<DataDef *, DataDefPTR *> ptr_type_cache; // cached pointer-to-T DataDefs
    registration_map<DataDef *, DataDefREF *> ref_type_cache; // cached reference-to-T DataDefs (alias-spelled T&)
    registration_map<DataDef *, DataDefCONST *> const_type_cache; // cached const-T DataDefs
    funcdef_map_t  funcdef_map;		// function definitions
    variable_map_t literal_map;		// string literals
    namespace_map_t namespace_map;	// namespace registries (std::, etc.)
    namespace_datatype_map_t namespace_datatype_map; // namespace-owned type names
    std::map<std::string, std::vector<std::string>> inline_namespace_children;
    // Lexical namespace context. Each entry is the FULL active namespace
    // ("std::__cxx11"); back() is the active one (empty stack = global scope).
    // The idiomatic twin of class_scope_stack: a vector (not std::stack) so
    // the enclosing-chain walk stays possible. Mutate ONLY via NamespaceScope.
    std::vector<std::string> namespace_stack;
    const std::string &current_namespace() const
    {
	static const std::string global_scope;
	return namespace_stack.empty() ? global_scope : namespace_stack.back();
    }
    // RAII guard for namespace_stack — exception-safe by construction.
    class NamespaceScope
    {
	Program &pgm;
	NamespaceScope(const NamespaceScope &);
	NamespaceScope &operator=(const NamespaceScope &);
    public:
	NamespaceScope(Program &p, const std::string &ns) : pgm(p)
	{ pgm.namespace_stack.push_back(ns); }
	~NamespaceScope() { pgm.namespace_stack.pop_back(); }
    };
    // RAII guard for template_param_scopes — exception-safe by construction (the
    // idiomatic twin of NamespaceScope). Pushes one {param-name -> placeholder}
    // frame for the duration of a dependent template-body parse; pops on exit.
    class TemplateParamScope
    {
	Program &pgm;
	TemplateParamScope(const TemplateParamScope &);
	TemplateParamScope &operator=(const TemplateParamScope &);
    public:
	TemplateParamScope(Program &p, const std::vector<std::string> &names) : pgm(p)
	{
	    std::map<std::string, DataDef *> frame;
	    for ( size_t i = 0; i < names.size(); ++i )
		if ( !names[i].empty() )
		    frame[names[i]] = pgm.intern_template_param(names[i], (unsigned)i);
	    pgm.template_param_scopes.push_back(frame);
	}
	~TemplateParamScope() { pgm.template_param_scopes.pop_back(); }
    };
    // Canonical C++ spelling of the template-id being instantiated right now,
    // stashed by instantiate_template_use around the class re-parse so
    // TokenCLASS::parse can record it on the new DataDefCLASS for bodyless C++
    // method binding.
    std::string instantiating_canonical_spelling;
    // The spelling above belongs ONLY to the class the instantiation is
    // creating — a top-level (class-scope) definition. A class/struct defined
    // at FUNCTION-LOCAL (block) scope while the flag is set is a LOCAL class
    // ([class.local]) and must never take the template-id as its own canonical
    // spelling: a pattern-local `_Guard` stamped with `basic_string<...>`
    // aliased the enclosing class in every canonical-spelling type resolution.
    bool instantiating_spelling_applies_here() const
    { return !instantiating_canonical_spelling.empty() && compounds.empty(); }
    bool instantiating_dependent_surface;
    bool parsing_defaulted_member_template_constructor;
    std::vector<std::string> namespace_preference; // ordered namespace lookup; "c" means normal lexical/global resolution
    std::map<std::string, void *> dlopen_map;	// dlopen handles for loaded libraries
    // #load'd namespace functions (task #67): __dl_<ns>_<member> import name
    // -> the dlsym'd host address. The MIR import resolver consults this
    // per-link (cir_active_dl_syms) — dlsym(RTLD_DEFAULT) cannot see symbols
    // private to a #load'd handle, and the import name is madc-synthesized.
    std::map<std::string, void *> dl_symbol_map;
    std::vector<std::string> loaded_lib_paths;	// library names actually dlopen'd
						// (#load / -l) — the link-environment
						// closure a frozen forest re-loads
    // function-like macro definitions: #define NAME(params) body
    struct MacroDef {
	std::vector<std::string> params;  // parameter names
	bool variadic = false;           // trailing ... / __VA_ARGS__
	std::string variadic_param;       // GNU named varargs parameter (`args...`)
	std::string body;                 // body template with param names as placeholders
    };
    madc::dis::intern_keyed_map<MacroDef> macro_map;	// function-like macros (key = interned spelling-id)
    enum LazyKind { lkVariable = 1, lkFunction = 2, lkType = 3, lkStruct = 4 };
    struct LazyEntry { int header; LazyKind kind; };
    std::map<std::string, LazyEntry> lazy_map;	// deferred symbol registration
    madc::dis::intern_keyed_map<std::string> define_map;	// #define name value (key = interned spelling-id)
    std::set<std::string> disabled_builtin_names;	// -fno-builtin-foo from CLI/tests
    std::map<std::string, std::stack<std::string>> _macro_save_stack; // #pragma push_macro / pop_macro
    std::vector<std::string> include_paths;	// -I include search paths (for #include "file.h")
    std::vector<std::pair<std::string,std::string>> cli_defines;	// -DNAME[=VALUE] command-line defines (applied after builtins)
    // Selected C++ standard library flavor (-stdlib=), NULL = the build's
    // default. It picks a WHOLE generated search list, never a prefix — see
    // include/madc_sys_includes.h. Orthogonal to the target/object format: a
    // Mach-O target defaults to libc++, it never MEANS libc++.
    const madc_stdlib_flavor *stdlib_flavor = NULL;
    mutable std::vector<std::string> _canon_prefixes;		// cache: see sys_include_prefixes_canonical()
    mutable const madc_stdlib_flavor *_canon_prefix_flavor = NULL;
    void add_include_dir(const std::string &dir);	// normalize (trailing '/') + append to include_paths
    void add_cli_define(const std::string &def);	// split NAME[=VALUE] (bare => "1") into cli_defines
    std::map<std::string, bool> included_files;	// #include files already tokenized (require_once semantics)
    // realpath -> detected include-guard macro ("" = guard-less, always
    // re-tokenize). Drives gcc's multiple-include optimization in
    // should_tokenize_include.
    std::map<std::string, std::string> include_guard_by_file;
    std::stack<bool> ifdef_stack;	// conditional compilation state stack
    std::stack<bool> ifdef_done_stack;	// tracks if any branch in #if/#elif/#else was taken
    // --- B4a pack-time forest recording (grove payload v2; see
    // docs/plans/2026-07-04-forest-default-mode-design.md §2). Populated during
    // lex+parse ONLY when pack_recording is on (--freeze / --freeze-append);
    // consumed by madc_cir_freeze to stage the v2 segments. All hooks are
    // no-ops when the flag is off, so default lexing/parsing is unchanged.
    struct PackMacroEvent {		// one PP-export delta, in directive order
	enum : uint8_t { peDefine = 0, peDefineFn = 1, peUndef = 2 };
	std::string name;
	uint8_t     tag;
	std::string value;		// object-like body (peDefine)
	MacroDef    macro;		// function-like payload (peDefineFn)
    };
    enum PackDeclKind : uint32_t {	// decl-index entry kinds (v2 wire values)
	pdkOther = 0, pdkTypedef = 1, pdkStruct = 2, pdkClass = 3, pdkEnum = 4,
	pdkFunction = 5, pdkVariable = 6, pdkTemplate = 7, pdkNamespace = 8
    };
    enum : uint32_t {			// PackDeclEntry aux flags (v2 wire values)
	PACK_DECL_SPANS_UNITS  = 1u,	// slice crosses unit boundaries: bind must live-parse
	PACK_DECL_FUZZY_BOUNDS = 2u	// stream pushback was non-empty at a boundary
    };
    struct PackDeclEntry {
	std::string name;		// exported (namespace-qualified) name
	uint32_t    kind;		// PackDeclKind
	uint32_t    begin, end;		// GLOBAL token-stream indices [begin,end)
	uint32_t    aux;		// PACK_DECL_* flags
    };
    struct PackDeclFrame {		// one in-flight top-level decl parse
	size_t begin;			// stream cursor at open
	bool   fuzzy;			// pushback non-empty at open
	std::vector<std::pair<std::string, uint32_t> > names; // (name, kind)
    };
    bool pack_recording = false;
    bool class_registration_taps_muted = false;
    std::vector<const char *> pack_unit_order;	// first-tokenization order (interned)
    std::set<const char *> pack_units_seen;
    std::map<const char *, std::vector<PackMacroEvent> > pack_pp_exports; // unit -> ordered deltas
    std::map<const char *, std::vector<const char *> > pack_unit_edges;   // includer -> includees, in order
    std::set<std::string> pack_branch_macros;	// names PP conditionals consulted
    std::vector<PackDeclEntry> pack_decls;	// parse-time top-level decl boundaries
    std::vector<PackDeclFrame> pack_decl_stack;	// open frames (namespace bodies nest)
    // Recording hooks (PP side in lexer.cpp, decl side in parser.cpp; every
    // one gates on pack_recording so default builds pay one predicted branch).
    void pack_note_unit(const char *interned_file);
    void pack_record_define(const std::string &name, const std::string &value);
    void pack_record_define_fn(const std::string &name, const MacroDef &m);
    void pack_record_undef(const std::string &name);
    void pack_record_edge(const std::string &includee);	// includer = current source
    void pack_record_branch_macro(const std::string &name);
    const char *pack_current_unit();	// interned current source file (NULL off)
    void dump_macros(FILE *out);	// -dM: effective macro table, sorted
    void pack_open_toplevel_decl();	// loop-top: push a decl frame
    void pack_close_toplevel_decl();	// after parseStatement: pop + emit entries
    void pack_tap_name(const std::string &name, uint32_t kind); // registration tap
    void pack_tap_type(const std::string &name);   // flat + ns-qualified typedef tap
    void pack_tap_struct(const std::string &name); // struct_map-key tap
    void dump_registered_names(FILE *out); // --dump-registered: oracle side B
    std::vector<TokenBase *> ast;	// Abstract Syntax Tree in source order
    TokenStream tokens;			// parsed token stream (flat arena + cursor; P1)
    std::deque<TokenBase *> injected_tokens; // synthetic lexer output for lowered directives
    // --show-stats: token-stream traffic counters (diagnostic only).
    unsigned long long _tok_produced = 0; // tokens emitted into the stream by the lexer
    unsigned long long _tok_consumed = 0; // nextToken() advances (incl. re-reads on backtrack)
    unsigned long long _tok_reread   = 0; // distinct token objects consumed >1x
    uint32_t           _tok_max_reads = 0; // highest read_count seen on a single token
    // --show-stats: total source bytes the lexer read — main file + every header
    // (embedded + filesystem #include) + load_buffer input. Accumulated on the
    // Program (NOT on Source: each #include uses a throwaway Source that is
    // discarded on restore, so a per-Source counter would be lost).
    unsigned long long _input_bytes = 0;
    size_t input_bytes() const { return (size_t)_input_bytes; }
    // --show-stats: wall time (seconds) spent loading source into the lex stream
    // (file I/O via copybuf + embedded-header str()). Lexing time is then the
    // tokenize() wall time minus this; measured by the caller (madc.cpp).
    double _read_seconds = 0.0;
    // --show-stats: CIR build time (madc AST -> cir_node tree via
    // CirBuilder::translate_module, incl. on-demand lazy template-body
    // materialization) — the phase BETWEEN parse and c2mir. Set by madc_cir.cpp.
    double _cir_build_seconds = 0.0;
    // --show-stats: c2mir compile time (cir_node tree -> MIR module) and the JIT
    // execution wall time (the main() call, incl. lazy MIR_gen of called fns).
    // Set by the CIR backend (madc_cir.cpp); printed by madc.cpp.
    double _c2mir_seconds = 0.0;
    double _exec_seconds  = 0.0;
    // --show-stats: forest-bind startup breakdown (startup-latency R0). Map =
    // container discovery + mmap (cir_forest_map_image); open = directory +
    // string-pool/arena binds + name indexes (CirFrozenForest::open); bind =
    // the per-#include forest_bind_include walks (aux-segment decode + PP
    // install), with the per-unit self-cost list alongside; restore =
    // forest_restore_decls total, of which declidx is the all-units decl-index
    // demand-verdict sweep (materialize + unit-load time is forest-owned:
    // CirFrozenForest::_stat_mat_secs / _stat_unitload_secs).
    double _forest_map_seconds = 0.0;
    double _forest_open_seconds = 0.0;
    double _forest_bind_seconds = 0.0;
    double _forest_restore_seconds = 0.0;
    double _forest_declidx_seconds = 0.0;
    std::vector<std::pair<std::string, double> > _forest_unit_bind_costs;
    // Plain snapshot of the above plus the forest-owned counters (unit loads,
    // arena materialize, reader decode) so display code needs no
    // CirFrozenForest type. Implemented beside ensure_bind_forest (lexer.cpp).
    struct ForestBindStats {
	bool opened = false;			// a container bound this compile
	uint32_t units_total = 0;		// packed units in the container
	unsigned long long units_bound = 0;	// forest_chain closure size
	double map_secs = 0.0, open_secs = 0.0, bind_secs = 0.0;
	double restore_secs = 0.0, declidx_secs = 0.0;
	double unitload_secs = 0.0, mat_secs = 0.0;
	unsigned long long unitload_count = 0;
	unsigned long long zstd_frames = 0, zstd_bytes = 0;
	double zstd_secs = 0.0;
	unsigned long long copy_calls = 0, copy_bytes = 0;
    };
    ForestBindStats forest_bind_stats() const;
    // --show-stats: template-instantiation time + count, carved out of parse
    // time. _inst_seconds is depth-guarded (only the OUTERMOST instantiation
    // subtree accumulates wall time, so nested instantiations are not double
    // counted; parsing an instantiated body counts as instantiation cost, which
    // is the point). _inst_count is every instantiation entry (all depths). The
    // parse time MINUS _inst_seconds is the declaration-parsing share — the
    // split that bounds how much a pre-parsed header cache could save.
    double             _inst_seconds = 0.0;
    unsigned long long _inst_count   = 0;
    int                _inst_depth   = 0;
    unsigned long long _inst_opaque_count = 0;
    unsigned long long _inst_class_count = 0;
    unsigned long long _inst_alias_count = 0;
    unsigned long long _inst_fn_count = 0;
    unsigned long long _inst_member_fn_count = 0;
    unsigned long long _inst_member_ctor_count = 0;
    unsigned long long _inst_capture_count = 0;
    // --show-stats: env-gated two-tree body-instantiation engagement. A
    // tsubst "hit" means CIR built the concrete body from the retained Tree-1
    // recipe. A fallback means an instantiated member-template body had tsubst
    // metadata but still used the parsed concrete body.
    unsigned long long _tsubst_body_hits = 0;
    unsigned long long _tsubst_body_fallbacks = 0;
    struct TsubstBodyProfile {
	unsigned long long count;
	std::string sample;
	std::string reason;	// why this template shape fell back (first seen)
	TsubstBodyProfile() : count(0), sample(), reason() {}
    };
    std::map<std::string, TsubstBodyProfile> _tsubst_body_fallback_profile;
    // Class-template dispatch accounting. A parse is counted only after the
    // selected body is known to take the sole parser lane; cache and dependent
    // shells are disjoint outcomes. ClassParseReason stays typed until the
    // --show-stats rendering boundary.
    struct ClassParseProfileKey {
	std::string identity;
	ClassParseReason reason;
	bool operator<(const ClassParseProfileKey &o) const
	{
	    return identity < o.identity
		|| (identity == o.identity && reason < o.reason);
	}
    };
    struct ClassParseProfile {
	unsigned long long count;
	unsigned long long body_calls;
	unsigned long long base_specs;
	std::string sample;
	std::map<ClassDeclKind, unsigned long long> decls;
	ClassParseProfile() : count(0), body_calls(0), base_specs(0), sample() {}
    };
    unsigned long long _class_inst_pattern = 0;
    unsigned long long _class_inst_parse = 0;
    unsigned long long _class_inst_cache = 0;
    unsigned long long _class_inst_opaque = 0;
    bool class_parse_observability = false;
    // Lazy LIVE pattern capture (capture-on-repeat-demand at
    // instantiate_template_use). Off by default: with the current
    // per-instantiation pattern-lane costs (nested-template re-registration,
    // journal open/close, resolver re-derivation) the live lane measures
    // slightly net-negative on small TUs; the bound leg (pack-time capture)
    // is unaffected by this flag. Enable via MADC_CLASS_PATTERN_LIVE=1 or
    // directly in unit tests; flips to default-on when the pattern lane's
    // per-instantiation overheads land (B4+).
    bool class_pattern_live_capture = false;
    std::map<ClassParseProfileKey, ClassParseProfile> _class_parse_profile;
    std::vector<ClassParseProfileKey> _class_parse_census_stack;
    static const char *class_parse_reason_name(ClassParseReason reason);
    static const char *class_decl_kind_name(ClassDeclKind kind);
    void note_class_parse(const std::string &identity,
			  ClassParseReason reason, const std::string &sample);
    void note_class_body_parse();
    void note_class_base_spec();
    void note_class_decl(ClassDeclKind kind, unsigned long long count = 1);
    class ClassParseCensusScope
    {
	Program &pgm;
	bool active;
	ClassParseCensusScope(const ClassParseCensusScope &);
	ClassParseCensusScope &operator=(const ClassParseCensusScope &);
    public:
	ClassParseCensusScope(Program &p, const std::string &identity,
		ClassParseReason reason, const std::string &sample);
	~ClassParseCensusScope();
    };
    // User-defined function AST nodes, in source order. Parallel to the
    // ast queue. Populated by parseFunction / parseLambda; consumed by
    // Program::compile in a pre-pass to create funcnodes (labels) before
    // globals compile, so global fn-pointer inits can LEA the target label.
    std::vector<TokenBase *> pending_funcs;
    struct DeferredFunctionBody {
	Variable *var;
	Method *method;
	std::vector<TokenBase *> body_tokens;
	std::vector<TokenBase *> definition_tokens;
	// A captured trailing return type (`-> T`) of an `auto`-return method,
	// resolved when the deferred body materializes (parameters back in scope).
	std::vector<TokenBase *> trailing_ret_tokens;
	// A constructor's mem-initializer-list tokens (after ':', before the
	// body '{'), parsed at class COMPLETION like the body — initializer
	// ARGUMENTS are complete-class context ([class.base.init]): real
	// _Vector_base's `: _M_impl(..., std::move(__x._M_impl))` names a
	// member declared after the constructor.
	std::vector<TokenBase *> ctor_init_tokens;
	bool full_definition;
	const char *file;
	int line;
	int column;
	DeferredFunctionBody() : var(NULL), method(NULL), full_definition(false), file(NULL), line(0), column(0) {}
    };
    std::map<DataDefCLASS *, std::vector<DeferredFunctionBody> >
	*class_pattern_body_capture = NULL;
    std::vector<DeferredFunctionBody> *deferred_function_body_sink;
    // Mem-initializer tokens captured for the NEXT enqueue_deferred_function_body
    // (set by parseFunction's ':' arm when a class-body ctor defers).
    std::vector<TokenBase *> pending_deferred_ctor_inits;
    TokenBase *parse_ctor_initializer_list(FuncDef *func);
    // Lazy member-function-body instantiation ([temp.inst] conformance): for a
    // system-header class (typically a template instantiation), member-function
    // BODIES are NOT parsed at class-completion time — they are stashed here,
    // keyed by emit symbol (var->name), and parsed only when ODR-used. This
    // mirrors g++ (a class instantiation instantiates declarations, not member
    // definitions) and dissolves walls in unused inline bodies. The cir_builder
    // reachability fixpoint
    // materializes a deferred body the moment its symbol enters referenced_funcs.
    registration_map<std::string, DeferredFunctionBody> deferred_lazy_bodies;
    // Out-of-line member DEFINITIONS of a class template
    // (`template<class T> RET ClassName<T>::member(params){body}` — the bits/*.tcc
    // shape, e.g. std::vector::_M_realloc_insert). A class instantiation re-parses
    // only the class BODY (in-class member defs); an out-of-line def is a separate
    // top-level template, so its body was never captured -> "import of undefined
    // item". Captured at parse time keyed by "<defining-ns>::<class-name>"; when
    // ClassName<Args> is monomorphized, each captured def is substituted with the
    // concrete args and registered as a full_definition deferred_lazy_body keyed by
    // the instantiated member's emit symbol, so the body materializes on ODR-use
    // ([temp.inst]p2) exactly like an in-class-defined member (lazy, not eager —
    // unused out-of-line members never instantiate).
    struct OutOfLineMemberDef {
	std::string member_name;
	std::vector<std::string> typeparams;	// CLASS (outer) type-params
	std::vector<TokenBase *> decl;	// full decl incl body, owned clones
	// An out-of-line member TEMPLATE (`template<class T> template<class U>
	// RET S<T>::f(U){body}`, two-level head — e.g. vector::_M_realloc_insert's
	// C++11 variadic form) attaches its body to the monomorphized member as a
	// member_template_decl, instantiated per-call by the sub-gap-5 path; a plain
	// member def becomes a deferred (ODR-use-lazy) full body. inner_typeparams /
	// inner_is_pack are the MEMBER (inner) template parameters.
	bool is_member_template = false;
	std::vector<std::string> inner_typeparams;
	std::vector<bool> inner_is_pack;
	std::vector<bool> inner_is_type;	// false = non-type param (`size_t... _Indexes`)
    };
    registration_map<std::string, std::vector<OutOfLineMemberDef> >
	out_of_line_member_defs;
    struct OutOfLineMemberInstantiation {
	std::string registered_mangled;
	std::vector<TokenDataType *> arg_types_by_slot;
	std::vector<std::vector<TokenBase *> > arg_tokens_by_slot;
    };
    registration_map<std::string, std::vector<OutOfLineMemberInstantiation> >
	out_of_line_member_instantiations;
    void attach_outofline_member_instantiations(
	const std::string &class_name, const std::string &defining_namespace,
	const std::string &registered_mangled, DataDefCLASS *ddc,
	const std::vector<TokenDataType *> &arg_types_by_slot,
	const std::vector<std::vector<TokenBase *> > &arg_tokens_by_slot);
    void register_outofline_member_instantiations(
	const std::string &class_name, const std::string &defining_namespace,
	const std::string &registered_mangled, DataDefCLASS *ddc,
	const std::vector<TokenDataType *> &arg_types_by_slot,
	const std::vector<std::vector<TokenBase *> > &arg_tokens_by_slot);
    // An out-of-line NESTED-CLASS definition of a class template
    // (`template<...> class Owner<T>::Nested { ... };` — basic_istream's
    // `sentry`, [class.nest] + [temp]). NOT a specialization of Owner: the
    // class-head name is qualified. Captured keyed by "<ns>::<Owner>"; when
    // Owner<Args> is monomorphized the decl is substituted (typeparams ->
    // concrete args, the Owner template-id -> the mangled instantiation tag)
    // and parsed as a qualified nested-class definition of the instantiated
    // owner. The SHELL parses eagerly with the owner (name lookup inside the
    // owner's member bodies needs the type); method bodies inside it stay
    // ODR-use-lazy via the normal member-body deferral.
    struct OutOfLineNestedClassDef {
	std::string nested_name;
	std::vector<std::string> typeparams;	// owner type-params (positional)
	std::vector<bool> typeparam_is_pack;
	std::vector<TokenBase *> decl;	// full decl incl body, owned clones
    };
    registration_map<std::string, std::vector<OutOfLineNestedClassDef> >
	out_of_line_nested_class_defs;
    void instantiate_outofline_nested_classes(
	const std::string &class_name, const std::string &defining_namespace,
	const std::string &registered_mangled,
	const std::vector<std::vector<TokenBase *> > &arg_tokens_by_slot);
    bool parsing_cpp_struct_class;
    // Set by TokenSTRUCT::parse when delegating a UNION with class-only syntax
    // to the class parser ([class.union]); TokenCLASS::parse consumes it and
    // marks the DataDefCLASS union_layout.
    bool parsing_cpp_union_class = false;
    // `struct X final { }` delegated from TokenSTRUCT::parse: that parser must
    // CONSUME `final` before it can decide to delegate (otherwise the body
    // mis-parses), so the class parser never sees the token. Same one-class-only
    // handoff as parsing_cpp_union_class above — set before the delegation, read
    // and cleared on entry, so nested members do not inherit it.
    bool parsing_cpp_final_class = false;
    // The pseudo-namespace of the SCOPED enum whose body is currently being
    // parsed ([dcl.enum]/5: an enumerator is usable in the enum's own body
    // immediately after its definition — `general = fixed | scientific`,
    // libc++ __charconv/chars_format.h). Set by TokenENUM::parse around the
    // body loop (save/restore, nesting-safe); consulted by
    // resolve_integer_constant's unqualified-identifier arm. NULL elsewhere.
    variable_map_t *active_scoped_enum_ns = NULL;
    // Statement-level qualified-call (`php::foo(args);`): the CALLEE namespace
    // override for HEAD resolution only ([basic.lookup] — the qualification
    // applies to the called name, not the arguments). Deliberately NOT part of
    // namespace_stack (it is a qualification override, not lexical scope):
    // active_cpp_lookup_namespace() consults it first, and parseCallFunc /
    // parseCallMethod clear it before the argument loop so arguments resolve
    // in the lexical namespace. Set/restored only via QualifiedCalleeScope.
    std::string stmt_callee_namespace;
    class QualifiedCalleeScope
    {
	Program &pgm;
	std::string saved;
	QualifiedCalleeScope(const QualifiedCalleeScope &);
	QualifiedCalleeScope &operator=(const QualifiedCalleeScope &);
    public:
	QualifiedCalleeScope(Program &p, const std::string &ns)
	    : pgm(p), saved(p.stmt_callee_namespace)
	{ pgm.stmt_callee_namespace = ns; }
	~QualifiedCalleeScope() { pgm.stmt_callee_namespace = saved; }
    };
    // When set, TokenCLASS::parse parses and registers the class/struct
    // DEFINITION only and returns at the closing '}', WITHOUT consuming a
    // trailing instance declarator. Used when a method-bearing struct is
    // nested inside another aggregate's body: the class parser registers the
    // nested type, then the enclosing struct-body parser claims the trailing
    // member declarator (`struct Inner { void f(){} } member;`).
    bool class_definition_only;
    // Source-ordered top-level declarations for CIR tree generation.
    // Each entry records what was declared and in what order, matching
    // the order c2m's parser produces in its MODULE LIST. The legacy
    // JIT/compile path ignores this; only the CIR backend consumes it.
    enum class DeclKind { dkTypedef, dkStruct, dkUnion, dkEnum, dkGlobalVar };
    struct TopDecl {
	DeclKind kind;
	std::string name;	// typedef alias, struct tag, or variable name
	DataDef *dd;		// the DataDef (struct, typedef target, etc.)
	TokenDataType *tdt;	// for typedefs: the TokenDataType entry
	Variable *var;		// for global vars: the Variable
	const char *file;
	int line;
	TokenBase *origin;	// per-occurrence source token (alias/tag/decl head); NULL → use file/line
	TokenDecl *decl;	// for global vars: the TokenDecl carrying initializer (initialize/init_list); NULL → no init
	bool struct_body;	// dkTypedef only: this combined `typedef struct Tag {...} Alias;` carries the tag's full body (it is the tag's definition point). A `typedef struct Tag *p;` referencing the tag is false.
	bool forest_system;	// restored from a bound forest unit whose header path is a system include; the emission-side system-origin verdict when file/origin are NULL (a restored decl carries no parse position)
	TopDecl() : kind(DeclKind::dkStruct), dd(nullptr), tdt(nullptr), var(nullptr), file(nullptr), line(0), origin(nullptr), decl(nullptr), struct_body(false), forest_system(false) {}
    };
    std::vector<TopDecl> top_decls;
    // Host-callback registrations (libmadc register_function): the embedding
    // host exposes a native function to scripts. _parser_init declares each
    // as an ordinary prototype (add_host_callbacks), and the CIR builder
    // synthesizes a module trampoline definition
    //   RET name(params) { return __madc_host_cb_<k>([bound,] params...); }
    // whose import symbol the JIT session resolves to `entry` at MIR link.
    // Types are carried as Kind codes (not DataDef*) so registrations
    // survive Program re-creation across recompiles.
    struct HostCallbackReg {
	enum Kind { K_VOID = 0, K_BOOL, K_INT, K_REAL, K_CSTR };
	std::string name;	 // script-visible function name
	std::string import_sym;	 // __madc_host_cb_<k> the trampoline calls
	uintptr_t entry = 0;	 // host address bound to import_sym at link
	uintptr_t bound = 0;	 // deduced-form callback passed as hidden first arg (0 = none)
	int returns = K_VOID;
	std::vector<int> params;
    };
    std::vector<HostCallbackReg> host_callback_regs;
    // Names registered via a user `typedef` (populated when a typedef is
    // recorded). Used to decide when a type was written with a typedef
    // alias so CIR emits ID("alias"); distinguishes user typedefs from
    // builtin type keywords (whose token str can differ from the DataDef
    // name, e.g. `int` -> "int32_t", making a name compare unreliable).
    registration_set<std::string> user_typedef_names;
    std::stack<TokenCpnd *> compounds;	// stack to manage nested brackets
    std::vector<TokenSWITCH *> switch_stack; // active switch parse contexts for nested case/default hoisting
    std::vector<TokenCASE *> switch_case_stack; // current active case/default while parsing each switch
    TokenProgram *tkProgram;		// program token
    TokenCpnd *tkFunction;		// function we are currently in
    int try_depth;			// >0 when compiling inside a try body
    std::string cur_func_name;		// name of current function being compiled (for diagnostics)
    throwstream Throw;			// throw an error
    int script_argc;			// argc for the .mad script
    char **script_argv;			// argv for the .mad script
    bool keep_trivia = false;		// full-fidelity: preserve whitespace/comments
					// as TokenBase::leading_trivia (IDE/.mc11);
					// off by default → zero cost for batch
    std::string _trailing_trivia;	// whitespace/comments after the last token
					// (full-fidelity; reconstruct_source appends it)
    bool _include_iostream;		// #include <iostream> was seen during tokenization
    bool _include_stdio;		// #include <stdio.h> was seen during tokenization
    bool _include_string;		// #include <string> was seen during tokenization
    bool _include_ns_madc;		// #include <ns_madc> was seen — main gets the __madc_sys_init injection
    // Intern file paths so TokenBase::file pointers stay stable for
    // the program's lifetime. Lexer used to store `c_str()` of a
    // stack-local std::string into tokens — the pointer dangled the
    // moment the include scope ended, leaving every later read of
    // tb->file undefined. NOTE: this must be its OWN store —
    // `included_files` (the #include guard map) is CLEARED by
    // _tokenizer_init() per tokenize, which dangled every previously
    // interned pointer (the header-line-under-main-file diagnostic
    // misattribution, and expression-unit user tokens whose file read
    // as reused-heap garbage).
    const char *intern_file(const std::string &s) {
	return interned_files.emplace(s).first->c_str();
    }
    std::set<std::string> interned_files;	// stable token file names (never cleared)
    // Single-entry cache for finalize_pop1_rec's per-token file_id intern. Tokens
    // are finalized in stream order, so consecutive tokens almost always share the
    // same (stable, intern_file'd) file pointer — caching the last (ptr -> id) skips
    // re-hashing the filename for every one of ~144K tokens. Correct regardless of
    // pointer stability: a miss just re-interns (intern dedups to the same id).
    const char *_finalize_last_file = nullptr;
    uint32_t    _finalize_last_file_id = 0;

    // Interned identifier-spelling table (arena-model hashstr; see
    // include/stringpool.h and docs/plans/2026-06-23-arena-interning-HANDOFF.md).
    // P0 step 2: getToken() interns each ttIdentifier spelling -> uint32 id on the
    // token. Steps 3/4 re-key the hot string maps and drop the per-token string.
    madc::dis::intern_table strpool;
    uint32_t   intern_spelling(const std::string &s) { return strpool.intern(s); }
    const char *spelling(uint32_t id) const { return strpool.c_str(id); }
    // Wide-value pool (P0 slice 2): >64-bit integer values live here behind a
    // uint32 handle (TokenInt::wide_handle); ≤64-bit values stay inline in the
    // token (_token). The handle is the token-record/cir_node literal reference
    // shape the forest serializes.
    madc::dis::value_pool valpool;
    // Re-spell a token (interning Step 4): the single write path for the handful
    // of sites that RENAME an identifier/type token (operator-arity disambiguation,
    // mangled-name rewrites). Keeps the interned rec.spelling_id in sync with the
    // new bytes — the bare `t->str = x` rewrites left spelling_id stale. Defined in
    // parser.cpp (needs the full TokenIdent type).
    void set_token_spelling(class TokenIdent *t, const std::string &s);

    enum class LinkageSpec { Cpp, C };
    LinkageSpec current_linkage = LinkageSpec::Cpp;
    bool parsing_extern_decl = false;	// current declaration originated from `extern`
    bool parsing_static_decl = false;	// current declaration originated from `static` (propagates through `static struct X x;` path so parseDeclaration knows to allocate persistent storage)
    bool parsing_const_decl = false;	// current declaration originated from `const` — set vfCONSTANT on the variable
    bool parsing_inline_decl = false;	// current declaration carries the C++ `inline` specifier (TokenCppKeyword::parse sets it; parseDeclaration consumes it like parsing_static_decl) — vague linkage for external-linkage functions/variables
    bool parsing_typedef_decl = false;	// propagates through `typedef const struct ...` path

    // ---- Script mode: STD_MADC file-scope statements → synthesized main.
    // Owner plan docs/plans/2026-07-21-script-mode-auto-main.md. The parser
    // adopts non-declaration top-level statements into a lazily created
    // `int main(int argc, char **argv)` — a real TokenFunc, so every
    // downstream surface (CIR, --emit=c11, native AOT, --dump-cir) sees an
    // ordinary function. Dialect-gated in file_scope_statement_starter /
    // adopt_script_statement; declarations keep their file-scope meaning.
    TokenFunc *script_main_tf = NULL;	// the synthesized main (created at first adopted statement)
    Method *script_main_method = NULL;
    Variable *script_argc_var = NULL;	// main's params; also created on first
    Variable *script_argv_var = NULL;	// argc/argv resolution inside a statement
    bool parsing_script_statement = false;	// arms argc/argv resolution (script_param_lookup)
    bool file_scope_statement_starter(TokenBase *tb);
    bool script_statement_result(TokenBase *ts) const;
    bool adopt_script_statement(TokenBase *ts);
    void ensure_script_main(TokenBase *loc);
    void finalize_script_main();
    Variable *script_param_var(const std::string &id);
    Variable *script_param_lookup(const std::string &id);
    bool token_is_tu_origin(TokenBase *tb) const;

    // #pragma pack state, GCC semantics: `pack(N)` sets the current value,
    // `pack()` resets it, `pack(push[, N])` saves the current value (then
    // optionally sets it), `pack(pop)` restores the last saved value.
    //
    // The file is fully tokenized BEFORE parsing, so lexer-time state would
    // be stale when struct layout reads it (a balanced push/pop region has
    // already reset by parse time). Pack events therefore ride a side
    // channel keyed by the first real token AFTER the directive: the lexer
    // queues ops in _pending_pack_ops, push_token_with_literal_concat pins
    // them to the next emitted token, and nextToken() applies them one-shot
    // at the single consume chokepoint — invisible to peekToken/scanners.
    // Op encoding: {0,N} set current to N (0 = default layout), {1,N} push
    // current then set N when N != 0, {2,0} pop.
    std::stack<int> _pack_stack;	// saved values (push/pop)
    int _pack_current = 0;		// current alignment; 0 = default layout
    int pack_current() const { return _pack_current; }
    std::vector<std::pair<int,int> > _pending_pack_ops;
    std::unordered_map<const TokenBase *, std::vector<std::pair<int,int> > >
	_pragma_pack_events;
    void apply_pragma_pack_op(int op, int val)
    {
	if ( op == 1 )
	{
	    _pack_stack.push(_pack_current);
	    if ( val )
		_pack_current = val;
	}
	else if ( op == 2 )
	{
	    if ( !_pack_stack.empty() )
	    {
		_pack_current = _pack_stack.top();
		_pack_stack.pop();
	    }
	    else
		_pack_current = 0;
	}
	else
	    _pack_current = val;
    }

    bool colors;
    enum LanguageStd {
	STD_MADC,	// default: C++ keywords reserved
	STD_C78, STD_C86, STD_C88, STD_C89, STD_C90, STD_C94, STD_C95, STD_C99, STD_C11, STD_C17, STD_C23,
	STD_CPP98, STD_CPP03, STD_CPP11, STD_CPP14, STD_CPP17, STD_CPP20, STD_CPP23, STD_CPP26
    } language_std;
    // The C++ level that plain madc (STD_MADC) — and any non-C++ selection that
    // still presents as C++ — targets: the single configurable "default bar",
    // NOT a literal scattered across the predefined-macro table + the
    // __cplusplus switch. It deliberately LAGS the highest enum value
    // (STD_CPP26): the enum is what a user MAY request via `--std=`; this is what
    // gets presented when nothing is requested. Raising the bar madc defaults to
    // is one assignment here (CLI / embedder can override). See
    // cplusplus_value_for_std(). The bar is gated low because every step up
    // forces madc to parse more of the C++20+ system-header surface
    // (ranges/concepts) — see docs/plans for the raise strategy.
    LanguageStd default_cpp_std;
    // GNU dialect modifier: `--std=gnuNN` / `--std=gnu++NN` selects the base
    // standard (language_std) WITHOUT strict-ANSI conformance — gcc's actual
    // default dialect is gnu17, not c17. Feature gating stays on
    // language_std; this flag only controls strictness presentation.
    bool gnu_dialect;
    bool is_c_mode() const { return language_std >= STD_C89 && language_std <= STD_C23; }
    bool is_cpp_mode() const { return language_std >= STD_CPP98 && language_std <= STD_CPP26; }
    // gcc parity for C modes: -std=cNN defines __STRICT_ANSI__, -std=gnuNN
    // (gcc's default dialect) does not — real glibc headers branch on it
    // (features.h suppresses _DEFAULT_SOURCE under strict ANSI, hiding
    // timercmp/strdup-class declarations SMAUG needs).
    // KNOWN DIVERGENCE: STD_MADC and the C++ modes keep the captured strict
    // presentation even though plain g++ (gnu++17) is non-strict — lifting
    // it opens glibc's `!__STRICT_ANSI__` float regions (__HAVE_FLOAT128 →
    // __float128/_FloatN declarations) that madc cannot type yet. Lift once
    // __float128/_FloatN land (the __int128 P0 track's float sibling).
    bool strict_ansi_mode() const
    { return !(is_c_mode() && gnu_dialect); }
    // The __STDC_VERSION__ value a C mode mandates (gcc parity; c89/c90
    // predate the macro — NULL = leave undefined, like gcc -std=c89).
    const char *stdc_version_for_std() const {
	switch ( language_std ) {
	case STD_C94: case STD_C95: return "199409L";
	case STD_C99: return "199901L";
	case STD_C11: return "201112L";
	case STD_C17: return "201710L";
	case STD_C23: return "202311L";
	default:      return (const char *)0;
	}
    }
    // K&R-era recovery (old-style parameter declarations, file-scope
    // implicit-int definitions) is admitted ONLY under an explicit C
    // standard that predates C23 (which removed them) — never in the
    // madc dialect or any C++ mode.
    bool knr_supported() const { return language_std >= STD_C78 && language_std < STD_C23; }
    // The madc dialect is a C++ superset: like every explicit C++ mode it
    // presents to system headers as g++ (__cplusplus/__GNUG__ defined), so
    // the REAL glibc/libstdc++ headers configure their C++ surface. Only
    // explicit C standards present as plain gcc.
    bool presents_as_cpp() const { return language_std == STD_MADC || is_cpp_mode(); }
    bool auto_includes_enabled() const { return language_std == STD_MADC; }
    // A C++ reserved keyword introduced in `min_std` is active iff we are in the
    // madc dialect (reserves the full C++ keyword set) or in an explicit C++ mode
    // at/after that standard. The C++ enumerators are contiguous and ordered
    // (CPP98 < CPP03 < ... < CPP26), so the `>=` compare is a pure version floor;
    // is_cpp_mode() excludes the (lower-valued) C enumerators. NEVER active in C.
    bool cpp_keyword_active(LanguageStd min_std) const
    { return language_std == STD_MADC || (is_cpp_mode() && language_std >= min_std); }
    // The __cplusplus value a given C++ LanguageStd mandates (C++26 uses g++'s
    // provisional 202400L until the standard fixes one).
    static const char *cplusplus_value_for(LanguageStd std) {
	switch ( std ) {
	case STD_CPP98: case STD_CPP03: return "199711L";
	case STD_CPP11: return "201103L";
	case STD_CPP14: return "201402L";
	case STD_CPP17: return "201703L";
	case STD_CPP20: return "202002L";
	case STD_CPP23: return "202302L";
	case STD_CPP26: return "202400L";
	default:        return "201703L";
	}
    }
    // The __cplusplus value the ACTIVE mode presents. An explicit C++ `--std=`
    // uses that level; STD_MADC (and any non-C++ selection) falls back to the
    // single configurable `default_cpp_std` bar — no magic literal here.
    const char *cplusplus_value_for_std() const {
	return cplusplus_value_for(is_cpp_mode() ? language_std : default_cpp_std);
    }
    bool set_language_standard(const std::string &standard);
    bool set_language_standard_option(const std::string &arg);
    bool aot_tracking;
    bool aot_skip_eval_shims;	// this build's artifact can never be host-called
				// through the value ABI (standalone executable; any
				// non--shared artifact of an emit-only cross build),
				// so the CIR build skips the __madc_shim_* eval
				// adapters — keeps pure programs runtime-free (the
				// libmadc.so.0 DT_NEEDED cover-drop can fire)
    bool instrument_functions;
    bool skip_includes;		// --emit-function: lex without processing #include
    std::set<std::string> pending_auto_include_headers;
    std::set<std::string> pending_auto_include_identifiers;
    bool suppress_auto_include_scan;
    struct AotDataRef {
	uint32_t label_id;
	uintptr_t address;
	size_t data_offset;
	uint8_t imm_offset;
    };
    std::vector<AotDataRef> aot_data_refs;
    std::vector<char *> aot_string_constants;
    struct AotDiscoveredData {
	std::string name;
	void *address;
	size_t size;
	DataDef *type;
	uint32_t count;
    };
    std::vector<AotDiscoveredData> aot_discovered_data;
    std::map<uintptr_t, size_t> aot_discovered_data_index;
    struct AotDataRange {
	uintptr_t start;
	size_t size;
	size_t data_offset;
    };
    std::map<uintptr_t, size_t> aot_layout_offsets;
    std::vector<AotDataRange> aot_layout_ranges;
    std::map<uintptr_t, std::string> external_symbol_map;
    fVOIDFUNC root_fn;

    Program();
    explicit Program(MadcEngine *eng);
    // Releases the process-ambient token pools (TokenBase::_active_strpool /
    // _active_valpool, madc_active_project_types) when they point at THIS
    // Program's members — a token constructed after its owning Program dies
    // must take the guarded no-pool path, never intern into freed memory.
    ~Program();
    void attach_engine(MadcEngine *eng);
    bool lookup_aot_data_offset(uintptr_t address, size_t &out_offset) const;
    size_t aot_variable_storage_size(const Variable *var) const;
    void record_aot_variable_data(Variable *var);
    void record_aot_data(const std::string &name, void *address, size_t size,
	DataDef *type = NULL, uint32_t count = 1);

    void add_keywords();
    void add_datatypes();
    void ensure_registration_config();
    void clear_diagnostics();
    void clear_error();
    std::istream &input();
    std::ostream &output();
    std::ostream &error();
    void add_diagnostic(DiagnosticSeverity severity, DiagnosticPhase phase,
	const std::string &message, const char *file=NULL, int line=0, int column=0);
    const Diagnostic *last_diagnostic() const;
    void report_warning(DiagnosticPhase phase, const std::string &message,
	const char *file=NULL, int line=0, int column=0);
    void report_error(DiagnosticPhase phase, const std::string &message,
	const char *file=NULL, int line=0, int column=0);
    void set_error(DiagnosticPhase phase, const std::string &message,
	const char *file=NULL, int line=0, int column=0);
    void set_error(const std::string &message, const char *file=NULL, int line=0, int column=0);
    const char *diagnostic_severity_name(DiagnosticSeverity severity) const;
    const char *diagnostic_phase_name(DiagnosticPhase phase) const;
    bool can_show_diagnostic_source(const Diagnostic &diag) const;
    void print_diagnostic(std::ostream &os, const Diagnostic &diag, const char *suffix=NULL);
    void print_last_diagnostic(std::ostream &os, const char *suffix=NULL);
    void print_unrendered_diagnostic();
    void populate_builtin_registry();
    bool is_builtin_disabled(const std::string &name) const;
    void populate_namespace_registry();
    void register_function_specs(const std::vector<FunctionRegistrationSpec> &specs);
    void register_namespace_specs();
    void add_core_functions();
    void add_process_functions();
    void add_dlfcn_functions();
    void add_functions();
    void add_globals();
    void add_host_callbacks();	// declares host_callback_regs prototypes (libmadc register_function)
    void add_iostream();	// populates lazy_map for cout, cin, cerr (via #include <iostream>)
    void add_stdio();		// placeholder for #include <stdio.h> registration
    Variable *lazy_resolve(const std::string &name);	// on-demand variable/function registration
    DataDef  *lazy_resolve_type(const std::string &name);	// on-demand type/struct registration
    // Phase 6 (forest = serialized Tree-1): RECONSTRUCT symbol tables from a
    // loaded forest's typed decl records — never re-parse. Slice 1b: file-scope
    // typedefs. (Declared with an incomplete CirFrozenForest — pointer/ref only.)
    void forest_restore_decls(class CirFrozenForest &forest);
    // Phase 6 slice 2 — parse-time grove binding (opt-in --forest-bind). A
    // system #include naming a frozen grove unit BINDS instead of tokenizing:
    // its PP-export delta installs along the include DAG, then the forest's
    // decl records restore into the symbol tables (forest_restore_decls) — the
    // header is never re-parsed. All gated on
    // registration_policy.enable_forest_bind (the ONE owner of "may this
    // compile bind frozen state"), so the default path is one predicted
    // branch; knob OFF = byte-identical behavior.
    std::string forest_bind_path;	// explicit container; empty = the discovery chain
    // v24: the TU's ROOT source file (set by tokenize/tokenize_buffer). The
    // forest holds the #include files' state ONLY — never the program's — so
    // the freeze stamps every record whose defining file IS the root
    // (DF_TU_ROOT_ORIGIN / CIR_GLOBALF_TU_ROOT / CIR_TMPLF_TU_ROOT) and the
    // bind restore fences those out. The records stay in the arena for
    // --run-frozen's cross-process typeid->name closure.
    std::string forest_root_file;
    bool forest_is_tu_root_file(const char *f) const
    {
	return f && *f && !forest_root_file.empty() && forest_root_file == f;
    }
    CirFrozenForest *bind_forest = NULL;	// lazily opened on first system include
    bool bind_forest_tried = false;	// one-shot open attempt (success or fail)
    // AOT ledger carrier (forest-carriers S5): the container the emit lane
    // reads its C-lane runtime modules from under -static-libmadc. Usually
    // bind_forest itself; a SEPARATE open when the grove bind never happened
    // (a source with no system include) or fell through the v27 producer-
    // config gate — the ledger is madc's own runtime, target-specific but
    // dialect-agnostic, so a --std=c99 compile must still link it.
    CirFrozenForest *ledger_forest = NULL;
    bool ledger_forest_tried = false;	// one-shot open attempt (success or fail)
    // MIR module cache, rung 3 (JIT bind lane ONLY — the emit/dump lanes never
    // populate this, keeping their output byte-identical to live). Func names
    // exported by the container's MIR cache module: the m&l fixpoint emits a
    // forward proto instead of the loaded def for these, and the call resolves
    // as a MIR import against the loaded cache module at link.
    std::set<std::string> mir_cache_exports;
    bool forest_decls_restored = false;	// one-shot decl-record restore (forest-global for now)
    // v13: file-scope globals restored from a bound header. forest_restore_decls
    // runs during lexer #include handling, BEFORE tkProgram exists, so the globals
    // (which live in tkProgram->variables + dkGlobalVar top_decls) are staged here
    // and flushed by flush_forest_pending_globals() once tkProgram is created. The
    // name/type/flags are the loaded CirRestoredGlobal fields (type owned by the forest).
    struct PendingForestGlobal {
	std::string name; std::string ns; DataDef *type;
	uint32_t flags; uint32_t gflags; int64_t init_value;
	// v25: the ctor-args raw-token run (CIR_GLOBALF_CTOR_ARG_TOKENS) — a
	// span into the bound forest's arena tokbytes; the flush re-runs the
	// args-list parse over it to rebuild TokenDecl::ctor_args.
	const uint8_t *ctor_bytes; uint32_t ctor_len, ctor_count;
	const char *ctor_file;
	bool system;		// declared by a system-header forest unit (TopDecl.forest_system for the flushed decl)
	PendingForestGlobal() : type(NULL), flags(0), gflags(0), init_value(0),
				ctor_bytes(NULL), ctor_len(0), ctor_count(0),
				ctor_file(NULL), system(false) {}
    };
    std::vector<PendingForestGlobal> forest_pending_globals;
    // Restored FLAT datatype_map registrations (typedef/enum/class names from a
    // bound header). The lexer PROMOTES any identifier found in the flat
    // datatype_map to a TokenDataType at TOKENIZE time (getToken ~4978), and in
    // a live compile tokenize fully precedes parse — no user-header type ever
    // influences the root's token shapes. An EAGER restore write (mid-tokenize,
    // during #include handling) gave the consumer's remaining tokens a shape
    // live never produces (`struct fd_set` — the restored typedef promoted the
    // tag position to a datatype token → "Expecting '{' or identifier after
    // struct"). Stage the writes here; the post-tokenize flush applies them in
    // restore order (last wins, the live registration semantics) before parse.
    // Namespace/struct/template maps stay eager — the lexer never reads them.
    std::vector<std::pair<std::string, TokenDataType *> > forest_pending_datatypes;
    std::set<std::string> forest_pending_datatype_names;	// staged-key guard
    // RC2: free-function declarations restored from a bound header. Same deferral
    // as the globals — the Variable lives in tkProgram scope, so registration
    // (funcdef_map + addVariable + Method, the parseFunction prototype shape)
    // waits for the post-tkProgram flush. The FuncDef is owned by the forest.
    struct PendingForestFunc {
	std::string name;
	FuncDef *fd;
	Variable *mvar;		// non-NULL: a restored class METHOD — the class's own
				// Variable (live keeps ONE object shared by tkProgram
				// scope and methods/method_map; its Method(owner_class)
				// is attached at materialization)
	// v26 piece (a): the fn's NAMED parameters (restored aliasrec run) —
	// the flush fills the new Method's parameters from these so a deferred
	// free-fn body's re-parse resolves its parameter names.
	std::vector<std::pair<const char *, DataDef *> > mparams;
	PendingForestFunc() : fd(NULL), mvar(NULL) {}
    };
    std::vector<PendingForestFunc> forest_pending_funcs;
    // v26: origin file of each plain/anonymous-enum ENUMERATOR constant
    // (TokenENUM's global branch — the constants live in tkProgram->variables
    // with no TopDecl and no back-link to the enum tag). Stamped at the one
    // live registration under forest_arena_enabled; the freeze reads it to
    // serialize the constant (CIR_GLOBALF_CONST_SCALAR) and classify its
    // TU-root fence.
    std::map<std::string, const char *> forest_enum_const_origin;
    // Default-arg RAW-TOKEN capture (parseFunction's `= expr` param branches).
    // begin() returns true and snapshots the stream position (buffer cursor + a
    // copy of the pushback LIFO) when arena recording is on, or when the caller
    // explicitly requests the one-time ClassPattern definition capture; end()
    // clones the consumed run — the popped pushback entries (a template-
    // instantiation replay feeds parseFunction from the injection LIFO — the
    // v26 widening; before it, every instantiated method's default silently
    // failed to capture) followed by the consumed buffer range, compensating a
    // consumed-then-pushed-back stop token — into `out`. The clones ride
    // FuncDef::param_default_tokens into the DK_FUNC record's paramrec runs.
    struct DefCapState { size_t cap_begin; std::vector<TokenBase *> pb; };
    bool param_default_capture_begin(DefCapState &st,
				     bool for_class_pattern = false);
    void param_default_capture_end(const DefCapState &st,
				   std::vector<TokenBase *> &out);
    // v21: body-bearing MEMBER function templates restored from a bound header
    // (CIR_TMPLK_MEMBER records). The flush HYDRATES the restored placeholder
    // FuncDef (funcdef_map[key], restored verbatim from its methodrec at its
    // saved __oN rank) with the pattern fields the live registration derives
    // from the tokens; only when the placeholder did not restore does it fall
    // back to the full re-run (register_skipped_class_template_function, which
    // mints a fresh rank). Registration needs tkProgram, hence the stage.
    struct PendingForestMemberTmpl {
	DataDefCLASS *owner;
	std::string key;			// the placeholder's funcdef_map symbol (the record key)
	std::string disp;			// live method_display_name (the declarator name)
	std::vector<TokenBase *> tokens;	// decl + params + body (sans template<> header)
	std::vector<TokenBase *> ret_tokens;	// v34 decl-only: the dependent return-type range (no decl tokens exist)
	std::vector<std::string> typeparams;
	std::vector<bool> is_pack;
	// Per-param TYPE-ness (parallel to typeparams) — the record's
	// CIR_TMPLP_IS_TYPE bit. A non-type member-template param
	// (`template <unsigned long __a>`) must thaw as non-type or the
	// restored pattern instantiates as if it took a type.
	std::vector<bool> is_type;
	// v36: per-param DEFAULT token runs (parallel to typeparams; empty
	// run = no default) — a member template's [temp.deduct]/8 SFINAE
	// payload (`typename = decltype(declval<_Tp1&>().~_Tp1())`).
	std::vector<std::vector<TokenBase *> > typeparam_defaults;
	// v38: per-param CONSTRAINT-type runs (parallel; empty =
	// unconstrained) — ride the record's spec slot like the FN lane's
	// typeparam_constraints (v33 precedent).
	std::vector<std::vector<TokenBase *> > typeparam_constraints;
	PendingForestMemberTmpl() : owner(NULL) {}
    };
    std::vector<PendingForestMemberTmpl> forest_pending_member_tmpls;
    void flush_forest_pending_globals();	// build Variable + dkGlobalVar TopDecl (post-tkProgram); also registers pending free functions
    std::vector<uint32_t> forest_chain;		// bound units, include order (bind-order record)
    std::set<uint32_t> forest_chain_set;	// membership + DAG-walk prune
    std::set<uint32_t> forest_bind_walking;	// units on the in-flight bind recursion (cycle break)
    // The ordered carrier discovery chain (explicit → self-image → library
    // image → sidecars → $MADC_FOREST). Walked by BOTH forest consumers;
    // require_config_match applies the v27 producer-config gate (grove bind
    // yes, AOT ledger no). Sets config_mismatch when an arm was rejected for
    // that reason alone. header_only stops at the container directory —
    // enough for the container-global segments (the AOT ledger), and it does
    // NOT need a live string pool, which a no-parse lane has no reason to own.
    // Implemented in lexer.cpp.
    CirFrozenForest *probe_forest_chain(bool require_config_match,
					bool &config_mismatch,
					bool header_only = false);
    CirFrozenForest *ensure_bind_forest();	// open on first use; NULL if unavailable
    // The container the AOT ledger is read from (-static-libmadc, S5): the
    // already-bound one when it opened, else the SAME discovery chain walked
    // with the producer-config gate off and stopping at the directory (the
    // ledger is a container-global segment). NULL = no carrier at all.
    CirFrozenForest *ensure_ledger_forest();
    void forest_missing_fallback(bool config_mismatch); // discovery exhausted: apply forest_missing_policy (mismatch = container seen, wrong std/-D)
    std::string forest_probed_arms() const;	// the arms probe_forest_chain walked, for the failure diagnostics (one owner)
    int forest_unit_for_include(const std::string &incfile); // spelling/path lookup; -1 miss
    void forest_bind_include(uint32_t unit);	// bind time: DAG walk — install PP + arm chain
    void forest_install_pp(uint32_t unit);	// apply one unit's frozen macro delta to the live tables
    void add_namespaces();
    void add_madc_namespace();
    void add_array_methods();	// native count()/size() on the builtin array (ddARRAY)
    bool is_namespace_registration_enabled(const std::string &name) const;
    bool is_dynamic_library_loading_enabled() const;
    bool is_auto_library_loading_enabled() const;
    bool is_dynamic_symbol_fallback_enabled() const;
    bool is_runtime_eval_source_scope_access_enabled() const;
    bool is_runtime_eval_expression_scope_access_enabled() const;
    bool is_embedded_header_allowed(const std::string &name) const;
    // True iff a real system header named `name` exists in a glibc/libstdc++
    // include dir (i.e. NOT the compiler-owned freestanding dir, and not a
    // madc-own header with no real twin). Used to bypass embedded system-library
    // shims to the real headers while keeping madc-own + freestanding embedded.
    bool embedded_header_is_system_library_shim(const std::string &name) const;
    // True iff a search directory that OUTRANKS madc's embedded set supplies
    // `name`. The embedded freestanding headers ARE madc's compiler resource
    // dir, so they sit at the compiler_owned_include_dir() slot: C++ stdlib
    // dirs (and every -I dir) come before it and win; C library dirs come after
    // it and lose. Lets a real libc++/libstdc++ wrapper header take precedence
    // while an unsupplied name still resolves from the embedded copy.
    bool embedded_header_outranked(const std::string &name) const;
    bool is_dynamic_symbol_allowed(const std::string &name) const;
    bool is_known_namespace(const std::string &name) const;
    Variable *runtime_eval_scope_target(Variable *var) const;
    Variable *runtime_eval_scope_public_target(Variable *var);
    void collect_runtime_eval_scope_variables(std::vector<Variable *> &out) const;
    // The variable whose declaration INITIALIZER is currently being parsed
    // (saved/restored around the init-expression parse, so nesting is safe).
    // Runtime-eval scope capture must skip it: `int x = madc::eval_*(…)`
    // would otherwise capture the uninitialized x AND emit the capture
    // setter ahead of x's C declaration.
    Variable *decl_init_self = NULL;
    void set_namespace_preference(const std::vector<std::string> &order, TokenBase *tb = NULL);
    Variable *find_namespace_member(const std::string &ns_name, const std::string &member_name);
    std::string canonical_nested_namespace(const std::string &parent, const std::string &comp);
    std::vector<std::string> inline_namespace_descendants(const std::string &ns) const;
    std::string canonical_namespace_path(const std::string &base, const std::string &dotted);
    Variable *resolve_preferred_identifier(class TokenIdent *ident_tb, bool expression_head);
    void set_expression_context_root(const madc::value *root);
    void clear_expression_context_root();
    bool has_expression_context_root() const;
    TokenBase *resolve_expression_context_identifier(class TokenIdent *ident_tb);
    TokenBase *resolve_expression_context_member(TokenBase *lhs, class TokenIdent *member_tb);
    std::string current_source_directory();
    bool include_already_seen(const std::string &path);
    std::string resolve_include_path(const std::string &incfile, bool is_system);
    std::string resolve_include_next_path(const std::string &incfile);
    bool is_system_header_path(const char *path) const;
    // The ONE reader of the generated include tables: every consumer asks these
    // rather than the globals, so selecting a -stdlib= flavor switches the whole
    // search order in one place. Both fall back when the build host had no
    // compiler to probe.
    const char *const *sys_include_paths() const;
    const char *compiler_owned_include_dir() const;
    // realpath'd copy of the above, cached per flavor — a compiler's reported
    // search list is not always canonical (clang: `.../bin/../include/c++/v1`).
    const std::vector<std::string> &sys_include_prefixes_canonical() const;
    // -stdlib=<name>: true when consumed. An unknown/unbuilt flavor is NOT
    // consumed, so the caller reports it (available flavors are a build-host
    // property, so the diagnostic has to name what this binary actually has).
    bool set_stdlib_flavor_option(const std::string &arg);
    std::string stdlib_flavor_names() const;	// ", "-joined, for that diagnostic
    // The selected flavor, defaulted: table entry 0 when no -stdlib= was given.
    const madc_stdlib_flavor *active_stdlib_flavor() const;
    // Push the std ABI inline namespace into the mangler when `name` is one of
    // the stdlib config macros (_LIBCPP_ABI_NAMESPACE / _GLIBCXX_USE_CXX11_ABI).
    // Called at every define_map write site: directive, forest replay, CLI -D.
    void note_std_abi_define(const std::string &name, const std::string &value);
    std::string expandIfMacros(const std::string &raw);
    bool should_tokenize_include(const std::string &path);
    bool auto_include_standard_identifier(const std::string &word);
    void inject_pending_auto_includes();
    void expand_pending_auto_include_macros(size_t original_start);
    std::vector<TokenBase *> tokenize_auto_include_define(const std::string &value,
							  const TokenBase *origin);
    void mark_embedded_include_flag(const std::string &incfile);
    void push_runtime_scope();
    void pop_runtime_scope();
    static Program *active_runtime_program();
    bool runtime_eval_source(const std::string &source_text,
			     madc::value &result,
			     const std::string &display_name = "__madc_runtime_eval",
			     const madc::value *context = NULL,
			     const char *wrapper_return_type = NULL);
    bool runtime_eval_expression(const std::string &expression,
				 madc::value &result,
				 const std::string &display_name = "__madc_runtime_eval_expression",
				 const madc::value *context = NULL);

    Variable *addFunction(std::string, datatype_vec_t, fVOIDFUNC, bool isMethod=false, bool builtin_registration=false);

    // manage compound nesting
    void pushCompound();
    void popCompound();
    static std::string hoisted_decl_symbol(const std::string &owner_symbol,
					    const std::string &source_name,
					    size_t ordinal,
					    HoistedDeclKind kind);
    std::string function_body_emission_symbol(const Variable &var) const;
    HoistedDeclIdentity declare_hoisted_declaration(TokenCpnd *scope,
						    HoistedDeclKind kind,
						    const std::string &source_name,
						    TokenBase *origin);
    bool find_hoisted_declaration(TokenCpnd *scope, HoistedDeclKind kind,
					  const std::string &source_name,
					  HoistedDeclIdentity &out) const;
    void remember_hoisted_identity(const HoistedDeclIdentity &identity,
				   TokenBase *origin);
    bool function_local_class_identity(DataDefCLASS *cdd,
				       HoistedDeclIdentity &out) const;

    // generate tokens
    TokenBase *getToken();
    TokenBase *getRealToken();
    void consume_directive_line_tail();
    // One pragma implementation, two callers: handle_pragma_body() reads the
    // pragma text off `source` wherever it came from, and
    // handle_pragma_operator() destringizes a _Pragma("...") operand into
    // `source` before calling it. Keeping the body text-driven is what stops
    // the operator needing a second copy of the pack / push_macro handling.
    void handle_pragma_body();
    void handle_pragma_operator();
//  TokenProgram *tokenize(std::istream &);
    TokenProgram *tokenize(const char *);
    // Bind this Program's pools to the process-global active-owner statics
    // (TokenBase spelling()/wival(), cir_node datadef_id). Any phase that
    // processes a Program's tokens after ANOTHER Program has tokenized/parsed
    // (--project builds every TU's tree after all TUs are parsed) must
    // re-activate the owning Program first.
    void activate_token_pools();
    // Full-fidelity source reconstruction from the token stream (requires
    // keep_trivia set before tokenizing). See TokenBase::leading_trivia.
    std::string reconstruct_source();
    TokenProgram *tokenize_buffer(const std::string &source_text,
				  const std::string &display_name);
    // C/C++ translation phase 6: adjacent string literals concatenate.
    // Funnel every tokens.push_back through this helper so an
    // included `SYSTEM_DIR "file.dat"` (= `"../system/" "file.dat"`)
    // ends up as one merged literal, not two adjacent tokens whose
    // first one gets dropped by parser exStack semantics.
    void push_token_with_literal_concat(TokenBase *tb);
    void pin_pending_pack_ops(TokenBase *tb);

    // for debugging
    void printt(TokenBase *);
//  void showerror(std::istream &);

    // accessing token queue
    inline TokenBase *peekToken() { if (tokens.empty()) return NULL; return tokens.front(); }
    inline TokenBase *prevToken() { return _prv_token; }
    inline TokenBase *curToken()  { return _cur_token; }
    inline void resetPrevToken() { _prv_token = NULL; }
    inline void setTokenContext(TokenBase *current, TokenBase *previous)
    {
	_cur_token = current;
	_prv_token = previous;
    }
    inline void pushToken(TokenBase *t) { tokens.push_front(t); }

    inline bool keywordStartsUnaryOperandContext(TokenID id)
    {
	return id == TokenID::tkRETURN
	    || id == TokenID::tkCASE
	    || id == TokenID::tkTHROW;
    }

    // helper: is prevToken in a position where the next operator would be unary?
    // true when prevToken is NULL, ;, {, (, ,, =, or any operator except ) ],
    // and postfix ++/--
    inline bool isUnaryPosition()
    {
	if ( !_prv_token ) return true;
	TokenID id = _prv_token->id();
	if ( id == TokenID::tkSemi || id == TokenID::tkOpBrc
	||   id == TokenID::tkOpBrk || id == TokenID::tkComma
	||   id == TokenID::tkAssign ) return true;
	if ( _prv_token->is_operator()
	&&   id != TokenID::tkClBrk && id != TokenID::tkClSqr
	&&   id != TokenID::tkInc && id != TokenID::tkDec ) return true;
	// Only expression-leading keywords open a unary operand context.
	// Contextual keyword identifiers such as `class` can also appear as
	// member names; after `p->class`, a following `&&` is binary logic,
	// not GNU label-address syntax.
	if ( _prv_token->type() == TokenType::ttKeyword )
	    return keywordStartsUnaryOperandContext(id);
	return false;
    }
    // helper: is the CURRENT name in class-member-access position — directly
    // after `.` or `->`? [basic.lookup.classref]/1 looks such a name up in the
    // CLASS's scope first, so a member named like a TYPE is the member, never
    // the type. Without this, `lk.mutex()` (libc++ unique_lock's accessor,
    // named for class `mutex`) was read as a functional cast and produced an
    // object where a pointer was required.
    inline bool isMemberAccessPosition()
    {
	if ( !_prv_token ) return false;
	TokenID id = _prv_token->id();
	return id == TokenID::tkDot || id == TokenID::tkDeRef;
    }
    // helper: is prevToken in a position where the next operator would be postfix?
    // true when prevToken is ), ], or a non-operator value token
    inline bool isPostfixPosition()
    {
	if ( !_prv_token ) return false;
	TokenID id = _prv_token->id();
	if ( _prv_token->type() == TokenType::ttKeyword )
	    return !keywordStartsUnaryOperandContext(id);
	// Symbols that open or continue expression contexts aren't values
	// either — `{`, `(`, `,`, `;`, `=` mean the next `-` / `!` is
	// unary, not binary. Without this guard `int x[] = { -5 };`
	// converted TokenNeg → TokenSub at the unary-after-`{` slot and
	// emitted a binary subtraction missing its left operand.
	if ( id == TokenID::tkOpBrc || id == TokenID::tkOpBrk
	||   id == TokenID::tkComma || id == TokenID::tkSemi
	||   id == TokenID::tkAssign )
	    return false;
	return id == TokenID::tkClBrk || id == TokenID::tkClSqr
	    || id == TokenID::tkInc || id == TokenID::tkDec
	    || !_prv_token->is_operator();
    }
    inline TokenBase *nextToken()
    {
	if ( tokens.empty() )
	    throw "Unexpected end of data";
        _prv_token = _cur_token;
	_cur_token = tokens.front();
//	DBG(cout << "nextToken(" << (int)ret->type() << ", " << (int)ret->id() << ')' << endl);
	tokens.pop_front();
	// Update global parse position so newly created tokens inherit it
	if ( _cur_token ) {
	    ++_tok_consumed;
	    if ( ++_cur_token->read_count == 2 )
		++_tok_reread;
	    if ( _cur_token->read_count > _tok_max_reads )
		_tok_max_reads = _cur_token->read_count;
	    TokenBase::_parse_file   = _cur_token->file;
	    TokenBase::_parse_line   = _cur_token->line;
	    TokenBase::_parse_column = _cur_token->column;
	    // #pragma pack events pinned to this token (see _pragma_pack_events):
	    // applied once, at first consumption — the empty() guard keeps the
	    // hot path free for the (usual) pack-less TU.
	    if ( !_pragma_pack_events.empty() )
	    {
		auto pe = _pragma_pack_events.find(_cur_token);
		if ( pe != _pragma_pack_events.end() )
		{
		    for ( size_t i = 0; i < pe->second.size(); ++i )
			apply_pragma_pack_op(pe->second[i].first,
					     pe->second[i].second);
		    _pragma_pack_events.erase(pe);
		}
	    }
	}
	return _cur_token;
    }
    // parse tokens into AST
    bool load_file(const char *fname);
    bool load_buffer(const std::string &source_text,
		     const std::string &display_name);
    bool parse(TokenProgram *);
    TokenBase *parse_expression_unit(TokenProgram *);
    void parseIdentifier(TokenIdent *);
    void parseFunction(DataDef &, std::string &, DataDefCLASS *owner_class = NULL,
		       std::vector<DataDef *> *multi_ret = NULL,
		       bool return_ref = false,
		       std::string return_typedef_alias = std::string(),
		       bool static_class_method = false,
		       bool inline_specified = false);
    TokenBase *parseKeyword(TokenKeyword *);
    TokenBase *parseCallFunc(TokenCallFunc *);
    // Consume `{ ... }` from the stream, appending its scalars to `args` and
    // flattening nested braces (the declaration path's model).
    void collect_braced_init_args(std::vector<TokenBase *> &args);
    TokenBase *parseCallMethod(TokenCallMethod *);
    // C++17 init-statement, shared by `if` and `switch` ([stmt.pre]): call
    // immediately after consuming the opening `(`. When a top-level `;`
    // precedes the matching `)`, parse the init-statement (simple-declaration
    // or expression-statement), consume its trailing `;`, and leave the stream
    // positioned at the condition/switch-expression. Returns the parsed
    // init-statement node, or NULL when no init-statement is present (stream
    // untouched). The init-statement's declarations share the condition's
    // enclosing scope.
    TokenBase *parse_optional_init_statement();
    // Consume the operator symbol token(s) following an `operator` keyword token
    // and return the canonical operator-function-id name ("operator<",
    // "operator()", "operatornew", "operator[]", …). The `operator` token itself
    // is the argument; its trailing symbol(s) are consumed from the stream.
    // When `consumed` is non-null, each consumed symbol token is appended to it
    // (so angle-bracket scanners can re-emit the operator-id verbatim).
    std::string parseOperatorId(TokenBase *operator_tok,
				std::vector<TokenBase *> *consumed = NULL);
    // True when `t` is the `operator` keyword that introduces an
    // operator-function-id. The scanners use this so an operator symbol
    // (`operator<`, `operator>>`, …) is treated as part of a NAME, never as an
    // angle-bracket / delimiter — fixing the operator< template mis-parse in
    // ONE place instead of every hand-rolled scanner.
    bool isOperatorIdStart(TokenBase *t);
    // Stream form of the balanced-delimiter step: `t` was already pulled via
    // nextToken(). Update `d`; if `t` is an operator-id keyword, consume its
    // trailing symbol token(s) from the stream (appended to `extra` if given so
    // a token-collecting caller can re-emit the full operator-function-id).
    void delimStepStream(TokenBase *t, DelimDepth &d,
			 std::vector<TokenBase *> *extra = NULL);
    // Constant-expression evaluator (recursive descent over the token stream).
    // Each rung consumes from the stream via nextToken()/peekToken() and folds
    // an integer-constant-expression — used for array dimensions, bit-field
    // widths, case labels, static_assert, enum values, etc. The rungs follow
    // C operator precedence; `parse_constant_integer_expression` is the entry.
    // The rungs compute in the 128-bit fold carrier (madc_wide_int, P0 slice 3
    // — gcc's wide_int model); int64 consumers truncate at the assignment
    // boundary, which is gcc's own #if/intmax_t semantics.
    madc_wide_int parse_constant_primary();
    madc_wide_int parse_constant_mul();
    madc_wide_int parse_constant_add();
    madc_wide_int parse_constant_shift();
    madc_wide_int parse_constant_rel();
    madc_wide_int parse_constant_eq();
    madc_wide_int parse_constant_band();
    madc_wide_int parse_constant_bxor();
    madc_wide_int parse_constant_bor();
    madc_wide_int parse_constant_land();
    madc_wide_int parse_constant_lor();
    // Short-circuit token-skip: consume (without evaluating) the RHS operand of
    // a `&&`/`||` whose result the LHS already determines. C++ [expr.const]: the
    // skipped operand need not be a constant expression. stop_at_and=true for a
    // `&&` RHS (a bor-operand, ends at the next `&&`); false for a `||` RHS (a
    // land-operand, spans `&&`). Keeps the cursor positioned for the caller.
    void skip_const_logical_operand(bool stop_at_and);
    madc_wide_int parse_constant_ternary();
    madc_wide_int parse_constant_integer_expression();
    // Materialize a folded constant as a TokenInt: values in int64 range keep
    // the historical typing (default int; >32-bit magnitude widens to
    // ddINT64/ddUINT64); a wider value stores its low 64 bits on the token,
    // parks the full value in Program::valpool under wide_handle, and types
    // the token ddINT128/ddUINT128 (case labels, fold results).
    TokenInt *make_folded_integer_token(madc_wide_int v);
    // C++20 requires-expression evaluation (`requires` already consumed by the
    // caller): parse the optional `(param-list)` then `{ requirement-seq }`,
    // and return 1 iff every requirement is satisfied. Params are modeled as
    // `std::declval<Type&>()` values substituted into each requirement; a
    // requirement's expression is checked WELL-FORMED via parseExpression in
    // unevaluated context (constraint_expression_well_formed). Used by the
    // `requires` arm of parse_constant_primary so concept constraints that
    // contain requires-expressions fold to a constant.
    int64_t evaluate_requires_expression_constant();
    // True iff `exprtoks` resolves as a well-formed expression in unevaluated
    // (decltype-like) context. On success, *out_type (if non-NULL) receives the
    // expression's DataDef. Isolated stream + muted diagnostics: a SFINAE-style
    // miss leaves no error trail.
    bool constraint_expression_well_formed(const std::vector<TokenBase *> &exprtoks,
					   DataDef **out_type = NULL);
    // `if constexpr` support (C++17 [stmt.if]/2). The stream is positioned just
    // AFTER the condition's `(`; collect the balanced condition tokens (consuming
    // the matching `)`) and fold them to a constant. Returns true + value on a
    // successful fold; on failure restores the stream (condition tokens + `)` back)
    // and returns false so the caller falls back to a runtime `if`.
    bool fold_if_constexpr_condition(int64_t &out);
    // Skip ONE statement's tokens (a `{...}` block, balanced, or a single statement
    // through its terminating `;`) WITHOUT parsing — so a discarded `if constexpr`
    // branch is never instantiated or emitted.
    void skip_discarded_statement();
    // Speculatively fold a static-member initializer ('=' already consumed) to a
    // constant int, expecting ';'. Consumes the initializer on success, restores
    // and returns false otherwise. See parser.cpp.
    bool capture_constant_initializer_value(int64_t &out,
					    bool brace_form = false);
    // Parse a bit-field width `: N` (the ':' already consumed) for a member of
    // integer type `member_dd`; `named` rejects a zero width; `target` supplies
    // the storage-size rule. Shared by the struct and class body parsers.
    size_t parse_bitfield_width(TokenBase *loc, DataDef *member_dd, bool named, DataDefSTRUCT &target);
    // Capture a C++11 default member initializer (NSDMI: `= expr` / `{...}`) that
    // begins at `tn` (the token after a data-member declarator), parse it in an
    // isolated stream, and record it under member name `mname` on `dds`. Returns
    // the token following the initializer (the `,`/`;`), or `tn` unchanged if `tn`
    // does not begin an initializer. Shared by the struct and class body parsers.
    TokenBase *capture_member_default_init(TokenBase *tn, DataDefSTRUCT *dds,
					   const std::string &mname);
    // Array-dimension classification: decide whether the upcoming `[ … ]`
    // dimension is a VLA (needs a runtime value) or a constant fold.
    // `bracket_dim_needs_runtime_value` is the entry; the other three are its
    // helpers (speculative constant-parse, runtime-name scan, sizeof/alignof
    // fold-query probe). Scans the token stream from the current position.
    bool bracket_dim_constant_expression_parses();
    bool bracket_dim_uses_runtime_value(
		const std::set<std::string> *runtime_names = NULL);
    bool bracket_dim_has_constant_fold_query();
    bool bracket_dim_needs_runtime_value(
		const std::set<std::string> *runtime_names = NULL);
    // After a method call's arguments are parsed, re-bind `tc` to the overload
    // of `cls`'s method `id` that best matches the argument types (the initial
    // findMethod() bound the first by-name match). Returns `tc` unchanged when
    // it already names the best overload, or a fresh TokenCallMethod on `recv`
    // bound to the winning overload (same parameters / parent_expr / position).
    class TokenCallMethod *reselect_method_overload(class TokenCallMethod *tc,
		Variable &recv, class DataDefCLASS *cls, const std::string &id);
    // [over.match.funcs]/4 — cv of the implicit object ARGUMENT a member call
    // on `recv` supplies. The hidden __this receiver takes the ENCLOSING
    // method's cv (a const member's this points at const T); a named receiver
    // takes its declared constness (vfCONSTANT-family flags or a DataDefCONST
    // identity, reference-transparent). 1 = const, 0 = non-const, -1 = unknown.
    int implicit_object_constness(Variable &recv);
    // Static-member-call analogue of reselect_method_overload: a qualified
    // static call (`Owner::m(args)`) resolves its callee by name+arity BEFORE
    // the args are parsed, so once the arg types are known reselect the overload
    // by [over.match] + [temp.func.order] (findMethodOverload's partial-order
    // tiebreak). Returns a NEW TokenCallFunc bound to the better overload (with
    // the member-template instantiation hooks re-run on it) when one is found,
    // else the original `tc`.
    class TokenCallFunc *reselect_static_member_overload(class TokenCallFunc *tc,
		class DataDefCLASS *owner, const std::string &member);
    // The class-object DataDef an operand expression DENOTES for operator /
    // overload resolution: its datadef's class, or — for a reference-typed
    // expression / a REFERENCE variable (`const A &p`, vfREFERENCE stored as
    // DataDefPTR(A)) — the referenced class. A plain `A*` pointer operand
    // stays NULL: only the reference representation is transparent here.
    class DataDefCLASS *operand_object_class(TokenBase *operand);
    // The VALUE view of an operand's type (reference expression / vfREFERENCE
    // variable -> the referenced type) for deduction and overload ranking.
    // A CALL operand types by its RESOLVED callee's return — see
    // resolved_call_funcdef.
    DataDef *operand_value_datadef(TokenBase *operand);
    // The RESOLVED callee of a call token whose parse-bound Variable may be an
    // arbitrary member of a late-bound namespace overload set (overloads /
    // fn-template instantiations register after the call site parses). Re-ranks
    // the set with the call's argument value types; the parse-side mirror of
    // CirBuilder::call_target_funcdef. Returns the bound FuncDef when no set
    // applies; sets *no_winner when a set applies but no candidate is viable.
    FuncDef *resolved_call_funcdef(class TokenCallFunc *tc,
				   bool *no_winner = NULL);
    // Type an operator expression on a class-object operand with the operator's
    // return type (Part A of generic operator-overload support). No-op unless the
    // left operand is a class object declaring the matching binary operator, or
    // (W2) a captured FREE namespace operator on the operand classes returns a
    // class by value (std::operator+ on strings).
    void resolve_object_operator_type(class TokenOperator *to);
    // Array-to-pointer decay ([conv.array]) for an operator operand: returns the
    // decayed `element *` type for a fixed-array variable / array member /
    // array-typed expression, else NULL. See parser.cpp.
    DataDef *array_decay_pointer(TokenBase *operand);
    // The return CLASS of a captured FREE namespace binary operator on class
    // operands whose return is a class BY VALUE deducing to one of the operand
    // classes. Structural template-head check only — full deduction, overload
    // arbitration, and symbol binding happen at lowering
    // (CirBuilder::resolve_free_operator_byvalue). NULL = no such operator.
    DataDef *free_binary_operator_return_class(class DataDefCLASS *lc,
		const std::string &opname, TokenBase *right);
    // Literal-lhs sibling (`"pre" + s`): param[0] non-class pointer/value,
    // param[1] the class by const-ref; returns the by-value return class.
    DataDef *free_binary_operator_return_class_nonclass_lhs(TokenBase *left,
		const std::string &opname, TokenBase *right);
    TokenBase *parseCompound();
    TokenBase *parseStatement(TokenBase *);
    TokenBase *parseDeclaration(TokenDataType *, bool is_static = false);
    DataDefPTR *getPointerType(DataDef *base);
    DataDefREF *getReferenceType(DataDef *base);
    // const-qualify a type: const T (idempotent — getConstType(const T) == const T).
    // Cached in const_type_cache. Const has no runtime/ABI effect; this exists for
    // TYPE IDENTITY (so const T != T survives deduction / instantiation keying).
    // See docs/plans/2026-06-19-const-qualified-types.md.
    DataDefCONST *getConstType(DataDef *base);
    // Id-addressable derived-type API — the boundary adapter for the type table
    // (design docs/plans/2026-06-12-type-table-value-abi-design.md §2/§6.1).
    // "pointer-to(id)" / "reference-to(id)" / "const(id)" resolved by typeid:
    // operand_id names the operand DataDef (via type_from_id); the canonical
    // derived DataDef is obtained through the SAME getPointerType /
    // getReferenceType / getConstType cache the compiler uses, then stamped via
    // type_id_for. ONE source of truth — compiler internals keep using DataDef*;
    // this converts only at the boundary (serialization / the value ABI /
    // position-independent pools, where a DataDef* cannot live). Idempotent:
    // same (kind, operand_id) -> same derived id. MADC_TYPEID_INVALID for an
    // unknown operand id.
    uint32_t derived_type_id(DerivedKind kind, uint32_t operand_id);
    // The return type to CONSTRUCT a FuncDef with: a DataDefREF for a
    // reference return (routed through getReferenceType — the single
    // reference-creation/collapse path), else the bare type. Centralizes the
    // "a reference return is born as a DataDefREF" decision so FuncDef::returns
    // holds the real reference type, not a bare referent + parallel flag
    // (first-class refs Phase 2).
    DataDef &returnDecl(DataDef &dd, bool is_ref)
	{ return is_ref ? *(DataDef *)getReferenceType(&dd) : dd; }
    // Fold a trailing template-argument declarator suffix (`*`, `&`, `&&`) off the
    // token stream into the argument's type, returning the wrapped type token.
    // One owner of the rule, shared by every template-argument parser — a pointer
    // OR reference type is a valid template argument (`Vec<T*>`,
    // `conditional<b, int&, long>`, `__conditional_t<…, remove_reference_t<R>&&, …>`).
    TokenDataType *fold_template_arg_declarator(TokenDataType *adt, TokenBase *origin);
    // Resolve a fn-template parameter's DEFAULT token run to a concrete type,
    // substituting the already-bound type parameters in, then resolving in an
    // isolated token stream (handles trait-expression / template-id defaults like
    // `R = __conditional_t<...>`). Returns NULL for a still-dependent default.
    // require_full_parse: the run must consume to the sentinel (SFINAE
    // constraint evaluation — a member-type-chain miss leaves tokens behind).
    DataDef *resolve_template_param_default_type(
		const std::vector<TokenBase *> &default_tokens,
		const std::map<std::string, DataDef *> &binding,
		DataDefCLASS *owner, bool require_full_parse = false);
    // Consume a declarator's pointer-star run: a sequence of `*` interleaved with
    // cv-qualifiers (const/volatile/restrict). Each `*` on a NON-fn-ptr base wraps
    // `dd` via getPointerType (the type reflects the indirection); a DataDefFPTR
    // base (function-type typedef) is left UNWRAPPED — fn-ptr call detection keys
    // on the type being DataDefFPTR, so the count is returned for the caller to
    // record (Variable::fnptr_explicit_stars) and the emitter renders exactly the
    // stars written. Returns the number of `*` consumed; if out_const_after_star
    // is non-NULL, sets it to whether a `const` followed the last `*` (top-level
    // const pointer `T * const p`). One shared loop instead of the copy-pasted
    // `while(tkMul){...}` declarator loops.
    int consume_declarator_stars(DataDef *&dd, bool *out_const_after_star = nullptr);
    // Shared cv-qualifier consumers (const/volatile/restrict) for committed
    // type reads. Held form returns the first non-qualifier token; peek form
    // consumes the run and leaves the following token unread.
    TokenBase *skip_cv_qualifier_tokens(TokenBase *held);
    void skip_cv_qualifier_tokens();
    // parse a `(params)` list after the opening '(' has been consumed; used by
    // function-pointer typedefs. Builds a FuncDef with the given return type.
    // Parameter names are accepted but discarded. Stops after consuming ')'.
    FuncDef *parseFnPtrParams(DataDef &returns);
    // Pointer-to-array declarator suffix `[N][M]...` — stream positioned AT
    // the first '['; stops with the token after the last ']' unconsumed.
    // Returns the true pointer-to-array DataDefPTR(DataDefCArray(elem,...)).
    // Shared by the declaration, parameter, and cast `(T (*)[N])` arms;
    // `what` names the arm for diagnostics. A runtime dim (C11 6.7.6.2
    // variably-modified: `int (*rp)[m]`) becomes the CArray's count_expr;
    // capture_runtime_dims captures each at the declaration point (the
    // declaration arm — params capture at function entry instead).
    DataDef *parse_ptr_array_suffix(DataDef *elem_dd, TokenBase *ctx,
				    const char *what,
				    bool capture_runtime_dims = false);
    // The FuncDef giving a CALL's signature: the variable's own FuncDef, or
    // the target signature behind a function-pointer type (DataDefFPTR).
    // NULL when the variable isn't callable-typed. Parse-time twin of the CIR
    // builder's call_target_funcdef — never blind-cast var.type to FuncDef
    // (a fn-ptr call's DataDefFPTR read as FuncDef is UB: SMAUG's
    // `(*skill->spell_fun)(...)` arity check read garbage).
    static FuncDef *call_signature_funcdef(const Variable &var);
    // Function-pointer MEMBER declarator tail: parses `name ) ( params )`
    // after the leading `RET ( *` was consumed; returns the DataDefFPTR and
    // sets mname. Shared by the top-level and nested-aggregate struct member
    // parsers (e.g. glibc sigevent's `void (*_function)(__sigval_t);` inside
    // an anonymous union).
    DataDefFPTR *parse_fnptr_member_tail(DataDef &returns, std::string &mname,
					 TokenBase *open_tok);
    TokenBase *parseExpression(TokenBase *, bool conditional=false,
			       bool ternary_branch=false,
			       bool stop_on_closing_paren=false,
			       int initial_brackets=0,
			       bool push_back_comma=false,
			       bool cast_operand=false);
    // Control-flow signal an extracted parseExpression switch-arm handler
    // returns to the shunting-yard loop, one-to-one with the arm's original
    // inline control flow: Break = fall to the per-token epilogue (peek/advance),
    // Continue = re-enter the loop (the arm already advanced `tb`), Redo =
    // re-dispatch the (rewritten) `tb` without advancing (was `goto
    // redo_expression_token`), Done = terminate the expression (was
    // `done = true`). Lets the giant per-type arms move into named methods
    // while the loop keeps owning the stacks and the goto/continue.
    enum class ExprStep { Break, Continue, Redo, Done, Return };
    // ttDataType switch-arm of parseExpression: a type name appearing in
    // expression context (member name after ./->, contextual identifier /
    // variable, functional-cast, ns-qualified callee, or an inline declaration).
    // `tb` is in/out so a Redo can hand back a rewritten token.
    ExprStep parseExpr_dataTypeArm(TokenBase *&tb,
				   std::stack<TokenBase *> &exStack,
				   std::stack<TokenBase *> &opStack,
				   TokenCpnd *code);
    // ttSymbol switch-arm of parseExpression: statement/expression terminators
    // and the C comma operator (`;` ends; `,` either builds a comma-expression
    // node inside parens or ends the expression). Read-only `brackets`.
    ExprStep parseExpr_symbolArm(TokenBase *tb,
				 std::stack<TokenBase *> &exStack,
				 std::stack<TokenBase *> &opStack,
				 int brackets, bool push_back_comma);
    // ttIdentifier switch-arm of parseExpression: the largest arm — identifier
    // resolution in expression context (variables, function/method calls,
    // member access, template-ids, qualified/namespaced names, casts,
    // new/sizeof-family, …). `tb` is in/out as it advances through the stream.
    // ttKeyword falls through into this for keywords that are contextual
    // identifiers.
    ExprStep parseExpr_identifierArm(TokenBase *&tb,
				     std::stack<TokenBase *> &exStack,
				     std::stack<TokenBase *> &opStack,
				     TokenCpnd *code);
    // ttMultiOp/ttOperator switch-arm of parseExpression: the operator
    // shunting-yard core (precedence climbing, unary/binary disambiguation,
    // parentheses/subscript/ternary/cast/comma). Mutates `brackets` (by ref)
    // as it tracks paren depth.
    // `result` carries the value for an ExprStep::Return exit (an early
    // return from parseExpression that bypasses the operator-stack drain).
    ExprStep parseExpr_operatorArm(TokenBase *&tb,
				   std::stack<TokenBase *> &exStack,
				   std::stack<TokenBase *> &opStack,
				   int &brackets, TokenCpnd *code,
				   bool conditional, bool ternary_branch,
				   bool stop_on_closing_paren,
				   int initial_brackets, bool push_back_comma,
				   TokenBase *&result, bool cast_operand=false);
    // Old-style (K&R) parameter declarations: detection + parsing of the
    // `int f(a, b) int a; char *b; { … }` form (C only). The detectors peek
    // the stream; parse_old_style_parameter_declaration fills param_types.
    bool old_style_parameter_head_has_declaration_suffix();
    bool is_old_style_parameter_head(TokenBase *tb);
    bool try_parse_implicit_int_function_definition(TokenBase *tb);
    bool is_old_style_parameter_declaration_start(TokenBase *tb);
    DataDef *parse_old_style_parameter_base(TokenBase *&nt);
    void parse_old_style_parameter_declaration(TokenBase *nt,
		const std::vector<std::string> &param_ids,
		std::map<std::string, DataDef *> &param_types);
    bool scan_old_style_definition_suffix(std::vector<TokenBase *> &suffix);
    // Namespace resolution helpers: walk the enclosing-namespace chain to find
    // a member, resolve a bare name against the active namespace scope, and
    // report the namespace the current C++ scope looks up in.
    Variable *find_namespace_member_in_scope_chain(const std::string &ns_name,
						   const std::string &member_name);
    std::string resolve_namespace_name_in_scope(const std::string &name);
    std::string active_cpp_lookup_namespace();
    // static_assert: parse the statement form (folds the constant condition,
    // throws with the message on failure), consume the deferred form inside an
    // uninstantiated template body, and consume the class-scope declaration.
    void consume_deferred_static_assert_statement(TokenBase *tb);
    TokenBase *parse_static_assert_statement(TokenBase *tb);
    void consume_class_static_assert_declaration(TokenBase *tb);
    // Type queries: sizeof/alignof/typeof and the runtime type-query operators.
    // resolve_type_query_datadef resolves the operand to a DataDef (+ optional
    // folded value); evaluate_type_query folds sizeof/alignof; parse_typeof_datatype
    // yields the typeof()'d type; try_parse_dynamic_type_query handles the runtime
    // form; try_parse_constant_offsetof_address folds offsetof-style addresses.
    DataDef *resolve_type_query_datadef(TokenBase *type_tb,
					const std::string &op_name,
					bool &have_value, size_t &query_value);
    size_t evaluate_type_query(TokenBase *op_tb, const std::string &op_name);
    // [expr.unary.noexcept]: parse `( expression )` UNEVALUATED (decltype's
    // twin) and fold to 0/1 — the noexcept-spec conjunction over the operand's
    // parsed tree. Throws when the answer cannot be derived faithfully; a
    // constant-fold caller's isolated-stream try records that as "unfoldable"
    // rather than a silently wrong bool. See parser.cpp noexcept_eval_expr.
    size_t evaluate_noexcept_operator(TokenBase *op_tb);
    // Type-trait builtins (__is_class/__is_base_of/…): parse `( type-list )` and
    // fold to a bool constant token. See parser.cpp for the supported (faithful)
    // set; unsupported traits are not recognized (clear error, never a wrong bool).
    TokenBase *evaluate_type_trait(TokenBase *op_tb, const std::string &name);
    TokenBase *try_parse_dynamic_type_query(TokenBase *op_tb,
					    const std::string &op_name);
    TokenDataType *parse_typeof_datatype(TokenBase *op_tb);
    bool try_parse_constant_offsetof_address(int64_t &out);
    // Named C++ casts (static_cast/reinterpret_cast/const_cast/dynamic_cast):
    // parse the expression form and the constant-folded form; plus the
    // C-style cast operand helpers (deref/function-call/literal materialization).
    madc_wide_int parse_constant_named_cpp_cast(TokenBase *cast_tb,
						const std::string &cast_name);
    bool try_parse_constant_functional_cast(TokenBase *type_tb,
					    madc_wide_int &out);
    TokenBase *parse_named_cpp_cast(TokenBase *cast_tb,
				    const std::string &cast_name);
    TokenBase *parse_cast_unary_deref_operand(TokenBase *star);
    TokenBase *parse_cast_function_call_operand(TokenBase *head);
    TokenBase *materialize_cast_literal_operand(TokenBase *tb);
    // Template-machinery leaf consumers: recognize a template-argument-list
    // close (`>` or split `>>`), detect whether we're in an instantiated member
    // body, consume a template-parameter type suffix, and collect a template
    // default-argument token run.
    bool parsing_template_instantiated_member_body();
    bool consume_template_close(TokenBase *tok);
    bool consume_template_parameter_type_suffix();
    std::vector<TokenBase *> collect_template_default_argument();
    // Template-machinery core: <…> argument scanning, template-id / alias /
    // opaque instantiation, dependent/opaque member-type materialization,
    // deferred-instantiation completion, and the template-decl skippers.
    void skip_template_id_suffix(std::vector<TokenBase *> *out = NULL);
    // Consume a C++20 requires-clause at the current token (constraints are
    // not evaluated — madc has no concepts; the constrained declaration
    // parses as if unconstrained). Returns true if a clause was consumed.
    bool skip_requires_clause();
    // The constraint scanner behind it (the `requires` keyword already
    // consumed) — also used for trailing requires-clauses by the
    // template-declaration skipper.
    void skip_constraint_expression();
    void skip_template_nonclass_declaration(TokenBase *first,
					    std::vector<TokenBase *> *seen = NULL);
    void capture_extern_template_class_instantiation();
    void apply_template_call_return_inference(TokenCallFunc *tc);
    DataDef *resolve_namespace_fn_template_call_return_type(TokenCallFunc *tc,
							    bool *ret_ref);
    // Key-based core of the above: resolve a free/namespace function-template
    // call's return type from "ns::name" + the explicit type arguments. Called
    // by the TokenCallFunc entry AND recursively for a `decltype(inner_call)`
    // return (declval's `decltype(__declval<_Tp>(0))`). `depth` bounds the
    // recursion. No emission — resolves purely from the retained decl tokens.
    // `call_arg_types` (optional) enables DEDUCED binding ([temp.deduct.call],
    // return-only): the call's argument value types, paired positionally with
    // the candidate's parameter spellings — serves an unevaluated deduced call
    // (`decltype(addr(x))`) whose explicit-args list is empty.
    DataDef *resolve_fn_template_return_by_key(const std::string &key,
				const std::vector<DataDef *> &explicit_args,
				bool *ret_ref, int depth,
				const std::vector<DataDef *> *call_arg_types = NULL);
    // Resolve `decltype ( IDENT < targs > ( args ) )` (substituted tokens) by
    // recursing into IDENT's template return type in namespace `ns`. No emit.
    DataDef *resolve_decltype_call_return(const std::vector<TokenBase *> &sub,
				const std::string &ns, bool *ret_ref, int depth);
    TokenBase *collect_template_argument_spelling(TokenBase *first,
						  std::string &spelling,
						  std::vector<TokenBase *> *tokens_out = NULL);
    std::vector<TokenBase *> collect_template_class_prefix();
    TokenBase *consume_template_type_arg_qualifiers(TokenBase *tb,
						    std::string &spelling);
    void consume_trailing_type_arg_qualifiers(std::string &spelling);
    TokenDataType *instantiate_template_use(const std::string &tname,
					    TokenBase *tb,
					    const std::string &ns_hint = std::string(),
					    DataDefCLASS *owner_hint = NULL);
    TokenDataType *instantiate_template_alias_use(const std::string &tname,
						  TokenBase *tb,
						  const std::string &ns_hint = std::string(),
						  DataDefCLASS *owner_hint = NULL);
    // True iff EVERY collected alias-use argument is non-dependent (concrete):
    // each non-type arg folds to a constant and each type arg resolves to a
    // type with no unresolved dependent surface. Lets instantiate_template_alias_use
    // tell a genuine SFINAE failure (concrete args, absent `::type`) from a
    // deferred dependent use.
    bool alias_use_args_all_concrete(const TemplateAliasDef &td,
			 const std::vector<std::vector<TokenBase *> > &arg_tokens);
    // An arg token sequence is DEPENDENT when it names a live template
    // parameter or carries a dependent-surface type token — such an arg
    // legitimately fails to resolve NOW and resolves at instantiation;
    // any other resolution failure is a substitution failure ([temp.deduct]).
    bool alias_arg_tokens_dependent(const std::vector<TokenBase *> &toks);
    // Owner of the [basic.lookup.unqual] enclosing-namespace-chain walk for a
    // bare type name (std::pmr -> std, stopping before global scope):
    // namespace-scope aliases live ONLY in namespace_datatype_map (never
    // flat-leaked), so `true_type` used unqualified inside namespace std
    // resolves here. Returns the shared prototype token — callers clone.
    TokenDataType *namespace_chain_datatype(const std::string &nm);
    // Resolve a template-id `Name<...>` to its concrete type: an alias template
    // first, then a class template (the order every call site used by hand).
    // Single seam for the namespace hint so qualified uses pick the right
    // same-named variant. Returns NULL if Name is not a (alias-or-class)
    // template-id.
    TokenDataType *instantiate_template_id(const std::string &tname,
					   TokenBase *tb,
					   const std::string &ns_hint = std::string(),
					   DataDefCLASS *owner_hint = NULL);
    TokenDataType *instantiate_opaque_template_use(TemplateDef &td,
						   const std::string &tname,
						   TokenBase *tb);
    // tsubst TYPE-half of a dependent template-id shell: replay SUBSTITUTED
    // structural arg-token runs (concrete TokenDataType / TokenInt elements —
    // never source text) through instantiate_template_use to rebuild the
    // CONCRETE instantiation the shell stood for. `arg_runs` tokens are
    // consumed (pushed into the stream); the stream is drained back to its
    // pre-replay depth on failure. Returns NULL when the template is unknown
    // or instantiation fails.
    TokenDataType *instantiate_shell_origin_replay(
			const struct DependentShellOrigin &org,
			const std::vector<std::vector<TokenBase *> > &arg_runs);
    // COMPLETE an opaque template shell in place of a completeness demand:
    // look up the shell's recorded origin and replay it (above). A shell minted
    // during a dependent parse may by now hold fully CONCRETE args, in which
    // case the replay yields the real instantiation. SILENT — cerr muted and
    // diagnostics rewound, so a replay that cannot re-enter cleanly leaves the
    // caller exactly as it was. Returns NULL when the shell stays opaque.
    DataDefCLASS *complete_shell_class_type(DataDefCLASS *cls);
    // GCC's __integer_pack(N) builtin (libstdc++-13 GCC-path index-sequence
    // primitive): valid only as the entire pattern of a template-argument pack
    // expansion `X<... __integer_pack(N)... ...>`; at substitution time (N a
    // concrete constant) it yields the pack [0,1,...,N-1]. Rewrites the live
    // token stream IN PLACE, between a just-consumed `<` and its matching close,
    // splicing each occurrence into literal integer args so the ordinary
    // template-argument loops then collect plain non-type args.
    void expand_integer_pack_template_args();
    // clang's __make_integer_seq builtin template (the libc++ index-sequence
    // primitive): `__make_integer_seq<S, T, N>` (N a concrete non-negative
    // constant) IS `S<T, 0, 1, ..., N-1>`. Rewrites the live stream's
    // `< S , T , N >` region in place and delegates instantiation to S;
    // returns NULL (ordinary path) when N is dependent/non-constant. Only
    // called for a namespace-level lookup (never owner-scoped — the stream
    // rewrite must not fire on a speculative member probe).
    TokenDataType *instantiate_make_integer_seq(TokenBase *tb,
						const std::string &ns_hint);
    // clang's __type_pack_element builtin template:
    // `__type_pack_element<I, T0, ..., Tn>` (I a concrete constant) IS the
    // I-th type argument. Consumes the live stream's `< I , T... >` region
    // and returns the selected type directly (no instantiation — the result
    // is one of the given args); NULL (ordinary path) when I is dependent /
    // non-constant / out of range. Same owner gating as the sibling above.
    TokenDataType *instantiate_type_pack_element(TokenBase *tb,
						 const std::string &ns_hint);
    TemplateAliasDef *find_template_alias(const std::string &name,
					  const std::string &ns_hint = std::string(),
					  DataDefCLASS *owner_hint = NULL);
    TemplateAliasDef *find_template_alias(uint32_t name_id,
					  const std::string &ns_hint,
					  DataDefCLASS *owner_hint);
    void register_template_alias(const TemplateAliasDef &td);
    // Owner is any dependent-surface type: an opaque placeholder CLASS or a
    // bare TEMPLATE-PARAMETER placeholder (`typename T::member` in a dependent
    // pattern/capture parse) — only name/canonical spelling are read from it.
    DataDefCLASS *materialize_dependent_member_type(DataDef *owner,
						    const std::string &member_name);
    DataDefCLASS *materialize_opaque_class_type(const std::string &name,
						const std::string &canonical);
    // Stamp a freshly-minted opaque dependent shell/tag with its mint
    // context: outside a dependent (pattern) parse the placeholder is a
    // CONCRETE forward tag (e.g. the empty-pack recursion tail
    // _Tuple_impl<1>) — legitimate frozen state (v21 empty shape), never
    // a pack artifact the freeze kills.
    void stamp_opaque_mint_context(DataDefCLASS *dep);
    DataDef *dependent_deref_result_type(DataDef *dd);
    void complete_pending_template_instantiations(const std::string &class_name);
    bool request_template_instantiation_completion(const std::string &mangled_name);
    // Is a lazy completion RECORDED for this mangled instance (the
    // :7776-arm's pending record)? Read-only twin of the request above.
    bool has_pending_template_instantiation(const std::string &mangled_name) const;
    DataDef *complete_class_type_on_demand(DataDef *dd);
    bool template_declared_in_namespace(const std::string &name,
					const std::string &ns_name);
    TokenBase *consume_unresolved_dependent_call(TokenBase *open);
    // Type-token resolution: resolve a declared / namespaced / typename-qualified
    // type name (optionally consuming the tokens, allowing lazy types, walking a
    // class-member chain) to its TokenDataType.
    TokenDataType *resolve_typename_type_token(TokenBase *first,
					       bool allow_lazy_types,
					       TokenBase *typename_tb);
    // committed_type_context: the caller KNOWS `owner::name` must be a type
    // (base-specifier position, where `typename` is forbidden) — the
    // first-segment probe then admits the opaque dependent-member escape a
    // speculative (expression-position) caller must never take.
    TokenDataType *resolve_class_member_type_chain(DataDefCLASS *owner,
						   TokenBase *owner_tb,
						   bool committed_type_context = false);
    TokenDataType *resolve_member_chain_or_type(TokenDataType *type_tok,
						TokenBase *tb,
						bool consume_class_member_chain);
    TokenDataType *resolve_declared_type_token(TokenBase *tb,
					       bool consume_ns_tokens,
					       bool allow_lazy_types,
					       bool consume_class_member_chain = true);
    // Resolve a TYPE that spans a token RANGE (e.g. a member-template return
    // type `std::pair<iterator, bool>`) through the canonical type resolver,
    // in an isolated token stream so the live parse position is untouched.
    // Returns the resolved DataDef, or NULL when the range is not a type.
    DataDef *resolve_type_token_range(const std::vector<TokenBase *> &toks,
				      size_t start, size_t end);
    TokenDataType *resolve_namespaced_type_token(TokenBase *tb, bool consume_tokens);
    // Type-name resolution helpers: look up a named DataDef (struct/typedef/lazy),
    // a current-class type alias, a variable matching a contextual type name, and
    // the class scope an expression name resolves to.
    DataDef *resolve_named_datadef(const std::string &name);
    // Element type for madc `array` subscript READS (string-first — the
    // Python/PHP element model; long fallback when no string class is known).
    DataDef *madc_array_element_type();
    static DataDef *resolve_builtin_type_spelling(const std::string &name);
    static DataDef *builtin_va_list_type();
    DataDef *use_builtin_va_list();
    // The one DataDefCOMPLEX per element type (process-wide cache) — lexer,
    // parser, and CIR builder all resolve `_Complex <elem>` through here so
    // complex types compare by pointer identity. NULL elem -> _Complex double.
    static DataDef *complex_type_of(DataDef *elem);
    // Parse the operand of __real__/__imag__ into a TokenComplexPart (shared
    // by the identifier-expression arm and the cast-operand dispatch).
    TokenBase *parse_complex_component_operand(bool want_imag, TokenBase *anchor);
    bool typedef_alias_matches_datadef(const std::string &alias, DataDef *dd);
    DataDef *resolve_current_class_type_alias(const std::string &name);
    bool resolve_current_class_static_member_const_value(const std::string &name, int64_t &out);
    bool fold_constant_qualified_member(TokenBase *first, madc_wide_int &out);
    bool fold_constant_qualified_member_walk(TokenBase *first,
					     madc_wide_int &out);
    Variable *find_variable_for_contextual_type_name(const std::string &name);
    DataDefCLASS *resolve_expression_class_scope(const std::string &name);
    // What a qualifier names before `::` in an expression — THE one classify
    // policy for the expression arms (postfix chain, address-of, identifier
    // arm). Owns alias resolution and the collision diagnosis; each arm reads
    // the registry slice its continuation actually serves.
    struct QualifierScope {
	DataDefCLASS *cls;	// non-NULL: names a class in expression scope
	std::string ns_name;	// alias-resolved qualifier spelling
	bool has_variable_ns;	// present in namespace_map
	bool has_datatype_ns;	// present in namespace_datatype_map
	bool is_namespace() const { return has_variable_ns || has_datatype_ns; }
    };
    QualifierScope classify_qualifier_before_scope(const std::string &name,
						   TokenBase *at);
    // Class-body parsing: detect when a struct body needs the class parser /
    // an inline enum follows, parse anonymous aggregates, bind a declared C++
    // member symbol, mint a unique overload symbol, collect a deferred function
    // body's tokens and parse them later, and promote a struct base to a class.
    bool cpp_struct_body_needs_class_parser(const std::string &tag_name,
					    TokenBase *after_tag);
    bool consume_anonymous_aggregate_open(bool &packed);
    DataDefSTRUCT *parse_class_anonymous_aggregate(TokenBase *kw);
    void parse_class_anonymous_aggregate_members(DataDefSTRUCT *agg,
						 TokenBase *loc);
    bool class_body_enum_definition_follows();
    struct ClassMethodRegistration {
	ClassMethodKind kind;
	std::string display_name;
	uint32_t access_flags;
	bool is_static;
	bool is_virtual;
	bool is_operator;
	bool bind_cpp_symbol;
	std::string local_emit_name;
	ClassMethodRegistration()
	    : kind(ClassMethodKind::Method), access_flags(0), is_static(false),
	      is_virtual(false), is_operator(false), bind_cpp_symbol(true) {}
    };
    TokenDataType *register_class_shell(DataDefCLASS *ddc,
		const std::string &registered_name,
		const std::string &source_name,
		const std::string &constructor_source_name,
		DataDefCLASS *owner, bool completing_forward,
		bool register_source_alias);
    void initialize_class_bases(DataDefCLASS *ddc,
				const std::vector<BaseSpec> &bases);
    void register_class_method_signature(DataDefCLASS *ddc, Variable *mvar,
					 const ClassMethodRegistration &spec);
    void complete_class_aggregate(DataDefCLASS *ddc);
    void bind_declared_cpp_symbol(DataDefCLASS *ddc, Variable *mvar,
				  CppSymKind kind, const std::string &mname,
				  bool is_operator);
    std::string unique_overload_symbol(std::string base);
    // C++20 abbreviated function template ([dcl.fct]/18): token-level
    // desugar — `auto` parameter placeholders become invented identifiers
    // under a synthesized `template<...>` head pushed onto the stream, so
    // the existing template machinery owns the declaration. Returns true
    // when it desugared (the stream head is then tkTEMPLATE).
    bool desugar_abbreviated_fn_template();
    std::vector<TokenBase *> collect_compound_body_tokens(TokenBase *open);
    void enqueue_deferred_function_body(Variable *var,
					Method *method, TokenBase *open,
					const std::vector<TokenBase *> *trailing_ret = NULL);
    void parse_deferred_function_body(DeferredFunctionBody &body);
    void parse_deferred_function_bodies(std::vector<DeferredFunctionBody> &bodies);
    // Lazy member-function-body instantiation: parse a single deferred body by
    // emit symbol on first ODR-use (returns the materialized TokenFunc, or NULL
    // if the symbol has no deferred body / was already parsed).
    TokenFunc *parse_deferred_lazy_body(const std::string &emit_symbol);
    bool has_deferred_lazy_body(const std::string &emit_symbol) const
	{ return deferred_lazy_bodies.find(emit_symbol) != deferred_lazy_bodies.end(); }
    DataDefCLASS *promote_struct_base_to_class(const std::string &name,
					       DataDef *dd);
    // GNU/C23 attribute consumers: skip/collect __attribute__((…)) (optionally
    // capturing attrs / alias target / alignment / vector size), an asm("label")
    // alias, [[…]] C23 attributes, a vector_size attribute value, and `...`.
    TokenBase *consume_gnu_attributes(TokenBase *nt,
				      std::set<std::string> *attrs = NULL,
				      std::string *alias_target = NULL,
				      size_t *explicit_align = NULL,
				      size_t *vector_bytes = NULL);
    // Set by consume_gnu_attributes on optimize("-fno-strict-aliasing") in any
    // position; consumed (and cleared) by the function-declaration parse.
    bool pending_no_strict_aliasing;
    TokenBase *consume_gnu_asm_label(TokenBase *nt, std::string *alias_target);
    // Skip (or lower the recognized `=r`/`+r`/`+m`/... copy shapes of) a GNU
    // asm STATEMENT. `tb` is the asm introducer (identifier or reserved
    // tkCPPKEYWORD spelling). Shared by both the ttIdentifier and ttKeyword
    // arms of parseStatement so reserving `asm` as a keyword does not lose the
    // statement-level skip.
    TokenBase *skip_gnu_asm_statement(TokenBase *tb);
    void skip_c23_attributes();
    size_t parse_gnu_vector_size_attribute();
    void consume_typedef_gnu_attributes(std::string *mode_name = NULL,
					size_t *vector_bytes = NULL);
    bool consume_ellipsis();
    // Parameter-signature / qualified-declarator parsing: count queued call args,
    // resolve a qualified class owner, parse a qualified declarator part, split an
    // upcoming function parameter list, resolve/parse parameter signatures, find a
    // matching constructor, and parse a qualified special-member definition.
    size_t count_queued_call_arguments();
    DataDefCLASS *resolve_qualified_class_owner(const std::vector<std::string> &scope_parts);
    std::string parse_qualified_declarator_part(TokenBase *part_tb);
    bool split_upcoming_function_params(std::vector<std::vector<TokenBase *> > &params);
    DataDef *resolve_param_type_from_tokens(const std::vector<TokenBase *> &param,
					    size_t &idx);
    bool parse_param_sig_from_tokens(std::vector<TokenBase *> param,
				     ParsedParamSig &sig);
    bool upcoming_param_signatures(std::vector<ParsedParamSig> &sigs);
    Variable *find_constructor_for_upcoming_params(DataDefCLASS *owner);
    // The METHOD twin: which declared overload an out-of-line member definition
    // defines, matched on the upcoming parameter signature. NULL unless exactly
    // one non-template overload matches (see parser.cpp for why an ambiguous
    // probe must decline rather than guess).
    Variable *find_method_definition_target(DataDefCLASS *owner,
					    const std::string &member);
    // pre_owner + src_class_name: the TEMPLATE-ID-qualified form
    // (`bs<0,0>::bs(...) {}` — an explicit specialization's out-of-line
    // special member, defined WITHOUT a template<> prefix per
    // [temp.expl.spec]/5). The caller already resolved the template-id to
    // the instantiated class; src_class_name is the SOURCE spelling the
    // ctor/dtor name is compared against (the owner's registered name is
    // the mangled instantiation key and can never match the source text).
    bool parse_qualified_special_member_definition(TokenBase *first_tb,
	    DataDefCLASS *pre_owner = NULL,
	    const std::string *src_class_name = NULL);
    // Assorted parse helpers (expression/declaration/statement support).
    DataDef *effective_pointer_type_for_member_access(TokenBase *tb);
    // C++ canon operator-> rewrite: when lhs is a class OBJECT (not a
    // pointer) whose class declares operator->, return the
    // `lhs.operator->()` call token (its datadef() is the pointer the
    // real '->' then applies to). NULL when the rewrite does not apply.
    class TokenCallMethod *arrow_operator_call(TokenBase *lhs,
					       TokenBase *loc_tb);
    DataDef *parse_typedef_array_suffix(DataDef *base_dd,
					const std::string &alias_name,
					TokenBase *err_tok);
    TokenBase *consume_balanced_parenthesized_suffix(TokenBase *open);
    TokenBase *make_expression_context_literal(const madc::value &resolved,
					       TokenBase *src);
    TokenBase *materialize_runtime_struct_size_captures(TokenCpnd *code,
							DataDefSTRUCT *dds, TokenBase *loc);
    TokenBase *materialize_vla_dim_capture(TokenCpnd *code,
					   TokenBase *&dim_expr, TokenBase *loc);
    TokenBase *try_parse_vla_variable_sizeof(TokenBase *op_tb,
					     const std::string &op_name);
    TokenBase *try_parse_vla_row_sizeof(TokenBase *op_tb, class Variable *v,
					bool paren, bool deref,
					size_t after_ix);
    TokenBase *parse_functional_type_expression(TokenBase *type_tb,
						DataDef *type_dd);
    TokenBase *parse_namespace_block(bool inline_namespace);
    TokenBase *parse_parenthesized_expression(const char *context,
					      bool stop_on_closing_paren);
    TokenBase *reference_bind_address_expr(TokenBase *expr,
					   DataDef *referent_type);
    TokenBase *skip_expression_whitespace();
    TokenCASE *parse_switch_label(TokenSWITCH *sw, TokenBase *tn);
    TokenObjTemp *try_parse_functional_ctor(TokenBase *name_tb);
    bool paren_opens_call_on_receiver(std::stack<TokenBase *> &exStack);
    Variable *resolve_c_identifier(TokenIdent *ident_tb, bool expression_head);
    bool datatype_statement_starts_functional_expr();
    bool datatype_statement_starts_qualified_expr();
    bool is_shared_global_extern_reference(TokenCpnd *code, Variable *var);
    bool next_parenthesized_type_is_compound_literal();
    bool paren_group_is_function_def();
    bool paren_group_is_nonclass_direct_init();
    bool paren_group_can_be_param_decl_clause();
    bool unqualified_name_is_type_or_template(const std::string &nm);
    // task #69: the HOST-flavor twin of a namespace public's Itanium symbol
    // (the marshalling boundary's bind target when script flavor != host).
    std::string host_flavor_fn_symbol(const std::string &ns_name,
				      const std::string &member_name,
				      FuncDef *fd);
    bool parse_array_designator_initializer(TokenBase *&next_init,
					    size_t &first_index, size_t &last_index);
    bool parse_builtin_types_compatible_operand(TokenBase *type_tb,
						std::string &sig);
    bool resolve_integer_constant(TokenBase *tb, madc_wide_int &out);
    bool token_starts_type_name(TokenBase *tb);
    void configure_nested_function_captures(FuncDef *func);
    void mirror_inline_namespace_into_parent(const std::string &parent_ns,
					     const std::string &inline_ns);
    // Statement-level expression parse: parseExpression + comma-chain.
    // parseExpression treats `,` as a hard stop (callers like for-loop
    // init/incr and call-arg lists rely on this). In statement contexts
    // (`expr1, expr2;` or a brace-less `while (c) e1, e2;` body), the
    // comma is the C comma operator and both expressions must run for
    // side effects. parseExprStmt collects them into a TokenComma chain
    // whose compile() evaluates left for effects and returns right.
    TokenBase *parseExprStmt(TokenBase *);
    // Parse an identifier followed by any chain of postfix operators
    // (->ident / .ident / [expr] / ++ / --) and return the resulting
    // expression node. Stops at the first non-postfix token (binary
    // operator, comma, semicolon, etc.) and pushes it back on the
    // token stream. Used by unary `*` and `&` to avoid
    // parseExpression's greedy consumption of trailing binary ops.
    TokenBase *parsePostfixChain(TokenBase *head);
    TokenBase *parsePostfixChainFrom(TokenBase *result, Variable *var);
    // Parse the operand after unary `&`, preserving C precedence by
    // stopping before trailing binary operators.
    TokenBase *parseAddressOfExpression(TokenBase *ampersand);
    TokenFunc *build_expression_function(TokenProgram *tp,
					 TokenBase *expr,
					 DataDef *return_type,
					 const std::string &function_name,
					 bool have_result,
					 const std::string &result_name = "__madc_expr_value");
    TokenBase *parseLambda();  // parse [](params) { body } lambda expression
    // THE lambda dispatch for expression context: parse the lambda AND
    // continue the postfix chain, so an immediately-invoked lambda
    // (`[](int x){ return x + 99; }(1)`) becomes a call and not a value
    // followed by a stray parenthesized expression.
    ExprStep parseLambdaExprStep(std::stack<TokenBase *> &exStack,
				 std::stack<TokenBase *> &opStack,
				 bool &done);
    // Is the lambda whose introducer is at the head of the token stream
    // IMMEDIATELY INVOKED? Looks past the balanced introducer / parameter
    // list / body with the shared DelimDepth tracker.
    bool lambdaIsImmediatelyInvoked();

    // Compile the parsed program. The asmjit JIT codegen backend has
    // been removed; CIR (madc parse → cir_node → c2mir → MIR) is the
    // sole backend, invoked via madc_cir_execute(). This stub always
    // fails so legacy JIT-pipeline callers degrade cleanly.
    bool compile();

    // AOT object/executable output is unavailable on the CIR backend.
    // These stubs report a clear error and return false; emit C and
    // compile with gcc/clang instead.
    bool save_object(const std::string &path) const;
    bool save_executable(const std::string &path);

    // load a previously saved ELF .o and map it for execution
    struct LoadedObject
    {
	void *code_base;
	size_t code_size;
	std::map<std::string, void *> functions;
	LoadedObject() : code_base(NULL), code_size(0) {}
    };
    LoadedObject loaded_object;
    bool load_object(const std::string &path);
    bool has_loaded_function(const std::string &name) const;
    void *loaded_function_ptr(const std::string &name) const;
    void unload_object();

    // execute the resulting code
    void execute();

    // data management
    DataDef *findType(std::string &);
    Variable *addVariable(TokenCpnd *, DataDef &, const std::string &, int c=1, void *init=NULL, bool alloc=true);
    Variable *resolve_global_storage_variable(Variable *var) const;
    Variable *addGlobal(DataDef &d, std::string str, int c=1, void *init=NULL)
    {
	return addVariable(NULL, d, str, c, init, true);
    }
    Variable *findVariable(TokenCpnd *, const std::string &);
    Variable *findVariable(const std::string &);
    Variable *addLiteral(const std::string &);
    Variable *addWideLiteral(const std::string &);
//  Method *findMethod(const std::string &);
};

class MadcEngine
{
public:
    enum class LogLevel
    {
	emerg,
	alert,
	crit,
	err,
	warn,
	notice,
	info,
	debug
    };

    typedef std::function<void(LogLevel, const std::string &)> LogSink;

    std::istream *input_stream;
    std::ostream *output_stream;
    std::ostream *error_stream;
    std::streambuf *default_input_buf;
    std::streambuf *default_output_buf;
    std::streambuf *default_error_buf;
    std::unique_ptr<std::istringstream> owned_input_buffer;
    std::unique_ptr<std::ostringstream> owned_output_buffer;
    std::unique_ptr<std::ostringstream> owned_error_buffer;
    std::unique_ptr<MadcTeeBuf> output_tee_buf;
    std::unique_ptr<MadcTeeBuf> error_tee_buf;
    bool log_timestamps;
    bool log_level_prefixes;
    LogLevel log_threshold;
    bool log_to_error_stream;
    bool syslog_active;
    std::string syslog_ident;
    int syslog_option;
    int syslog_facility;
    bool file_sink_active;
    std::unique_ptr<std::ofstream> log_file;
    std::string log_file_path;
    size_t log_file_max_bytes;
    int log_file_max_files;
    bool json_sink_active;
    std::unique_ptr<std::ofstream> json_file;
    std::string json_file_path;
    std::vector<LogSink> log_sinks;
    bool verbose = false;
    Program::RegistrationPolicy registration_policy;
    Program::BuiltinRegistry builtin_registry;
    Program::NamespaceRegistry namespace_registry;

    MadcEngine();
    ~MadcEngine();
    std::istream &input();
    std::ostream &output();
    std::ostream &error();
    void bind_input_stream(std::istream &is);
    void bind_output_stream(std::ostream &os);
    void bind_error_stream(std::ostream &os);
    void bind_input_string(const std::string &text);
    void capture_output_to_buffer();
    void capture_error_to_buffer();
    void tee_output_stream(std::ostream &os);
    void tee_error_stream(std::ostream &os);
    void tee_output_to_buffer();
    void tee_error_to_buffer();
    const char *log_level_name(LogLevel level) const;
    std::string format_log_message(LogLevel level, const std::string &message) const;
    bool should_log(LogLevel level) const;
    void write_log(LogLevel level, const std::string &message);
    void write_builtin_sinks(LogLevel level, const std::string &message);
    void add_log_sink(LogSink sink);
    void clear_log_sinks();
    static int syslog_priority_for(LogLevel level);
    void enable_syslog_sink(const char *ident = "madc", int option = -1, int facility = -1);
    void disable_syslog_sink();
    bool enable_file_sink(const std::string &path,
			  size_t max_bytes = 0,
			  int max_files = 5);
    void disable_file_sink();
    void rotate_log_file();
    void reopen_log_file();
    bool enable_json_sink(const std::string &path);
    void disable_json_sink();
    static std::string json_escape(const std::string &s);
    std::string format_json_log_line(LogLevel level, const std::string &message) const;
    void write_syslog_sink(LogLevel level, const std::string &message);
    void write_file_sink(LogLevel level, const std::string &message);
    void write_json_sink(LogLevel level, const std::string &message);

    struct Config
    {
	LogLevel threshold;
	bool timestamps;
	bool level_prefixes;
	bool error_stream;

	bool file_sink;
	std::string file_path;
	size_t file_max_bytes;
	int file_max_files;

	bool syslog_sink;
	std::string syslog_ident;
	int syslog_option;
	int syslog_facility;

	bool json_sink;
	std::string json_path;

	Config()
	    : threshold(LogLevel::debug),
	      timestamps(false),
	      level_prefixes(true),
	      error_stream(true),
	      file_sink(false),
	      file_max_bytes(0),
	      file_max_files(5),
	      syslog_sink(false),
	      syslog_ident("madc"),
	      syslog_option(-1),
	      syslog_facility(-1),
	      json_sink(false)
	{}
    };

    bool apply_log_config(const Config &cfg);
    bool has_output_buffer() const;
    bool has_error_buffer() const;
    std::string output_buffer_str() const;
    std::string error_buffer_str() const;
    void clear_output_buffer();
    void clear_error_buffer();
    void reset_standard_streams();
    void populate_default_registries();
    void configure_program(Program &pgm) const;
    std::unique_ptr<Program> create_program();
    void bind_log_streams();
    static void unbind_log_streams();
};

class MadcLogStreambuf : public std::streambuf
{
public:
    explicit MadcLogStreambuf(MadcEngine::LogLevel lvl);

    void set_engine(MadcEngine *eng) { _engine = eng; }
    MadcEngine *engine() const { return _engine; }
    MadcEngine::LogLevel level() const { return _level; }

protected:
    virtual int overflow(int ch = EOF) override;
    virtual std::streamsize xsputn(const char *s, std::streamsize n) override;
    virtual int sync() override;

private:
    MadcEngine::LogLevel _level;
    MadcEngine *_engine;
    std::string _line;
    void flush_line();
};

class MadcLogStream : public std::ostream
{
public:
    explicit MadcLogStream(MadcEngine::LogLevel lvl);

    void set_engine(MadcEngine *eng) { _buf.set_engine(eng); }
    MadcEngine *engine() const { return _buf.engine(); }
    MadcEngine::LogLevel level() const { return _buf.level(); }

private:
    MadcLogStreambuf _buf;
};

namespace madc
{
    extern MadcLogStream emerg;
    extern MadcLogStream alert;
    extern MadcLogStream crit;
    extern MadcLogStream err;
    extern MadcLogStream warn;
    extern MadcLogStream notice;
    extern MadcLogStream info;
    extern MadcLogStream debug;
}

#define ANSI_RED "\e[1;31m"
#define ANSI_WHITE "\e[1;37m"
#define ANSI_RESET "\e[m"

// Macros for generating extern "C" thin wrappers.
// Usage: MADC_EXTERN_C1(int64_t, php_trim, void *)
// Expands to: extern "C" int64_t __php_trim(void *a) { return php_trim(a); }
#define MADC_EXTERN_C0(ret, name) \
    extern "C" ret __##name(void) { return name(); }
#define MADC_EXTERN_C1(ret, name, T1) \
    extern "C" ret __##name(T1 a) { return name(a); }
#define MADC_EXTERN_C2(ret, name, T1, T2) \
    extern "C" ret __##name(T1 a, T2 b) { return name(a, b); }
#define MADC_EXTERN_C3(ret, name, T1, T2, T3) \
    extern "C" ret __##name(T1 a, T2 b, T3 c) { return name(a, b, c); }
#define MADC_EXTERN_C4(ret, name, T1, T2, T3, T4) \
    extern "C" ret __##name(T1 a, T2 b, T3 c, T4 d) { return name(a, b, c, d); }

#endif // __MADC_H
