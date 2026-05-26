///////////////////////////////////////////////////////////////////////////
//                                                                     //
// madc ruby:: namespace                                               //
//                                                                     //
// Ruby-unique string and array operations.                            //
//                                                                     //
///////////////////////////////////////////////////////////////////////////

#include <string>
#include <cstring>
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#include <queue>
#include <stack>
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include <asmjit/x86.h>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "ns_common.h"

using namespace std;
using namespace asmjit;

// ---- C++ wrapper functions ----

// ruby::squeeze — collapse consecutive duplicate characters
// "aaabbbccc" -> "abc", "aabbcc" -> "abc"
void *ruby_squeeze(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	if ( s.length() < 2 ) return ptr;
	std::string result;
	result += s[0];
	for ( size_t i = 1; i < s.length(); ++i )
		if ( s[i] != s[i-1] )
			result += s[i];
	s = result;
	return ptr;
}

// ruby::tr — transliterate characters (like Unix tr)
// ruby::tr(str, "aeiou", "*") -> replace each vowel with *
void *ruby_tr(void *ptr, void *from, void *to)
{
	std::string &s = *(std::string *)ptr;
	std::string &f = *(std::string *)from;
	std::string &t = *(std::string *)to;
	if ( f.empty() ) return ptr;
	for ( size_t i = 0; i < s.length(); ++i )
	{
		size_t pos = f.find(s[i]);
		if ( pos != std::string::npos )
			s[i] = pos < t.length() ? t[pos] : t.back();
	}
	return ptr;
}

// ruby::chars — split string into array of individual characters
void *ruby_chars(void *arr, void *str)
{
	MadArray &a = *(MadArray *)arr;
	std::string &s = *(std::string *)str;
	a.data.clear();
	a.assoc.clear();
	for ( size_t i = 0; i < s.length(); ++i )
		a.push(MadValue(std::string(1, s[i])));
	return arr;
}

// ruby::rotate — rotate array elements by n positions
// [1,2,3,4,5].rotate(2) -> [3,4,5,1,2]
void *ruby_rotate(void *arr, int64_t n)
{
	MadArray &a = *(MadArray *)arr;
	if ( a.data.empty() ) return arr;
	int64_t sz = (int64_t)a.data.size();
	n = ((n % sz) + sz) % sz; // handle negative rotation
	std::rotate(a.data.begin(), a.data.begin() + n, a.data.end());
	return arr;
}

// ruby::compact — remove empty string entries from array
void *ruby_compact(void *arr)
{
	MadArray &a = *(MadArray *)arr;
	std::vector<MadValue> cleaned;
	for ( auto &v : a.data )
	{
		if ( v.is_string() && v.as_string().empty() )
			continue;
		cleaned.push_back(v);
	}
	a.data = cleaned;
	return arr;
}

// ruby::flatten — flatten nested string (split by any whitespace into array)
void *ruby_flatten(void *arr, void *str)
{
	MadArray &a = *(MadArray *)arr;
	std::string &s = *(std::string *)str;
	a.data.clear();
	a.assoc.clear();
	std::string word;
	for ( size_t i = 0; i < s.length(); ++i )
	{
		if ( isspace(s[i]) )
		{
			if ( !word.empty() )
			{
				a.push(MadValue(word));
				word.clear();
			}
		}
		else
			word += s[i];
	}
	if ( !word.empty() )
		a.push(MadValue(word));
	return arr;
}

// ruby::capitalize — capitalize first char, lowercase rest (Ruby semantics)
void *ruby_capitalize(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	if ( !s.empty() )
	{
		s[0] = toupper(s[0]);
		for ( size_t i = 1; i < s.length(); ++i )
			s[i] = tolower(s[i]);
	}
	return ptr;
}

// ruby::delete_chars — remove all occurrences of specified characters
// "hello".delete("lo") -> "he"
void *ruby_delete_chars(void *ptr, void *chars)
{
	std::string &s = *(std::string *)ptr;
	std::string &c = *(std::string *)chars;
	std::string result;
	for ( size_t i = 0; i < s.length(); ++i )
		if ( c.find(s[i]) == std::string::npos )
			result += s[i];
	s = result;
	return ptr;
}

