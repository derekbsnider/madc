///////////////////////////////////////////////////////////////////////////
//                                                                     //
// madc js:: namespace                                                 //
//                                                                     //
// JavaScript/web-oriented functions: base64, URL encoding, JSON,      //
// parseInt with radix.                                                //
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

// ---- Base64 tables ----

static const char b64_encode_table[] =
	"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

static int b64_decode_char(char c)
{
	if (c >= 'A' && c <= 'Z') return c - 'A';
	if (c >= 'a' && c <= 'z') return c - 'a' + 26;
	if (c >= '0' && c <= '9') return c - '0' + 52;
	if (c == '+') return 62;
	if (c == '/') return 63;
	return -1;
}

// ---- C++ wrapper functions ----

// js::btoa — base64 encode
void *js_btoa(void *result, void *input)
{
	std::string &in = *(std::string *)input;
	std::string &out = *(std::string *)result;
	out.clear();
	for ( size_t i = 0; i < in.length(); i += 3 )
	{
		size_t remain = in.length() - i;
		unsigned char b0 = (unsigned char)in[i];
		unsigned char b1 = remain > 1 ? (unsigned char)in[i+1] : 0;
		unsigned char b2 = remain > 2 ? (unsigned char)in[i+2] : 0;

		out += b64_encode_table[(b0 >> 2) & 0x3F];
		out += b64_encode_table[((b0 & 0x03) << 4) | ((b1 >> 4) & 0x0F)];
		out += remain > 1 ? b64_encode_table[((b1 & 0x0F) << 2) | ((b2 >> 6) & 0x03)] : '=';
		out += remain > 2 ? b64_encode_table[b2 & 0x3F] : '=';
	}
	return result;
}

// js::atob — base64 decode
void *js_atob(void *result, void *input)
{
	std::string &in = *(std::string *)input;
	std::string &out = *(std::string *)result;
	out.clear();
	size_t i = 0;
	while ( i < in.length() )
	{
		int b[4] = {0, 0, 0, 0};
		size_t n = 0;
		for ( size_t j = 0; j < 4 && i < in.length(); ++j )
		{
			while ( i < in.length() && in[i] == '=' ) ++i;
			if ( i < in.length() )
			{
				b[j] = b64_decode_char(in[i++]);
				if ( b[j] >= 0 ) ++n;
			}
		}
		if ( n >= 2 ) out += (char)((b[0] << 2) | (b[1] >> 4));
		if ( n >= 3 ) out += (char)(((b[1] & 0x0F) << 4) | (b[2] >> 2));
		if ( n >= 4 ) out += (char)(((b[2] & 0x03) << 6) | b[3]);
	}
	return result;
}

// js::encodeURIComponent — URL-encode a string
void *js_encodeURIComponent(void *result, void *input)
{
	std::string &in = *(std::string *)input;
	std::string &out = *(std::string *)result;
	out.clear();
	out.reserve(in.length() * 3);
	for ( size_t i = 0; i < in.length(); ++i )
	{
		char c = in[i];
		if ( isalnum(c) || c == '-' || c == '_' || c == '.' || c == '~' )
			out += c;
		else
		{
			char hex[4];
			snprintf(hex, sizeof(hex), "%%%02X", (unsigned char)c);
			out += hex;
		}
	}
	return result;
}

// js::decodeURIComponent — URL-decode a string
void *js_decodeURIComponent(void *result, void *input)
{
	std::string &in = *(std::string *)input;
	std::string &out = *(std::string *)result;
	out.clear();
	for ( size_t i = 0; i < in.length(); ++i )
	{
		if ( in[i] == '%' && i + 2 < in.length() )
		{
			int hi = 0, lo = 0;
			char h = in[i+1], l = in[i+2];
			if ( h >= '0' && h <= '9' ) hi = h - '0';
			else if ( h >= 'A' && h <= 'F' ) hi = h - 'A' + 10;
			else if ( h >= 'a' && h <= 'f' ) hi = h - 'a' + 10;
			if ( l >= '0' && l <= '9' ) lo = l - '0';
			else if ( l >= 'A' && l <= 'F' ) lo = l - 'A' + 10;
			else if ( l >= 'a' && l <= 'f' ) lo = l - 'a' + 10;
			out += (char)((hi << 4) | lo);
			i += 2;
		}
		else if ( in[i] == '+' )
			out += ' ';
		else
			out += in[i];
	}
	return result;
}

