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
#include "ns_common.h"

using namespace std;
using namespace asmjit;

int64_t rust_contains(void *str, void *needle)
{
	return ns_common::contains(*(std::string *)str,
				   *(std::string *)needle) ? 1 : 0;
}

int64_t rust_starts_with(void *str, void *prefix)
{
	return ns_common::starts_with(*(std::string *)str,
				      *(std::string *)prefix) ? 1 : 0;
}

int64_t rust_ends_with(void *str, void *suffix)
{
	return ns_common::ends_with(*(std::string *)str,
				    *(std::string *)suffix) ? 1 : 0;
}

void *rust_trim(void *ptr)
{
	ns_common::trim(*(std::string *)ptr, true, true);
	return ptr;
}

void *rust_trim_start(void *ptr)
{
	ns_common::trim(*(std::string *)ptr, true, false);
	return ptr;
}

void *rust_trim_end(void *ptr)
{
	ns_common::trim(*(std::string *)ptr, false, true);
	return ptr;
}

void *rust_replace(void *ptr, void *from, void *to)
{
	ns_common::replace_all(*(std::string *)ptr,
			       *(std::string *)from,
			       *(std::string *)to);
	return ptr;
}

void *rust_repeat(void *ptr, int64_t count)
{
	ns_common::repeat(*(std::string *)ptr, count);
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
	ns_common::split_by_delim(*(MadArray *)arr,
				  *(std::string *)str,
				  *(std::string *)delim);
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
	ns_common::join_with_sep(*(std::string *)result,
				 *(MadArray *)arr,
				 *(std::string *)sep);
	return result;
}

void *rust_first(void *result, void *arr)
{
	std::string &res = *(std::string *)result;
	MadArray &a = *(MadArray *)arr;
	res.clear();
	if ( !a.data.empty() )
		ns_common::value_to_string(a.data[0], res);
	return result;
}

void *rust_last(void *result, void *arr)
{
	std::string &res = *(std::string *)result;
	MadArray &a = *(MadArray *)arr;
	res.clear();
	if ( !a.data.empty() )
		ns_common::value_to_string(a.data[a.data.size() - 1], res);
	return result;
}

void *rust_get(void *result, void *arr, int64_t idx)
{
	std::string &res = *(std::string *)result;
	MadArray &a = *(MadArray *)arr;
	res.clear();
	if ( idx >= 0 && (size_t)idx < a.data.size() )
		ns_common::value_to_string(a.data[(size_t)idx], res);
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
	res.clear();
	ns_common::value_to_string(v, res);
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

extern "C" {
// Thin C-linkage wrappers for transpiler import resolution
int64_t __rust_contains(void *a, void *b) { return rust_contains(a, b); }
int64_t __rust_starts_with(void *a, void *b) { return rust_starts_with(a, b); }
int64_t __rust_ends_with(void *a, void *b) { return rust_ends_with(a, b); }
void *__rust_trim(void *a) { return rust_trim(a); }
void *__rust_trim_start(void *a) { return rust_trim_start(a); }
void *__rust_trim_end(void *a) { return rust_trim_end(a); }
void *__rust_replace(void *a, void *b, void *c) { return rust_replace(a, b, c); }
void *__rust_repeat(void *a, int64_t b) { return rust_repeat(a, b); }
int64_t __rust_len(void *a) { return rust_len(a); }
int64_t __rust_is_empty(void *a) { return rust_is_empty(a); }
void *__rust_split(void *a, void *b, void *c) { return rust_split(a, b, c); }
void *__rust_split_whitespace(void *a, void *b) { return rust_split_whitespace(a, b); }
void *__rust_join(void *a, void *b, void *c) { return rust_join(a, b, c); }
void *__rust_first(void *a, void *b) { return rust_first(a, b); }
void *__rust_last(void *a, void *b) { return rust_last(a, b); }
void *__rust_get(void *a, void *b, int64_t c) { return rust_get(a, b, c); }
void *__rust_push(void *a, void *b) { return rust_push(a, b); }
void *__rust_pop(void *a, void *b) { return rust_pop(a, b); }
}
