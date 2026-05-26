///////////////////////////////////////////////////////////////////////////
//                                                                     //
// madc php:: namespace                                                //
//                                                                     //
// Functions unique to PHP that have no direct C/C++ equivalent.       //
// C-standard functions (strlen, strpos, toupper, etc.) belong in      //
// the global scope or a future c:: namespace, not here.               //
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

// ---- C++ wrapper functions called by JIT ----

// php::trim — trim whitespace from both ends (no C/C++ equivalent)
void *php_trim(void *ptr)
{
	ns_common::trim(*(std::string *)ptr, true, true);
	return ptr;
}

// php::ltrim — trim whitespace from left
void *php_ltrim(void *ptr)
{
	ns_common::trim(*(std::string *)ptr, true, false);
	return ptr;
}

// php::rtrim — trim whitespace from right
void *php_rtrim(void *ptr)
{
	ns_common::trim(*(std::string *)ptr, false, true);
	return ptr;
}

// php::ucfirst — capitalize first character
void *php_ucfirst(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	if ( !s.empty() )
		s[0] = toupper(s[0]);
	return ptr;
}

// php::lcfirst — lowercase first character
void *php_lcfirst(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	if ( !s.empty() )
		s[0] = tolower(s[0]);
	return ptr;
}

// php::str_repeat — repeat string n times
void *php_str_repeat(void *ptr, int64_t count)
{
	ns_common::repeat(*(std::string *)ptr, count);
	return ptr;
}

// php::str_replace — replace all occurrences of search with replace in subject
void *php_str_replace(void *search, void *replace, void *subject)
{
	ns_common::replace_all(*(std::string *)subject,
			       *(std::string *)search,
			       *(std::string *)replace);
	return subject;
}

// php::str_pad — pad string to a given length (default right-pad with spaces)
void *php_str_pad(void *ptr, int64_t length, void *pad_str)
{
	std::string &s = *(std::string *)ptr;
	std::string &pad = *(std::string *)pad_str;
	if ( pad.empty() || (int64_t)s.length() >= length )
		return ptr;
	while ( (int64_t)s.length() < length )
	{
		size_t remaining = (size_t)length - s.length();
		s += pad.substr(0, remaining);
	}
	return ptr;
}

// php::str_word_count — count words in string
int64_t php_str_word_count(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	int64_t count = 0;
	bool in_word = false;
	for ( size_t i = 0; i < s.length(); ++i )
	{
		if ( isspace(s[i]) )
			in_word = false;
		else if ( !in_word )
		{
			in_word = true;
			++count;
		}
	}
	return count;
}

// php::nl2br — convert newlines to "<br>\n"
void *php_nl2br(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	std::string result;
	result.reserve(s.length() * 2);
	for ( size_t i = 0; i < s.length(); ++i )
	{
		if ( s[i] == '\n' )
			result += "<br>\n";
		else if ( s[i] == '\r' )
		{
			result += "<br>\r";
			if ( i + 1 < s.length() && s[i+1] == '\n' )
				result += s[++i]; // consume \n after \r
		}
		else
			result += s[i];
	}
	s = result;
	return ptr;
}

// php::str_rot13 — ROT13 encoding
void *php_str_rot13(void *ptr)
{
	std::string &s = *(std::string *)ptr;
	for ( size_t i = 0; i < s.length(); ++i )
	{
		char c = s[i];
		if ( c >= 'a' && c <= 'z' )
			s[i] = 'a' + (c - 'a' + 13) % 26;
		else if ( c >= 'A' && c <= 'Z' )
			s[i] = 'A' + (c - 'A' + 13) % 26;
	}
	return ptr;
}

// php::chunk_split — insert separator every chunklen characters
void *php_chunk_split(void *ptr, int64_t chunklen, void *separator)
{
	std::string &s = *(std::string *)ptr;
	std::string &sep = *(std::string *)separator;
	if ( chunklen <= 0 || s.empty() ) return ptr;
	std::string result;
	result.reserve(s.length() + (s.length() / chunklen + 1) * sep.length());
	for ( size_t i = 0; i < s.length(); i += (size_t)chunklen )
	{
		result += s.substr(i, (size_t)chunklen);
		result += sep;
	}
	s = result;
	return ptr;
}

// php::number_format — format number with thousands separator
void *php_number_format(void *result, int64_t number, void *thousands_sep)
{
	std::string &sep = *(std::string *)thousands_sep;
	std::string &res = *(std::string *)result;
	bool negative = number < 0;
	if ( negative ) number = -number;
	std::string digits = std::to_string(number);
	res.clear();
	int count = 0;
	for ( int i = (int)digits.length() - 1; i >= 0; --i )
	{
		if ( count > 0 && count % 3 == 0 )
			res = sep + res;
		res = digits[i] + res;
		++count;
	}
	if ( negative ) res = "-" + res;
	return result;
}

