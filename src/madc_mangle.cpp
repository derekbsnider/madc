// madc_mangle.cpp — Itanium C++ ABI name mangler.
//
// Produces mangled symbol names following the Itanium ABI spec:
// https://itanium-cxx-abi.github.io/cxx-abi/abi.html#mangling
//
// Covers: builtin types, pointers, references, const, classes,
// nested names, constructors (C1), destructors (D1).

#include "madc_mangle.h"
#include <cstring>

// Length-prefixed source name: "Foo" → "3Foo"
static std::string source_name(const std::string &name)
{
	return std::to_string(name.size()) + name;
}

// Strip leading/trailing whitespace
static std::string strip(const std::string &s)
{
	size_t start = s.find_first_not_of(" \t");
	if (start == std::string::npos) return "";
	size_t end = s.find_last_not_of(" \t");
	return s.substr(start, end - start + 1);
}

// Check if a string ends with a suffix
static bool ends_with(const std::string &s, const std::string &suffix)
{
	if (suffix.size() > s.size()) return false;
	return s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

// Try to match a builtin type name, return its Itanium code or empty string
static std::string builtin_code(const std::string &t)
{
	if (t == "void")                return "v";
	if (t == "bool")                return "b";
	if (t == "char")                return "c";
	if (t == "signed char")         return "a";
	if (t == "unsigned char")       return "h";
	if (t == "short")               return "s";
	if (t == "short int")           return "s";
	if (t == "unsigned short")      return "t";
	if (t == "unsigned short int")  return "t";
	if (t == "int")                 return "i";
	if (t == "unsigned int")        return "j";
	if (t == "unsigned")            return "j";
	if (t == "long")                return "l";
	if (t == "long int")            return "l";
	if (t == "unsigned long")       return "m";
	if (t == "unsigned long int")   return "m";
	if (t == "long long")           return "x";
	if (t == "long long int")       return "x";
	if (t == "unsigned long long")  return "y";
	if (t == "float")               return "f";
	if (t == "double")              return "d";
	if (t == "long double")         return "e";
	if (t == "wchar_t")             return "w";

	// Fixed-width aliases (Linux x86-64 mappings)
	if (t == "int8_t")              return "a";
	if (t == "uint8_t")             return "h";
	if (t == "int16_t")             return "s";
	if (t == "uint16_t")            return "t";
	if (t == "int32_t")             return "i";
	if (t == "uint32_t")            return "j";
	if (t == "int64_t")             return "l";
	if (t == "uint64_t")            return "m";
	if (t == "size_t")              return "m";
	if (t == "ssize_t")             return "l";
	if (t == "ptrdiff_t")           return "l";

	return "";
}

std::string itanium_encode_type(const std::string &raw_type)
{
	std::string t = strip(raw_type);
	if (t.empty()) return "v";

	// Pointer: strip trailing * and recurse
	if (ends_with(t, "*")) {
		std::string inner = strip(t.substr(0, t.size() - 1));
		return "P" + itanium_encode_type(inner);
	}

	// Reference: strip trailing & and recurse
	if (ends_with(t, "&")) {
		std::string inner = strip(t.substr(0, t.size() - 1));
		return "R" + itanium_encode_type(inner);
	}

	// Const: strip leading "const " and wrap with K
	if (t.substr(0, 6) == "const ") {
		std::string inner = strip(t.substr(6));
		return "K" + itanium_encode_type(inner);
	}

	// Builtin type
	std::string code = builtin_code(t);
	if (!code.empty()) return code;

	// User-defined type: length-prefixed name
	return source_name(t);
}

std::string itanium_encode_params(const std::vector<std::string> &param_types)
{
	if (param_types.empty()) return "v";
	std::string result;
	for (const auto &pt : param_types)
		result += itanium_encode_type(pt);
	return result;
}

std::string itanium_mangle(const std::string &func_name,
                            const std::vector<std::string> &param_types)
{
	return "_Z" + source_name(func_name) + itanium_encode_params(param_types);
}

std::string itanium_mangle_method(const std::string &class_name,
                                   const std::string &method_name,
                                   const std::vector<std::string> &param_types)
{
	return "_ZN" + source_name(class_name) + source_name(method_name)
	       + "E" + itanium_encode_params(param_types);
}

std::string itanium_mangle_ctor(const std::string &class_name,
                                 const std::vector<std::string> &param_types)
{
	return "_ZN" + source_name(class_name) + "C1"
	       + "E" + itanium_encode_params(param_types);
}

std::string itanium_mangle_dtor(const std::string &class_name)
{
	return "_ZN" + source_name(class_name) + "D1Ev";
}

static std::string operator_code(const std::string &op)
{
	if (op == "==") return "eq";
	if (op == "!=") return "ne";
	if (op == "<")  return "lt";
	if (op == ">")  return "gt";
	if (op == "<=") return "le";
	if (op == ">=") return "ge";
	if (op == "+")  return "pl";
	if (op == "-")  return "mi";
	if (op == "*")  return "ml";
	if (op == "/")  return "dv";
	if (op == "%")  return "rm";
	if (op == "=")  return "aS";
	if (op == "+=") return "pL";
	if (op == "-=") return "mI";
	if (op == "*=") return "mL";
	if (op == "/=") return "dV";
	if (op == "<<") return "ls";
	if (op == ">>") return "rs";
	if (op == "[]") return "ix";
	if (op == "()") return "cl";
	if (op == "++") return "pp";
	if (op == "--") return "mm";
	if (op == "!")  return "nt";
	if (op == "~")  return "co";
	if (op == "&")  return "an";
	if (op == "|")  return "or";
	if (op == "^")  return "eo";
	return "";
}

std::string itanium_mangle_operator(const std::string &class_name,
                                     const std::string &op,
                                     const std::vector<std::string> &param_types)
{
	std::string code = operator_code(op);
	if (code.empty()) return "";
	return "_ZN" + source_name(class_name) + code
	       + "E" + itanium_encode_params(param_types);
}

std::string itanium_mangle_nested(const std::vector<std::string> &qualifiers,
                                   const std::string &name,
                                   const std::vector<std::string> &param_types)
{
	std::string result = "_ZN";
	for (const auto &q : qualifiers)
		result += source_name(q);
	result += source_name(name);
	result += "E" + itanium_encode_params(param_types);
	return result;
}
