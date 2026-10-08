/* madc_keywords.cpp — the standards' keyword lists.
 *
 * One row per keyword: the C standard and the C++ standard it first arrived
 * in, or the extension it belongs to. These are facts about the languages,
 * read by `?name` (plan §41.8a) to say where a keyword comes from. What madc
 * reserves in a given mode is add_keywords' policy (lexer.cpp); where that
 * policy gates on a version, it reads the version from here, so the fact has
 * one home.
 *
 * Thread contract: immutable data; any thread may read it.
 */

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <stack>
#include <list>
#include <queue>
#include <iostream>
#include <sstream>
#include <memory>
#include <stdint.h>

#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

namespace {

typedef Program::KeywordOrigin::Extension Ext;
const Program::LanguageStd NONE = Program::STD_MADC;	// read only when in_* is set

#define KW_C_CPP(s, c, p) { s, true, Program::c, true, Program::p, Ext::none }
#define KW_C(s, c)        { s, true, Program::c, false, NONE, Ext::none }
#define KW_CPP(s, p)      { s, false, NONE, true, Program::p, Ext::none }
#define KW_EXT(s, e)      { s, false, NONE, false, NONE, Ext::e }

const Program::KeywordOrigin kKeywords[] = {
    // K&R C (1978), and C++ from its first standard.
    KW_C_CPP("auto", STD_C78, STD_CPP98),     KW_C_CPP("break", STD_C78, STD_CPP98),
    KW_C_CPP("case", STD_C78, STD_CPP98),     KW_C_CPP("char", STD_C78, STD_CPP98),
    KW_C_CPP("continue", STD_C78, STD_CPP98), KW_C_CPP("default", STD_C78, STD_CPP98),
    KW_C_CPP("do", STD_C78, STD_CPP98),       KW_C_CPP("double", STD_C78, STD_CPP98),
    KW_C_CPP("else", STD_C78, STD_CPP98),     KW_C_CPP("extern", STD_C78, STD_CPP98),
    KW_C_CPP("float", STD_C78, STD_CPP98),    KW_C_CPP("for", STD_C78, STD_CPP98),
    KW_C_CPP("goto", STD_C78, STD_CPP98),     KW_C_CPP("if", STD_C78, STD_CPP98),
    KW_C_CPP("int", STD_C78, STD_CPP98),      KW_C_CPP("long", STD_C78, STD_CPP98),
    KW_C_CPP("register", STD_C78, STD_CPP98), KW_C_CPP("return", STD_C78, STD_CPP98),
    KW_C_CPP("short", STD_C78, STD_CPP98),    KW_C_CPP("sizeof", STD_C78, STD_CPP98),
    KW_C_CPP("static", STD_C78, STD_CPP98),   KW_C_CPP("struct", STD_C78, STD_CPP98),
    KW_C_CPP("switch", STD_C78, STD_CPP98),   KW_C_CPP("typedef", STD_C78, STD_CPP98),
    KW_C_CPP("union", STD_C78, STD_CPP98),    KW_C_CPP("unsigned", STD_C78, STD_CPP98),
    KW_C_CPP("while", STD_C78, STD_CPP98),
    // C89.
    KW_C_CPP("const", STD_C89, STD_CPP98),    KW_C_CPP("enum", STD_C89, STD_CPP98),
    KW_C_CPP("signed", STD_C89, STD_CPP98),   KW_C_CPP("void", STD_C89, STD_CPP98),
    KW_C_CPP("volatile", STD_C89, STD_CPP98),
    // C99.
    KW_C_CPP("inline", STD_C99, STD_CPP98),
    KW_C("restrict", STD_C99),  KW_C("_Bool", STD_C99),
    KW_C("_Complex", STD_C99),  KW_C("_Imaginary", STD_C99),
    // C11.
    KW_C("_Alignas", STD_C11),  KW_C("_Alignof", STD_C11),
    KW_C("_Atomic", STD_C11),   KW_C("_Generic", STD_C11),
    KW_C("_Noreturn", STD_C11), KW_C("_Static_assert", STD_C11),
    KW_C("_Thread_local", STD_C11),
    // C23, several of them C++'s spellings.
    KW_C_CPP("alignas", STD_C23, STD_CPP11),       KW_C_CPP("alignof", STD_C23, STD_CPP11),
    KW_C_CPP("constexpr", STD_C23, STD_CPP11),     KW_C_CPP("nullptr", STD_C23, STD_CPP11),
    KW_C_CPP("static_assert", STD_C23, STD_CPP11), KW_C_CPP("thread_local", STD_C23, STD_CPP11),
    KW_C_CPP("bool", STD_C23, STD_CPP98),          KW_C_CPP("false", STD_C23, STD_CPP98),
    KW_C_CPP("true", STD_C23, STD_CPP98),
    KW_C("typeof", STD_C23),       KW_C("typeof_unqual", STD_C23),
    KW_C("_BitInt", STD_C23),      KW_C("_Decimal32", STD_C23),
    KW_C("_Decimal64", STD_C23),   KW_C("_Decimal128", STD_C23),
    // C++98.
    KW_CPP("asm", STD_CPP98),          KW_CPP("catch", STD_CPP98),
    KW_CPP("class", STD_CPP98),        KW_CPP("const_cast", STD_CPP98),
    KW_CPP("delete", STD_CPP98),       KW_CPP("dynamic_cast", STD_CPP98),
    KW_CPP("explicit", STD_CPP98),     KW_CPP("export", STD_CPP98),
    KW_CPP("friend", STD_CPP98),       KW_CPP("mutable", STD_CPP98),
    KW_CPP("namespace", STD_CPP98),    KW_CPP("new", STD_CPP98),
    KW_CPP("operator", STD_CPP98),     KW_CPP("private", STD_CPP98),
    KW_CPP("protected", STD_CPP98),    KW_CPP("public", STD_CPP98),
    KW_CPP("reinterpret_cast", STD_CPP98), KW_CPP("static_cast", STD_CPP98),
    KW_CPP("template", STD_CPP98),     KW_CPP("this", STD_CPP98),
    KW_CPP("throw", STD_CPP98),        KW_CPP("try", STD_CPP98),
    KW_CPP("typeid", STD_CPP98),       KW_CPP("typename", STD_CPP98),
    KW_CPP("using", STD_CPP98),        KW_CPP("virtual", STD_CPP98),
    KW_CPP("wchar_t", STD_CPP98),
    // C++98's alternative tokens ([lex.digraph]).
    KW_CPP("and", STD_CPP98),    KW_CPP("and_eq", STD_CPP98), KW_CPP("bitand", STD_CPP98),
    KW_CPP("bitor", STD_CPP98),  KW_CPP("compl", STD_CPP98),  KW_CPP("not", STD_CPP98),
    KW_CPP("not_eq", STD_CPP98), KW_CPP("or", STD_CPP98),     KW_CPP("or_eq", STD_CPP98),
    KW_CPP("xor", STD_CPP98),    KW_CPP("xor_eq", STD_CPP98),
    // C++11.
    KW_CPP("char16_t", STD_CPP11), KW_CPP("char32_t", STD_CPP11),
    KW_CPP("decltype", STD_CPP11), KW_CPP("noexcept", STD_CPP11),
    // C++20.
    KW_CPP("char8_t", STD_CPP20),   KW_CPP("concept", STD_CPP20),
    KW_CPP("consteval", STD_CPP20), KW_CPP("constinit", STD_CPP20),
    KW_CPP("co_await", STD_CPP20),  KW_CPP("co_return", STD_CPP20),
    KW_CPP("co_yield", STD_CPP20),  KW_CPP("requires", STD_CPP20),
    // The madc dialect's own.
    KW_EXT("defer", madc), KW_EXT("prefer", madc),
    // GNU spellings madc reserves.
    KW_EXT("__volatile", gnu), KW_EXT("__volatile__", gnu), KW_EXT("__thread", gnu),
};

#undef KW_C_CPP
#undef KW_C
#undef KW_CPP
#undef KW_EXT

} // namespace

