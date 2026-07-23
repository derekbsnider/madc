///////////////////////////////////////////////////////////////////////////
//                                                                     //
// madc perl:: namespace                                               //
//                                                                     //
// Functions unique to Perl that have no direct C/C++ equivalent.      //
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
#include <glob.h>
#include <regex>
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "ns_common.h"

using namespace std;

static std::string perl_text_arg(const char *ptr)
{
	return std::string(ptr ? ptr : "");
}

// ---- C++ wrapper functions called by JIT ----

// perl::chop — remove last character from string, return it
// Perl: $removed = chop($str);
int64_t perl_chop(std::string *ptr)
{
	std::string &s = *ptr;
	if ( s.empty() ) return 0;
	char ch = s.back();
	s.pop_back();
	return (int64_t)ch;
}

// perl::chomp — remove trailing newline(s), return count removed
// Perl: $count = chomp($str);
int64_t perl_chomp(std::string *ptr)
{
	std::string &s = *ptr;
	int64_t count = 0;
	while ( !s.empty() && (s.back() == '\n' || s.back() == '\r') )
	{
		s.pop_back();
		++count;
	}
	return count;
}

// perl::grep — filter array, keeping elements that match regex pattern
// Perl: @matches = grep { /pattern/ } @array;
void perl_grep(madc::value *dest, const char *needle, madc::value *src)
{
	std::string n = perl_text_arg(needle);
	*dest = madc::value::make_array();
	if ( !src->is_array() )
		return;
	try {
		std::regex re(n);
		for ( auto &v : src->as_array() )
		{
			if ( v.is_string() && std::regex_search(v.as_string(), re) )
				dest->array().push_back(v);
		}
	} catch (std::regex_error &) {
		// fallback to substring match on invalid regex
		for ( auto &v : src->as_array() )
		{
			if ( v.is_string() && v.as_string().find(n) != std::string::npos )
				dest->array().push_back(v);
		}
	}
}

// perl::glob — file globbing, returns array of matching filenames
// Perl: @files = glob("*.txt");
void perl_glob(madc::value *arr, const char *pattern)
{
	std::string p = perl_text_arg(pattern);
	*arr = madc::value::make_array();

	glob_t globbuf;
	int ret = ::glob(p.c_str(), GLOB_NOSORT, NULL, &globbuf);
	if ( ret == 0 )
	{
		for ( size_t i = 0; i < globbuf.gl_pathc; ++i )
			arr->array().push_back(madc::value(std::string(globbuf.gl_pathv[i])));
		globfree(&globbuf);
	}
}

// perl::scalar — return count of elements in array (Perl's scalar @array)
int64_t perl_scalar(madc::value *arr)
{
	return (int64_t)ns_common::value_count(*arr);
}

// perl::push — append value to array (Perl: push @arr, $val)
void perl_push(madc::value *arr, const char *str)
{
	ns_common::value_array_for_write(*arr, "perl::push")
		.push_back(madc::value(perl_text_arg(str)));
}

// perl::pop — remove last element (Perl: $val = pop @arr)
std::string *perl_pop(std::string *result, madc::value *arr)
{
	std::string &res = *result;
	res.clear();
	if ( !arr->is_array() || arr->as_array().empty() )
		return result;
	madc::value v = std::move(arr->array().back());
	arr->array().pop_back();
	ns_common::value_to_string_no_real(v, res);
	return result;
}

// perl::shift — remove first element (Perl: $val = shift @arr)
std::string *perl_shift(std::string *result, madc::value *arr)
{
	std::string &res = *result;
	res.clear();
	if ( !arr->is_array() || arr->as_array().empty() )
		return result;
	madc::value v = std::move(arr->array().front());
	arr->array().erase(arr->array().begin());
	ns_common::value_to_string_no_real(v, res);
	return result;
}

