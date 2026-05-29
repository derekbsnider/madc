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

bool value_to_string(const MadValue &v, std::string &out)
{
	if ( v.is_string() )
	{
		out = v.as_string();
		return true;
	}
	if ( v.is_int() )
	{
		out = std::to_string(v.as_int());
		return true;
	}
	if ( v.is_double() )
	{
		out = std::to_string(v.as_double());
		return true;
	}
	return false;
}

void split_by_delim(MadArray &out, const std::string &s,
		    const std::string &delim)
{
	out.data.clear();
	out.assoc.clear();
	if ( delim.empty() )
	{
		out.push(MadValue(s));
		return;
	}
	size_t start = 0;
	size_t end;
	while ( (end = s.find(delim, start)) != std::string::npos )
	{
		out.push(MadValue(s.substr(start, end - start)));
		start = end + delim.length();
	}
	out.push(MadValue(s.substr(start)));
}

void join_with_sep(std::string &out, const MadArray &arr,
		   const std::string &sep)
{
	out.clear();
	std::string tmp;
	for ( size_t i = 0; i < arr.data.size(); ++i )
	{
		if ( i > 0 ) out += sep;
		if ( value_to_string(arr.data[i], tmp) )
			out += tmp;
	}
}

}  // namespace ns_common
