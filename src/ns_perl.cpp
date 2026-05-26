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
#include <asmjit/x86.h>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

using namespace std;
using namespace asmjit;

// ---- C++ wrapper functions called by JIT ----

// perl::chop — remove last character from string, return it
// Perl: $removed = chop($str);
int64_t perl_chop(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	if ( s.empty() ) return 0;
	char ch = s.back();
	s.pop_back();
	return (int64_t)ch;
}

// perl::chomp — remove trailing newline(s), return count removed
// Perl: $count = chomp($str);
int64_t perl_chomp(void *ptr)
{
	std::string &s = *(std::string *)ptr;
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
void *perl_grep(void *dest, void *needle, void *src)
{
	MadArray &d = *(MadArray *)dest;
	std::string &n = *(std::string *)needle;
	MadArray &s = *(MadArray *)src;
	d.data.clear();
	d.assoc.clear();
	try {
		std::regex re(n);
		for ( auto &v : s.data )
		{
			if ( v.is_string() && std::regex_search(v.as_string(), re) )
				d.data.push_back(v);
		}
	} catch (std::regex_error &) {
		// fallback to substring match on invalid regex
		for ( auto &v : s.data )
		{
			if ( v.is_string() && v.as_string().find(n) != std::string::npos )
				d.data.push_back(v);
		}
	}
	return dest;
}

// perl::glob — file globbing, returns array of matching filenames
// Perl: @files = glob("*.txt");
void *perl_glob(void *arr, void *pattern)
{
	MadArray &a = *(MadArray *)arr;
	std::string &p = *(std::string *)pattern;
	a.data.clear();
	a.assoc.clear();

	glob_t globbuf;
	int ret = ::glob(p.c_str(), GLOB_NOSORT, NULL, &globbuf);
	if ( ret == 0 )
	{
		for ( size_t i = 0; i < globbuf.gl_pathc; ++i )
			a.push(MadValue(std::string(globbuf.gl_pathv[i])));
		globfree(&globbuf);
	}
	return arr;
}

// perl::scalar — return count of elements in array (Perl's scalar @array)
int64_t perl_scalar(void *arr)
{
	return (int64_t)((MadArray *)arr)->count();
}

// perl::push — append value to array (Perl: push @arr, $val)
void *perl_push(void *arr, void *str)
{
	((MadArray *)arr)->push(MadValue(*(std::string *)str));
	return arr;
}

// perl::pop — remove last element (Perl: $val = pop @arr)
void *perl_pop(void *result, void *arr)
{
	MadValue v = ((MadArray *)arr)->pop();
	std::string &res = *(std::string *)result;
	if ( v.is_string() ) res = v.as_string();
	else if ( v.is_int() ) res = std::to_string(v.as_int());
	else res.clear();
	return result;
}

// perl::shift — remove first element (Perl: $val = shift @arr)
void *perl_shift(void *result, void *arr)
{
	MadArray &a = *(MadArray *)arr;
	std::string &res = *(std::string *)result;
	if ( a.data.empty() ) { res.clear(); return result; }
	MadValue v = a.data.front();
	a.data.erase(a.data.begin());
	if ( v.is_string() ) res = v.as_string();
	else if ( v.is_int() ) res = std::to_string(v.as_int());
	else res.clear();
	return result;
}

// perl::unshift — prepend value (Perl: unshift @arr, $val)
void *perl_unshift(void *arr, void *str)
{
	MadArray &a = *(MadArray *)arr;
	a.data.insert(a.data.begin(), MadValue(*(std::string *)str));
	return arr;
}

// perl::join — join array with separator (Perl: join(",", @arr))
void *perl_join(void *result, void *sep, void *arr)
{
	std::string &res = *(std::string *)result;
	std::string &s = *(std::string *)sep;
	MadArray &a = *(MadArray *)arr;
	res.clear();
	for ( size_t i = 0; i < a.data.size(); ++i )
	{
		if ( i > 0 ) res += s;
		if ( a.data[i].is_string() ) res += a.data[i].as_string();
		else if ( a.data[i].is_int() ) res += std::to_string(a.data[i].as_int());
	}
	return result;
}

// perl::split — split string by regex pattern into array (Perl: split(/pattern/, $str))
void *perl_split(void *arr, void *delim, void *str)
{
	MadArray &a = *(MadArray *)arr;
	std::string &d = *(std::string *)delim;
	std::string &s = *(std::string *)str;
	a.data.clear();
	a.assoc.clear();
	if ( d.empty() ) { a.push(MadValue(s)); return arr; }
	try {
		std::regex re(d);
		std::sregex_token_iterator it(s.begin(), s.end(), re, -1);
		std::sregex_token_iterator end;
		for ( ; it != end; ++it )
			a.push(MadValue(it->str()));
	} catch (std::regex_error &) {
		// fallback to literal delimiter on invalid regex
		size_t start = 0, pos;
		while ( (pos = s.find(d, start)) != std::string::npos )
		{
			a.push(MadValue(s.substr(start, pos - start)));
			start = pos + d.length();
		}
		a.push(MadValue(s.substr(start)));
	}
	return arr;
}

// perl::reverse — reverse string in place (Perl: reverse $str)
void *perl_reverse(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	std::reverse(s.begin(), s.end());
	return ptr;
}

// perl::lc — lowercase (Perl: lc $str)
void *perl_lc(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	std::transform(s.begin(), s.end(), s.begin(), ::tolower);
	return ptr;
}

// perl::uc — uppercase (Perl: uc $str)
void *perl_uc(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	std::transform(s.begin(), s.end(), s.begin(), ::toupper);
	return ptr;
}

// perl::ucfirst — capitalize first character (Perl: ucfirst $str)
void *perl_ucfirst(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	if ( !s.empty() ) s[0] = toupper(s[0]);
	return ptr;
}

// perl::lcfirst — lowercase first character (Perl: lcfirst $str)
void *perl_lcfirst(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	if ( !s.empty() ) s[0] = tolower(s[0]);
	return ptr;
}

// perl::index — find position of needle in haystack (Perl: index($str, $substr))
int64_t perl_index(void *haystack, void *needle)
{
	std::string &h = *(std::string *)haystack;
	std::string &n = *(std::string *)needle;
	size_t pos = h.find(n);
	return pos == std::string::npos ? -1 : (int64_t)pos;
}

// perl::rindex — find last position (Perl: rindex($str, $substr))
int64_t perl_rindex(void *haystack, void *needle)
{
	std::string &h = *(std::string *)haystack;
	std::string &n = *(std::string *)needle;
	size_t pos = h.rfind(n);
	return pos == std::string::npos ? -1 : (int64_t)pos;
}

// perl::length — string length (Perl: length $str)
int64_t perl_length(void *ptr)
{
	return (int64_t)((std::string *)ptr)->length();
}

// perl::substr — extract/replace substring (Perl: substr($str, $offset, $length))
void *perl_substr(void *result, void *str, int64_t offset, int64_t length)
{
	std::string &s = *(std::string *)str;
	std::string &res = *(std::string *)result;
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
	try {
		return std::regex_match(*(std::string *)str, std::regex(*(std::string *)pattern)) ? 1 : 0;
	} catch (std::regex_error &) { return 0; }
}

// madc::regex_search(string, pattern) — returns 1 if pattern found anywhere in string
int64_t madc_regex_search(void *str, void *pattern)
{
	try {
		return std::regex_search(*(std::string *)str, std::regex(*(std::string *)pattern)) ? 1 : 0;
	} catch (std::regex_error &) { return 0; }
}

// madc::regex_replace(result, string, pattern, replacement) — regex replace, result = modified string
void *madc_regex_replace(void *result, void *str, void *pattern, void *replacement)
{
	try {
		*(std::string *)result = std::regex_replace(*(std::string *)str,
			std::regex(*(std::string *)pattern), *(std::string *)replacement);
	} catch (std::regex_error &) {
		*(std::string *)result = *(std::string *)str;
	}
	return result;
}


// ---- Namespace registration ----

void Program::add_perl_namespace()
{
	variable_map_t &perl_ns = namespace_map["perl"];
	Variable *var;

	// chop/chomp — Perl-unique
	var = addFunction("__perl_chop",      datatype_vec_t{DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)perl_chop);
	if (var) perl_ns["chop"] = var;

	var = addFunction("__perl_chomp",     datatype_vec_t{DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)perl_chomp);
	if (var) perl_ns["chomp"] = var;

	// grep — filter array by pattern
	var = addFunction("__perl_grep",      datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)perl_grep);
	if (var) perl_ns["grep"] = var;

	// glob — file globbing
	var = addFunction("__perl_glob",      datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtSTRING}, (fVOIDFUNC)perl_glob);
	if (var) perl_ns["glob"] = var;

	// array operations
	var = addFunction("__perl_scalar",    datatype_vec_t{DataType::dtINT64, DataType::dtARRAY}, (fVOIDFUNC)perl_scalar);
	if (var) perl_ns["scalar"] = var;

	var = addFunction("__perl_push",      datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtSTRING}, (fVOIDFUNC)perl_push);
	if (var) perl_ns["push"] = var;

	var = addFunction("__perl_pop",       datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)perl_pop);
	if (var) perl_ns["pop"] = var;

	var = addFunction("__perl_shift",     datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)perl_shift);
	if (var) perl_ns["shift"] = var;

	var = addFunction("__perl_unshift",   datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtSTRING}, (fVOIDFUNC)perl_unshift);
	if (var) perl_ns["unshift"] = var;

	// join/split
	var = addFunction("__perl_join",      datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)perl_join);
	if (var) perl_ns["join"] = var;

	var = addFunction("__perl_split",     datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)perl_split);
	if (var) perl_ns["split"] = var;

	// string functions
	var = addFunction("__perl_reverse",   datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)perl_reverse);
	if (var) perl_ns["reverse"] = var;

	var = addFunction("__perl_lc",        datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)perl_lc);
	if (var) perl_ns["lc"] = var;

	var = addFunction("__perl_uc",        datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)perl_uc);
	if (var) perl_ns["uc"] = var;

	var = addFunction("__perl_ucfirst",   datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)perl_ucfirst);
	if (var) perl_ns["ucfirst"] = var;

	var = addFunction("__perl_lcfirst",   datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)perl_lcfirst);
	if (var) perl_ns["lcfirst"] = var;

	var = addFunction("__perl_index",     datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)perl_index);
	if (var) perl_ns["index"] = var;

	var = addFunction("__perl_rindex",    datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)perl_rindex);
	if (var) perl_ns["rindex"] = var;

	var = addFunction("__perl_length",    datatype_vec_t{DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)perl_length);
	if (var) perl_ns["length"] = var;

	var = addFunction("__perl_substr",    datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtINT64, DataType::dtINT64}, (fVOIDFUNC)perl_substr);
	if (var) perl_ns["substr"] = var;

	DBG(std::cout << "add_perl_namespace() registered perl:: with " << perl_ns.size() << " members" << std::endl);
}

