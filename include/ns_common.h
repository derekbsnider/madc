#ifndef __NS_COMMON_H
#define __NS_COMMON_H 1
//////////////////////////////////////////////////////////////////////////
//									//
// ns_common — shared string/array helpers for the user-facing		//
// php::, python::, ruby::, rust:: namespaces.				//
//									//
// The user-facing namespaces stay distinct; only the underlying	//
// implementation logic is unified here. Each helper takes references	//
// to std::string / MadArray / MadValue, never the void* shim layer.	//
//									//
//////////////////////////////////////////////////////////////////////////

#include <string>
#include <cstdint>

#include "datadef.h"

namespace ns_common {

// ---- String mutators --------------------------------------------------

// Trim whitespace (" \t\n\r\f\v") from `s`. left=true trims leading,
// right=true trims trailing. Both true == full trim.
void trim(std::string &s, bool left, bool right);

// Replace every non-overlapping occurrence of `needle` in `s` with `repl`.
// Returns the number of replacements made. No-op (returns 0) when needle
// is empty.
size_t replace_all(std::string &s, const std::string &needle,
		   const std::string &repl);

// Replace the first occurrence of `needle` in `s` with `repl`. Returns
// true if a replacement was made. No-op when needle is empty.
bool replace_first(std::string &s, const std::string &needle,
		   const std::string &repl);

// Repeat `s` `count` times in place. count <= 0 clears `s`.
void repeat(std::string &s, int64_t count);

// ---- String predicates ------------------------------------------------

bool starts_with(const std::string &s, const std::string &prefix);
bool ends_with  (const std::string &s, const std::string &suffix);
bool contains   (const std::string &s, const std::string &needle);

// ---- MadValue stringification ----------------------------------------

// Stringify a MadValue for join/column-style output. Handles string,
// int, and double. Returns true on a recognized type (out is set);
// returns false otherwise (out is left untouched). Callers decide
// whether to clear out or skip the value.
bool value_to_string(const MadValue &v, std::string &out);

// ---- Array helpers ---------------------------------------------------

// Split `s` by literal `delim` and store the pieces as strings in `out`.
// `out` is cleared first. An empty delim pushes the whole string as a
// single element.
void split_by_delim(MadArray &out, const std::string &s,
		    const std::string &delim);

// Join `arr`'s string-coercible elements with `sep` into `out`. `out`
// is cleared first. Elements that are not string/int/double are
// skipped silently (matches the prior php_implode / rust_join shape).
void join_with_sep(std::string &out, const MadArray &arr,
		   const std::string &sep);

}  // namespace ns_common

#endif // __NS_COMMON_H
