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
void ruby_chars(madc::value *arr, const char *str)
{
	std::string s = ruby_text_arg(str);
	std::vector<madc::value> &data
		= ns_common::value_array_reset_for_write(*arr, "ruby::chars");
	for ( size_t i = 0; i < s.length(); ++i )
		data.push_back(madc::value(std::string(1, s[i])));
}

// ruby::rotate — rotate array elements by n positions
// [1,2,3,4,5].rotate(2) -> [3,4,5,1,2]
void ruby_rotate(madc::value *arr, int64_t n)
{
	std::vector<madc::value> &data
		= ns_common::value_array_for_write(*arr, "ruby::rotate");
	if ( data.empty() ) return;
	int64_t sz = (int64_t)data.size();
	n = ((n % sz) + sz) % sz; // handle negative rotation
	std::rotate(data.begin(), data.begin() + n, data.end());
}

// ruby::compact — remove empty string entries from array
void ruby_compact(madc::value *arr)
{
	std::vector<madc::value> &data
		= ns_common::value_array_for_write(*arr, "ruby::compact");
	std::vector<madc::value> cleaned;
	for ( auto &v : data )
	{
		if ( v.is_string() && v.as_string().empty() )
			continue;
		cleaned.push_back(v);
	}
	data = std::move(cleaned);
}

// ruby::flatten — flatten nested string (split by any whitespace into array)
void ruby_flatten(madc::value *arr, const char *str)
{
	std::string s = ruby_text_arg(str);
	std::vector<madc::value> &data
		= ns_common::value_array_reset_for_write(*arr, "ruby::flatten");
	std::string word;
	for ( size_t i = 0; i < s.length(); ++i )
	{
		if ( isspace(s[i]) )
		{
			if ( !word.empty() )
			{
				data.push_back(madc::value(word));
				word.clear();
			}
		}
		else
			word += s[i];
	}
	if ( !word.empty() )
		data.push_back(madc::value(word));
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

// ---- Lean primaries (Leg 0b, dialect-lean.md) --------------------------
// Ruby-parity NON-MUTATING forms: these are the non-bang methods, which
// return a NEW string in Ruby — ring-lifetime text (the c_str contract)
// over the SAME in-place cores the guarded std::string publics use.

using ns_common::ring_apply;

const char *ruby_squeeze_cstr(const char *s)	{ return ring_apply(s, ruby_squeeze); }
const char *ruby_squeeze_value(const madc::value *v)	{ return ring_apply(v, ruby_squeeze); }
const char *ruby_capitalize_cstr(const char *s)	{ return ring_apply(s, ruby_capitalize); }
const char *ruby_capitalize_value(const madc::value *v)	{ return ring_apply(v, ruby_capitalize); }

const char *ruby_tr_cstr(const char *s, const char *from, const char *to)
{
	std::string &slot = ns_common::ring_slot();
	slot = s ? s : "";
	ruby_tr(&slot, from, to);
	return slot.c_str();
}
const char *ruby_tr_value(const madc::value *v, const char *from,
			  const char *to)
{
	std::string &slot = ns_common::value_text_slot(v);
	ruby_tr(&slot, from, to);
	return slot.c_str();
}

const char *ruby_delete_cstr(const char *s, const char *chars)
{
	std::string &slot = ns_common::ring_slot();
	slot = s ? s : "";
	ruby_delete_chars(&slot, chars);
	return slot.c_str();
}
const char *ruby_delete_value(const madc::value *v, const char *chars)
{
	std::string &slot = ns_common::value_text_slot(v);
	ruby_delete_chars(&slot, chars);
	return slot.c_str();
}

const char *ruby_gsub_cstr(const char *s, const char *pattern,
			   const char *replacement)
{
	std::string &slot = ns_common::ring_slot();
	slot = s ? s : "";
	ruby_gsub(&slot, pattern, replacement);
	return slot.c_str();
}
const char *ruby_gsub_value(const madc::value *v, const char *pattern,
			    const char *replacement)
{
	std::string &slot = ns_common::value_text_slot(v);
	ruby_gsub(&slot, pattern, replacement);
	return slot.c_str();
}

const char *ruby_sub_cstr(const char *s, const char *pattern,
			  const char *replacement)
{
	std::string &slot = ns_common::ring_slot();
	slot = s ? s : "";
	ruby_sub(&slot, pattern, replacement);
	return slot.c_str();
}
const char *ruby_sub_value(const madc::value *v, const char *pattern,
			   const char *replacement)
{
	std::string &slot = ns_common::value_text_slot(v);
	ruby_sub(&slot, pattern, replacement);
	return slot.c_str();
}

// ruby::freeze / frozen check — mark string as frozen (read-only)
// simplified: just returns the string, actual freeze enforcement would need compiler support


extern "C" {
// Thin C-linkage wrappers for transpiler import resolution
const char *__rb_squeeze_cstr(const char *a) { return ruby_squeeze_cstr(a); }
const char *__rb_squeeze_value(madc::value *a) { return ruby_squeeze_value(a); }
const char *__rb_capitalize_cstr(const char *a) { return ruby_capitalize_cstr(a); }
const char *__rb_capitalize_value(madc::value *a) { return ruby_capitalize_value(a); }
const char *__rb_tr_cstr(const char *a, const char *b, const char *c) { return ruby_tr_cstr(a, b, c); }
const char *__rb_tr_value(madc::value *a, const char *b, const char *c) { return ruby_tr_value(a, b, c); }
const char *__rb_delete_cstr(const char *a, const char *b) { return ruby_delete_cstr(a, b); }
const char *__rb_delete_value(madc::value *a, const char *b) { return ruby_delete_value(a, b); }
const char *__rb_gsub_cstr(const char *a, const char *b, const char *c) { return ruby_gsub_cstr(a, b, c); }
const char *__rb_gsub_value(madc::value *a, const char *b, const char *c) { return ruby_gsub_value(a, b, c); }
const char *__rb_sub_cstr(const char *a, const char *b, const char *c) { return ruby_sub_cstr(a, b, c); }
const char *__rb_sub_value(madc::value *a, const char *b, const char *c) { return ruby_sub_value(a, b, c); }
std::string *__rb_squeeze(std::string *a) { return ruby_squeeze(a); }
std::string *__rb_tr(std::string *a, const char *b, const char *c) { return ruby_tr(a, b, c); }
void __rb_chars(madc::value *a, const char *b) { ruby_chars(a, b); }
std::string *__rb_capitalize(std::string *a) { return ruby_capitalize(a); }
std::string *__rb_delete(std::string *a, const char *b) { return ruby_delete_chars(a, b); }
int64_t __rb_count(const char *a, const char *b) { return ruby_count(a, b); }
int64_t __rb_include(const char *a, const char *b) { return ruby_include(a, b); }
std::string *__rb_gsub(std::string *a, const char *b, const char *c) { return ruby_gsub(a, b, c); }
std::string *__rb_sub(std::string *a, const char *b, const char *c) { return ruby_sub(a, b, c); }
void __rb_rotate(madc::value *a, int64_t b) { ruby_rotate(a, b); }
void __rb_compact(madc::value *a) { ruby_compact(a); }
void __rb_flatten(madc::value *a, const char *b) { ruby_flatten(a, b); }
}