extern "C" {
// Thin C-linkage wrappers for transpiler import resolution
int64_t __perl_chop(void *a) { return perl_chop(a); }
int64_t __perl_chomp(void *a) { return perl_chomp(a); }
void *__perl_grep(void *a, void *b, void *c) { return perl_grep(a, b, c); }
void *__perl_glob(void *a, void *b) { return perl_glob(a, b); }
int64_t __perl_scalar(void *a) { return perl_scalar(a); }
void *__perl_push(void *a, void *b) { return perl_push(a, b); }
void *__perl_pop(void *a, void *b) { return perl_pop(a, b); }
void *__perl_shift(void *a, void *b) { return perl_shift(a, b); }
void *__perl_unshift(void *a, void *b) { return perl_unshift(a, b); }
void *__perl_join(void *a, void *b, void *c) { return perl_join(a, b, c); }
void *__perl_split(void *a, void *b, void *c) { return perl_split(a, b, c); }
void *__perl_reverse(void *a) { return perl_reverse(a); }
void *__perl_lc(void *a) { return perl_lc(a); }
void *__perl_uc(void *a) { return perl_uc(a); }
void *__perl_ucfirst(void *a) { return perl_ucfirst(a); }
void *__perl_lcfirst(void *a) { return perl_lcfirst(a); }
int64_t __perl_index(void *a, void *b) { return perl_index(a, b); }
int64_t __perl_rindex(void *a, void *b) { return perl_rindex(a, b); }
int64_t __perl_length(void *a) { return perl_length(a); }
void *__perl_substr(void *a, void *b, int64_t c, int64_t d) { return perl_substr(a, b, c, d); }
}
