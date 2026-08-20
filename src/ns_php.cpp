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
#include <iterator>
#define DBG(x) do { if(madc_verbose){x;} } while(0)
#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "ns_common.h"

using namespace std;

// ---- C++ wrapper functions called by JIT ----

// php::trim — trim whitespace from both ends (no C/C++ equivalent)
std::string *php_trim(std::string *ptr)
{
	ns_common::trim(*ptr, true, true);
	return ptr;
}

// php::ltrim — trim whitespace from left
std::string *php_ltrim(std::string *ptr)
{
	ns_common::trim(*ptr, true, false);
	return ptr;
}

// php::rtrim — trim whitespace from right
std::string *php_rtrim(std::string *ptr)
{
	ns_common::trim(*ptr, false, true);
	return ptr;
}

// php::ucfirst — capitalize first character
std::string *php_ucfirst(std::string *ptr)
{
	std::string &s = *ptr;
	if ( !s.empty() )
		s[0] = toupper(s[0]);
	return ptr;
}

// php::lcfirst — lowercase first character
std::string *php_lcfirst(std::string *ptr)
{
	std::string &s = *ptr;
	if ( !s.empty() )
		s[0] = tolower(s[0]);
	return ptr;
}

// php::str_repeat — repeat string n times
std::string *php_str_repeat(std::string *ptr, int64_t count)
{
	ns_common::repeat(*ptr, count);
	return ptr;
}

// php::str_replace — replace all occurrences of search with replace in subject
std::string *php_str_replace(std::string *search, std::string *replace,
			     std::string *subject)
{
	ns_common::replace_all(*subject, *search, *replace);
	return subject;
}