// php::wordwrap — wrap text at specified width
void *php_wordwrap(void *ptr, int64_t width, void *brk)
{
	std::string &s = *(std::string *)ptr;
	std::string &br = *(std::string *)brk;
	if ( width <= 0 ) return ptr;
	std::string result;
	int64_t col = 0;
	size_t last_space = std::string::npos;
	for ( size_t i = 0; i < s.length(); ++i )
	{
		result += s[i];
		++col;
		if ( s[i] == ' ' ) last_space = result.length() - 1;
		if ( s[i] == '\n' ) { col = 0; last_space = std::string::npos; }
		if ( col >= width && last_space != std::string::npos )
		{
			result.replace(last_space, 1, br);
			col = (int64_t)(result.length() - last_space - br.length());
			last_space = std::string::npos;
		}
	}
	s = result;
	return ptr;
}


// ---- MadArray-based functions ----

// php::explode — split string by delimiter into array
void *php_explode(void *arr, void *delim, void *str)
{
	ns_common::split_by_delim(*(MadArray *)arr,
				  *(std::string *)str,
				  *(std::string *)delim);
	return arr;
}

// php::implode — join array elements with glue string
void *php_implode(void *result, void *glue, void *arr)
{
	ns_common::join_with_sep(*(std::string *)result,
				 *(MadArray *)arr,
				 *(std::string *)glue);
	return result;
}

// php::count — return number of elements
int64_t php_count(void *arr)
{
	return (int64_t)((MadArray *)arr)->count();
}

// php::array_push — append value (string) to array
void *php_array_push_str(void *arr, void *str)
{
	((MadArray *)arr)->push(MadValue(*(std::string *)str));
	return arr;
}

// php::array_push_int — append integer to array
void *php_array_push_int(void *arr, int64_t val)
{
	((MadArray *)arr)->push(MadValue(val));
	return arr;
}

// php::array_push_array — append nested array by deep copy
void *php_array_push_array(void *arr, void *value)
{
	((MadArray *)arr)->push(MadValue(*(MadArray *)value));
	return arr;
}

// php::array_pop — remove and return last element as string
void *php_array_pop(void *result, void *arr)
{
	MadValue v = ((MadArray *)arr)->pop();
	std::string &res = *(std::string *)result;
	if ( v.is_string() )
		res = v.as_string();
	else if ( v.is_int() )
		res = std::to_string(v.as_int());
	else
		res.clear();
	return result;
}

// php::array_get — get element at integer index as string
void *php_array_get(void *result, void *arr, int64_t index)
{
	MadArray &a = *(MadArray *)arr;
	std::string &res = *(std::string *)result;
	if ( index < 0 || (size_t)index >= a.data.size() )
	{
		res.clear();
		return result;
	}
	MadValue &v = a.data[(size_t)index];
	if ( v.is_string() )
		res = v.as_string();
	else if ( v.is_int() )
		res = std::to_string(v.as_int());
	else if ( v.is_double() )
		res = std::to_string(v.as_double());
	else
		res.clear();
	return result;
}

// php::array_get_int — get element at integer index as int
int64_t php_array_get_int(void *arr, int64_t index)
{
	MadArray &a = *(MadArray *)arr;
	if ( index < 0 || (size_t)index >= a.data.size() )
		return 0;
	MadValue &v = a.data[(size_t)index];
	if ( v.is_int() ) return v.as_int();
	if ( v.is_double() ) return (int64_t)v.as_double();
	if ( v.is_string() ) { try { return std::stoll(v.as_string()); } catch(...) { return 0; } }
	return 0;
}

// php::array_reverse — reverse array in place
void *php_array_reverse(void *arr)
{
	MadArray &a = *(MadArray *)arr;
	std::reverse(a.data.begin(), a.data.end());
	return arr;
}

// php::in_array — check if value exists in array (string comparison)
int64_t php_in_array(void *needle, void *arr)
{
	std::string &n = *(std::string *)needle;
	MadArray &a = *(MadArray *)arr;
	for ( auto &v : a.data )
		if ( v.is_string() && v.as_string() == n )
			return 1;
	return 0;
}

