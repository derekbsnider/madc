#ifndef __NS_COMMON_H
#define __NS_COMMON_H 1
//////////////////////////////////////////////////////////////////////////
//									//
// ns_common — shared string/array helpers for the user-facing		//
// php::, python::, ruby::, rust:: namespaces.				//
//									//
// The user-facing namespaces stay distinct; only the underlying	//
// implementation logic is unified here. Each helper takes references	//
// to std::string / madc::value, never the void* shim layer.		//
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

// ---- madc::value stringification --------------------------------------

// Stringify a madc::value for join/column-style output. Handles string,
// integer, and real. Returns true on a recognized kind (out is set);
// returns false otherwise (out is left untouched). Callers decide
// whether to clear out or skip the value.
bool value_to_string(const madc::value &v, std::string &out);

// String/integer-only variant for the pop/shift/join family, whose
// historical (MadValue-era, test-pinned) behavior excluded reals.
bool value_to_string_no_real(const madc::value &v, std::string &out);

// ---- Mutable container access at the extern-C boundary ----------------

// Mutable kind::array / kind::object access for the extern-C script
// helpers. kind::null vivifies; the right kind returns the live
// container. Any OTHER kind is a script-level type error: one diagnostic
// line goes to stderr (an error path — never DBG-gated, and never a C++
// exception, which would escape the extern-C boundary into MIR-JIT
// frames and abort the process) and a per-thread dummy container is
// returned so the write degrades to a no-op. `who` names the calling
// script helper in the diagnostic.
std::vector<madc::value> &value_array_for_write(madc::value &v,
						const char *who);
std::map<std::string, madc::value> &value_object_for_write(madc::value &v,
							    const char *who);

// ---- Array helpers ---------------------------------------------------

// Element count of a madc::value: array size, object size, or 0 for any
// other kind (a never-touched script array is kind::null == empty).
size_t value_count(const madc::value &v);

// Split `s` by literal `delim` and store the pieces as strings in `out`.
// `out` is reset to an empty kind::array first. An empty delim pushes the
// whole string as a single element.
void split_by_delim(madc::value &out, const std::string &s,
		    const std::string &delim);

// Join `arr`'s string-coercible elements with `sep` into `out`. `out`
// is cleared first. A non-array `arr` (null included) joins as empty.
// Elements that are not string/integer/real are skipped silently
// (matches the prior php_implode / rust_join shape).
void join_with_sep(std::string &out, const madc::value &arr,
		   const std::string &sep);

}  // namespace ns_common

#endif // __NS_COMMON_H
