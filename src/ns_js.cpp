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
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"

using namespace std;

static std::string js_text_arg(const char *ptr)
{
	return std::string(ptr ? ptr : "");
}

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
std::string *js_btoa(std::string *result, const char *input)
{
	std::string in = js_text_arg(input);
	std::string &out = *result;
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
std::string *js_atob(std::string *result, const char *input)
{
	std::string in = js_text_arg(input);
	std::string &out = *result;
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
std::string *js_encodeURIComponent(std::string *result, const char *input)
{
	std::string in = js_text_arg(input);
	std::string &out = *result;
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
std::string *js_decodeURIComponent(std::string *result, const char *input)
{
	std::string in = js_text_arg(input);
	std::string &out = *result;
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
int64_t js_parseInt(const char *str, int64_t radix)
{
	std::string s = js_text_arg(str);
	if ( radix < 2 || radix > 36 ) radix = 10;
	try { return std::stoll(s, nullptr, (int)radix); }
	catch (...) { return 0; }
}

// js::stringify — simple JSON-like serialization of a madc array value
std::string *js_stringify(std::string *result, madc::value *arr)
{
	std::string &out = *result;
	out = "[";
	const std::vector<madc::value> empty;
	const std::vector<madc::value> &data = arr->is_array() ? arr->as_array()
							       : empty;
	for ( size_t i = 0; i < data.size(); ++i )
	{
		if ( i > 0 ) out += ",";
		if ( data[i].is_string() )
		{
			out += "\"";
			// escape special characters
			for ( char c : data[i].as_string() )
			{
				if ( c == '"' ) out += "\\\"";
				else if ( c == '\\' ) out += "\\\\";
				else if ( c == '\n' ) out += "\\n";
				else if ( c == '\t' ) out += "\\t";
				else out += c;
			}
			out += "\"";
		}
		else if ( data[i].is_integer() )
			out += std::to_string(data[i].as_integer());
		else if ( data[i].is_real() )
			out += std::to_string(data[i].as_real());
		else
			out += "null";
	}
	out += "]";
	return result;
}

// js::typeof — return type name as string
std::string *js_typeof(std::string *result, const char *val)
{
	// For now, works with strings — returns "string"
	std::string &res = *result;
	res = "string";
	return result;
}

// js::typeof_int — type check for integers
std::string *js_typeof_int(std::string *result, int64_t val)
{
	std::string &res = *result;
	res = "number";
	return result;
}


extern "C" {
// Thin C-linkage wrappers for transpiler import resolution
std::string *__js_btoa(std::string *a, const char *b) { return js_btoa(a, b); }
std::string *__js_atob(std::string *a, const char *b) { return js_atob(a, b); }
std::string *__js_encodeURIComponent(std::string *a, const char *b) { return js_encodeURIComponent(a, b); }
std::string *__js_decodeURIComponent(std::string *a, const char *b) { return js_decodeURIComponent(a, b); }
int64_t __js_parseInt(const char *a, int64_t b) { return js_parseInt(a, b); }
std::string *__js_stringify(std::string *a, madc::value *b) { return js_stringify(a, b); }
}
