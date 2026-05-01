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
#include <asmjit/x86.h>
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

using namespace std;
using namespace asmjit;

static void rust_trim_chars(std::string &s, bool left, bool right)
{
	size_t start = 0;
	size_t end = s.length();

	if ( left )
	{
		start = s.find_first_not_of(" \t\n\r\f\v");
		if ( start == std::string::npos )
		{
			s.clear();
			return;
		}
	}

	if ( right )
	{
		size_t last = s.find_last_not_of(" \t\n\r\f\v");
		if ( last == std::string::npos )
		{
			s.clear();
			return;
		}
		end = last + 1;
	}

	if ( left || right )
		s = s.substr(start, end - start);
}

static void rust_value_to_string(std::string &out, MadValue &v)
{
	if ( v.is_string() )
		out = v.as_string();
	else if ( v.is_int() )
		out = std::to_string(v.as_int());
	else if ( v.is_double() )
		out = std::to_string(v.as_double());
	else
		out.clear();
}

int64_t rust_contains(void *str, void *needle)
{
	std::string &s = *(std::string *)str;
	std::string &n = *(std::string *)needle;
	return s.find(n) != std::string::npos ? 1 : 0;
}

int64_t rust_starts_with(void *str, void *prefix)
{
	std::string &s = *(std::string *)str;
	std::string &p = *(std::string *)prefix;
	return s.length() >= p.length() && s.compare(0, p.length(), p) == 0 ? 1 : 0;
}

int64_t rust_ends_with(void *str, void *suffix)
{
	std::string &s = *(std::string *)str;
	std::string &x = *(std::string *)suffix;
	return s.length() >= x.length() && s.compare(s.length() - x.length(), x.length(), x) == 0 ? 1 : 0;
}

void *rust_trim(void *ptr)
{
	rust_trim_chars(*(std::string *)ptr, true, true);
	return ptr;
}

void *rust_trim_start(void *ptr)
{
	rust_trim_chars(*(std::string *)ptr, true, false);
	return ptr;
}

void *rust_trim_end(void *ptr)
{
	rust_trim_chars(*(std::string *)ptr, false, true);
	return ptr;
}

void *rust_replace(void *ptr, void *from, void *to)
{
	std::string &s = *(std::string *)ptr;
	std::string &f = *(std::string *)from;
	std::string &r = *(std::string *)to;
	if ( f.empty() ) return ptr;
	size_t pos = 0;
	while ( (pos = s.find(f, pos)) != std::string::npos )
	{
		s.replace(pos, f.length(), r);
		pos += r.length();
	}
	return ptr;
}

void *rust_repeat(void *ptr, int64_t count)
{
	std::string &s = *(std::string *)ptr;
	std::string orig = s;
	if ( count <= 0 )
	{
		s.clear();
		return ptr;
	}
	s.clear();
	s.reserve(orig.length() * (size_t)count);
	for ( int64_t i = 0; i < count; ++i )
		s += orig;
	return ptr;
}

int64_t rust_len(void *ptr)
{
	return (int64_t)((std::string *)ptr)->length();
}

int64_t rust_is_empty(void *ptr)
{
	return ((std::string *)ptr)->empty() ? 1 : 0;
}

void *rust_split(void *arr, void *str, void *delim)
{
	MadArray &a = *(MadArray *)arr;
	std::string &s = *(std::string *)str;
	std::string &d = *(std::string *)delim;
	a.data.clear();
	a.assoc.clear();
	if ( d.empty() )
	{
		a.push(MadValue(s));
		return arr;
	}
	size_t start = 0;
	size_t end = 0;
	while ( (end = s.find(d, start)) != std::string::npos )
	{
		a.push(MadValue(s.substr(start, end - start)));
		start = end + d.length();
	}
	a.push(MadValue(s.substr(start)));
	return arr;
}

void *rust_split_whitespace(void *arr, void *str)
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

void *rust_join(void *result, void *arr, void *sep)
{
	std::string &res = *(std::string *)result;
	MadArray &a = *(MadArray *)arr;
	std::string &s = *(std::string *)sep;
	std::string tmp;
	res.clear();
	for ( size_t i = 0; i < a.data.size(); ++i )
	{
		if ( i > 0 ) res += s;
		rust_value_to_string(tmp, a.data[i]);
		res += tmp;
	}
	return result;
}