const Program::KeywordOrigin *Program::keyword_origin(const madc::dis::istring &spelling)
{
    for ( size_t i = 0; i < sizeof(kKeywords) / sizeof(kKeywords[0]); ++i )
	if ( spelling == kKeywords[i].spelling )
	    return &kKeywords[i];
    return NULL;
}

madc::dis::istring Program::keyword_provenance(const madc::dis::istring &spelling) const
{
    const KeywordOrigin *k = keyword_origin(spelling);
    if ( !k )
	return madc::dis::istring();
    switch ( k->ext )
    {
	case KeywordOrigin::Extension::madc:
	    return "madc";
	case KeywordOrigin::Extension::gnu:
	    return "GNU C";
	case KeywordOrigin::Extension::none:
	    break;
    }
    // The standard it first arrived in, among the session's languages: a C
    // session's is C (C++'s only for a keyword C lacks); C++ and the madc
    // dialect read both, since C++ has C's keywords.
    const bool c_session = language_std != STD_MADC && !is_cpp_mode();
    bool have = false;
    LanguageStd first = lowest_standard(false);
    auto consider = [&](LanguageStd s) {
	if ( !have || standard_year(s) < standard_year(first) )
	{
	    first = s;
	    have = true;
	}
    };
    if ( k->in_c )
	consider(k->c_since);
    if ( k->in_cpp && (!c_session || !k->in_c) )
	consider(k->cpp_since);
    if ( !have )
	return madc::dis::istring();
    // A keyword the language has had from its first standard is the
    // language's own.
    if ( first == lowest_standard(false) )
	return "C";
    if ( first == lowest_standard(true) )
	return "C++";
    return standard_display_name(first);
}
