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

// ---- C++ wrapper functions ----

// python::title — title case ("hello world" -> "Hello World")
void *python_title(void *ptr)
{
	std::string &s = *(std::string *)ptr;
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
void *python_swapcase(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	for ( size_t i = 0; i < s.length(); ++i )
	{
		if ( isupper(s[i]) ) s[i] = tolower(s[i]);
		else if ( islower(s[i]) ) s[i] = toupper(s[i]);
	}
	return ptr;
}

// python::center — center string with fill char ("hi", 10, "-") -> "----hi----"
void *python_center(void *ptr, int64_t width, void *fillchar)
{
	std::string &s = *(std::string *)ptr;
	std::string &fill = *(std::string *)fillchar;
	char fc = fill.empty() ? ' ' : fill[0];
	if ( (int64_t)s.length() >= width ) return ptr;
	int64_t pad = width - (int64_t)s.length();
	int64_t left = pad / 2;
	int64_t right = pad - left;
	s = std::string((size_t)left, fc) + s + std::string((size_t)right, fc);
	return ptr;
}

// python::ljust — left-justify, pad right ("hi", 10, ".") -> "hi........"
void *python_ljust(void *ptr, int64_t width, void *fillchar)
{
	std::string &s = *(std::string *)ptr;
	std::string &fill = *(std::string *)fillchar;
	char fc = fill.empty() ? ' ' : fill[0];
	if ( (int64_t)s.length() >= width ) return ptr;
	s += std::string((size_t)(width - (int64_t)s.length()), fc);
	return ptr;
}

// python::rjust — right-justify, pad left ("hi", 10, ".") -> "........hi"
void *python_rjust(void *ptr, int64_t width, void *fillchar)
{
	std::string &s = *(std::string *)ptr;
	std::string &fill = *(std::string *)fillchar;
	char fc = fill.empty() ? ' ' : fill[0];
	if ( (int64_t)s.length() >= width ) return ptr;
	s = std::string((size_t)(width - (int64_t)s.length()), fc) + s;
	return ptr;
}

// python::zfill — zero-pad numeric string ("42", 8) -> "00000042"
void *python_zfill(void *ptr, int64_t width)
{
	std::string &s = *(std::string *)ptr;
	if ( (int64_t)s.length() >= width ) return ptr;
	bool negative = !s.empty() && s[0] == '-';
	std::string digits = negative ? s.substr(1) : s;
	size_t zeros = (size_t)width - s.length();
	s = (negative ? "-" : "") + std::string(zeros, '0') + digits;
	return ptr;
}

// python::count — count non-overlapping occurrences of substring
int64_t python_count(void *haystack, void *needle)
{
	std::string &h = *(std::string *)haystack;
	std::string &n = *(std::string *)needle;
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
int64_t python_startswith(void *str, void *prefix)
{
	return ns_common::starts_with(*(std::string *)str,
				      *(std::string *)prefix) ? 1 : 0;
}

// python::endswith
int64_t python_endswith(void *str, void *suffix)
{
	return ns_common::ends_with(*(std::string *)str,
				    *(std::string *)suffix) ? 1 : 0;
}

// python::isdigit — check if all characters are digits
int64_t python_isdigit(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	if ( s.empty() ) return 0;
	for ( char c : s )
		if ( !isdigit(c) ) return 0;
	return 1;
}

// python::isalpha — check if all characters are alphabetic
int64_t python_isalpha(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	if ( s.empty() ) return 0;
	for ( char c : s )
		if ( !isalpha(c) ) return 0;
	return 1;
}

// python::isalnum — check if all characters are alphanumeric
int64_t python_isalnum(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	if ( s.empty() ) return 0;
	for ( char c : s )
		if ( !isalnum(c) ) return 0;
	return 1;
}

// python::isspace — check if all characters are whitespace
int64_t python_isspace(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	if ( s.empty() ) return 0;
	for ( char c : s )
		if ( !isspace(c) ) return 0;
	return 1;
}

// python::replace — like str_replace but Python naming (returns modified string)
void *python_replace(void *ptr, void *old_str, void *new_str)
{
	ns_common::replace_all(*(std::string *)ptr,
			       *(std::string *)old_str,
			       *(std::string *)new_str);
	return ptr;
}

// python::format — simple Python-style string formatting
// "Hello {}, you are {}" with positional args from array
void *python_format(void *result, void *fmt, void *args)
{
	std::string &res = *(std::string *)result;
	std::string &f = *(std::string *)fmt;
	MadArray &a = *(MadArray *)args;
	res = f;
	size_t arg_idx = 0;
	size_t pos = 0;
	while ( (pos = res.find("{}", pos)) != std::string::npos && arg_idx < a.data.size() )
	{
		std::string val;
		if ( a.data[arg_idx].is_string() )
			val = a.data[arg_idx].as_string();
		else if ( a.data[arg_idx].is_int() )
			val = std::to_string(a.data[arg_idx].as_int());
		else if ( a.data[arg_idx].is_double() )
			val = std::to_string(a.data[arg_idx].as_double());
		res.replace(pos, 2, val);
		pos += val.length();
		++arg_idx;
	}
	return result;
}


// ---- Namespace registration ----

void Program::add_python_namespace()
{
	variable_map_t &py_ns = namespace_map["python"];
	Variable *var;

	// case transforms
	var = addFunction("__py_title",       datatype_vec_t{&ddSTRING, &ddSTRING}, (fVOIDFUNC)python_title);
	if (var) py_ns["title"] = var;

	var = addFunction("__py_swapcase",    datatype_vec_t{&ddSTRING, &ddSTRING}, (fVOIDFUNC)python_swapcase);
	if (var) py_ns["swapcase"] = var;

	// alignment
	var = addFunction("__py_center",      datatype_vec_t{&ddSTRING, &ddSTRING, DataType::dtINT64, &ddSTRING}, (fVOIDFUNC)python_center);
	if (var) py_ns["center"] = var;

	var = addFunction("__py_ljust",       datatype_vec_t{&ddSTRING, &ddSTRING, DataType::dtINT64, &ddSTRING}, (fVOIDFUNC)python_ljust);
	if (var) py_ns["ljust"] = var;

	var = addFunction("__py_rjust",       datatype_vec_t{&ddSTRING, &ddSTRING, DataType::dtINT64, &ddSTRING}, (fVOIDFUNC)python_rjust);
	if (var) py_ns["rjust"] = var;

	var = addFunction("__py_zfill",       datatype_vec_t{&ddSTRING, &ddSTRING, DataType::dtINT64}, (fVOIDFUNC)python_zfill);
	if (var) py_ns["zfill"] = var;

	// counting / searching
	var = addFunction("__py_count",       datatype_vec_t{DataType::dtINT64, &ddSTRING, &ddSTRING}, (fVOIDFUNC)python_count);
	if (var) py_ns["count"] = var;

	var = addFunction("__py_startswith",  datatype_vec_t{DataType::dtINT64, &ddSTRING, &ddSTRING}, (fVOIDFUNC)python_startswith);
	if (var) py_ns["startswith"] = var;

	var = addFunction("__py_endswith",    datatype_vec_t{DataType::dtINT64, &ddSTRING, &ddSTRING}, (fVOIDFUNC)python_endswith);
	if (var) py_ns["endswith"] = var;

	// character class tests
	var = addFunction("__py_isdigit",     datatype_vec_t{DataType::dtINT64, &ddSTRING}, (fVOIDFUNC)python_isdigit);
	if (var) py_ns["isdigit"] = var;

	var = addFunction("__py_isalpha",     datatype_vec_t{DataType::dtINT64, &ddSTRING}, (fVOIDFUNC)python_isalpha);
	if (var) py_ns["isalpha"] = var;

	var = addFunction("__py_isalnum",     datatype_vec_t{DataType::dtINT64, &ddSTRING}, (fVOIDFUNC)python_isalnum);
	if (var) py_ns["isalnum"] = var;

	var = addFunction("__py_isspace",     datatype_vec_t{DataType::dtINT64, &ddSTRING}, (fVOIDFUNC)python_isspace);
	if (var) py_ns["isspace"] = var;

	// string manipulation
	var = addFunction("__py_replace",     datatype_vec_t{&ddSTRING, &ddSTRING, &ddSTRING, &ddSTRING}, (fVOIDFUNC)python_replace);
	if (var) py_ns["replace"] = var;

	// formatting
	var = addFunction("__py_format",      datatype_vec_t{&ddSTRING, &ddSTRING, &ddSTRING, DataType::dtARRAY}, (fVOIDFUNC)python_format);
	if (var) py_ns["format"] = var;

	DBG(std::cout << "add_python_namespace() registered python:: with " << py_ns.size() << " members" << std::endl);
}

extern "C" {
// Thin C-linkage wrappers for transpiler import resolution
void *__py_title(void *a) { return python_title(a); }
void *__py_swapcase(void *a) { return python_swapcase(a); }
void *__py_center(void *a, int64_t b, void *c) { return python_center(a, b, c); }
void *__py_ljust(void *a, int64_t b, void *c) { return python_ljust(a, b, c); }
void *__py_rjust(void *a, int64_t b, void *c) { return python_rjust(a, b, c); }
void *__py_zfill(void *a, int64_t b) { return python_zfill(a, b); }
int64_t __py_count(void *a, void *b) { return python_count(a, b); }
int64_t __py_startswith(void *a, void *b) { return python_startswith(a, b); }
int64_t __py_endswith(void *a, void *b) { return python_endswith(a, b); }
int64_t __py_isdigit(void *a) { return python_isdigit(a); }
int64_t __py_isalpha(void *a) { return python_isalpha(a); }
int64_t __py_isalnum(void *a) { return python_isalnum(a); }
int64_t __py_isspace(void *a) { return python_isspace(a); }
void *__py_replace(void *a, void *b, void *c) { return python_replace(a, b, c); }
void *__py_format(void *a, void *b, void *c) { return python_format(a, b, c); }
}