// perl::unshift — prepend value (Perl: unshift @arr, $val)
void perl_unshift(madc::value *arr, const char *str)
{
	std::vector<madc::value> &data
		= ns_common::value_array_for_write(*arr, "perl::unshift");
	data.insert(data.begin(), madc::value(perl_text_arg(str)));
}

// perl::join — join array with separator (Perl: join(",", @arr))
std::string *perl_join(std::string *result, const char *sep, madc::value *arr)
{
	std::string &res = *result;
	std::string s = perl_text_arg(sep);
	res.clear();
	if ( !arr->is_array() )
		return result;
	const std::vector<madc::value> &data = arr->as_array();
	std::string tmp;
	for ( size_t i = 0; i < data.size(); ++i )
	{
		if ( i > 0 ) res += s;
		tmp.clear();
		if ( ns_common::value_to_string_no_real(data[i], tmp) )
			res += tmp;
	}
	return result;
}

// perl::split — split string by regex pattern into array (Perl: split(/pattern/, $str))
void perl_split(madc::value *arr, const char *delim, const char *str)
{
	std::string d = perl_text_arg(delim);
	std::string s = perl_text_arg(str);
	*arr = madc::value::make_array();
	if ( d.empty() ) { arr->array().push_back(madc::value(s)); return; }
	try {
		std::regex re(d);
		std::sregex_token_iterator it(s.begin(), s.end(), re, -1);
		std::sregex_token_iterator end;
		for ( ; it != end; ++it )
			arr->array().push_back(madc::value(it->str()));
	} catch (std::regex_error &) {
		// fallback to literal delimiter on invalid regex
		size_t start = 0, pos;
		while ( (pos = s.find(d, start)) != std::string::npos )
		{
			arr->array().push_back(madc::value(s.substr(start, pos - start)));
			start = pos + d.length();
		}
		arr->array().push_back(madc::value(s.substr(start)));
	}
}

// perl::reverse — reverse string in place (Perl: reverse $str)
std::string *perl_reverse(std::string *ptr)
{
	std::string &s = *ptr;
	std::reverse(s.begin(), s.end());
	return ptr;
}

// perl::lc — lowercase (Perl: lc $str)
std::string *perl_lc(std::string *ptr)
{
	std::string &s = *ptr;
	std::transform(s.begin(), s.end(), s.begin(), ::tolower);
	return ptr;
}

// perl::uc — uppercase (Perl: uc $str)
std::string *perl_uc(std::string *ptr)
{
	std::string &s = *ptr;
	std::transform(s.begin(), s.end(), s.begin(), ::toupper);
	return ptr;
}

// perl::ucfirst — capitalize first character (Perl: ucfirst $str)
std::string *perl_ucfirst(std::string *ptr)
{
	std::string &s = *ptr;
	if ( !s.empty() ) s[0] = toupper(s[0]);
	return ptr;
}

// perl::lcfirst — lowercase first character (Perl: lcfirst $str)
std::string *perl_lcfirst(std::string *ptr)
{
	std::string &s = *ptr;
	if ( !s.empty() ) s[0] = tolower(s[0]);
	return ptr;
}

// perl::index — find position of needle in haystack (Perl: index($str, $substr))
int64_t perl_index(const char *haystack, const char *needle)
{
	std::string h = perl_text_arg(haystack);
	std::string n = perl_text_arg(needle);
	size_t pos = h.find(n);
	return pos == std::string::npos ? -1 : (int64_t)pos;
}

// perl::rindex — find last position (Perl: rindex($str, $substr))
int64_t perl_rindex(const char *haystack, const char *needle)
{
	std::string h = perl_text_arg(haystack);
	std::string n = perl_text_arg(needle);
	size_t pos = h.rfind(n);
	return pos == std::string::npos ? -1 : (int64_t)pos;
}

// perl::length — string length (Perl: length $str)
int64_t perl_length(const char *ptr)
{
	return ptr ? (int64_t)strlen(ptr) : 0;
}

