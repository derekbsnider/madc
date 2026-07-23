///////////////////////////////////////////////////////////////////////////
//                                                                     //
// madc python:: namespace                                             //
//                                                                     //
// Python-unique string operations: formatting, alignment, case        //
// transforms, and substring counting.                                 //
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

static std::string python_text_arg(const char *ptr)
{
	return std::string(ptr ? ptr : "");
}

// ---- C++ wrapper functions ----

// python::title — title case ("hello world" -> "Hello World")
std::string *python_title(std::string *ptr)
{
	std::string &s = *ptr;
	bool cap_next = true;
	for ( size_t i = 0; i < s.length(); ++i )
	{
		if ( isspace(s[i]) )
			cap_next = true;
		else if ( cap_next )
		{
			s[i] = toupper(s[i]);
			cap_next = false;
		}
		else
			s[i] = tolower(s[i]);
	}
	return ptr;
}

// python::swapcase — swap upper/lower ("Hello" -> "hELLO")
std::string *python_swapcase(std::string *ptr)
{
	std::string &s = *ptr;
	for ( size_t i = 0; i < s.length(); ++i )
	{
		if ( isupper(s[i]) ) s[i] = tolower(s[i]);
		else if ( islower(s[i]) ) s[i] = toupper(s[i]);
	}
	return ptr;
}

// python::center — center string with fill char ("hi", 10, "-") -> "----hi----"
std::string *python_center(std::string *ptr, int64_t width, const char *fillchar)
{
	std::string &s = *ptr;
	std::string fill = python_text_arg(fillchar);
	char fc = fill.empty() ? ' ' : fill[0];
	if ( (int64_t)s.length() >= width ) return ptr;
	int64_t pad = width - (int64_t)s.length();
	int64_t left = pad / 2;
	int64_t right = pad - left;
	s = std::string((size_t)left, fc) + s + std::string((size_t)right, fc);
	return ptr;
}

// python::ljust — left-justify, pad right ("hi", 10, ".") -> "hi........"
std::string *python_ljust(std::string *ptr, int64_t width, const char *fillchar)
{
	std::string &s = *ptr;
	std::string fill = python_text_arg(fillchar);
	char fc = fill.empty() ? ' ' : fill[0];
	if ( (int64_t)s.length() >= width ) return ptr;
	s += std::string((size_t)(width - (int64_t)s.length()), fc);
	return ptr;
}

// python::rjust — right-justify, pad left ("hi", 10, ".") -> "........hi"
std::string *python_rjust(std::string *ptr, int64_t width, const char *fillchar)
{
	std::string &s = *ptr;
	std::string fill = python_text_arg(fillchar);
	char fc = fill.empty() ? ' ' : fill[0];
	if ( (int64_t)s.length() >= width ) return ptr;
	s = std::string((size_t)(width - (int64_t)s.length()), fc) + s;
	return ptr;
}

// python::zfill — zero-pad numeric string ("42", 8) -> "00000042"
std::string *python_zfill(std::string *ptr, int64_t width)
{
	std::string &s = *ptr;
	if ( (int64_t)s.length() >= width ) return ptr;
	bool negative = !s.empty() && s[0] == '-';
	std::string digits = negative ? s.substr(1) : s;
	size_t zeros = (size_t)width - s.length();
	s = (negative ? "-" : "") + std::string(zeros, '0') + digits;
	return ptr;
}

// python::count — count non-overlapping occurrences of substring
int64_t python_count(const char *haystack, const char *needle)
{
	std::string h = python_text_arg(haystack);
	std::string n = python_text_arg(needle);
	if ( n.empty() ) return (int64_t)h.length() + 1;
	int64_t count = 0;
	size_t pos = 0;
	while ( (pos = h.find(n, pos)) != std::string::npos )
	{
		++count;
		pos += n.length();
	}
	return count;
}

