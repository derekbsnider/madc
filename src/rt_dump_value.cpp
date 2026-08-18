/* rt_dump_value.cpp — the RUNTIME half of php::print_r / php::var_dump: the
 * walk over a madc::value.
 *
 * WHY A RUNTIME WALK AT ALL. Every other type this arc dumps is walked by the
 * COMPILER (src/cir_dump.cpp): the call site knows T, so the member census, the
 * columns and the type words are all compile-time constants and the runtime
 * carries only output primitives. A madc::value is the one type where that is
 * impossible — its kind is a property of the VALUE, not of the type, and a
 * single `value v;` may hold an integer on one line and an array of maps on the
 * next. So this is one ordinary recursive function, and the owner is right that
 * it is the EASIEST of the remaining shapes rather than the hardest: nothing
 * here has to be expanded per level, because the depth is a parameter.
 *
 * WHY IT IS C++ AND NOT IN src/rt/. src/rt/ is the strict-C11 AOT ledger lane
 * (scripts/ledger_sources.txt is its membership owner) so that a
 * -static-libmadc image can dump without the C++ runtime. A value's `array` and
 * `object` kinds are backed by std::vector<value> / std::map<string,value>
 * (include/libmadc/value.h), so walking one inherently needs the C++ script
 * runtime — and a program holding a value ALREADY does: scripts/
 * forest_ledger_gate.sh leg 6 exists precisely to assert that such a program
 * refuses the ledger path with a Tier-B message, naming madarray_* as the
 * reason. Putting the walk here therefore costs nothing that was reachable, and
 * moving it into rt_dump.c would have made that C11 file depend on C++ — which
 * would break dumping of ORDINARY C types in the very lane the rule protects.
 *
 * PHP is the oracle for print_r's framing (tmp/or_value.php, captured from
 * php-cli 8.3.6 with cat -A). var_dump keeps its one deliberate divergence:
 * where a C type is named by its C spelling, a value is named by its KIND —
 * madc::value::kind_name, the existing single owner of those spellings. A
 * dynamically typed slot's real type IS its kind, so `integer(42)` /
 * `real(3.5)` / `object(3) {` say what the storage word could not: `long(42)`
 * would be true of the payload and silent about the slot, and no storage word
 * exists at all that distinguishes `string` from `bytes` or `array` from
 * `object`. null keeps PHP's `NULL`, the exception already documented in
 * rt_dump.c ("no value" is not a C type).
 */

#include <algorithm>
#include <cstring>
#include <exception>
#include <map>
#include <string>
#include <vector>

#include "libmadc/value.h"
#include "rt/rt_dump.h"