// php::array_search — find index of value in array, returns -1 if not found
int64_t php_array_search(void *needle, void *arr)
{
	std::string &n = *(std::string *)needle;
	MadArray &a = *(MadArray *)arr;
	for ( size_t i = 0; i < a.data.size(); ++i )
		if ( a.data[i].is_string() && a.data[i].as_string() == n )
			return (int64_t)i;
	return -1;
}

// php::array_unique — remove duplicate string values
void *php_array_unique(void *arr)
{
	MadArray &a = *(MadArray *)arr;
	std::vector<MadValue> unique;
	for ( auto &v : a.data )
	{
		bool found = false;
		if ( v.is_string() )
		{
			for ( auto &u : unique )
				if ( u.is_string() && u.as_string() == v.as_string() )
				{ found = true; break; }
		}
		if ( !found ) unique.push_back(v);
	}
	a.data = unique;
	return arr;
}

// php::array_shift — remove first element, shift rest down
void *php_array_shift(void *result, void *arr)
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

// php::array_unshift — prepend value to beginning
void *php_array_unshift(void *arr, void *str)
{
	MadArray &a = *(MadArray *)arr;
	a.data.insert(a.data.begin(), MadValue(*(std::string *)str));
	return arr;
}

// php::sort — sort array (string comparison)
void *php_sort(void *arr)
{
	MadArray &a = *(MadArray *)arr;
	std::sort(a.data.begin(), a.data.end(), [](const MadValue &a, const MadValue &b) {
		if ( a.is_string() && b.is_string() )
			return a.as_string() < b.as_string();
		if ( a.is_int() && b.is_int() )
			return a.as_int() < b.as_int();
		return false;
	});
	return arr;
}

// php::rsort — sort array in reverse
void *php_rsort(void *arr)
{
	php_sort(arr);
	php_array_reverse(arr);
	return arr;
}

// php::array_slice — extract a slice of the array
void *php_array_slice(void *dest, void *src, int64_t offset, int64_t length)
{
	MadArray &d = *(MadArray *)dest;
	MadArray &s = *(MadArray *)src;
	d.data.clear();
	d.assoc.clear();
	if ( offset < 0 ) offset = (int64_t)s.data.size() + offset;
	if ( offset < 0 ) offset = 0;
	if ( length < 0 ) length = (int64_t)s.data.size() + length - offset;
	if ( length < 0 ) length = 0;
	for ( int64_t i = offset; i < offset + length && (size_t)i < s.data.size(); ++i )
		d.data.push_back(s.data[(size_t)i]);
	return dest;
}

// php::array_merge — merge two arrays
void *php_array_merge(void *dest, void *src)
{
	MadArray &d = *(MadArray *)dest;
	MadArray &s = *(MadArray *)src;
	for ( auto &v : s.data )
		d.data.push_back(v);
	return dest;
}


// php::array_column — extract one integer-indexed column from nested arrays
void *php_array_column(void *dest, void *src, int64_t column_index)
{
	MadArray &d = *(MadArray *)dest;
	MadArray &s = *(MadArray *)src;
	d.data.clear();
	d.assoc.clear();
	if ( column_index < 0 )
		return dest;
	for ( auto &row : s.data )
	{
		if ( !row.is_array() )
			continue;
		MadArray &row_arr = row.as_array();
		size_t idx = (size_t)column_index;
		if ( idx >= row_arr.data.size() )
			continue;
		std::string value;
		if ( ns_common::value_to_string(row_arr.data[idx], value) )
			d.push(MadValue(value));
	}
	return dest;
}

// ---- Namespace registration ----

// std::for_each — iterate array, call function pointer per element (string version)
void std_for_each(void *arr, int64_t fn_ptr)
{
    MadArray &a = *(MadArray *)arr;
    typedef void (*fn_str_t)(void *);
    fn_str_t fn = (fn_str_t)fn_ptr;

    for ( size_t i = 0; i < a.data.size(); ++i )
    {
	MadValue &v = a.data[i];
	if ( v.is_string() )
	{
	    std::string s = v.as_string();
	    fn(&s);
	}
	else if ( v.is_int() )
	{
	    // convert int to string for the callback
	    std::string s = std::to_string(v.as_int());
	    fn(&s);
	}
	else if ( v.is_double() )
	{
	    std::string s = std::to_string(v.as_double());
	    fn(&s);
	}
    }
}

