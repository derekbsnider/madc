///////////////////////////////////////////////////////////////////////////
//                                                                     //
// madc rust:: namespace                                               //
//                                                                     //
// Rust-inspired string and array helpers built on madc's existing     //
// runtime types. This is intentionally namespace sugar, not Rust      //
// ownership / borrowing semantics.                                    //
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

static std::string rust_text_arg(const char *ptr)
{
	return std::string(ptr ? ptr : "");
}

int64_t rust_contains(const char *str, const char *needle)
{
	std::string s = rust_text_arg(str);
	std::string n = rust_text_arg(needle);
	return ns_common::contains(s, n) ? 1 : 0;
}

int64_t rust_starts_with(const char *str, const char *prefix)
{
	std::string s = rust_text_arg(str);
	std::string p = rust_text_arg(prefix);
	return ns_common::starts_with(s, p) ? 1 : 0;
}

int64_t rust_ends_with(const char *str, const char *suffix)
{
	std::string s = rust_text_arg(str);
	std::string p = rust_text_arg(suffix);
	return ns_common::ends_with(s, p) ? 1 : 0;
}

std::string *rust_trim(std::string *ptr)
{
	ns_common::trim(*ptr, true, true);
	return ptr;
}

std::string *rust_trim_start(std::string *ptr)
{
	ns_common::trim(*ptr, true, false);
	return ptr;
}

std::string *rust_trim_end(std::string *ptr)
{
	ns_common::trim(*ptr, false, true);
	return ptr;
}

std::string *rust_replace(std::string *ptr, const char *from, const char *to)
{
	std::string f = rust_text_arg(from);
	std::string t = rust_text_arg(to);
	ns_common::replace_all(*ptr, f, t);
	return ptr;
}

std::string *rust_repeat(std::string *ptr, int64_t count)
{
	ns_common::repeat(*ptr, count);
	return ptr;
}

int64_t rust_len(const char *ptr)
{
	return ptr ? (int64_t)strlen(ptr) : 0;
}

int64_t rust_is_empty(const char *ptr)
{
	return (!ptr || !ptr[0]) ? 1 : 0;
}

void rust_split(madc::value *arr, const char *str, const char *delim)
{
	std::string s = rust_text_arg(str);
	std::string d = rust_text_arg(delim);
	ns_common::split_by_delim(*arr, s, d, "rust::split");
}

void rust_split_whitespace(madc::value *arr, const char *str)
{
	std::string s = rust_text_arg(str);
	std::vector<madc::value> &data
		= ns_common::value_array_reset_for_write(*arr, "rust::split_whitespace");
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

std::string *rust_join(std::string *result, madc::value *arr, const char *sep)
{
	std::string s = rust_text_arg(sep);
	ns_common::join_with_sep(*result, *arr, s);
	return result;
}

std::string *rust_first(std::string *result, madc::value *arr)
{
	std::string &res = *result;
	res.clear();
	if ( arr->is_array() && !arr->as_array().empty() )
		ns_common::value_to_string(arr->as_array()[0], res);
	return result;
}

std::string *rust_last(std::string *result, madc::value *arr)
{
	std::string &res = *result;
	res.clear();
	if ( arr->is_array() && !arr->as_array().empty() )
		ns_common::value_to_string(arr->as_array().back(), res);
	return result;
}

std::string *rust_get(std::string *result, madc::value *arr, int64_t idx)
{
	std::string &res = *result;
	res.clear();
	if ( arr->is_array() && idx >= 0 && (size_t)idx < arr->as_array().size() )
		ns_common::value_to_string(arr->as_array()[(size_t)idx], res);
	return result;
}

void rust_push(madc::value *arr, const char *value)
{
	ns_common::value_array_for_write(*arr, "rust::push")
		.push_back(madc::value(rust_text_arg(value)));
}

std::string *rust_pop(std::string *result, madc::value *arr)
{
	std::string &res = *result;
	res.clear();
	madc::value v;
	if ( ns_common::value_pop_element(*arr, v, "rust::pop") )
		ns_common::value_to_string(v, res);
	return result;
}

extern "C" {
// Thin C-linkage wrappers for transpiler import resolution
int64_t __rust_contains(const char *a, const char *b) { return rust_contains(a, b); }
int64_t __rust_starts_with(const char *a, const char *b) { return rust_starts_with(a, b); }
int64_t __rust_ends_with(const char *a, const char *b) { return rust_ends_with(a, b); }
std::string *__rust_trim(std::string *a) { return rust_trim(a); }
std::string *__rust_trim_start(std::string *a) { return rust_trim_start(a); }
std::string *__rust_trim_end(std::string *a) { return rust_trim_end(a); }
std::string *__rust_replace(std::string *a, const char *b, const char *c) { return rust_replace(a, b, c); }
std::string *__rust_repeat(std::string *a, int64_t b) { return rust_repeat(a, b); }
int64_t __rust_len(const char *a) { return rust_len(a); }
int64_t __rust_is_empty(const char *a) { return rust_is_empty(a); }
void __rust_split(madc::value *a, const char *b, const char *c) { rust_split(a, b, c); }
void __rust_split_whitespace(madc::value *a, const char *b) { rust_split_whitespace(a, b); }
std::string *__rust_join(std::string *a, madc::value *b, const char *c) { return rust_join(a, b, c); }
std::string *__rust_first(std::string *a, madc::value *b) { return rust_first(a, b); }
std::string *__rust_last(std::string *a, madc::value *b) { return rust_last(a, b); }
std::string *__rust_get(std::string *a, madc::value *b, int64_t c) { return rust_get(a, b, c); }
void __rust_push(madc::value *a, const char *b) { rust_push(a, b); }
std::string *__rust_pop(std::string *a, madc::value *b) { return rust_pop(a, b); }
}
