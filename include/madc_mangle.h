#ifndef __MADC_MANGLE_H
#define __MADC_MANGLE_H 1

#include <string>
#include <vector>

// Every mangler here is the ONE substitution-aware Itanium encoder (the `_sub`
// entry points below) — byte-identical to g++/clang on tests/abi/mangle_corpus.cpp.
// A user symbol and a std:: symbol of the same signature must encode the same
// way, so there is no simpler second encoder: the naive family that once lived
// here spelled `13madc::channel` for a namespaced class and `3Vec` where the ABI
// wants the back-reference S_, and was retired (phase 0 of the C++ symbol-
// mangling feature).

// RTTI symbols for an un-namespaced user class. source_name = <len><name>.
//   itanium_typeinfo_sym("C")         → "_ZTI1C"   (typeinfo for C)
//   itanium_typeinfo_name_sym("C")    → "_ZTS1C"   (typeinfo name for C)
//   itanium_typeinfo_name_string("C") → "1C"       (the bare mangled name string)
std::string itanium_typeinfo_sym(const std::string &class_name);
std::string itanium_typeinfo_name_sym(const std::string &class_name);
std::string itanium_typeinfo_name_string(const std::string &class_name);
// Real vtable/typeinfo symbols from a canonical C++ spelling (St-aware, N..E for
// a namespaced or nested class), e.g. "std::bad_alloc" -> _ZTVSt9bad_alloc /
// _ZTISt9bad_alloc, "ns::VB" -> _ZTVN2ns2VBE / _ZTIN2ns2VBE / _ZTSN2ns2VBE with
// the typeinfo-name content "N2ns2VBE". Used to defer an externally-defined
// class's RTTI to its library, and the form every user class's RTTI must take
// to be ABI-identical to g++/clang (the bare-name forms above spell _ZTI2VB).
std::string itanium_vtable_sym_cpp(const std::string &cpp_spelling);
std::string itanium_typeinfo_sym_cpp(const std::string &cpp_spelling);
std::string itanium_typeinfo_name_sym_cpp(const std::string &cpp_spelling);
std::string itanium_typeinfo_name_string_cpp(const std::string &cpp_spelling);

// ---------------------------------------------------------------------------
// Substitution-aware (template-id capable) mangling.
//
// These accept fully-qualified C++ type strings that may be template-ids
// with nested template args, e.g.
//   "std::__cxx11::basic_string<char,std::char_traits<char>,std::allocator<char>>"
//   "std::vector<long,std::allocator<long>>"
//   "std::map<std::__cxx11::basic_string<...>,long,std::less<...>,std::allocator<std::pair<const std::__cxx11::basic_string<...>,long>>>"
// and reproduce the exact libstdc++ symbol, including Itanium substitution
// compression (S_, S0_, S1_, …) and the standard St/Sa/Sb/Ss/Si/So/Sd
// abbreviations. They are the path madc uses to call real libstdc++.
//
// The full canonical std:: type spellings are provided as helpers so callers
// don't have to hand-write the default template arguments.
// ---------------------------------------------------------------------------

// Encode a single (possibly template-id) C++ type as an Itanium <type>, with
// substitution compression computed in isolation (a fresh candidate table).
// THE type encoder's direct entry point: the RTTI _cpp symbols and the
// marshalling-boundary predicate read it, and the unit tests pin the builtin
// table (including the LP64/LLP64 width-carrying rows) through it. Memoized on
// the spelling, keyed by the std ABI generation AND the target data model.
std::string itanium_encode_type_sub(const std::string &cpp_type);

// Mangle a member function on a (possibly template-id) class.
//   itanium_mangle_member_sub(class_type, "size", {}, true)
std::string itanium_mangle_member_sub(const std::string &qualified_class,
                                       const std::string &member,
                                       const std::vector<std::string> &param_types,
                                       bool const_method);

// Mangle a member function template specialization on a (possibly template-id)
// class. `template_arg_types` are the deduced concrete template args; `return_type`
// and `param_types` may use "$T0","$T1",... placeholders for the function
// template parameters.
std::string itanium_mangle_member_template_sub(const std::string &qualified_class,
                                       const std::string &member,
                                       const std::vector<std::string> &template_arg_types,
                                       const std::string &return_type,
                                       const std::vector<std::string> &param_types,
                                       bool const_method);

