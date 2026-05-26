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

#endif // __MADC_MANGLE_H
