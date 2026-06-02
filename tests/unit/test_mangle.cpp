#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "madc_mangle.h"

TEST_SUITE("Itanium type encoding") {

	TEST_CASE("Builtin types") {
		CHECK(itanium_encode_type("void") == "v");
		CHECK(itanium_encode_type("bool") == "b");
		CHECK(itanium_encode_type("char") == "c");
		CHECK(itanium_encode_type("short") == "s");
		CHECK(itanium_encode_type("int") == "i");
		CHECK(itanium_encode_type("long") == "l");
		CHECK(itanium_encode_type("long long") == "x");
		CHECK(itanium_encode_type("float") == "f");
		CHECK(itanium_encode_type("double") == "d");
		CHECK(itanium_encode_type("long double") == "e");
		CHECK(itanium_encode_type("unsigned int") == "j");
		CHECK(itanium_encode_type("unsigned long") == "m");
		CHECK(itanium_encode_type("unsigned long long") == "y");
		CHECK(itanium_encode_type("signed char") == "a");
		CHECK(itanium_encode_type("unsigned char") == "h");
		CHECK(itanium_encode_type("wchar_t") == "w");
	}

	TEST_CASE("Fixed-width type aliases") {
		CHECK(itanium_encode_type("int8_t") == "a");
		CHECK(itanium_encode_type("uint8_t") == "h");
		CHECK(itanium_encode_type("int16_t") == "s");
		CHECK(itanium_encode_type("uint16_t") == "t");
		CHECK(itanium_encode_type("int32_t") == "i");
		CHECK(itanium_encode_type("uint32_t") == "j");
		CHECK(itanium_encode_type("int64_t") == "l");
		CHECK(itanium_encode_type("uint64_t") == "m");
		CHECK(itanium_encode_type("size_t") == "m");
	}

	TEST_CASE("Pointer types") {
		CHECK(itanium_encode_type("int*") == "Pi");
		CHECK(itanium_encode_type("char*") == "Pc");
		CHECK(itanium_encode_type("void*") == "Pv");
		CHECK(itanium_encode_type("double*") == "Pd");
	}

	TEST_CASE("Const pointer types") {
		CHECK(itanium_encode_type("const char*") == "PKc");
		CHECK(itanium_encode_type("const int*") == "PKi");
	}

	TEST_CASE("Reference types") {
		CHECK(itanium_encode_type("int&") == "Ri");
		CHECK(itanium_encode_type("const int&") == "RKi");
		CHECK(itanium_encode_type("const char*&") == "RPKc");
	}

	TEST_CASE("User-defined types") {
		CHECK(itanium_encode_type("Foo") == "3Foo");
		CHECK(itanium_encode_type("MyClass") == "7MyClass");
		CHECK(itanium_encode_type("X") == "1X");
	}

	TEST_CASE("Pointer to user type") {
		CHECK(itanium_encode_type("Foo*") == "P3Foo");
		CHECK(itanium_encode_type("const Foo*") == "PK3Foo");
		CHECK(itanium_encode_type("Foo&") == "R3Foo");
	}

	TEST_CASE("Parameter list encoding") {
		CHECK(itanium_encode_params({}) == "v");
		CHECK(itanium_encode_params({"int"}) == "i");
		CHECK(itanium_encode_params({"int", "double"}) == "id");
		CHECK(itanium_encode_params({"int", "const char*"}) == "iPKc");
	}
}

