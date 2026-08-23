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

// Reset-and-fill variant for OUT-parameter arrays (explode / split / grep /
// glob / slice / column / chars ...): the destination becomes a fresh empty
// array and its live container is returned. A frozen destination reports
// once and returns the per-thread dummy so the fill degrades to a no-op;
// any non-frozen kind is retagged to array (the same semantics as assigning
// value::make_array() over it, which these helpers historically did).
std::vector<madc::value> &value_array_reset_for_write(madc::value &v,
						      const char *who);

// ---- Array helpers ---------------------------------------------------

// Element count of a madc::value: array size, object size, or 0 for any
// other kind (a never-touched script array is kind::null == empty).
// This is the PHP semantics php::count / perl::scalar keep; the script
// .count()/.size() methods use value_length below instead.
size_t value_count(const madc::value &v);

// The script-facing .count()/.size() semantics (owner ruling 2026-08-18):
// containers count ELEMENTS, string kinds count LENGTH, anything else is an
// error. Sets *ok false (and returns 0) for a kind with no answer — the
// caller decides how to report it, because "an error" means a script
// exception in the runtime thunk and could mean a diagnostic elsewhere.
// null is 0 with ok TRUE: `array a;` is kind null until a mutator vivifies
// it, so an unfilled carrier is an EMPTY container, not a non-container.
size_t value_length(const madc::value &v, bool *ok);

// Split `s` by literal `delim` and store the pieces as strings in `out`.
// `out` is reset to an empty kind::array first (via
// value_array_reset_for_write — a frozen `out` degrades to a loud no-op).
// An empty delim pushes the whole string as a single element. `who` names
// the script-facing caller in the degrade diagnostics.
void split_by_delim(madc::value &out, const std::string &s,
		    const std::string &delim, const char *who);

// ---- Transient text returns ------------------------------------------

// The ONE thread-local text ring behind every ring-lifetime const char*
// return (the c_str() contract: valid to pass onward or copy immediately;
// a pointer stays valid until its slot recycles — 8 slots, the inet_ntoa
// model). Runtime entries and php:: functions returning transient text
// use this; never a private static buffer.
// ring_slot() lends the next slot DIRECTLY — assign/build into it and
// return its c_str(); slot capacity persists across uses, so steady-state
// callers allocate nothing. ring_text() is the move-in convenience for a
// string you already built.
std::string &ring_slot();
const char *ring_text(std::string s);

// Fill the NEXT ring slot with `v`'s text view (string kinds copy their
// payload; other scalar kinds render via value_to_string; null and
// containers clear) and return the slot. The value-argument twin of
// ring_slot() for lean `const char*` returns: transform the slot in
// place, then return its c_str().
std::string &value_text_slot(const madc::value *v);

// THE lean-primary adapter pair: copy the subject into the lent ring
// slot, run an in-place std::string core over it, return the slot's
// text. The core is any namespace's in-place transform (std::string*
// in, same pointer out) — the lean form and the guarded std::string
// public share that ONE core by construction.
const char *ring_apply(const char *s, std::string *(*core)(std::string *));
const char *ring_apply(const madc::value *v,
		       std::string *(*core)(std::string *));

// ---- Element move-out (the value-out return convention) ----------------

// Move the last/first element of `arr` into `dst`; `dst` becomes null when
// the container is empty (or the write degrades). Container access routes
// through value_array_for_write, so a frozen/mismatched `arr` reports and
// no-ops. Returns true when an element actually moved.
bool value_pop_element(madc::value &arr, madc::value &dst, const char *who);
bool value_shift_element(madc::value &arr, madc::value &dst, const char *who);

// Join `arr`'s string-coercible elements with `sep` into `out`. `out`
// is cleared first. A non-array `arr` (null included) joins as empty.
// Elements that are not string/integer/real are skipped silently
// (matches the prior php_implode / rust_join shape).
void join_with_sep(std::string &out, const madc::value &arr,
		   const std::string &sep);

}  // namespace ns_common

// ---- madc:: runtime-eval internals (defined in src/parser.cpp) ---------
// The single real eval pipeline behind the madc:: script namespace: the
// namespace madc publics and the extern-C __madc_*_runtime exports in
// src/ns_madc.cpp both delegate here. The internals live in parser.cpp
// because they drive the active Program's runtime_eval_source /
// runtime_eval_expression machinery. void* parameters: result/source/expr
// are std::string*, ctx is madc::value* (the unified script array).
void *madc_runtime_eval(void *result, void *source);
bool madc_runtime_eval_bool(void *source);
int64_t madc_runtime_eval_int(void *source);
double madc_runtime_eval_double(void *source);
void *madc_runtime_eval_string(void *result, void *source);
void *madc_runtime_eval_ctx(void *result, void *source, void *ctx);
bool madc_runtime_eval_bool_ctx(void *source, void *ctx);
int64_t madc_runtime_eval_int_ctx(void *source, void *ctx);
double madc_runtime_eval_double_ctx(void *source, void *ctx);
void *madc_runtime_eval_string_ctx(void *result, void *source, void *ctx);
void *madc_runtime_eval_expression(void *result, void *expr);
bool madc_runtime_eval_expression_bool(void *expr);
int64_t madc_runtime_eval_expression_int(void *expr);
double madc_runtime_eval_expression_double(void *expr);
void *madc_runtime_eval_expression_string(void *result, void *expr);
void *madc_runtime_eval_expression_ctx(void *result, void *expr, void *ctx);
bool madc_runtime_eval_expression_bool_ctx(void *expr, void *ctx);
int64_t madc_runtime_eval_expression_int_ctx(void *expr, void *ctx);
double madc_runtime_eval_expression_double_ctx(void *expr, void *ctx);
void *madc_runtime_eval_expression_string_ctx(void *result, void *expr, void *ctx);

#endif // __NS_COMMON_H
