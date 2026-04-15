///////////////////////////////////////////////////////////////////////////
//                                                                     //
// madc php:: namespace — PHP-style string functions                   //
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

using namespace std;
using namespace asmjit;

// ---- C++ wrapper functions called by JIT ----

// php::strlen — return length of string
int64_t php_strlen(void *ptr)
{
	return (int64_t)((std::string *)ptr)->length();
}

// php::trim — trim whitespace from both ends, modifies in place
void *php_trim(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	size_t start = s.find_first_not_of(" \t\n\r\f\v");
	size_t end = s.find_last_not_of(" \t\n\r\f\v");
	if ( start == std::string::npos )
		s.clear();
	else
		s = s.substr(start, end - start + 1);
	return ptr;
}

// php::ltrim — trim whitespace from left
void *php_ltrim(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	size_t start = s.find_first_not_of(" \t\n\r\f\v");
	if ( start == std::string::npos )
		s.clear();
	else
		s.erase(0, start);
	return ptr;
}

// php::rtrim — trim whitespace from right
void *php_rtrim(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	size_t end = s.find_last_not_of(" \t\n\r\f\v");
	if ( end == std::string::npos )
		s.clear();
	else
		s.erase(end + 1);
	return ptr;
}

// php::strtolower — convert to lowercase in place
void *php_strtolower(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	std::transform(s.begin(), s.end(), s.begin(), ::tolower);
	return ptr;
}

// php::strtoupper — convert to uppercase in place
void *php_strtoupper(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	std::transform(s.begin(), s.end(), s.begin(), ::toupper);
	return ptr;
}

// php::strrev — reverse string in place
void *php_strrev(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	std::reverse(s.begin(), s.end());
	return ptr;
}

// php::strpos — find position of needle in haystack, returns -1 if not found
int64_t php_strpos(void *haystack, void *needle)
{
	std::string &h = *(std::string *)haystack;
	std::string &n = *(std::string *)needle;
	size_t pos = h.find(n);
	return pos == std::string::npos ? -1 : (int64_t)pos;
}

// php::substr — extract substring, modifies string in place
void *php_substr(void *ptr, int64_t start, int64_t length)
{
	std::string &s = *(std::string *)ptr;
	if ( start < 0 ) start = (int64_t)s.length() + start;
	if ( start < 0 ) start = 0;
	if ( length < 0 ) length = (int64_t)s.length() + length - start;
	if ( length < 0 ) length = 0;
	s = s.substr((size_t)start, (size_t)length);
	return ptr;
}

// php::str_repeat — repeat string n times, modifies in place
void *php_str_repeat(void *ptr, int64_t count)
{
	std::string &s = *(std::string *)ptr;
	std::string orig = s;
	s.clear();
	for ( int64_t i = 0; i < count; ++i )
		s += orig;
	return ptr;
}

// php::str_replace — replace all occurrences of search with replace in subject
void *php_str_replace(void *search, void *replace, void *subject)
{
	std::string &srch = *(std::string *)search;
	std::string &repl = *(std::string *)replace;
	std::string &subj = *(std::string *)subject;
	size_t pos = 0;
	while ( (pos = subj.find(srch, pos)) != std::string::npos )
	{
		subj.replace(pos, srch.length(), repl);
		pos += repl.length();
	}
	return subject;
}

// php::str_contains — check if string contains substring
int64_t php_str_contains(void *haystack, void *needle)
{
	return ((std::string *)haystack)->find(*(std::string *)needle) != std::string::npos ? 1 : 0;
}

// php::str_starts_with
int64_t php_str_starts_with(void *str, void *prefix)
{
	std::string &s = *(std::string *)str;
	std::string &p = *(std::string *)prefix;
	return s.length() >= p.length() && s.compare(0, p.length(), p) == 0 ? 1 : 0;
}

// php::str_ends_with
int64_t php_str_ends_with(void *str, void *suffix)
{
	std::string &s = *(std::string *)str;
	std::string &x = *(std::string *)suffix;
	return s.length() >= x.length() && s.compare(s.length() - x.length(), x.length(), x) == 0 ? 1 : 0;
}

// ---- Namespace registration ----

void Program::add_php_namespace()
{
	variable_map_t &php_ns = namespace_map["php"];
	Variable *var;

	// string → int functions
	var = addFunction("__php_strlen",         datatype_vec_t{DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)php_strlen);
	if (var) php_ns["strlen"] = var;

	var = addFunction("__php_strpos",         datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_strpos);
	if (var) php_ns["strpos"] = var;

	var = addFunction("__php_str_contains",   datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_str_contains);
	if (var) php_ns["str_contains"] = var;

	var = addFunction("__php_str_starts_with",datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_str_starts_with);
	if (var) php_ns["str_starts_with"] = var;

	var = addFunction("__php_str_ends_with",  datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_str_ends_with);
	if (var) php_ns["str_ends_with"] = var;

	// string → string functions (modify in place, return same pointer)
	var = addFunction("__php_trim",           datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_trim);
	if (var) php_ns["trim"] = var;

	var = addFunction("__php_ltrim",          datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_ltrim);
	if (var) php_ns["ltrim"] = var;

	var = addFunction("__php_rtrim",          datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_rtrim);
	if (var) php_ns["rtrim"] = var;

	var = addFunction("__php_strtolower",     datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_strtolower);
	if (var) php_ns["strtolower"] = var;

	var = addFunction("__php_strtoupper",     datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_strtoupper);
	if (var) php_ns["strtoupper"] = var;

	var = addFunction("__php_strrev",         datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_strrev);
	if (var) php_ns["strrev"] = var;

	// string + int → string
	var = addFunction("__php_str_repeat",     datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtINT64}, (fVOIDFUNC)php_str_repeat);
	if (var) php_ns["str_repeat"] = var;

	// substr(str, start, length)
	var = addFunction("__php_substr",         datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtINT64, DataType::dtINT64}, (fVOIDFUNC)php_substr);
	if (var) php_ns["substr"] = var;

	// str_replace(search, replace, subject) — all strings
	var = addFunction("__php_str_replace",    datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_str_replace);
	if (var) php_ns["str_replace"] = var;

	DBG(std::cout << "add_php_namespace() registered php:: with " << php_ns.size() << " members" << std::endl);
}