TEST_SUITE("Itanium function mangling") {

	TEST_CASE("Free functions") {
		CHECK(itanium_mangle("foo", {}) == "_Z3foov");
		CHECK(itanium_mangle("foo", {"int"}) == "_Z3fooi");
		CHECK(itanium_mangle("foo", {"int", "double"}) == "_Z3fooid");
		CHECK(itanium_mangle("foo", {"const char*"}) == "_Z3fooPKc");
		CHECK(itanium_mangle("add", {"int", "int"}) == "_Z3addii");
	}

	TEST_CASE("Method mangling") {
		CHECK(itanium_mangle_method("Foo", "bar", {"int"}) == "_ZN3Foo3barEi");
		CHECK(itanium_mangle_method("Foo", "bar", {"int", "double"}) == "_ZN3Foo3barEid");
		CHECK(itanium_mangle_method("Foo", "bar", {"const char*"}) == "_ZN3Foo3barEPKc");
		CHECK(itanium_mangle_method("MyClass", "method", {"int", "double", "const char*"})
		      == "_ZN7MyClass6methodEidPKc");
		CHECK(itanium_mangle_method("Foo", "bar", {}) == "_ZN3Foo3barEv");
	}

	TEST_CASE("Constructor mangling") {
		CHECK(itanium_mangle_ctor("Foo", {}) == "_ZN3FooC1Ev");
		CHECK(itanium_mangle_ctor("Foo", {"int"}) == "_ZN3FooC1Ei");
		CHECK(itanium_mangle_ctor("Foo", {"const char*"}) == "_ZN3FooC1EPKc");
	}

	TEST_CASE("Destructor mangling") {
		CHECK(itanium_mangle_dtor("Foo") == "_ZN3FooD1Ev");
		CHECK(itanium_mangle_dtor("MyClass") == "_ZN7MyClassD1Ev");
	}

	TEST_CASE("Nested name mangling") {
		CHECK(itanium_mangle_nested({"ns", "Foo"}, "bar", {"int"})
		      == "_ZN2ns3Foo3barEi");
		CHECK(itanium_mangle_nested({"std", "string"}, "assign", {"const char*"})
		      == "_ZN3std6string6assignEPKc");
	}
}

TEST_SUITE("Itanium operator mangling") {

	TEST_CASE("Comparison operators") {
		CHECK(itanium_mangle_operator("Counter", "==", {"int"}) == "_ZN7CountereqEi");
		CHECK(itanium_mangle_operator("Counter", "!=", {"int"}) == "_ZN7CounterneEi");
		CHECK(itanium_mangle_operator("Counter", "<", {"int"}) == "_ZN7CounterltEi");
		CHECK(itanium_mangle_operator("Counter", ">", {"int"}) == "_ZN7CountergtEi");
		CHECK(itanium_mangle_operator("Counter", "<=", {"int"}) == "_ZN7CounterleEi");
		CHECK(itanium_mangle_operator("Counter", ">=", {"int"}) == "_ZN7CountergeEi");
	}

	TEST_CASE("Arithmetic operators") {
		CHECK(itanium_mangle_operator("Vec", "+", {"Vec"}) == "_ZN3VecplE3Vec");
		CHECK(itanium_mangle_operator("Vec", "-", {"Vec"}) == "_ZN3VecmiE3Vec");
		CHECK(itanium_mangle_operator("Vec", "*", {"int"}) == "_ZN3VecmlEi");
	}

	TEST_CASE("Unary operators") {
		CHECK(itanium_mangle_operator("Iter", "++", {}) == "_ZN4IterppEv");
		CHECK(itanium_mangle_operator("Iter", "--", {}) == "_ZN4ItermmEv");
	}

	TEST_CASE("Unknown operator returns empty") {
		CHECK(itanium_mangle_operator("Foo", "???", {"int"}) == "");
	}
}

// ===========================================================================
// Substitution-aware (template-id capable) mangling.
//
// Every expected symbol below was captured from `g++ -std=gnu++11 -O0 -S`
// on this machine and verified with c++filt. These are the exact symbols
// libstdc++ exports, so an exact string match means madc can dlsym them.
// ===========================================================================

TEST_SUITE("Itanium substitution: std::string") {

	// std::__cxx11::basic_string<char, char_traits<char>, allocator<char>>
	static const std::string S =
		"std::__cxx11::basic_string<char,std::char_traits<char>,"
		"std::allocator<char>>";

	TEST_CASE("string ctor / dtor") {
		CHECK(itanium_mangle_ctor_sub(S, {})
		      == "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1Ev");
		// note RKS3_ = const ref to substitution #3 (allocator<char>)
		CHECK(itanium_mangle_ctor_sub(S, {"const char*", "const std::allocator<char>&"})
		      == "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1EPKcRKS3_");
		// copy ctor: RKS4_ = const ref to substitution #4 (the whole string)
		CHECK(itanium_mangle_ctor_sub(S, {"const " + S + "&"})
		      == "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEC1ERKS4_");
		CHECK(itanium_mangle_dtor_sub(S)
		      == "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEED1Ev");
	}

	TEST_CASE("string const members") {
		CHECK(itanium_mangle_member_sub(S, "c_str", {}, true)
		      == "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE5c_strEv");
		CHECK(itanium_mangle_member_sub(S, "size", {}, true)
		      == "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE4sizeEv");
		CHECK(itanium_mangle_member_sub(S, "length", {}, true)
		      == "_ZNKSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEE6lengthEv");
	}

	TEST_CASE("string operators") {
		CHECK(itanium_mangle_operator_sub(S, "=", {"const char*"}, false)
		      == "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEaSEPKc");
		CHECK(itanium_mangle_operator_sub(S, "=", {"const " + S + "&"}, false)
		      == "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEaSERKS4_");
		CHECK(itanium_mangle_operator_sub(S, "+=", {"const char*"}, false)
		      == "_ZNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEpLEPKc");
	}

	TEST_CASE("std_string_type() helper matches") {
		CHECK(std_string_type() == S);
	}
}