// js::parseInt — parse integer with radix support
int64_t js_parseInt(void *str, int64_t radix)
{
	std::string &s = *(std::string *)str;
	if ( radix < 2 || radix > 36 ) radix = 10;
	try { return std::stoll(s, nullptr, (int)radix); }
	catch (...) { return 0; }
}

// js::stringify — simple JSON-like serialization of a MadArray
void *js_stringify(void *result, void *arr)
{
	MadArray &a = *(MadArray *)arr;
	std::string &out = *(std::string *)result;
	out = "[";
	for ( size_t i = 0; i < a.data.size(); ++i )
	{
		if ( i > 0 ) out += ",";
		if ( a.data[i].is_string() )
		{
			out += "\"";
			// escape special characters
			for ( char c : a.data[i].as_string() )
			{
				if ( c == '"' ) out += "\\\"";
				else if ( c == '\\' ) out += "\\\\";
				else if ( c == '\n' ) out += "\\n";
				else if ( c == '\t' ) out += "\\t";
				else out += c;
			}
			out += "\"";
		}
		else if ( a.data[i].is_int() )
			out += std::to_string(a.data[i].as_int());
		else if ( a.data[i].is_double() )
			out += std::to_string(a.data[i].as_double());
		else
			out += "null";
	}
	out += "]";
	return result;
}

// js::typeof — return type name as string
void *js_typeof(void *result, void *val)
{
	// For now, works with strings — returns "string"
	std::string &res = *(std::string *)result;
	res = "string";
	return result;
}

// js::typeof_int — type check for integers
void *js_typeof_int(void *result, int64_t val)
{
	std::string &res = *(std::string *)result;
	res = "number";
	return result;
}


// ---- Namespace registration ----

void Program::add_js_namespace()
{
	variable_map_t &js_ns = namespace_map["js"];
	Variable *var;

	// base64
	var = addFunction("__js_btoa",               datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)js_btoa);
	if (var) js_ns["btoa"] = var;

	var = addFunction("__js_atob",               datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)js_atob);
	if (var) js_ns["atob"] = var;

	// URL encoding
	var = addFunction("__js_encodeURIComponent", datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)js_encodeURIComponent);
	if (var) js_ns["encodeURIComponent"] = var;

	var = addFunction("__js_decodeURIComponent", datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtSTRING}, (fVOIDFUNC)js_decodeURIComponent);
	if (var) js_ns["decodeURIComponent"] = var;

	// parsing
	var = addFunction("__js_parseInt",           datatype_vec_t{DataType::dtINT64, DataType::dtSTRING, DataType::dtINT64}, (fVOIDFUNC)js_parseInt);
	if (var) js_ns["parseInt"] = var;

	// JSON
	var = addFunction("__js_stringify",          datatype_vec_t{DataType::dtSTRING, DataType::dtSTRING, DataType::dtARRAY}, (fVOIDFUNC)js_stringify);
	if (var) js_ns["stringify"] = var;

	DBG(std::cout << "add_js_namespace() registered js:: with " << js_ns.size() << " members" << std::endl);
}

extern "C" {
// Thin C-linkage wrappers for transpiler import resolution
void *__js_btoa(void *a, void *b) { return js_btoa(a, b); }
void *__js_atob(void *a, void *b) { return js_atob(a, b); }
void *__js_encodeURIComponent(void *a, void *b) { return js_encodeURIComponent(a, b); }
void *__js_decodeURIComponent(void *a, void *b) { return js_decodeURIComponent(a, b); }
int64_t __js_parseInt(void *a, int64_t b) { return js_parseInt(a, b); }
void *__js_stringify(void *a, void *b) { return js_stringify(a, b); }
}