// python::startswith
int64_t python_startswith(const char *str, const char *prefix)
{
	std::string s = python_text_arg(str);
	std::string p = python_text_arg(prefix);
	return ns_common::starts_with(s, p) ? 1 : 0;
}

// python::endswith
int64_t python_endswith(const char *str, const char *suffix)
{
	std::string s = python_text_arg(str);
	std::string p = python_text_arg(suffix);
	return ns_common::ends_with(s, p) ? 1 : 0;
}

// python::isdigit — check if all characters are digits
int64_t python_isdigit(const char *ptr)
{
	std::string s = python_text_arg(ptr);
	if ( s.empty() ) return 0;
	for ( char c : s )
		if ( !isdigit(c) ) return 0;
	return 1;
}

// python::isalpha — check if all characters are alphabetic
int64_t python_isalpha(const char *ptr)
{
	std::string s = python_text_arg(ptr);
	if ( s.empty() ) return 0;
	for ( char c : s )
		if ( !isalpha(c) ) return 0;
	return 1;
}

// python::isalnum — check if all characters are alphanumeric
int64_t python_isalnum(const char *ptr)
{
	std::string s = python_text_arg(ptr);
	if ( s.empty() ) return 0;
	for ( char c : s )
		if ( !isalnum(c) ) return 0;
	return 1;
}

// python::isspace — check if all characters are whitespace
int64_t python_isspace(const char *ptr)
{
	std::string s = python_text_arg(ptr);
	if ( s.empty() ) return 0;
	for ( char c : s )
		if ( !isspace(c) ) return 0;
	return 1;
}

// python::replace — like str_replace but Python naming (returns modified string)
std::string *python_replace(std::string *ptr, const char *old_str, const char *new_str)
{
	std::string old_text = python_text_arg(old_str);
	std::string new_text = python_text_arg(new_str);
	ns_common::replace_all(*ptr, old_text, new_text);
	return ptr;
}

// python::format — simple Python-style string formatting
// "Hello {}, you are {}" with positional args from array
std::string *python_format(std::string *result, const char *fmt, madc::value *args)
{
	std::string &res = *result;
	std::string f = python_text_arg(fmt);
	res = f;
	if ( !args->is_array() )
		return result;
	const std::vector<madc::value> &data = args->as_array();
	size_t arg_idx = 0;
	size_t pos = 0;
	while ( (pos = res.find("{}", pos)) != std::string::npos && arg_idx < data.size() )
	{
		std::string val;
		ns_common::value_to_string(data[arg_idx], val);
		res.replace(pos, 2, val);
		pos += val.length();
		++arg_idx;
	}
	return result;
}


extern "C" {
// Thin C-linkage wrappers for transpiler import resolution
std::string *__py_title(std::string *a) { return python_title(a); }
std::string *__py_swapcase(std::string *a) { return python_swapcase(a); }
std::string *__py_center(std::string *a, int64_t b, const char *c) { return python_center(a, b, c); }
std::string *__py_ljust(std::string *a, int64_t b, const char *c) { return python_ljust(a, b, c); }
std::string *__py_rjust(std::string *a, int64_t b, const char *c) { return python_rjust(a, b, c); }
std::string *__py_zfill(std::string *a, int64_t b) { return python_zfill(a, b); }
int64_t __py_count(const char *a, const char *b) { return python_count(a, b); }
int64_t __py_startswith(const char *a, const char *b) { return python_startswith(a, b); }
int64_t __py_endswith(const char *a, const char *b) { return python_endswith(a, b); }
int64_t __py_isdigit(const char *a) { return python_isdigit(a); }
int64_t __py_isalpha(const char *a) { return python_isalpha(a); }
int64_t __py_isalnum(const char *a) { return python_isalnum(a); }
int64_t __py_isspace(const char *a) { return python_isspace(a); }
std::string *__py_replace(std::string *a, const char *b, const char *c) { return python_replace(a, b, c); }
std::string *__py_format(std::string *a, const char *b, madc::value *c) { return python_format(a, b, c); }
}
