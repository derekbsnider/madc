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