// php::str_pad — pad string to a given length (default right-pad with spaces)
std::string *php_str_pad(std::string *ptr, int64_t length, std::string *pad_str)
{
	std::string &s = *ptr;
	std::string &pad = *pad_str;
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
int64_t php_str_word_count(std::string *ptr)
{
	std::string &s = *ptr;
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
std::string *php_nl2br(std::string *ptr)
{
	std::string &s = *ptr;
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
std::string *php_str_rot13(std::string *ptr)
{
	std::string &s = *ptr;
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
std::string *php_chunk_split(std::string *ptr, int64_t chunklen,
			     std::string *separator)
{
	std::string &s = *ptr;
	std::string &sep = *separator;
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
std::string *php_number_format(std::string *result, int64_t number,
			       std::string *thousands_sep)
{
	std::string &sep = *thousands_sep;
	std::string &res = *result;
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
std::string *php_wordwrap(std::string *ptr, int64_t width, std::string *brk)
{
	std::string &s = *ptr;
	std::string &br = *brk;
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


// ---- Array (madc::value) functions ----
// A script `array` is a madc::value: indexed data is kind::array. A
// never-touched array is kind::null and reads as empty; the mutating
// `array()` accessor vivifies null to an empty array.

// php::explode — split string by delimiter into array
void php_explode(madc::value *arr, const char *delim, const char *str)
{
	const char *s = (const char *)str;
	const char *d = (const char *)delim;
	ns_common::split_by_delim(*arr,
				  std::string(s ? s : ""),
				  std::string(d ? d : ""));
}

// php::implode — join array elements with glue string
std::string *php_implode(std::string *result, const char *glue, madc::value *arr)
{
	ns_common::join_with_sep(*result, *arr, std::string(glue ? glue : ""));
	return result;
}

// php::count — return number of elements
int64_t php_count(madc::value *arr)
{
	return (int64_t)ns_common::value_count(*arr);
}

// php::array_push — ONE overloaded name (PHP parity: no array_push_int
// madc-ism). Every entry returns the array's NEW element count, like PHP's
// array_push(). The count is read back through value_count — the one owner
// of element counting — so the degrade paths stay honest: a frozen carrier
// reports its unchanged count, a non-array carrier reports 0 (both after
// value_array_for_write's stderr notice).

// string-kind push
int64_t php_array_push_str(madc::value *arr, const char *str)
{
	const char *s = (const char *)str;
	ns_common::value_array_for_write(*arr, "php::array_push")
		.push_back(madc::value(std::string(s ? s : "")));
	return (int64_t)ns_common::value_count(*arr);
}

// integer-kind push
int64_t php_array_push_int(madc::value *arr, int64_t val)
{
	ns_common::value_array_for_write(*arr, "php::array_push")
		.push_back(madc::value(val));
	return (int64_t)ns_common::value_count(*arr);
}

// real-kind push
int64_t php_array_push_real(madc::value *arr, double val)
{
	ns_common::value_array_for_write(*arr, "php::array_push")
		.push_back(madc::value(val));
	return (int64_t)ns_common::value_count(*arr);
}

// boolean-kind push
int64_t php_array_push_bool(madc::value *arr, bool val)
{
	ns_common::value_array_for_write(*arr, "php::array_push")
		.push_back(madc::value(val));
	return (int64_t)ns_common::value_count(*arr);
}

// carrier push — a value of ANY kind, kind-preserving deep copy (the value
// copy ctor); pushing an array-kind carrier nests it. Serves both the
// script's (array&, value&) overload and the legacy __php_array_push_array
// plumbing symbol.
int64_t php_array_push_value(madc::value *arr, madc::value *value)
{
	ns_common::value_array_for_write(*arr, "php::array_push")
		.push_back(*value);
	return (int64_t)ns_common::value_count(*arr);
}

// php::array_pop — remove and return last element as string
std::string *php_array_pop(std::string *result, madc::value *arr)
{
	std::string &res = *result;
	res.clear();
	if ( !arr->is_array() || arr->as_array().empty() )
		return result;
	madc::value v = std::move(arr->array().back());
	arr->array().pop_back();
	ns_common::value_to_string_no_real(v, res);
	return result;
}

// The ONE nth-element resolver behind the range-for fetchers: array kind
// answers its vector, the OBJECT kind answers the nth VALUE in key order
// (std::map iteration order — deterministic; the keyed-access wave's L5
// contract, docs/plans/2026-08-20-adventure-430-plan.md). NULL when out of
// range or a non-container kind. Key access rides the keyed subscript /
// php::array_key_exists, never an index.
static const madc::value *php_container_nth(const madc::value *arr,
					    int64_t index)
{
	if ( !arr || index < 0 )
		return NULL;
	if ( arr->is_array() )
	{
		const std::vector<madc::value> &data = arr->as_array();
		return (size_t)index < data.size() ? &data[(size_t)index] : NULL;
	}
	if ( arr->is_object() )
	{
		const std::map<std::string, madc::value> &m = arr->as_object();
		if ( (size_t)index >= m.size() )
			return NULL;
		std::map<std::string, madc::value>::const_iterator it = m.begin();
		std::advance(it, (size_t)index);
		return &it->second;
	}
	return NULL;
}

// php::array_get — get element at integer index as string
std::string *php_array_get(std::string *result, madc::value *arr, int64_t index)
{
	std::string &res = *result;
	res.clear();
	if ( !arr->is_array() )
		return result;
	const std::vector<madc::value> &data = arr->as_array();
	if ( index < 0 || (size_t)index >= data.size() )
		return result;
	ns_common::value_to_string(data[(size_t)index], res);
	return result;
}

// php::array_get_int — get element at integer index as int (array kind and,
// per the L5 contract, the object kind's nth value in key order).
int64_t php_array_get_int(madc::value *arr, int64_t index)
{
	const madc::value *vp = php_container_nth(arr, index);
	if ( !vp )
		return 0;
	const madc::value &v = *vp;
	if ( v.is_integer() ) return v.as_integer();
	if ( v.is_real() ) return (int64_t)v.as_real();
	if ( v.is_string() ) { try { return std::stoll(v.as_string()); } catch(...) { return 0; } }
	return 0;
}

// Copy element `index` into `dst` — the range-for fill for a `value` LOOP
// ELEMENT (`for (value v : a)`), where the int/cstr fetchers above lose the
// element's kind. A COPY by design: elements have no stable address (the same
// model that refuses `value &v`), and value::operator= is the one owner of
// retagging + freeze rejection. Out of range / non-container resets dst to
// null — the generated loop is bounded by madarray_size so it cannot reach
// this, but a host calling directly must not read the previous iteration's
// value.
void php_array_get_value(madc::value *arr, int64_t index, madc::value *dst)
{
	if ( !dst )
		return;
	if ( const madc::value *v = php_container_nth(arr, index) )
		*dst = *v;
	else
		*dst = madc::value();
}

const char *php_array_get_cstr(madc::value *arr, int64_t index)
{
	thread_local std::string res;
	res.clear();
	if ( const madc::value *v = php_container_nth(arr, index) )
		ns_common::value_to_string(*v, res);
	return res.c_str();
}

// php::array_reverse — reverse array in place
void php_array_reverse(madc::value *arr)
{
	if ( !arr->is_array() )
		return;
	std::reverse(arr->array().begin(), arr->array().end());
}

// php::in_array — check if value exists in array (string comparison)
int64_t php_in_array(const char *needle, madc::value *arr)
{
	if ( !arr->is_array() )
		return 0;
	std::string n(needle ? needle : "");
	for ( auto &v : arr->as_array() )
		if ( v.is_string() && v.as_string() == n )
			return 1;
	return 0;
}

// php::array_key_exists — PHP parity (key first): does the key exist in the
// container? Object kind answers the map (string key, or the int key's
// decimal spelling — PHP coerces int keys); array kind answers the index
// range for an int key. Never vivifies — this is THE non-mutating
// existence question beside the vivifying keyed subscript
// (madarray_key_slot).
int64_t php_array_key_exists(const char *key, madc::value *arr)
{
	if ( !arr || !key )
		return 0;
	if ( arr->is_object() )
		return arr->as_object().count(key) ? 1 : 0;
	return 0;
}

int64_t php_array_key_exists_int(int64_t key, madc::value *arr)
{
	if ( !arr )
		return 0;
	if ( arr->is_array() )
		return (key >= 0 && (size_t)key < arr->as_array().size()) ? 1 : 0;
	if ( arr->is_object() )
		return arr->as_object().count(std::to_string(key)) ? 1 : 0;
	return 0;
}

// php::strtolower / php::strtoupper — PHP parity: byte-wise ASCII case
// transforms (locale-independent, PHP 8 semantics). Scalar kinds coerce
// to their text (PHP's coercive typing); containers have no PHP-legal
// text and answer "". Ring-lifetime returns (the c_str contract,
// value-first.md): the transform builds IN the lent ring slot — one
// bounded pass, zero temporary allocations at steady state.

// Fill the lent slot with the value's PHP text view (no temp string).
static std::string &php_text_slot(const madc::value *v)
{
	std::string &slot = ns_common::ring_slot();
	if ( !v || v->is_null() )
		slot.clear();
	else if ( v->is_string() )
		slot.assign((const char *)v->data(), v->size());
	else if ( !ns_common::value_to_string(*v, slot) )
		slot.clear();
	return slot;
}

static void php_ascii_case_inplace(std::string &s, bool up)
{
	for ( size_t i = 0; i < s.size(); ++i )
	{
		char c = s[i];
		if ( up && c >= 'a' && c <= 'z' )
			s[i] = (char)(c - 32);
		else if ( !up && c >= 'A' && c <= 'Z' )
			s[i] = (char)(c + 32);
	}
}

static const char *php_case_cstr(const char *s, bool up)
{
	std::string &slot = ns_common::ring_slot();
	slot = s ? s : "";
	php_ascii_case_inplace(slot, up);
	return slot.c_str();
}
static const char *php_case_value(const madc::value *v, bool up)
{
	std::string &slot = php_text_slot(v);
	php_ascii_case_inplace(slot, up);
	return slot.c_str();
}
const char *php_strtolower(const char *s)
	{ return php_case_cstr(s, false); }
const char *php_strtolower_value(const madc::value *v)
	{ return php_case_value(v, false); }
const char *php_strtoupper(const char *s)
	{ return php_case_cstr(s, true); }
const char *php_strtoupper_value(const madc::value *v)
	{ return php_case_value(v, true); }

// php::ucfirst — first byte uppercased if ASCII a-z (PHP semantics).
const char *php_ucfirst_cstr(const char *s)
{
	std::string &slot = ns_common::ring_slot();
	slot = s ? s : "";
	if ( !slot.empty() && slot[0] >= 'a' && slot[0] <= 'z' )
		slot[0] = (char)(slot[0] - 32);
	return slot.c_str();
}
const char *php_ucfirst_value(const madc::value *v)
{
	std::string &slot = php_text_slot(v);
	if ( !slot.empty() && slot[0] >= 'a' && slot[0] <= 'z' )
		slot[0] = (char)(slot[0] - 32);
	return slot.c_str();
}

// php::ctype_digit — PHP parity: true iff the text is non-empty and
// every byte is 0-9. A non-string value answers false (PHP 8 dropped
// the legacy int-argument mode). Reads the payload directly — no copy.
int64_t php_ctype_digit(const char *s)
{
	if ( !s || !*s )
		return 0;
	for ( ; *s; ++s )
		if ( *s < '0' || *s > '9' )
			return 0;
	return 1;
}
int64_t php_ctype_digit_value(const madc::value *v)
{
	if ( !v || !v->is_string() || v->size() == 0 )
		return 0;
	const char *p = (const char *)v->data();
	size_t n = v->size();
	for ( size_t i = 0; i < n; ++i )
		if ( p[i] < '0' || p[i] > '9' )
			return 0;
	return 1;
}

// php::array_search — find index of value in array, returns -1 if not found
int64_t php_array_search(const char *needle, madc::value *arr)
{
	if ( !arr->is_array() )
		return -1;
	std::string n(needle ? needle : "");
	const std::vector<madc::value> &data = arr->as_array();
	for ( size_t i = 0; i < data.size(); ++i )
		if ( data[i].is_string() && data[i].as_string() == n )
			return (int64_t)i;
	return -1;
}

// php::array_unique — remove duplicate string values
void php_array_unique(madc::value *arr)
{
	if ( !arr->is_array() )
		return;
	std::vector<madc::value> unique;
	for ( auto &v : arr->as_array() )
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
	arr->array() = std::move(unique);
}

// php::array_shift — remove first element, shift rest down
std::string *php_array_shift(std::string *result, madc::value *arr)
{
	std::string &res = *result;
	res.clear();
	if ( !arr->is_array() || arr->as_array().empty() )
		return result;
	madc::value v = std::move(arr->array().front());
	arr->array().erase(arr->array().begin());
	ns_common::value_to_string_no_real(v, res);
	return result;
}

// php::array_unshift — prepend value to beginning
void php_array_unshift(madc::value *arr, const char *str)
{
	const char *s = (const char *)str;
	std::vector<madc::value> &data
		= ns_common::value_array_for_write(*arr, "php::array_unshift");
	data.insert(data.begin(), madc::value(std::string(s ? s : "")));
}

// php::sort — sort array (string comparison)
void php_sort(madc::value *arr)
{
	if ( !arr->is_array() )
		return;
	std::vector<madc::value> &data = arr->array();
	std::sort(data.begin(), data.end(), [](const madc::value &a, const madc::value &b) {
		if ( a.is_string() && b.is_string() )
			return a.as_string() < b.as_string();
		if ( a.is_integer() && b.is_integer() )
			return a.as_integer() < b.as_integer();
		return false;
	});
}

// php::rsort — sort array in reverse
void php_rsort(madc::value *arr)
{
	php_sort(arr);
	php_array_reverse(arr);
}

// php::array_slice — extract a slice of the array
void php_array_slice(madc::value *dest, madc::value *src, int64_t offset, int64_t length)
{
	*dest = madc::value::make_array();
	if ( !src->is_array() )
		return;
	const std::vector<madc::value> &s = src->as_array();
	if ( offset < 0 ) offset = (int64_t)s.size() + offset;
	if ( offset < 0 ) offset = 0;
	if ( length < 0 ) length = (int64_t)s.size() + length - offset;
	if ( length < 0 ) length = 0;
	for ( int64_t i = offset; i < offset + length && (size_t)i < s.size(); ++i )
		dest->array().push_back(s[(size_t)i]);
}

// php::array_merge — merge two arrays
void php_array_merge(madc::value *dest, madc::value *src)
{
	if ( !src->is_array() )
		return;
	std::vector<madc::value> &d
		= ns_common::value_array_for_write(*dest, "php::array_merge");
	for ( auto &v : src->as_array() )
		d.push_back(v);
}


// php::array_column — extract one integer-indexed column from nested arrays
void php_array_column(madc::value *dest, madc::value *src, int64_t column_index)
{
	*dest = madc::value::make_array();
	if ( column_index < 0 || !src->is_array() )
		return;
	for ( auto &row : src->as_array() )
	{
		if ( !row.is_array() )
			continue;
		const std::vector<madc::value> &row_arr = row.as_array();
		size_t idx = (size_t)column_index;
		if ( idx >= row_arr.size() )
			continue;
		std::string value;
		if ( ns_common::value_to_string(row_arr[idx], value) )
			dest->array().push_back(madc::value(value));
	}
}

namespace php {

std::string &trim(std::string &s) { return *php_trim(&s); }
std::string &ltrim(std::string &s) { return *php_ltrim(&s); }
std::string &rtrim(std::string &s) { return *php_rtrim(&s); }
std::string &chop(std::string &s) { return rtrim(s); }
std::string &ucfirst(std::string &s) { return *php_ucfirst(&s); }
std::string &lcfirst(std::string &s) { return *php_lcfirst(&s); }
std::string &str_repeat(std::string &s, int64_t count) { return *php_str_repeat(&s, count); }
std::string &str_replace(std::string &search, std::string &replace, std::string &subject)
	{ return *php_str_replace(&search, &replace, &subject); }
std::string &str_pad(std::string &s, int64_t length, std::string &pad)
	{ return *php_str_pad(&s, length, &pad); }
int64_t str_word_count(std::string &s) { return php_str_word_count(&s); }
std::string &nl2br(std::string &s) { return *php_nl2br(&s); }
std::string &str_rot13(std::string &s) { return *php_str_rot13(&s); }
std::string &chunk_split(std::string &s, int64_t chunklen, std::string &separator)
	{ return *php_chunk_split(&s, chunklen, &separator); }
std::string &number_format(std::string &result, int64_t number, std::string &separator)
	{ return *php_number_format(&result, number, &separator); }
std::string &wordwrap(std::string &s, int64_t width, std::string &separator)
	{ return *php_wordwrap(&s, width, &separator); }

}

extern "C" {
// Thin C-linkage convenience wrappers over the C++ namespace surface.
std::string *__php_trim(std::string *a) { return &php::trim(*a); }
std::string *__php_ltrim(std::string *a) { return &php::ltrim(*a); }
std::string *__php_rtrim(std::string *a) { return &php::rtrim(*a); }
std::string *__php_ucfirst(std::string *a) { return &php::ucfirst(*a); }
std::string *__php_lcfirst(std::string *a) { return &php::lcfirst(*a); }
std::string *__php_str_repeat(std::string *a, int64_t b) { return &php::str_repeat(*a, b); }
std::string *__php_str_replace(std::string *a, std::string *b, std::string *c) { return &php::str_replace(*a, *b, *c); }
std::string *__php_str_pad(std::string *a, int64_t b, std::string *c) { return &php::str_pad(*a, b, *c); }
int64_t __php_str_word_count(std::string *a) { return php::str_word_count(*a); }
std::string *__php_nl2br(std::string *a) { return &php::nl2br(*a); }
std::string *__php_str_rot13(std::string *a) { return &php::str_rot13(*a); }
std::string *__php_chunk_split(std::string *a, int64_t b, std::string *c) { return &php::chunk_split(*a, b, *c); }
std::string *__php_number_format(std::string *a, int64_t b, std::string *c) { return &php::number_format(*a, b, *c); }
std::string *__php_wordwrap(std::string *a, int64_t b, std::string *c) { return &php::wordwrap(*a, b, *c); }
void __php_explode(madc::value *a, const char *b, const char *c) { php_explode(a, b, c); }
std::string *__php_implode(std::string *a, const char *b, madc::value *c) { return php_implode(a, b, c); }
int64_t __php_count(madc::value *a) { return php_count(a); }
int64_t __php_array_push(madc::value *a, const char *b) { return php_array_push_str(a, b); }
int64_t __php_array_push_int(madc::value *a, int64_t b) { return php_array_push_int(a, b); }
int64_t __php_array_push_real(madc::value *a, double b) { return php_array_push_real(a, b); }
int64_t __php_array_push_bool(madc::value *a, bool b) { return php_array_push_bool(a, b); }
int64_t __php_array_push_array(madc::value *a, madc::value *b) { return php_array_push_value(a, b); }
int64_t __php_array_push_value(madc::value *a, madc::value *b) { return php_array_push_value(a, b); }
std::string *__php_array_pop(std::string *a, madc::value *b) { return php_array_pop(a, b); }
std::string *__php_array_get(std::string *a, madc::value *b, int64_t c) { return php_array_get(a, b, c); }
int64_t __php_array_get_int(madc::value *a, int64_t b) { return php_array_get_int(a, b); }
const char *__php_array_get_cstr(madc::value *a, int64_t b) { return php_array_get_cstr(a, b); }
void __php_array_get_value(madc::value *a, int64_t b, madc::value *c) { php_array_get_value(a, b, c); }
void __php_array_reverse(madc::value *a) { php_array_reverse(a); }
int64_t __php_in_array(const char *a, madc::value *b) { return php_in_array(a, b); }
int64_t __php_array_key_exists(const char *a, madc::value *b) { return php_array_key_exists(a, b); }
int64_t __php_array_key_exists_int(int64_t a, madc::value *b) { return php_array_key_exists_int(a, b); }
const char *__php_strtolower(madc::value *a) { return php_strtolower_value(a); }
const char *__php_strtolower_cstr(const char *a) { return php_strtolower(a); }
const char *__php_strtoupper(madc::value *a) { return php_strtoupper_value(a); }
const char *__php_strtoupper_cstr(const char *a) { return php_strtoupper(a); }
const char *__php_ucfirst_value(madc::value *a) { return php_ucfirst_value(a); }
const char *__php_ucfirst_cstr2(const char *a) { return php_ucfirst_cstr(a); }
int64_t __php_ctype_digit(madc::value *a) { return php_ctype_digit_value(a); }
int64_t __php_ctype_digit_cstr(const char *a) { return php_ctype_digit(a); }
int64_t __php_array_search(const char *a, madc::value *b) { return php_array_search(a, b); }
void __php_array_unique(madc::value *a) { php_array_unique(a); }
std::string *__php_array_shift(std::string *a, madc::value *b) { return php_array_shift(a, b); }
void __php_array_unshift(madc::value *a, const char *b) { php_array_unshift(a, b); }
void __php_sort(madc::value *a) { php_sort(a); }
void __php_rsort(madc::value *a) { php_rsort(a); }
void __php_array_slice(madc::value *a, madc::value *b, int64_t c, int64_t d) { php_array_slice(a, b, c, d); }
void __php_array_merge(madc::value *a, madc::value *b) { php_array_merge(a, b); }
void __php_array_column(madc::value *a, madc::value *b, int64_t c) { php_array_column(a, b, c); }
// Aliases
std::string *__php_chop(std::string *a) { return &php::chop(*a); }  // chop = rtrim in PHP
}