// Mangle a constructor on a (possibly template-id) class. `flavor` picks the
// Itanium variant: "C1" (complete object, the default — what a caller invokes),
// "C2" (base object — what a DERIVED class's ctor invokes for the base
// subobject), "C5" (the COMDAT group both share when the ctor has vague
// linkage, e.g. a class-template instantiation — a group signature, not a
// call target).
std::string itanium_mangle_ctor_sub(const std::string &qualified_class,
                                      const std::vector<std::string> &param_types,
                                      const char *flavor = "C1");

// Mangle a destructor on a (possibly template-id) class. `flavor` picks
// the Itanium variant: "D1" (complete, the default), "D2" (base-object —
// the one a derived dtor calls for a base SUBOBJECT; it must not destroy
// virtual bases), "D0" (deleting).
std::string itanium_mangle_dtor_sub(const std::string &qualified_class,
                                    const char *flavor = "D1");

// Mangle an operator (operator=, operator[], operator+=, …) on a
// (possibly template-id) class. The arity of `param_types` decides the code
// where a spelling is both unary and binary: operator-() is `ng`,
// operator-(const T&) is `mi` (likewise + ps/pl, * de/ml, & ad/an).
//   itanium_mangle_operator_sub(class_type, "+=", {"const char*"}, false)
std::string itanium_mangle_operator_sub(const std::string &qualified_class,
                                         const std::string &op,
                                         const std::vector<std::string> &param_types,
                                         bool const_method);

// Mangle a conversion function `operator <type>() [const]` on a (possibly
// template-id) class — `cv <type>`, never a symbolic operator code.
//   itanium_mangle_conversion_sub("Foo", "bool", true) → "_ZNK3FoocvbEv"
std::string itanium_mangle_conversion_sub(const std::string &qualified_class,
                                           const std::string &target_type,
                                           bool const_method);

// Mangle a non-member std:: function template (operator or named), e.g.
//   std::operator<< <char_traits<char>>(basic_ostream<char,_Traits>&, const char*)
// Form: _ZSt <op-or-source-name> I<targs>E <ret> <params...>. Function templates
// encode the return type. Template parameters in `ret`/`params` are written as
// "$T0","$T1",… (=> T_,T0_,…). `name` is an operator spelling ("<<") or a source
// name ("getline","endl").
std::string itanium_mangle_std_free_template(const std::string &name,
        const std::vector<std::string> &targs,
        const std::string &ret,
        const std::vector<std::string> &params);

// Mangle a function template specialization at a PARSE-FAITHFUL scope — the
// minter for USER function templates (the std:: one above is the flavor-
// agnostic spelling). `qualifiers` as itanium_mangle_nested_sub: none = global,
// {"std"} = the unversioned St, else a nested-name chain. `name` is the
// parse-faithful function name ("ident", or "operator<<"); `targs` the deduced
// concrete template arguments; `ret` / `params` written in the template's own
// parameters as "$T0","$T1",… (=> T_,T0_,…). Function templates encode the
// return type; no parameters is `v`.
//   ({}, "ident", {"int"}, "$T0", {"$T0"})   → "_Z5identIiET_S0_"
//   ({"ns"}, "nident", {"int"}, "$T0", {"$T0"}) → "_ZN2ns6nidentIiEET_S1_"
std::string itanium_mangle_function_template_sub(
        const std::vector<std::string> &qualifiers,
        const std::string &name,
        const std::vector<std::string> &targs,
        const std::string &ret,
        const std::vector<std::string> &params);

// Mangle a non-template function at any scope — global (no qualifiers:
// _Z<name><params>), std (the St abbreviation), or a qualifier chain (N..E) —
// using the substitution-aware type encoder for parameter spellings. `name` may
// be an operator-function-id ("operator<<" → the operator code, unary iff one
// parameter). `internal_linkage` (a `static` function) prefixes the source name
// with L: _ZL4s_fni, _ZN2nsL1fEv — the FuncDef::internal_linkage fact.
std::string itanium_mangle_nested_sub(const std::vector<std::string> &qualifiers,
                                      const std::string &name,
                                      const std::vector<std::string> &param_types,
                                      bool internal_linkage = false);