TEST_SUITE("Itanium substitution: std::vector") {

	TEST_CASE("vector<long>") {
		std::string V = std_vector_type("long");
		CHECK(itanium_mangle_ctor_sub(V, {})  == "_ZNSt6vectorIlSaIlEEC1Ev");
		CHECK(itanium_mangle_dtor_sub(V)       == "_ZNSt6vectorIlSaIlEED1Ev");
		CHECK(itanium_mangle_member_sub(V, "push_back", {"long&&"}, false)
		      == "_ZNSt6vectorIlSaIlEE9push_backEOl");
		CHECK(itanium_mangle_member_sub(V, "at", {"unsigned long"}, false)
		      == "_ZNSt6vectorIlSaIlEE2atEm");
		CHECK(itanium_mangle_member_sub(V, "size", {}, true)
		      == "_ZNKSt6vectorIlSaIlEE4sizeEv");
		CHECK(itanium_mangle_operator_sub(V, "[]", {"unsigned long"}, false)
		      == "_ZNSt6vectorIlSaIlEEixEm");
	}

	TEST_CASE("vector<int>") {
		std::string VI = std_vector_type("int");
		CHECK(itanium_mangle_operator_sub(VI, "[]", {"unsigned long"}, false)
		      == "_ZNSt6vectorIiSaIiEEixEm");
		CHECK(itanium_mangle_member_sub(VI, "push_back", {"int&&"}, false)
		      == "_ZNSt6vectorIiSaIiEE9push_backEOi");
	}

	TEST_CASE("vector<string>") {
		std::string VS = std_vector_type(std_string_type());
		CHECK(itanium_mangle_member_sub(VS, "push_back",
		                                {std_string_type() + "&&"}, false)
		      == "_ZNSt6vectorINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESaIS5_EE9push_backEOS5_");
	}
}

TEST_SUITE("Itanium substitution: std::map") {

	TEST_CASE("map<string,long>") {
		std::string M = std_map_type(std_string_type(), "long");
		CHECK(itanium_mangle_ctor_sub(M, {})
		      == "_ZNSt3mapINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEElSt4lessIS5_ESaISt4pairIKS5_lEEEC1Ev");
		CHECK(itanium_mangle_dtor_sub(M)
		      == "_ZNSt3mapINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEElSt4lessIS5_ESaISt4pairIKS5_lEEED1Ev");
		CHECK(itanium_mangle_operator_sub(M, "[]", {std_string_type() + "&&"}, false)
		      == "_ZNSt3mapINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEElSt4lessIS5_ESaISt4pairIKS5_lEEEixEOS5_");
		CHECK(itanium_mangle_member_sub(M, "find",
		                                {"const " + std_string_type() + "&"}, false)
		      == "_ZNSt3mapINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEElSt4lessIS5_ESaISt4pairIKS5_lEEE4findERS9_");
		CHECK(itanium_mangle_member_sub(M, "size", {}, true)
		      == "_ZNKSt3mapINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEElSt4lessIS5_ESaISt4pairIKS5_lEEE4sizeEv");
	}

	TEST_CASE("map<string,string>") {
		std::string M = std_map_type(std_string_type(), std_string_type());
		CHECK(itanium_mangle_operator_sub(M, "[]", {std_string_type() + "&&"}, false)
		      == "_ZNSt3mapINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES5_St4lessIS5_ESaISt4pairIKS5_S5_EEEixEOS5_");
	}
}