namespace {

// The ancestor chain, by identity, for PHP's *RECURSION* marker. An ANCESTOR
// stack and not a visited set: PHP prints a value that merely APPEARS twice in
// full, both times, and marks only a genuine CYCLE. Verified against the oracle
// — tmp/or_value.php's `$twice` prints its inner array twice with no marker,
// while `$cyc` marks it.
typedef std::vector<const void *> AncestorStack;

void dump_value_at(void *sink, const madc::value &v, int flavor, int depth,
		   bool nested, AncestorStack &anc);

// print_r ends a SCALAR entry with a newline; a nested aggregate ends itself
// (its ")" line plus a blank line). At top level there is no entry to end:
// print_r(42) is exactly "42". Every var_dump primitive terminates its own line.
void pr_end_scalar(void *sink, int flavor, int depth)
{
	if (flavor == MADC_DUMP_PRINT_R && depth > 0)
		__madc_dump_pr_nl(sink);
}

// A value's own text/bytes payload, written EXACTLY: the byte count is explicit
// (madc_value::size), so a NUL inside a string neither truncates the output nor
// gets scanned for.
void dump_payload(void *sink, const madc::value &v, int flavor, int depth,
		  const char *word)
{
	const char *p = (const char *)v.data();
	long long n = (long long)v.size();
	if (flavor == MADC_DUMP_VAR_DUMP) {
		__madc_dump_vd_text_open(sink, madc_dump_frame_col(flavor, depth),
					 word, n);
		__madc_dump_raw(sink, p, n);
		__madc_dump_vd_text_close(sink);
		return;
	}
	__madc_dump_raw(sink, p, n);
	pr_end_scalar(sink, flavor, depth);
}

// One entry of an aggregate: its key, then its value one level deeper. print_r
// spells an array index and a string key the same way ([0] => / [name] => );
// var_dump quotes a string key ("name") and not an index, exactly as PHP does.
void dump_entry(void *sink, const madc::value &child, const std::string *key,
		long long idx, int flavor, int depth, AncestorStack &anc)
{
	int kcol = madc_dump_entry_col(flavor, depth);
	if (!key) {
		if (flavor == MADC_DUMP_VAR_DUMP)
			__madc_dump_vd_key_idx(sink, kcol, idx);
		else
			__madc_dump_pr_key_idx(sink, kcol, idx);
	} else if (flavor == MADC_DUMP_VAR_DUMP) {
		std::string quoted = "\"" + *key + "\"";
		__madc_dump_vd_key(sink, kcol, quoted.c_str());
	} else {
		__madc_dump_pr_key(sink, kcol, key->c_str());
	}
	dump_value_at(sink, child, flavor, depth + 1, true, anc);
}

// An array or an object: the same frame, differing only in the keys. print_r
// calls both "Array" because a value's object kind IS a PHP associative array,
// and PHP frames one exactly like a list. var_dump names the kind, so the two
// stay distinguishable there.
void dump_aggregate(void *sink, const madc::value &v, int flavor, int depth,
		    bool nested, AncestorStack &anc)
{
	int col = madc_dump_frame_col(flavor, depth);
	const void *self = (const void *)&v;

	if (std::find(anc.begin(), anc.end(), self) != anc.end()) {
		if (flavor == MADC_DUMP_VAR_DUMP)
			__madc_dump_vd_recursion(sink, col);
		else
			__madc_dump_pr_recursion(sink, "Array");
		return;
	}
	anc.push_back(self);

	bool is_obj = v.type() == madc::value::kind::object;
	const std::vector<madc::value> *arr = NULL;
	const std::map<std::string, madc::value> *obj = NULL;
	if (is_obj)
		obj = &v.as_object();
	else
		arr = &v.as_array();
	long long count = (long long)(is_obj ? obj->size() : arr->size());
	const char *word = flavor == MADC_DUMP_VAR_DUMP
			 ? madc::value::kind_name(v.type())
			 : "Array";

	if (flavor == MADC_DUMP_VAR_DUMP)
		__madc_dump_vd_head(sink, col, word, count);
	else
		__madc_dump_pr_head(sink, col, word);

	if (is_obj) {
		std::map<std::string, madc::value>::const_iterator it;
		for (it = obj->begin(); it != obj->end(); ++it)
			dump_entry(sink, it->second, &it->first, 0, flavor,
				   depth, anc);
	} else {
		for (size_t i = 0; i < arr->size(); i++)
			dump_entry(sink, (*arr)[i], NULL, (long long)i, flavor,
				   depth, anc);
	}

	if (flavor == MADC_DUMP_VAR_DUMP)
		__madc_dump_vd_tail(sink, col);
	else
		__madc_dump_pr_tail(sink, col, nested ? 1 : 0);

	anc.pop_back();
}

void dump_value_at(void *sink, const madc::value &v, int flavor, int depth,
		   bool nested, AncestorStack &anc)
{
	int col = madc_dump_frame_col(flavor, depth);
	const char *word = madc::value::kind_name(v.type());

	switch (v.type()) {
	case madc::value::kind::null:
		// print_r renders null as the EMPTY string — nothing at all, not
		// even the entry's own newline is owed differently: PHP's
		// `[4] => $` for a null element is the key line and then the
		// end-of-entry newline. Oracle: tmp/or_value.php "pr list".
		if (flavor == MADC_DUMP_VAR_DUMP)
			__madc_dump_vd_null(sink, col);
		else
			pr_end_scalar(sink, flavor, depth);
		return;

	case madc::value::kind::boolean:
		if (flavor == MADC_DUMP_VAR_DUMP)
			__madc_dump_vd_bool(sink, col, word,
					    v.as_boolean() ? 1 : 0);
		else {
			__madc_dump_pr_bool(sink, v.as_boolean() ? 1 : 0);
			pr_end_scalar(sink, flavor, depth);
		}
		return;

	case madc::value::kind::integer:
		// A value's integer payload is int64_t, always signed.
		if (flavor == MADC_DUMP_VAR_DUMP)
			__madc_dump_vd_i64(sink, col, word,
					   (long long)v.as_integer(), 0);
		else {
			__madc_dump_pr_i64(sink, (long long)v.as_integer(), 0);
			pr_end_scalar(sink, flavor, depth);
		}
		return;

	case madc::value::kind::real:
		if (flavor == MADC_DUMP_VAR_DUMP)
			__madc_dump_vd_f64(sink, col, word, v.as_real());
		else {
			__madc_dump_pr_f64(sink, v.as_real());
			pr_end_scalar(sink, flavor, depth);
		}
		return;

	case madc::value::kind::string:
	case madc::value::kind::bytes:
		dump_payload(sink, v, flavor, depth, word);
		return;

	case madc::value::kind::array:
	case madc::value::kind::object:
		dump_aggregate(sink, v, flavor, depth, nested, anc);
		return;

	case madc::value::kind::instance:
		if (flavor == MADC_DUMP_VAR_DUMP)
			__madc_dump_vd_instance(sink, col, (long long)v.size(),
						(unsigned)v.type_id());
		else {
			__madc_dump_pr_instance(sink, (unsigned)v.type_id());
			pr_end_scalar(sink, flavor, depth);
		}
		return;
	}
}

// A broken value reports itself. The reachable case is a kind/backing mismatch
// (type_id says `array` with no vector behind it, which as_array() throws on) —
// no script can build one, but a host handing in a hand-assembled madc_value
// could. Printing a plausible empty aggregate instead would be exactly the
// silent wrong answer this arc refuses.
void dump_failure(void *sink, const char *what)
{
	static const char pre[] = "[madc::value dump failed: ";
	__madc_dump_raw(sink, pre, (long long)(sizeof pre - 1));
	if (what)
		__madc_dump_raw(sink, what, (long long)strlen(what));
	__madc_dump_raw(sink, "]\n", 2);
}

} // namespace

// The one entry point the CIR builder emits a call to. An exception must not
// cross this boundary: it is declared with C linkage and is called from
// generated code, which has no handler for one.
extern "C" void __madc_dump_value(void *sink, const void *vp, int flavor,
				  int depth, int nested)
{
	if (!vp) {
		dump_failure(sink, "null value pointer");
		return;
	}
	const madc::value *v = static_cast<const madc::value *>(vp);
	AncestorStack anc;
	try {
		dump_value_at(sink, *v, flavor, depth, nested != 0, anc);
	} catch (const std::exception &e) {
		dump_failure(sink, e.what());
	} catch (...) {
		dump_failure(sink, "unknown error");
	}
}
