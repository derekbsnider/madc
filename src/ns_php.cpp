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

using namespace std;
using namespace asmjit;

// ---- C++ wrapper functions called by JIT ----

// php::trim — trim whitespace from both ends (no C/C++ equivalent)
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
	std::string &s = *(std::string *)ptr;
	std::string orig = s;
	s.clear();
	s.reserve(orig.length() * count);
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
	if ( srch.empty() ) return subject;
	size_t pos = 0;
	while ( (pos = subj.find(srch, pos)) != std::string::npos )
	{
		subj.replace(pos, srch.length(), repl);
		pos += repl.length();
	}
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


// php::explode — extract the Nth piece of a string split by delimiter
// (full array version needs array type; this is the indexed accessor)
void *php_explode(void *result, void *delim, void *str, int64_t index)
{
	std::string &d = *(std::string *)delim;
	std::string &s = *(std::string *)str;
	std::string &res = *(std::string *)result;
	res.clear();
	if ( d.empty() ) { res = s; return result; }
	size_t start = 0, end;
	int64_t i = 0;
	while ( (end = s.find(d, start)) != std::string::npos )
	{
		if ( i == index ) { res = s.substr(start, end - start); return result; }
		start = end + d.length();
		++i;
	}
	if ( i == index ) res = s.substr(start);
	return result;
}

// php::explode_count — count how many pieces a split would produce
int64_t php_explode_count(void *delim, void *str)
{
	std::string &d = *(std::string *)delim;
	std::string &s = *(std::string *)str;
	if ( d.empty() ) return 1;
	int64_t count = 1;
	size_t pos = 0;
	while ( (pos = s.find(d, pos)) != std::string::npos )
	{
		++count;
		pos += d.length();
	}
	return count;
}

// ---- Namespace registration ----

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
	if (var) php_ns["rtrim"] = var;

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

	// explode — indexed accessor until arrays are implemented
	// explode(result, delim, str, index) — get Nth piece
	var = addFunction("__php_explode",        datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING, DataType::dtINT64}, (fVOIDFUNC)php_explode);
	if (var) php_ns["explode"] = var;

	var = addFunction("__php_explode_count",  datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)php_explode_count);
	if (var) php_ns["explode_count"] = var;

	DBG(std::cout << "add_php_namespace() registered php:: with " << php_ns.size() << " members" << std::endl);
}