void Program::add_php_namespace()
{
	variable_map_t &php_ns = namespace_map["php"];
	Variable *var;

	// trim family — no C/C++ equivalent
	var = addFunction("__php_trim",           datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_trim);
	if (var) php_ns["trim"] = var;

	var = addFunction("__php_ltrim",          datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_ltrim);
	if (var) php_ns["ltrim"] = var;

	var = addFunction("__php_rtrim",          datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_rtrim);
	if (var) { php_ns["rtrim"] = var; php_ns["chop"] = var; } // chop is alias for rtrim in PHP

	// case manipulation — ucfirst/lcfirst are PHP-unique
	var = addFunction("__php_ucfirst",        datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_ucfirst);
	if (var) php_ns["ucfirst"] = var;

	var = addFunction("__php_lcfirst",        datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_lcfirst);
	if (var) php_ns["lcfirst"] = var;

	// string building
	var = addFunction("__php_str_repeat",     datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtINT64}, (fVOIDFUNC)php_str_repeat);
	if (var) php_ns["str_repeat"] = var;

	var = addFunction("__php_str_replace",    datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_str_replace);
	if (var) php_ns["str_replace"] = var;

	var = addFunction("__php_str_pad",        datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)php_str_pad);
	if (var) php_ns["str_pad"] = var;

	// text analysis
	var = addFunction("__php_str_word_count", datatype_vec_t{DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)php_str_word_count);
	if (var) php_ns["str_word_count"] = var;

	// text transformation
	var = addFunction("__php_nl2br",          datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_nl2br);
	if (var) php_ns["nl2br"] = var;

	var = addFunction("__php_str_rot13",      datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_str_rot13);
	if (var) php_ns["str_rot13"] = var;

	var = addFunction("__php_chunk_split",    datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)php_chunk_split);
	if (var) php_ns["chunk_split"] = var;

	// formatting
	var = addFunction("__php_number_format",  datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)php_number_format);
	if (var) php_ns["number_format"] = var;

	var = addFunction("__php_wordwrap",       datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtINT64, DataType::dtSTRING}, (fVOIDFUNC)php_wordwrap);
	if (var) php_ns["wordwrap"] = var;

	// ---- MadArray functions ----

	// explode(arr, delim, str) — split string into array
	var = addFunction("__php_explode",        datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_explode);
	if (var) php_ns["explode"] = var;

	// implode(result, glue, arr) — join array into string
	var = addFunction("__php_implode",        datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)php_implode);
	if (var) php_ns["implode"] = var;

	// count(arr) — number of elements
	var = addFunction("__php_count",          datatype_vec_t{DataType::dtINT64, DataType::dtARRAY}, (fVOIDFUNC)php_count);
	if (var) php_ns["count"] = var;

	// array_push(arr, str) — append string
	var = addFunction("__php_array_push",     datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtSTRING}, (fVOIDFUNC)php_array_push_str);
	if (var) php_ns["array_push"] = var;

	// array_push_int(arr, int) — append int
	var = addFunction("__php_array_push_int", datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtINT64}, (fVOIDFUNC)php_array_push_int);
	if (var) php_ns["array_push_int"] = var;

	// array_push_array(arr, value) — append nested array
	var = addFunction("__php_array_push_array", datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtARRAY}, (fVOIDFUNC)php_array_push_array);
	if (var) php_ns["array_push_array"] = var;

	// array_pop(result, arr) — remove last, return as string
	var = addFunction("__php_array_pop",      datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)php_array_pop);
	if (var) php_ns["array_pop"] = var;

	// array_get(result, arr, index) — get element as string
	var = addFunction("__php_array_get",      datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY, DataType::dtINT64}, (fVOIDFUNC)php_array_get);
	if (var) php_ns["array_get"] = var;

	// array_get_int(arr, index) — get element as int
	var = addFunction("__php_array_get_int",  datatype_vec_t{DataType::dtINT64, DataType::dtARRAY, DataType::dtINT64}, (fVOIDFUNC)php_array_get_int);
	if (var) php_ns["array_get_int"] = var;

	// array_reverse(arr)
	var = addFunction("__php_array_reverse",  datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY}, (fVOIDFUNC)php_array_reverse);
	if (var) php_ns["array_reverse"] = var;

	// in_array(needle, arr) — check if string exists in array
	var = addFunction("__php_in_array",       datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)php_in_array);
	if (var) php_ns["in_array"] = var;

	// array_search(needle, arr) — find index of string, -1 if not found
	var = addFunction("__php_array_search",   datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)php_array_search);
	if (var) php_ns["array_search"] = var;

	// array_unique(arr) — remove duplicates
	var = addFunction("__php_array_unique",   datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY}, (fVOIDFUNC)php_array_unique);
	if (var) php_ns["array_unique"] = var;

	// array_shift(result, arr) — remove first element
	var = addFunction("__php_array_shift",    datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)php_array_shift);
	if (var) php_ns["array_shift"] = var;

	// array_unshift(arr, str) — prepend value
	var = addFunction("__php_array_unshift",  datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtSTRING}, (fVOIDFUNC)php_array_unshift);
	if (var) php_ns["array_unshift"] = var;

	// sort(arr) — sort ascending
	var = addFunction("__php_sort",           datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY}, (fVOIDFUNC)php_sort);
	if (var) php_ns["sort"] = var;

	// rsort(arr) — sort descending
	var = addFunction("__php_rsort",          datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY}, (fVOIDFUNC)php_rsort);
	if (var) php_ns["rsort"] = var;

	// array_slice(dest, src, offset, length)
	var = addFunction("__php_array_slice",    datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtARRAY, DataType::dtINT64, DataType::dtINT64}, (fVOIDFUNC)php_array_slice);
	if (var) php_ns["array_slice"] = var;

	// array_merge(dest, src) — append src elements to dest
	var = addFunction("__php_array_merge",    datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtARRAY}, (fVOIDFUNC)php_array_merge);
	if (var) php_ns["array_merge"] = var;

	// array_column(dest, src, index) — extract one integer-indexed column
	var = addFunction("__php_array_column",   datatype_vec_t{DataType::dtARRAY, DataType::dtARRAY, DataType::dtARRAY, DataType::dtINT64}, (fVOIDFUNC)php_array_column);
	if (var) php_ns["array_column"] = var;

	DBG(std::cout << "add_php_namespace() registered php:: with " << php_ns.size() << " members" << std::endl);
}