void *rust_first(void *result, void *arr)
{
	std::string &res = *(std::string *)result;
	MadArray &a = *(MadArray *)arr;
	if ( a.data.empty() )
	{
		res.clear();
		return result;
	}
	rust_value_to_string(res, a.data[0]);
	return result;
}

void *rust_last(void *result, void *arr)
{
	std::string &res = *(std::string *)result;
	MadArray &a = *(MadArray *)arr;
	if ( a.data.empty() )
	{
		res.clear();
		return result;
	}
	rust_value_to_string(res, a.data[a.data.size() - 1]);
	return result;
}

void *rust_get(void *result, void *arr, int64_t idx)
{
	std::string &res = *(std::string *)result;
	MadArray &a = *(MadArray *)arr;
	if ( idx < 0 || (size_t)idx >= a.data.size() )
	{
		res.clear();
		return result;
	}
	rust_value_to_string(res, a.data[(size_t)idx]);
	return result;
}

void *rust_push(void *arr, void *value)
{
	((MadArray *)arr)->push(MadValue(*(std::string *)value));
	return arr;
}

void *rust_pop(void *result, void *arr)
{
	std::string &res = *(std::string *)result;
	MadValue v = ((MadArray *)arr)->pop();
	rust_value_to_string(res, v);
	return result;
}

void Program::add_rust_namespace()
{
	variable_map_t &rust_ns = namespace_map["rust"];
	Variable *var;

	var = addFunction("__rust_contains",          datatype_vec_t{DataType::dtINT64,  DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)rust_contains);
	if (var) rust_ns["contains"] = var;

	var = addFunction("__rust_starts_with",       datatype_vec_t{DataType::dtINT64,  DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)rust_starts_with);
	if (var) rust_ns["starts_with"] = var;

	var = addFunction("__rust_ends_with",         datatype_vec_t{DataType::dtINT64,  DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)rust_ends_with);
	if (var) rust_ns["ends_with"] = var;

	var = addFunction("__rust_trim",              datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)rust_trim);
	if (var) rust_ns["trim"] = var;

	var = addFunction("__rust_trim_start",        datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)rust_trim_start);
	if (var) rust_ns["trim_start"] = var;

	var = addFunction("__rust_trim_end",          datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)rust_trim_end);
	if (var) rust_ns["trim_end"] = var;

	var = addFunction("__rust_replace",           datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)rust_replace);
	if (var) rust_ns["replace"] = var;

	var = addFunction("__rust_repeat",            datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtINT64}, (fVOIDFUNC)rust_repeat);
	if (var) rust_ns["repeat"] = var;

	var = addFunction("__rust_len",               datatype_vec_t{DataType::dtINT64,  DataType::dtSTRING}, (fVOIDFUNC)rust_len);
	if (var) rust_ns["len"] = var;

	var = addFunction("__rust_is_empty",          datatype_vec_t{DataType::dtINT64,  DataType::dtSTRING}, (fVOIDFUNC)rust_is_empty);
	if (var) rust_ns["is_empty"] = var;

	var = addFunction("__rust_split",             datatype_vec_t{DataType::dtARRAY,  DataType::dtARRAY, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)rust_split);
	if (var) rust_ns["split"] = var;

	var = addFunction("__rust_split_whitespace",  datatype_vec_t{DataType::dtARRAY,  DataType::dtARRAY, DataType::dtSTRING}, (fVOIDFUNC)rust_split_whitespace);
	if (var) rust_ns["split_whitespace"] = var;

	var = addFunction("__rust_join",              datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY, DataType::dtSTRING}, (fVOIDFUNC)rust_join);
	if (var) rust_ns["join"] = var;

	var = addFunction("__rust_first",             datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)rust_first);
	if (var) rust_ns["first"] = var;

	var = addFunction("__rust_last",              datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)rust_last);
	if (var) rust_ns["last"] = var;

	var = addFunction("__rust_get",               datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY, DataType::dtINT64}, (fVOIDFUNC)rust_get);
	if (var) rust_ns["get"] = var;

	var = addFunction("__rust_push",              datatype_vec_t{DataType::dtARRAY,  DataType::dtARRAY, DataType::dtSTRING}, (fVOIDFUNC)rust_push);
	if (var) rust_ns["push"] = var;

	var = addFunction("__rust_pop",               datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)rust_pop);
	if (var) rust_ns["pop"] = var;

	DBG(std::cout << "add_rust_namespace() registered rust:: with " << rust_ns.size() << " members" << std::endl);
}
