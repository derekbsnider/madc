#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

#include "madc_mangle.h"
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "datadef.h"

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

	// Task #46: the width-carrying rows follow the target data model.
	// x86_64-w64-mingw32-g++ mangles f(size_t) as _Z1fy (size_t is
	// unsigned long long there) where Linux g++ says _Z1fm; the 64-bit
	// dds desugar through `long long` on LLP64 (_Znwy, never _Znwm).
	TEST_CASE("LLP64 target model flips width-carrying rows only") {
		struct ModelGuard {
			TargetDataModel saved;
			ModelGuard() : saved(madc_target_data_model) {}
			~ModelGuard() { madc_target_data_model = saved; }
		} guard;
		madc_target_data_model = TargetDataModel::LLP64;
		CHECK(itanium_encode_type("size_t") == "y");
		CHECK(itanium_encode_type("std::size_t") == "y");
		CHECK(itanium_encode_type("int64_t") == "x");
		CHECK(itanium_encode_type("uint64_t") == "y");
		CHECK(itanium_encode_type("ssize_t") == "x");
		CHECK(itanium_encode_type("ptrdiff_t") == "x");
		// Plain long/long long letters are TYPE identity, not width —
		// they never move with the model.
		CHECK(itanium_encode_type("long") == "l");
		CHECK(itanium_encode_type("unsigned long") == "m");
		CHECK(itanium_encode_type("long long") == "x");
		CHECK(itanium_encode_type("unsigned long long") == "y");
		// The 64-bit desugar follows the model too. The desugar fires
		// only for PLAIN DataDef instances (the typeid guard) — the
		// parser-minted scalar-typedef alias shape — never for the
		// builtin dd subclasses, so test the shape the real path sees.
		DataDef off_alias("streamoff", 8, DataType::dtINT64);
		DataDef sz_alias("streamsize_u", 8, DataType::dtUINT64);
		CHECK(off_alias.mangle_scalar_spelling() == "long long");
		CHECK(sz_alias.mangle_scalar_spelling() == "unsigned long long");
		// Negative control: LP64 restores the Linux letters.
		madc_target_data_model = TargetDataModel::LP64;
		CHECK(itanium_encode_type("size_t") == "m");
		CHECK(off_alias.mangle_scalar_spelling() == "long");
	}

	// The darwin int64 alias (fourth target property): Apple is LP64 yet
	// its headers alias the int64 family to `long long` — so int64_t
	// mangles x while plain long stays l, and `long long` needs a dd
	// identity DISTINCT from long (the madcide-on-darwin wall: the host
	// exports __ZN4madc9chan_makeEx, the collapsed dd asked for ...El).
	TEST_CASE("darwin int64 alias: long long distinct from long on LP64") {
		struct AliasGuard {
			TargetInt64Alias saved;
			AliasGuard() : saved(madc_target_int64_alias) {}
			~AliasGuard() { madc_target_int64_alias = saved; }
		} guard;
		madc_target_int64_alias = TargetInt64Alias::LongLong;
		// The STRING rows keep the data-model letters: they encode the
		// pinned dds by name, and on darwin ddUINT64 carries size_t
		// (_Znwm — flipping these produced the _Znwy battery
		// regression). The x/y identity rides the distinct dds below.
		CHECK(itanium_encode_type("int64_t") == "l");
		CHECK(itanium_encode_type("uint64_t") == "m");
		CHECK(itanium_encode_type("size_t") == "m");
		CHECK(itanium_encode_type("ptrdiff_t") == "l");
		CHECK(itanium_encode_type("long") == "l");
		CHECK(itanium_encode_type("long long") == "x");
		// Distinct dd identity, subclass-exempt from the LP64 desugar.
		CHECK(dd_platform_longlong() != (DataDef *)&ddINT64);
		CHECK(dd_platform_longlong()->name == "long long");
		CHECK(dd_platform_longlong()->mangle_scalar_spelling() == "");
		CHECK(dd_platform_ulonglong()->name == "unsigned long long");
		CHECK(dd_platform_long() == (DataDef *)&ddINT64);
		// Negative control: the glibc alias collapses to one identity.
		madc_target_int64_alias = TargetInt64Alias::Long;
		CHECK(dd_platform_longlong() == (DataDef *)&ddINT64);
		CHECK(dd_platform_ulonglong() == (DataDef *)&ddUINT64);
		CHECK(itanium_encode_type("int64_t") == "l");
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

	TEST_CASE("Trailing ellipsis encodes as z") {
		CHECK(itanium_encode_params({"const char*", "..."}) == "PKcz");
		// g++ oracle: std::__throw_out_of_range_fmt(char const*, ...)
		CHECK(itanium_mangle_nested_sub({"std"}, "__throw_out_of_range_fmt",
		                                {"const char*", "..."})
		      == "_ZSt24__throw_out_of_range_fmtPKcz");
	}

	TEST_CASE("Free operators encode their operator code in EVERY scope") {
		// The std and general-qualified branches fell to source_name,
		// emitting the invalid _ZSt10operator<< — only the global branch
		// consulted operator_code(). Oracle: c++filt on each expected form
		// demangles to the intended declaration.
		// c++filt _ZStlsii -> std::operator<<(int, int)
		CHECK(itanium_mangle_nested_sub({"std"}, "operator<<", {"int", "int"})
		      == "_ZStlsii");
		// c++filt _ZN5mylibplEii -> mylib::operator+(int, int)
		CHECK(itanium_mangle_nested_sub({"mylib"}, "operator+", {"int", "int"})
		      == "_ZN5mylibplEii");
		// Plain names unchanged; real libstdc++ export.
		CHECK(itanium_mangle_nested_sub({"std"}, "terminate", {})
		      == "_ZSt9terminatev");
	}

	TEST_CASE("Desugared typedef params match libstdc++ exports") {
		// __basic_file<char>::seekoff(streamoff, _Ios_Seekdir) — streamoff
		// desugars to long ('l'); the enum keeps its own name. Oracle:
		// nm libstdc++ → _ZNSt12__basic_fileIcE7seekoffElSt12_Ios_Seekdir
		CHECK(itanium_mangle_member_sub("std::__basic_file<char>", "seekoff",
		                                {"long", "std::_Ios_Seekdir"}, false)
		      == "_ZNSt12__basic_fileIcE7seekoffElSt12_Ios_Seekdir");
		// basic_ostream<char>::_M_insert<long>(long) with the __ostream_type&
		// return desugared to the canonical class reference. Oracle:
		// nm libstdc++ → _ZNSo9_M_insertIlEERSoT_
		CHECK(itanium_mangle_member_template_sub(
			      "std::basic_ostream<char,std::char_traits<char>>",
			      "_M_insert", {"long"},
			      "std::basic_ostream<char,std::char_traits<char>>&",
			      {"$T0"}, false)
		      == "_ZNSo9_M_insertIlEERSoT_");
	}

	TEST_CASE("Class static data members match libstdc++ exports") {
		// A static data member of a LIBRARY-owned class must carry its real
		// Itanium symbol: the storage lives in libstdc++, and madc's own
		// `<tag>__<member>` spelling (numpunct_char__id) names nothing.
		// Oracles, all `nm -D libstdc++.so.6` (probe UNANCHORED — these
		// carry @@GLIBCXX version suffixes, and a `$`-anchored grep reports
		// a false zero).
		CHECK(itanium_mangle_nested_var({"std", "numpunct<char>"}, "id")
		      == "_ZNSt8numpunctIcE2idE");
		CHECK(itanium_mangle_nested_var({"std", "__cxx11", "numpunct<char>"}, "id")
		      == "_ZNSt7__cxx118numpunctIcE2idE");
		// A NON-TYPE template argument is an expression, not a type:
		// `false` is `Lb0E`, not the identifier `5false`. This one mangled
		// wrong until the literal encoding landed.
		CHECK(itanium_mangle_nested_var({"std", "moneypunct<char,false>"}, "id")
		      == "_ZNSt10moneypunctIcLb0EE2idE");
		CHECK(itanium_mangle_nested_var({"std", "moneypunct<char,true>"}, "id")
		      == "_ZNSt10moneypunctIcLb1EE2idE");
		// A plain namespace-scope variable is the same call with a shorter
		// chain — the class name is just one more qualifier.
		CHECK(itanium_mangle_nested_var({"std"}, "cout") == "_ZSt4cout");
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

	TEST_CASE("Dialect strict-equality operators (Itanium vendor-extended)") {
		// operator=== => v2 (binary vendor op) + source-name "eq3"
		CHECK(itanium_mangle_operator("Money", "===", {"const Money&"})
		      == "_ZN5Moneyv23eq3ERK5Money");
		CHECK(itanium_mangle_operator("Money", "!==", {"const Money&"})
		      == "_ZN5Moneyv23ne3ERK5Money");
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

	TEST_CASE("namespace function with string reference parameter") {
		CHECK(itanium_mangle_nested_sub({"php"}, "trim", {S + "&"})
		      == "_ZN3php4trimERNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
		CHECK(itanium_mangle_nested_sub({"php"}, "number_format",
		                                {S + "&", "long", S + "&"})
		      == "_ZN3php13number_formatERNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEElS6_");
	}
}

TEST_SUITE("Itanium substitution: madc::value (the unified script array)") {

	// ddARRAY's canonical_cpp_spelling() is madc::value; these pin the
	// class-parameter rendering against g++-verified literals (generated
	// from real prototypes via g++ -c + nm). The namespace prefix inside
	// _ZN4madc...E substitutes to S_, and the class must keep its N..E
	// wrap: RNS_5valueE, never RS_5value.

	TEST_CASE("madc::value& parameter — namespace back-ref keeps the N..E wrap") {
		// g++: void madc::context_set_int(madc::value&, std::string&, long)
		CHECK(itanium_mangle_nested_sub({"madc"}, "context_set_int",
		                                {"madc::value&", std_string_type() + "&", "long"})
		      == "_ZN4madc15context_set_intERNS_5valueERNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEEl");
		// g++: bool madc::eval_expression_bool_ctx(const char*, madc::value&)
		CHECK(itanium_mangle_nested_sub({"madc"}, "eval_expression_bool_ctx",
		                                {"const char*", "madc::value&"})
		      == "_ZN4madc24eval_expression_bool_ctxEPKcRNS_5valueE");
	}

	TEST_CASE("repeated madc::value& folds to a substitution") {
		// g++: void madc::context_set_array(madc::value&, std::string&, madc::value&)
		CHECK(itanium_mangle_nested_sub({"madc"}, "context_set_array",
		                                {"madc::value&", std_string_type() + "&", "madc::value&"})
		      == "_ZN4madc17context_set_arrayERNS_5valueERNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEES1_");
		// g++: long madc::eval_int_ctx(std::string&, madc::value&)
		CHECK(itanium_mangle_nested_sub({"madc"}, "eval_int_ctx",
		                                {std_string_type() + "&", "madc::value&"})
		      == "_ZN4madc12eval_int_ctxERNSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEERNS_5valueE");
	}
}

TEST_SUITE("marshalling-boundary text-carrier predicate") {

	// DataDef::marshals_value_text() lives in madc_mangle.cpp (the one
	// permitted home for std:: symbol knowledge); spellings compare
	// through the Itanium encoding, never raw string compares.

	TEST_CASE("spellings compare via Itanium encoding") {
		DataDef cxx11("basic_string", 32, DataType::dtVOID);
		cxx11.set_canonical_spelling(std_string_type());
		CHECK(cxx11.marshals_value_text());

		// template-arg spacing is a spelling variant, not a type
		DataDef spaced("basic_string", 32, DataType::dtVOID);
		spaced.set_canonical_spelling("std::__cxx11::basic_string<char, std::char_traits<char>, std::allocator<char>>");
		CHECK(spaced.marshals_value_text());

		// pre-C++11-ABI std::basic_string (no __cxx11) is a DIFFERENT
		// type (encodes Ss, not NSt7__cxx11...): must NOT match
		DataDef oldabi("basic_string", 32, DataType::dtVOID);
		oldabi.set_canonical_spelling("std::basic_string<char,std::char_traits<char>,std::allocator<char>>");
		CHECK_FALSE(oldabi.marshals_value_text());

		DataDef other("Foo", 8, DataType::dtVOID);
		CHECK_FALSE(other.marshals_value_text());

		DataDef unnamed("", 0, DataType::dtVOID);
		CHECK_FALSE(unnamed.marshals_value_text());
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

TEST_SUITE("Itanium substitution: file streams (ofstream/ifstream/basic_ios)") {

	// Plain std:: (NOT __cxx11) — basic_ofstream/ifstream/ios are not in the
	// __cxx11 inline namespace. Every expected symbol below was confirmed on
	// this machine via c++filt AND `nm -D libstdc++` (the open/close/is_open
	// symbols are real exports; good/eof are exported as weak vague-linkage
	// symbols), so an exact match means madc can dlsym them.
	static const std::string OFS =
		"std::basic_ofstream<char,std::char_traits<char>>";
	static const std::string IFS =
		"std::basic_ifstream<char,std::char_traits<char>>";
	static const std::string BIOS =
		"std::basic_ios<char,std::char_traits<char>>";

	TEST_CASE("ofstream ctor / dtor") {
		CHECK(itanium_mangle_ctor_sub(OFS, {})
		      == "_ZNSt14basic_ofstreamIcSt11char_traitsIcEEC1Ev");
		CHECK(itanium_mangle_dtor_sub(OFS)
		      == "_ZNSt14basic_ofstreamIcSt11char_traitsIcEED1Ev");
	}

	TEST_CASE("ofstream open / close / is_open") {
		// open(const char*, ios_base::openmode) — the defaulted 2nd arg is
		// std::_Ios_Openmode, encoded St13_Ios_Openmode (a non-template std type).
		CHECK(itanium_mangle_member_sub(OFS, "open",
		                                {"const char*", "std::_Ios_Openmode"}, false)
		      == "_ZNSt14basic_ofstreamIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode");
		CHECK(itanium_mangle_member_sub(OFS, "close", {}, false)
		      == "_ZNSt14basic_ofstreamIcSt11char_traitsIcEE5closeEv");
		// is_open() is non-const on basic_ofstream (it forwards to the filebuf)
		CHECK(itanium_mangle_member_sub(OFS, "is_open", {}, false)
		      == "_ZNSt14basic_ofstreamIcSt11char_traitsIcEE7is_openEv");
	}

	TEST_CASE("ifstream ctor / open") {
		CHECK(itanium_mangle_ctor_sub(IFS, {})
		      == "_ZNSt14basic_ifstreamIcSt11char_traitsIcEEC1Ev");
		CHECK(itanium_mangle_member_sub(IFS, "open",
		                                {"const char*", "std::_Ios_Openmode"}, false)
		      == "_ZNSt14basic_ifstreamIcSt11char_traitsIcEE4openEPKcSt13_Ios_Openmode");
	}

	TEST_CASE("basic_ios good / eof (const members)") {
		CHECK(itanium_mangle_member_sub(BIOS, "good", {}, true)
		      == "_ZNKSt9basic_iosIcSt11char_traitsIcEE4goodEv");
		CHECK(itanium_mangle_member_sub(BIOS, "eof", {}, true)
		      == "_ZNKSt9basic_iosIcSt11char_traitsIcEE3eofEv");
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

TEST_SUITE("Itanium substitution: non-member std template operators") {
	static const std::string TR = "std::char_traits<char>";
	static const std::string AL = "std::allocator<char>";
	// In the function-template SIGNATURE the string is expressed in the function's
	// own template params (_CharT=$T0, _Traits=$T1, _Alloc=$T2), NOT concrete types.
	static const std::string STRT =
		"std::__cxx11::basic_string<$T0,$T1,$T2>";

	TEST_CASE("operator<< (ostream&, const char*) / (ostream&, char)") {
		CHECK(itanium_mangle_std_free_template("<<", {TR},
		        "std::basic_ostream<char,$T0>&",
		        {"std::basic_ostream<char,$T0>&", "const char*"})
		      == "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_PKc");
		CHECK(itanium_mangle_std_free_template("<<", {TR},
		        "std::basic_ostream<char,$T0>&",
		        {"std::basic_ostream<char,$T0>&", "char"})
		      == "_ZStlsISt11char_traitsIcEERSt13basic_ostreamIcT_ES5_c");
	}

	TEST_CASE("operator<< (ostream&, const string&)") {
		CHECK(itanium_mangle_std_free_template("<<", {"char", TR, AL},
		        "std::basic_ostream<$T0,$T1>&",
		        {"std::basic_ostream<$T0,$T1>&", "const " + STRT + "&"})
		      == "_ZStlsIcSt11char_traitsIcESaIcEERSt13basic_ostreamIT_T0_ES7_RKNSt7__cxx1112basic_stringIS4_S5_T1_EE");
	}

	TEST_CASE("endl") {
		CHECK(itanium_mangle_std_free_template("endl", {"char", TR},
		        "std::basic_ostream<$T0,$T1>&",
		        {"std::basic_ostream<$T0,$T1>&"})
		      == "_ZSt4endlIcSt11char_traitsIcEERSt13basic_ostreamIT_T0_ES6_");
	}

	TEST_CASE("operator+ (const string&, const string&) -> string BY VALUE") {
		// The libstdc++-exported weak symbol (nm -D libstdc++.so.6); function
		// templates mangle the return type, here the by-value basic_string.
		CHECK(itanium_mangle_std_free_template("+", {"char", TR, AL},
		        STRT,
		        {"const " + STRT + "&", "const " + STRT + "&"})
		      == "_ZStplIcSt11char_traitsIcESaIcEENSt7__cxx1112basic_stringIT_T0_T1_EERKS8_SA_");
	}

	TEST_CASE("operator>> (istream&, string&) and getline") {
		CHECK(itanium_mangle_std_free_template(">>", {"char", TR, AL},
		        "std::basic_istream<$T0,$T1>&",
		        {"std::basic_istream<$T0,$T1>&", STRT + "&"})
		      == "_ZStrsIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE");
		CHECK(itanium_mangle_std_free_template("getline", {"char", TR, AL},
		        "std::basic_istream<$T0,$T1>&",
		        {"std::basic_istream<$T0,$T1>&", STRT + "&"})
		      == "_ZSt7getlineIcSt11char_traitsIcESaIcEERSt13basic_istreamIT_T0_ES7_RNSt7__cxx1112basic_stringIS4_S5_T1_EE");
	}
}

TEST_SUITE("Itanium: std globals + manipulator operator (function-pointer param)") {
	static const std::string OS =
		"std::basic_ostream<char,std::char_traits<char>>";

	TEST_CASE("std namespace-scope variables") {
		CHECK(itanium_mangle_nested_var({"std"}, "cout") == "_ZSt4cout");
		CHECK(itanium_mangle_nested_var({"std"}, "cin")  == "_ZSt3cin");
		CHECK(itanium_mangle_nested_var({"php"}, "value") == "_ZN3php5valueE");
		CHECK(itanium_mangle_std_var("cout") == "_ZSt4cout");
		CHECK(itanium_mangle_std_var("cin")  == "_ZSt3cin");
		CHECK(itanium_mangle_std_var("cerr") == "_ZSt4cerr");
		CHECK(itanium_mangle_std_var("clog") == "_ZSt4clog");
	}

	TEST_CASE("ostream manipulator operator<<(ostream& (*)(ostream&))") {
		CHECK(itanium_mangle_operator_sub(OS, "<<", {OS + "& (*)(" + OS + "&)"}, false)
		      == "_ZNSolsEPFRSoS_E");
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

TEST_SUITE("Stdlib flavor: LLVM ABI namespace from parsed config") {

	// The setters model what Program::note_std_abi_define pushes when the
	// PARSED stdlib config defines _LIBCPP_ABI_NAMESPACE (libc++) or
	// _GLIBCXX_USE_CXX11_ABI (libstdc++). Oracle: clang++-18 -stdlib=libc++
	// on the container (see _Z1fRKNSt3__112basic_stringI... below).

	TEST_CASE("libc++ spellings and encodings follow _LIBCPP_ABI_NAMESPACE") {
		madc_mangle_set_stdlib_llvm("__1");
		CHECK(std_string_type()
		      == "std::__1::basic_string<char,std::__1::char_traits<char>,"
		         "std::__1::allocator<char>>");
		// clang++-18: unsigned long f(const std::string&) →
		// _Z1fRKNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE
		CHECK(itanium_encode_type_sub(std_string_type())
		      == "NSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE");
		CHECK(itanium_mangle_nested_sub({}, "f",
		                                {"const " + std_string_type() + "&"})
		      == "_Z1fRKNSt3__112basic_stringIcNS_11char_traitsIcEENS_9allocatorIcEEEE");
		// clang++-18: &std::cout → _ZNSt3__14coutE
		CHECK(itanium_mangle_std_var("cout") == "_ZNSt3__14coutE");

		// marshals_value_text must re-evaluate its cached carrier on a
		// flavor flip — the libc++ string IS the text carrier now, the
		// GNU spelling is NOT.
		DataDef llvmstr("basic_string", 24, DataType::dtVOID);
		llvmstr.set_canonical_spelling(std_string_type());
		CHECK(llvmstr.marshals_value_text());
		DataDef gnustr("basic_string", 32, DataType::dtVOID);
		gnustr.set_canonical_spelling(
			"std::__cxx11::basic_string<char,std::char_traits<char>,"
			"std::allocator<char>>");
		CHECK_FALSE(gnustr.marshals_value_text());

		// Restore the build default and prove the flip back.
		madc_mangle_set_stdlib_gnu(true);
		CHECK(gnustr.marshals_value_text());
		CHECK_FALSE(llvmstr.marshals_value_text());
		CHECK(itanium_mangle_std_var("cout") == "_ZSt4cout");
	}

	TEST_CASE("libstdc++ pre-cxx11 ABI drops __cxx11 (Ss form)") {
		madc_mangle_set_stdlib_gnu(false);
		CHECK(std_string_type()
		      == "std::basic_string<char,std::char_traits<char>,"
		         "std::allocator<char>>");
		CHECK(itanium_encode_type_sub(std_string_type()) == "Ss");
		madc_mangle_set_stdlib_gnu(true);
		CHECK(itanium_encode_type_sub(std_string_type())
		      == "NSt7__cxx1112basic_stringIcSt11char_traitsIcESaIcEEE");
	}
}
