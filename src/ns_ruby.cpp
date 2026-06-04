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
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "ns_common.h"

using namespace std;

static std::string ruby_text_arg(const char *ptr)
{
	return std::string(ptr ? ptr : "");
}

// ---- C++ wrapper functions ----

// ruby::squeeze — collapse consecutive duplicate characters
// "aaabbbccc" -> "abc", "aabbcc" -> "abc"
std::string *ruby_squeeze(std::string *ptr)
{
	std::string &s = *ptr;
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
std::string *ruby_tr(std::string *ptr, const char *from, const char *to)
{
	std::string &s = *ptr;
	std::string f = ruby_text_arg(from);
	std::string t = ruby_text_arg(to);
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
void ruby_chars(MadArray *arr, const char *str)
{
	MadArray &a = *arr;
	std::string s = ruby_text_arg(str);
	a.data.clear();
	a.assoc.clear();
	for ( size_t i = 0; i < s.length(); ++i )
		a.push(MadValue(std::string(1, s[i])));
}

// ruby::rotate — rotate array elements by n positions
// [1,2,3,4,5].rotate(2) -> [3,4,5,1,2]
void ruby_rotate(MadArray *arr, int64_t n)
{
	MadArray &a = *arr;
	if ( a.data.empty() ) return;
	int64_t sz = (int64_t)a.data.size();
	n = ((n % sz) + sz) % sz; // handle negative rotation
	std::rotate(a.data.begin(), a.data.begin() + n, a.data.end());
}

// ruby::compact — remove empty string entries from array
void ruby_compact(MadArray *arr)
{
	MadArray &a = *arr;
	std::vector<MadValue> cleaned;
	for ( auto &v : a.data )
	{
		if ( v.is_string() && v.as_string().empty() )
			continue;
		cleaned.push_back(v);
	}
	a.data = cleaned;
}

// ruby::flatten — flatten nested string (split by any whitespace into array)
void ruby_flatten(MadArray *arr, const char *str)
{
	MadArray &a = *arr;
	std::string s = ruby_text_arg(str);
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
}

// ruby::capitalize — capitalize first char, lowercase rest (Ruby semantics)
std::string *ruby_capitalize(std::string *ptr)
{
	std::string &s = *ptr;
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
std::string *ruby_delete_chars(std::string *ptr, const char *chars)
{
	std::string &s = *ptr;
	std::string c = ruby_text_arg(chars);
	std::string result;
	for ( size_t i = 0; i < s.length(); ++i )
		if ( c.find(s[i]) == std::string::npos )
			result += s[i];
	s = result;
	return ptr;
}

// ruby::count — count occurrences of any of the specified characters
int64_t ruby_count(const char *str, const char *chars)
{
	std::string s = ruby_text_arg(str);
	std::string c = ruby_text_arg(chars);
	int64_t count = 0;
	for ( size_t i = 0; i < s.length(); ++i )
		if ( c.find(s[i]) != std::string::npos )
			++count;
	return count;
}

// ruby::include — check if string includes substring (returns 1 or 0)
int64_t ruby_include(const char *str, const char *substr)
{
	std::string s = ruby_text_arg(str);
	std::string sub = ruby_text_arg(substr);
	return ns_common::contains(s, sub) ? 1 : 0;
}

// ruby::gsub — global substitution (same as str_replace but Ruby naming)
std::string *ruby_gsub(std::string *ptr, const char *pattern, const char *replacement)
{
	std::string p = ruby_text_arg(pattern);
	std::string r = ruby_text_arg(replacement);
	ns_common::replace_all(*ptr, p, r);
	return ptr;
}

// ruby::sub — substitute first occurrence only
std::string *ruby_sub(std::string *ptr, const char *pattern, const char *replacement)
{
	std::string p = ruby_text_arg(pattern);
	std::string r = ruby_text_arg(replacement);
	ns_common::replace_first(*ptr, p, r);
	return ptr;
}

// ruby::freeze / frozen check — mark string as frozen (read-only)
// simplified: just returns the string, actual freeze enforcement would need compiler support


extern "C" {
// Thin C-linkage wrappers for transpiler import resolution
std::string *__rb_squeeze(std::string *a) { return ruby_squeeze(a); }
std::string *__rb_tr(std::string *a, const char *b, const char *c) { return ruby_tr(a, b, c); }
void __rb_chars(MadArray *a, const char *b) { ruby_chars(a, b); }
std::string *__rb_capitalize(std::string *a) { return ruby_capitalize(a); }
std::string *__rb_delete(std::string *a, const char *b) { return ruby_delete_chars(a, b); }
int64_t __rb_count(const char *a, const char *b) { return ruby_count(a, b); }
int64_t __rb_include(const char *a, const char *b) { return ruby_include(a, b); }
std::string *__rb_gsub(std::string *a, const char *b, const char *c) { return ruby_gsub(a, b, c); }
std::string *__rb_sub(std::string *a, const char *b, const char *c) { return ruby_sub(a, b, c); }
void __rb_rotate(MadArray *a, int64_t b) { ruby_rotate(a, b); }
void __rb_compact(MadArray *a) { ruby_compact(a); }
void __rb_flatten(MadArray *a, const char *b) { ruby_flatten(a, b); }
}