// Mangle a namespace-scope variable, e.g. std::cin -> "_ZSt3cin".
std::string itanium_mangle_nested_var(const std::vector<std::string> &qualifiers,
                                      const std::string &name);

// Mangle a namespace-scope std:: variable, e.g. std::cout → "_ZSt4cout"
// (GNU: cout lives directly in std, never in __cxx11). Under the LLVM
// flavor everything lives in the ABI namespace: → "_ZNSt3__14coutE".
std::string itanium_mangle_std_var(const std::string &name);

// Function-pointer types are supported in the _sub encoders via the spelling
//   "<ret> (*)(<param>, …)"   e.g. "X& (*)(X&)" → "PF<ret><params>E".
// (Used for the ostream manipulator operator<< — itanium_mangle_operator_sub
// with a function-pointer parameter.)

// ---------------------------------------------------------------------------
// Standard-library flavor / std ABI inline namespace.
//
// The canonical std:: spellings below follow the ACTIVE C++ standard library,
// recorded from the PARSED stdlib configuration — never a host #ifdef, never
// keyed on a flavor name. Each stdlib declares its ABI through the macro that
// exists for exactly this purpose:
//   libstdc++: _GLIBCXX_USE_CXX11_ABI gates the __cxx11 inline namespace on
//              the cxx11-tagged components only (basic_string,
//              basic_stringstream); the supporting templates (allocator,
//              char_traits, less, pair, vector, map, set) stay directly in std.
//   libc++:    _LIBCPP_ABI_NAMESPACE names the inline namespace EVERY
//              component lives in (e.g. "__1").
// Until a parsed config says otherwise, the state is the build default:
// the GNU cxx11 ABI. Program::note_std_abi_define() pushes the parsed fact
// here from every define_map write site (directive, forest replay, CLI -D).
// ---------------------------------------------------------------------------
enum MangleStdlib { mstdlibGnu = 0, mstdlibLlvm = 1 };
void madc_mangle_set_stdlib_gnu(bool cxx11_abi);             // _GLIBCXX_USE_CXX11_ABI
void madc_mangle_set_stdlib_llvm(const std::string &abi_ns); // _LIBCPP_ABI_NAMESPACE

// The ACTIVE mangling flavor (follows the parsed stdlib config = the SCRIPT's
// flavor). The HOST's flavor is the stdlib madc itself was built against —
// GNU cxx11 — a build fact, not a parse fact.
MangleStdlib madc_mangle_active_stdlib();
// The HOST's flavor: the stdlib madc itself was built against — a build fact
// (_LIBCPP_VERSION at compile time), never a parse fact.
MangleStdlib madc_mangle_host_stdlib();
// The script's flavor is not the host's: the marshalling boundary's gate
// (CirBuilder::flavor_marshal_candidate), the format intrinsic's std::string
// operand (cir_format.cpp); the lexer's __MADC_HOST_LIBCPP__ is the same fact
// for a dialect fragment's script-side rendering (bits/value_stream).
bool madc_mangle_flavor_differs();

// Scoped remint under the HOST flavor (task #69 flavor-ABI marshalling):
// host-implemented namespace publics export host-flavor symbols only, so the
// marshalling boundary mints their twin symbol with the mangler temporarily
// in the host state, then restores the script state.
struct MangleHostFlavorScope {
    MangleStdlib saved_stdlib;
    std::string saved_abi_ns;
    MangleHostFlavorScope();
    ~MangleHostFlavorScope();
};

// Canonical std:: type spellings (full default template args), so callers
// don't hand-write them. Each returns the C++ type string accepted by the
// _sub helpers above, spelled for the ACTIVE stdlib flavor (the __cxx11
// forms shown are the GNU-default shapes).
std::string std_string_type();                              // std::__cxx11::basic_string<char,...>
std::string std_vector_type(const std::string &elem);       // std::vector<elem, std::allocator<elem>>
std::string std_map_type(const std::string &key,
                         const std::string &val);            // std::map<key,val,std::less<key>,std::allocator<std::pair<const key,val>>>
std::string std_set_type(const std::string &elem);          // std::set<elem,std::less<elem>,std::allocator<elem>>
std::string std_stringstream_type();                        // std::__cxx11::basic_stringstream<char,...>

#endif // __MADC_MANGLE_H