extern "C" {
// Thin C-linkage wrappers for transpiler import resolution
void *__php_trim(void *a) { return php_trim(a); }
void *__php_ltrim(void *a) { return php_ltrim(a); }
void *__php_rtrim(void *a) { return php_rtrim(a); }
void *__php_ucfirst(void *a) { return php_ucfirst(a); }
void *__php_lcfirst(void *a) { return php_lcfirst(a); }
void *__php_str_repeat(void *a, int64_t b) { return php_str_repeat(a, b); }
void *__php_str_replace(void *a, void *b, void *c) { return php_str_replace(a, b, c); }
void *__php_str_pad(void *a, int64_t b, void *c) { return php_str_pad(a, b, c); }
int64_t __php_str_word_count(void *a) { return php_str_word_count(a); }
void *__php_nl2br(void *a) { return php_nl2br(a); }
void *__php_str_rot13(void *a) { return php_str_rot13(a); }
void *__php_chunk_split(void *a, int64_t b, void *c) { return php_chunk_split(a, b, c); }
void *__php_number_format(void *a, int64_t b, void *c) { return php_number_format(a, b, c); }
void *__php_wordwrap(void *a, int64_t b, void *c) { return php_wordwrap(a, b, c); }
void *__php_explode(void *a, void *b, void *c) { return php_explode(a, b, c); }
void *__php_implode(void *a, void *b, void *c) { return php_implode(a, b, c); }
int64_t __php_count(void *a) { return php_count(a); }
void *__php_array_push(void *a, void *b) { return php_array_push_str(a, b); }
void *__php_array_push_int(void *a, int64_t b) { return php_array_push_int(a, b); }
void *__php_array_push_array(void *a, void *b) { return php_array_push_array(a, b); }
void *__php_array_pop(void *a, void *b) { return php_array_pop(a, b); }
void *__php_array_get(void *a, void *b, int64_t c) { return php_array_get(a, b, c); }
int64_t __php_array_get_int(void *a, int64_t b) { return php_array_get_int(a, b); }
void *__php_array_reverse(void *a) { return php_array_reverse(a); }
int64_t __php_in_array(void *a, void *b) { return php_in_array(a, b); }
int64_t __php_array_search(void *a, void *b) { return php_array_search(a, b); }
void *__php_array_unique(void *a) { return php_array_unique(a); }
void *__php_array_shift(void *a, void *b) { return php_array_shift(a, b); }
void *__php_array_unshift(void *a, void *b) { return php_array_unshift(a, b); }
void *__php_sort(void *a) { return php_sort(a); }
void *__php_rsort(void *a) { return php_rsort(a); }
void *__php_array_slice(void *a, void *b, int64_t c, int64_t d) { return php_array_slice(a, b, c, d); }
void *__php_array_merge(void *a, void *b) { return php_array_merge(a, b); }
void *__php_array_column(void *a, void *b, int64_t c) { return php_array_column(a, b, c); }
// Aliases
void *__php_chop(void *a) { return php_rtrim(a); }  // chop = rtrim in PHP
}