// perl::substr — extract/replace substring (Perl: substr($str, $offset, $length))
std::string *perl_substr(std::string *result, const char *str, int64_t offset, int64_t length)
{
	std::string s = perl_text_arg(str);
	std::string &res = *result;
	if ( offset < 0 ) offset = (int64_t)s.length() + offset;
	if ( offset < 0 ) offset = 0;
	if ( length < 0 ) length = (int64_t)s.length() + length - offset;
	if ( length < 0 ) length = 0;
	res = s.substr((size_t)offset, (size_t)length);
	return result;
}


// ---- Regex helper functions (used by madc:: namespace) ----

// madc::regex_match(string, pattern) — returns 1 if entire string matches pattern
int64_t madc_regex_match(void *str, void *pattern)
{
	std::string s = perl_text_arg((const char *)str);
	std::string p = perl_text_arg((const char *)pattern);
	try {
		return std::regex_match(s, std::regex(p)) ? 1 : 0;
	} catch (std::regex_error &) { return 0; }
}

// madc::regex_search(string, pattern) — returns 1 if pattern found anywhere in string
int64_t madc_regex_search(void *str, void *pattern)
{
	std::string s = perl_text_arg((const char *)str);
	std::string p = perl_text_arg((const char *)pattern);
	try {
		return std::regex_search(s, std::regex(p)) ? 1 : 0;
	} catch (std::regex_error &) { return 0; }
}

// madc::regex_replace(result, string, pattern, replacement) — regex replace, result = modified string
void *madc_regex_replace(void *result, void *str, void *pattern, void *replacement)
{
	std::string s = perl_text_arg((const char *)str);
	std::string p = perl_text_arg((const char *)pattern);
	std::string r = perl_text_arg((const char *)replacement);
	try {
		*(std::string *)result = std::regex_replace(s, std::regex(p), r);
	} catch (std::regex_error &) {
		*(std::string *)result = s;
	}
	return result;
}


extern "C" {
// Thin C-linkage wrappers for transpiler import resolution
int64_t __perl_chop(std::string *a) { return perl_chop(a); }
int64_t __perl_chomp(std::string *a) { return perl_chomp(a); }
void __perl_grep(madc::value *a, const char *b, madc::value *c) { perl_grep(a, b, c); }
void __perl_glob(madc::value *a, const char *b) { perl_glob(a, b); }
int64_t __perl_scalar(madc::value *a) { return perl_scalar(a); }
void __perl_push(madc::value *a, const char *b) { perl_push(a, b); }
std::string *__perl_pop(std::string *a, madc::value *b) { return perl_pop(a, b); }
std::string *__perl_shift(std::string *a, madc::value *b) { return perl_shift(a, b); }
void __perl_unshift(madc::value *a, const char *b) { perl_unshift(a, b); }
std::string *__perl_join(std::string *a, const char *b, madc::value *c) { return perl_join(a, b, c); }
void __perl_split(madc::value *a, const char *b, const char *c) { perl_split(a, b, c); }
std::string *__perl_reverse(std::string *a) { return perl_reverse(a); }
std::string *__perl_lc(std::string *a) { return perl_lc(a); }
std::string *__perl_uc(std::string *a) { return perl_uc(a); }
std::string *__perl_ucfirst(std::string *a) { return perl_ucfirst(a); }
std::string *__perl_lcfirst(std::string *a) { return perl_lcfirst(a); }
int64_t __perl_index(const char *a, const char *b) { return perl_index(a, b); }
int64_t __perl_rindex(const char *a, const char *b) { return perl_rindex(a, b); }
int64_t __perl_length(const char *a) { return perl_length(a); }
std::string *__perl_substr(std::string *a, const char *b, int64_t c, int64_t d) { return perl_substr(a, b, c, d); }
}
// madc:: namespace regex functions
MADC_EXTERN_C2(int64_t, madc_regex_match, void *, void *)
MADC_EXTERN_C2(int64_t, madc_regex_search, void *, void *)
MADC_EXTERN_C4(void *, madc_regex_replace, void *, void *, void *, void *)
