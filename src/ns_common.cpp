//////////////////////////////////////////////////////////////////////////
//									//
// ns_common.cpp — shared implementation of helpers used by the		//
// php::, python::, ruby::, and rust:: namespaces.			//
//									//
//////////////////////////////////////////////////////////////////////////

#include <string>
#include <cstring>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <vector>
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "ns_common.h"

namespace ns_common {

static const char *const WHITESPACE = " \t\n\r\f\v";

void trim(std::string &s, bool left, bool right)
{
	if ( !left && !right ) return;

	size_t start = 0;
	size_t end = s.length();

	if ( left )
	{
		start = s.find_first_not_of(WHITESPACE);
		if ( start == std::string::npos )
		{
			s.clear();
			return;
		}
	}

	if ( right )
	{
		size_t last = s.find_last_not_of(WHITESPACE);
		if ( last == std::string::npos )
		{
			s.clear();
			return;
		}
		end = last + 1;
	}

	s = s.substr(start, end - start);
}

size_t replace_all(std::string &s, const std::string &needle,
		   const std::string &repl)
{
	if ( needle.empty() ) return 0;
	size_t pos = 0;
	size_t count = 0;
	while ( (pos = s.find(needle, pos)) != std::string::npos )
	{
		s.replace(pos, needle.length(), repl);
		pos += repl.length();
		++count;
	}
	return count;
}

bool replace_first(std::string &s, const std::string &needle,
		   const std::string &repl)
{
	if ( needle.empty() ) return false;
	size_t pos = s.find(needle);
	if ( pos == std::string::npos ) return false;
	s.replace(pos, needle.length(), repl);
	return true;
}

void repeat(std::string &s, int64_t count)
{
	if ( count <= 0 )
	{
		s.clear();
		return;
	}
	std::string orig = s;
	s.clear();
	s.reserve(orig.length() * (size_t)count);
	for ( int64_t i = 0; i < count; ++i )
		s += orig;
}

bool starts_with(const std::string &s, const std::string &prefix)
{
	return s.length() >= prefix.length()
	    && s.compare(0, prefix.length(), prefix) == 0;
}

bool ends_with(const std::string &s, const std::string &suffix)
{
	return s.length() >= suffix.length()
	    && s.compare(s.length() - suffix.length(), suffix.length(),
			 suffix) == 0;
}

bool contains(const std::string &s, const std::string &needle)
{
	return s.find(needle) != std::string::npos;
}

bool value_to_string(const madc::value &v, std::string &out)
{
	if ( v.is_string() )
	{
		out = v.as_string();
		return true;
	}
	if ( v.is_integer() )
	{
		out = std::to_string(v.as_integer());
		return true;
	}
	if ( v.is_real() )
	{
		out = std::to_string(v.as_real());
		return true;
	}
	return false;
}

bool value_to_string_no_real(const madc::value &v, std::string &out)
{
	if ( v.is_string() )
	{
		out = v.as_string();
		return true;
	}
	if ( v.is_integer() )
	{
		out = std::to_string(v.as_integer());
		return true;
	}
	return false;
}

static void report_kind_mismatch(const madc::value &v, const char *who,
				 const char *wanted)
{
	std::cerr << (who ? who : "madc array helper")
		  << ": value is " << madc::value::kind_name(v.type())
		  << ", not " << wanted << " — write ignored" << std::endl;
}

// Frozen (MADC_VF_CONST) target: the LOUD write-rejection path for script
// runtime wrappers. Mirrors the kind-mismatch convention (stderr + redirect
// to a dummy) so no C++ exception crosses the extern-C boundary into JIT
// frames; the madc::value methods themselves throw for C++ hosts.
static void report_frozen(const char *who)
{
	std::cerr << (who ? who : "madc array helper")
		  << ": value is frozen (read-only) — write ignored"
		  << std::endl;
}

std::vector<madc::value> &value_array_for_write(madc::value &v,
						const char *who)
{
	if ( v.is_frozen() )
	{
		report_frozen(who);
		thread_local madc::value dummy;
		dummy = madc::value::make_array();
		return dummy.array();
	}
	if ( v.is_null() || v.is_array() )
		return v.array();
	report_kind_mismatch(v, who, "an array");
	thread_local madc::value dummy;
	dummy = madc::value::make_array();
	return dummy.array();
}

std::map<std::string, madc::value> &value_object_for_write(madc::value &v,
							    const char *who)
{
	if ( v.is_frozen() )
	{
		report_frozen(who);
		thread_local madc::value dummy;
		dummy = madc::value::make_object();
		return dummy.object();
	}
	if ( v.is_null() || v.is_object() )
		return v.object();
	report_kind_mismatch(v, who, "an object");
	thread_local madc::value dummy;
	dummy = madc::value::make_object();
	return dummy.object();
}

std::vector<madc::value> &value_array_reset_for_write(madc::value &v,
						      const char *who)
{
	if ( v.is_frozen() )
	{
		report_frozen(who);
		thread_local madc::value dummy;
		dummy = madc::value::make_array();
		return dummy.array();
	}
	v = madc::value::make_array();
	return v.array();
}

size_t value_count(const madc::value &v)
{
	if ( v.is_array() )
		return v.as_array().size();
	if ( v.is_object() )
		return v.as_object().size();
	return 0;
}

size_t value_length(const madc::value &v, bool *ok)
{
	if ( ok )
		*ok = true;
	switch ( v.type() )
	{
	case madc::value::kind::array:
		return v.as_array().size();
	case madc::value::kind::object:
		return v.as_object().size();
	case madc::value::kind::string:
	case madc::value::kind::bytes:
		// The payload byte count IS the length for the text kinds.
		return v.size();
	case madc::value::kind::null:
		// An unfilled carrier is an EMPTY container (see header).
		return 0;
	case madc::value::kind::boolean:
	case madc::value::kind::integer:
	case madc::value::kind::real:
	case madc::value::kind::instance:
		break;
	}
	if ( ok )
		*ok = false;
	return 0;
}

void split_by_delim(madc::value &out, const std::string &s,
		    const std::string &delim, const char *who)
{
	std::vector<madc::value> &data = value_array_reset_for_write(out, who);
	if ( delim.empty() )
	{
		data.push_back(madc::value(s));
		return;
	}
	size_t start = 0;
	size_t end;
	while ( (end = s.find(delim, start)) != std::string::npos )
	{
		data.push_back(madc::value(s.substr(start, end - start)));
		start = end + delim.length();
	}
	data.push_back(madc::value(s.substr(start)));
}

void join_with_sep(std::string &out, const madc::value &arr,
		   const std::string &sep)
{
	out.clear();
	if ( !arr.is_array() )
		return;
	const std::vector<madc::value> &data = arr.as_array();
	std::string tmp;
	for ( size_t i = 0; i < data.size(); ++i )
	{
		if ( i > 0 ) out += sep;
		if ( value_to_string(data[i], tmp) )
			out += tmp;
	}
}

std::string &ring_slot()
{
	thread_local std::string ring[8];
	thread_local unsigned ring_i = 0;
	return ring[ring_i++ & 7u];
}

const char *ring_text(std::string s)
{
	std::string &slot = ring_slot();
	slot = std::move(s);
	return slot.c_str();
}

std::string &value_text_slot(const madc::value *v)
{
	std::string &slot = ring_slot();
	if ( !v || v->is_null() )
		slot.clear();
	else if ( v->is_string() )
		slot.assign((const char *)v->data(), v->size());
	else if ( !value_to_string(*v, slot) )
		slot.clear();
	return slot;
}

const char *ring_apply(const char *s, std::string *(*core)(std::string *))
{
	std::string &slot = ring_slot();
	slot = s ? s : "";
	core(&slot);
	return slot.c_str();
}

const char *ring_apply(const madc::value *v,
		       std::string *(*core)(std::string *))
{
	std::string &slot = value_text_slot(v);
	core(&slot);
	return slot.c_str();
}

bool value_pop_element(madc::value &arr, madc::value &dst, const char *who)
{
	std::vector<madc::value> &data = value_array_for_write(arr, who);
	if ( data.empty() )
	{
		dst = madc::value();
		return false;
	}
	dst = std::move(data.back());
	data.pop_back();
	return true;
}

bool value_shift_element(madc::value &arr, madc::value &dst, const char *who)
{
	std::vector<madc::value> &data = value_array_for_write(arr, who);
	if ( data.empty() )
	{
		dst = madc::value();
		return false;
	}
	dst = std::move(data.front());
	data.erase(data.begin());
	return true;
}

}  // namespace ns_common