// ruby::count — count occurrences of any of the specified characters
int64_t ruby_count(void *str, void *chars)
{
	std::string &s = *(std::string *)str;
	std::string &c = *(std::string *)chars;
	int64_t count = 0;
	for ( size_t i = 0; i < s.length(); ++i )
		if ( c.find(s[i]) != std::string::npos )
			++count;
	return count;
}

// ruby::include — check if string includes substring (returns 1 or 0)
int64_t ruby_include(void *str, void *substr)
{
	return ns_common::contains(*(std::string *)str,
				   *(std::string *)substr) ? 1 : 0;
}

// ruby::gsub — global substitution (same as str_replace but Ruby naming)
void *ruby_gsub(void *ptr, void *pattern, void *replacement)
{
	ns_common::replace_all(*(std::string *)ptr,
			       *(std::string *)pattern,
			       *(std::string *)replacement);
	return ptr;
}

// ruby::sub — substitute first occurrence only
void *ruby_sub(void *ptr, void *pattern, void *replacement)
{
	ns_common::replace_first(*(std::string *)ptr,
				 *(std::string *)pattern,
				 *(std::string *)replacement);
	return ptr;
}

// ruby::freeze / frozen check — mark string as frozen (read-only)
// simplified: just returns the string, actual freeze enforcement would need compiler support


// ---- Namespace registration ----

void Program::add_ruby_namespace()
{
	variable_map_t &rb_ns = namespace_map["ruby"];
	Variable *var;

	// Ruby-unique string operations
	var = addFunction("__rb_squeeze",      datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)ruby_squeeze);
	if (var) rb_ns["squeeze"] = var;

	var = addFunction("__rb_tr",           datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)ruby_tr);
	if (var) rb_ns["tr"] = var;

	var = addFunction("__rb_chars",        datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtSTRING}, (fVOIDFUNC)ruby_chars);
	if (var) rb_ns["chars"] = var;

	var = addFunction("__rb_capitalize",   datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)ruby_capitalize);
	if (var) rb_ns["capitalize"] = var;

	var = addFunction("__rb_delete",       datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)ruby_delete_chars);
	if (var) rb_ns["delete"] = var;

	var = addFunction("__rb_count",        datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)ruby_count);
	if (var) rb_ns["count"] = var;

	var = addFunction("__rb_include",      datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)ruby_include);
	if (var) rb_ns["include"] = var;

	var = addFunction("__rb_gsub",         datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)ruby_gsub);
	if (var) rb_ns["gsub"] = var;

	var = addFunction("__rb_sub",          datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)ruby_sub);
	if (var) rb_ns["sub"] = var;

	// Ruby-unique array operations
	var = addFunction("__rb_rotate",       datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtINT64}, (fVOIDFUNC)ruby_rotate);
	if (var) rb_ns["rotate"] = var;

	var = addFunction("__rb_compact",      datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY}, (fVOIDFUNC)ruby_compact);
	if (var) rb_ns["compact"] = var;

	var = addFunction("__rb_flatten",      datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtSTRING}, (fVOIDFUNC)ruby_flatten);
	if (var) rb_ns["flatten"] = var;

	DBG(std::cout << "add_ruby_namespace() registered ruby:: with " << rb_ns.size() << " members" << std::endl);
}

extern "C" {
// Thin C-linkage wrappers for transpiler import resolution
void *__rb_squeeze(void *a) { return ruby_squeeze(a); }
void *__rb_tr(void *a, void *b, void *c) { return ruby_tr(a, b, c); }
void *__rb_chars(void *a, void *b) { return ruby_chars(a, b); }
void *__rb_capitalize(void *a) { return ruby_capitalize(a); }
void *__rb_delete(void *a, void *b) { return ruby_delete_chars(a, b); }
int64_t __rb_count(void *a, void *b) { return ruby_count(a, b); }
int64_t __rb_include(void *a, void *b) { return ruby_include(a, b); }
void *__rb_gsub(void *a, void *b, void *c) { return ruby_gsub(a, b, c); }
void *__rb_sub(void *a, void *b, void *c) { return ruby_sub(a, b, c); }
void *__rb_rotate(void *a, int64_t b) { return ruby_rotate(a, b); }
void *__rb_compact(void *a) { return ruby_compact(a); }
void *__rb_flatten(void *a, void *b) { return ruby_flatten(a, b); }
}