TEST_SUITE("Itanium substitution: std::set") {

	TEST_CASE("set<string>") {
		std::string ST = std_set_type(std_string_type());
		CHECK(itanium_mangle_ctor_sub(ST, {})
		      == "_ZNSt3setINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt4lessIS5_ESaIS5_EEC1Ev");
		CHECK(itanium_mangle_dtor_sub(ST)
		      == "_ZNSt3setINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt4lessIS5_ESaIS5_EED1Ev");
		CHECK(itanium_mangle_member_sub(ST, "insert", {std_string_type() + "&&"}, false)
		      == "_ZNSt3setINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt4lessIS5_ESaIS5_EE6insertEOS5_");
		CHECK(itanium_mangle_member_sub(ST, "find",
		                                {"const " + std_string_type() + "&"}, false)
		      == "_ZNSt3setINSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEESt4lessIS5_ESaIS5_EE4findERKS5_");
	}
}

TEST_SUITE("Itanium substitution: std::stringstream") {

	TEST_CASE("stringstream ctor / dtor / str") {
		std::string SS = std_stringstream_type();
		CHECK(itanium_mangle_ctor_sub(SS, {})
		      == "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEEC1Ev");
		CHECK(itanium_mangle_dtor_sub(SS)
		      == "_ZNSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEED1Ev");
		CHECK(itanium_mangle_member_sub(SS, "str", {}, true)
		      == "_ZNKSt7__cxx1118basic_stringstreamIcSt11char_traitsIcESaIcEE3strEv");
	}
}

TEST_SUITE("Itanium substitution: complete-spec abbreviations (So/Si/Sd)") {

	static const std::string OS =
		"std::basic_ostream<char,std::char_traits<char>>";
	static const std::string IS =
		"std::basic_istream<char,std::char_traits<char>>";

	TEST_CASE("ostream member operator<< overloads") {
		CHECK(itanium_mangle_operator_sub(OS, "<<", {"double"}, false)       == "_ZNSolsEd");
		CHECK(itanium_mangle_operator_sub(OS, "<<", {"int"}, false)          == "_ZNSolsEi");
		CHECK(itanium_mangle_operator_sub(OS, "<<", {"long"}, false)         == "_ZNSolsEl");
		CHECK(itanium_mangle_operator_sub(OS, "<<", {"unsigned int"}, false) == "_ZNSolsEj");
		CHECK(itanium_mangle_operator_sub(OS, "<<", {"const void*"}, false)  == "_ZNSolsEPKv");
	}

	TEST_CASE("istream member operator>>") {
		CHECK(itanium_mangle_operator_sub(IS, ">>", {"int&"}, false) == "_ZNSirsERi");
	}

	TEST_CASE("complete-spec abbreviation as a standalone type / reference") {
		CHECK(itanium_encode_type_sub(OS)        == "So");
		CHECK(itanium_encode_type_sub(OS + "&")  == "RSo");
		CHECK(itanium_encode_type_sub(IS)        == "Si");
	}

	TEST_CASE("regression: Sa/Sb still take template args (NOT complete)") {
		CHECK(itanium_encode_type_sub("std::allocator<char>") == "SaIcE");
		// __cxx11 string still spelled out (uses Sb-family, not Ss)
		CHECK(itanium_encode_type_sub(std_string_type())
		      == "NSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
	}
}

TEST_SUITE("Itanium substitution: type encoder sanity") {

	TEST_CASE("Standalone std types wrap N..E only when nested") {
		// char_traits<char> under std: St abbrev, single source-name, no N..E
		CHECK(itanium_encode_type_sub("std::char_traits<char>")
		      == "St11char_traitsIcE");
		// allocator<char>: Sa abbreviation
		CHECK(itanium_encode_type_sub("std::allocator<char>") == "SaIcE");
		// __cxx11::basic_string: two source-names → standalone needs N..E
		CHECK(itanium_encode_type_sub(std_string_type())
		      == "NSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
	}

	TEST_CASE("Decorations: rvalue-ref O vs lvalue-ref R") {
		CHECK(itanium_encode_type_sub("long&&") == "Ol");
		CHECK(itanium_encode_type_sub("long&")  == "Rl");
		CHECK(itanium_encode_type_sub("const char*") == "PKc");
	}
}
