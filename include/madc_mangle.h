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

// Mangle a constructor (C1) on a (possibly template-id) class.
std::string itanium_mangle_ctor_sub(const std::string &qualified_class,
                                      const std::vector<std::string> &param_types);

// Mangle a destructor (D1) on a (possibly template-id) class.
std::string itanium_mangle_dtor_sub(const std::string &qualified_class);

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

// Mangle a namespace-scope std:: variable, e.g. std::cout → "_ZSt4cout".
std::string itanium_mangle_std_var(const std::string &name);

// Function-pointer types are supported in the _sub encoders via the spelling
//   "<ret> (*)(<param>, …)"   e.g. "X& (*)(X&)" → "PF<ret><params>E".
// (Used for the ostream manipulator operator<< — itanium_mangle_operator_sub
// with a function-pointer parameter.)

// Canonical std:: type spellings (full default template args), so callers
// don't hand-write them. Each returns the C++ type string accepted by the
// _sub helpers above.
std::string std_string_type();                              // std::__cxx11::basic_string<char,...>
std::string std_vector_type(const std::string &elem);       // std::vector<elem, std::allocator<elem>>
std::string std_map_type(const std::string &key,
                         const std::string &val);            // std::map<key,val,std::less<key>,std::allocator<std::pair<const key,val>>>
std::string std_set_type(const std::string &elem);          // std::set<elem,std::less<elem>,std::allocator<elem>>
std::string std_stringstream_type();                        // std::__cxx11::basic_stringstream<char,...>

#endif // __MADC_MANGLE_H
