#ifndef __MADC_MANGLE_H
#define __MADC_MANGLE_H 1

#include <string>
#include <vector>

// Encode a single C++ type as an Itanium ABI type string.
// Examples: "int" → "i", "const char*" → "PKc", "Foo" → "3Foo"
std::string itanium_encode_type(const std::string &cpp_type);

// Encode a parameter list. Empty list returns "v" (void).
std::string itanium_encode_params(const std::vector<std::string> &param_types);

// Mangle a free function.
// Example: ("foo", {"int", "double"}) → "_Z3fooid"
std::string itanium_mangle(const std::string &func_name,
                            const std::vector<std::string> &param_types);

// Mangle a class method.
// Example: ("Foo", "bar", {"int"}) → "_ZN3Foo3barEi"
std::string itanium_mangle_method(const std::string &class_name,
                                   const std::string &method_name,
                                   const std::vector<std::string> &param_types);

// Mangle a constructor (C1 = complete object ctor).
// Example: ("Foo", {"int"}) → "_ZN3FooC1Ei"
std::string itanium_mangle_ctor(const std::string &class_name,
                                 const std::vector<std::string> &param_types);

// Mangle a destructor (D1 = complete object dtor).
// Example: ("Foo") → "_ZN3FooD1Ev"
std::string itanium_mangle_dtor(const std::string &class_name);

// Mangle with namespace qualifiers.
// Example: ({"ns", "Foo"}, "bar", {"int"}) → "_ZN2ns3Foo3barEi"
std::string itanium_mangle_nested(const std::vector<std::string> &qualifiers,
                                   const std::string &name,
                                   const std::vector<std::string> &param_types);

// Mangle an operator: ("Counter", "==", {"int"}) → "_ZN7CountereqEi"
std::string itanium_mangle_operator(const std::string &class_name,
                                     const std::string &op,
                                     const std::vector<std::string> &param_types);

// RTTI symbols for an un-namespaced user class. source_name = <len><name>.
//   itanium_typeinfo_sym("C")         → "_ZTI1C"   (typeinfo for C)
//   itanium_typeinfo_name_sym("C")    → "_ZTS1C"   (typeinfo name for C)
//   itanium_typeinfo_name_string("C") → "1C"       (the bare mangled name string)
std::string itanium_typeinfo_sym(const std::string &class_name);
std::string itanium_typeinfo_name_sym(const std::string &class_name);
std::string itanium_typeinfo_name_string(const std::string &class_name);
// Real libstdc++ vtable/typeinfo symbols from a canonical C++ spelling (St-aware),
// e.g. "std::bad_alloc" -> _ZTVSt9bad_alloc / _ZTISt9bad_alloc. Used to defer an
// externally-defined class's vtable/typeinfo to the library.
std::string itanium_vtable_sym_cpp(const std::string &cpp_spelling);
std::string itanium_typeinfo_sym_cpp(const std::string &cpp_spelling);

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

// Encode a single (possibly template-id) C++ type as an Itanium <type>,
// with substitution compression computed in isolation (a fresh candidate
// table). Mainly useful for testing the type encoder directly.
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

// Mangle a constructor (C1) on a (possibly template-id) class.
std::string itanium_mangle_ctor_sub(const std::string &qualified_class,
                                      const std::vector<std::string> &param_types);

// Mangle a destructor on a (possibly template-id) class. `flavor` picks
// the Itanium variant: "D1" (complete, the default), "D2" (base-object —
// the one a derived dtor calls for a base SUBOBJECT; it must not destroy
// virtual bases), "D0" (deleting).
std::string itanium_mangle_dtor_sub(const std::string &qualified_class,
                                    const char *flavor = "D1");

// Mangle an operator (operator=, operator[], operator+=, …) on a
// (possibly template-id) class.
//   itanium_mangle_operator_sub(class_type, "+=", {"const char*"}, false)
std::string itanium_mangle_operator_sub(const std::string &qualified_class,
                                         const std::string &op,
                                         const std::vector<std::string> &param_types,
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

// Mangle a non-template namespace-scope function using the substitution-aware
// type encoder for parameter spellings.
std::string itanium_mangle_nested_sub(const std::vector<std::string> &qualifiers,
                                      const std::string &name,
                                      const std::vector<std::string> &param_types);

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
