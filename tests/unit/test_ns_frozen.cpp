// Frozen-carrier contract for the polyglot namespace runtime entries.
//
// value::array() THROWS on a frozen value — the designed contract for C++
// hosts. The extern-C script entries must never let that throw cross the
// extern-C boundary into MIR-JIT frames (process abort): every container
// mutation routes through ns_common::value_array_for_write /
// value_array_reset_for_write, which report to stderr and degrade the
// write to a no-op on a dummy container.
//
// Before the fix, each call below with a frozen array aborted the process
// with an uncaught std::runtime_error. The test IS the gate: it passes
// only if every entry survives and leaves the frozen value untouched.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "libmadc/value.h"

#include <cstdint>
#include <string>
#include <vector>

using madc::value;

extern "C" {
    void __php_explode(value *, const char *, const char *);
    std::string *__php_array_pop(std::string *, value *);
    std::string *__php_array_shift(std::string *, value *);
    void __php_array_reverse(value *);
    void __php_sort(value *);
    void __php_rsort(value *);
    void __php_array_unique(value *);
    void __php_array_slice(value *, value *, int64_t, int64_t);
    void __php_array_column(value *, value *, int64_t);
    void __perl_grep(value *, const char *, value *);
    void __perl_glob(value *, const char *);
    std::string *__perl_pop(std::string *, value *);
    std::string *__perl_shift(std::string *, value *);
    void __perl_split(value *, const char *, const char *);
    void __rb_chars(value *, const char *);
    void __rb_rotate(value *, int64_t);
    void __rb_compact(value *);
    void __rb_flatten(value *, const char *);
    void __rust_split(value *, const char *, const char *);
    void __rust_split_whitespace(value *, const char *);
    std::string *__rust_pop(std::string *, value *);
}

// A frozen two-element string array ["b", "a"] (unsorted on purpose so
// sort/reverse effects would be visible if a write leaked through).
static value frozen_pair()
{
    value v = value::make_array();
    v.array().push_back(value("b"));
    v.array().push_back(value("a"));
    v.freeze();
    return v;
}

static bool still_pair(const value &v)
{
    if ( !v.is_array() || v.as_array().size() != 2 )
	return false;
    return v.as_array()[0].as_string() == "b"
	&& v.as_array()[1].as_string() == "a";
}

TEST_SUITE("ns frozen-carrier degrade") {

    TEST_CASE("php in-place mutators no-op on a frozen array") {
	value v = frozen_pair();
	std::string res;

	__php_array_pop(&res, &v);
	CHECK(res.empty());
	CHECK(still_pair(v));

	__php_array_shift(&res, &v);
	CHECK(res.empty());
	CHECK(still_pair(v));

	__php_array_reverse(&v);
	CHECK(still_pair(v));

	__php_sort(&v);
	CHECK(still_pair(v));

	__php_rsort(&v);
	CHECK(still_pair(v));

	__php_array_unique(&v);
	CHECK(still_pair(v));
    }

    TEST_CASE("php out-param fillers no-op on a frozen destination") {
	value dest = frozen_pair();
	value src = value::make_array();
	src.array().push_back(value("x"));
	src.array().push_back(value("y"));

	__php_explode(&dest, ",", "p,q,r");
	CHECK(still_pair(dest));

	__php_array_slice(&dest, &src, 0, 2);
	CHECK(still_pair(dest));

	__php_array_column(&dest, &src, 0);
	CHECK(still_pair(dest));
    }

    TEST_CASE("perl entries no-op on a frozen array") {
	value v = frozen_pair();
	std::string res;

	__perl_pop(&res, &v);
	CHECK(res.empty());
	CHECK(still_pair(v));

	__perl_shift(&res, &v);
	CHECK(res.empty());
	CHECK(still_pair(v));

	value src = value::make_array();
	src.array().push_back(value("match"));
	__perl_grep(&v, "mat", &src);
	CHECK(still_pair(v));

	__perl_glob(&v, "*");
	CHECK(still_pair(v));

	__perl_split(&v, ",", "p,q");
	CHECK(still_pair(v));
    }

    TEST_CASE("ruby entries no-op on a frozen array") {
	value v = frozen_pair();

	__rb_chars(&v, "abc");
	CHECK(still_pair(v));

	__rb_rotate(&v, 1);
	CHECK(still_pair(v));

	__rb_compact(&v);
	CHECK(still_pair(v));

	__rb_flatten(&v, "a b c");
	CHECK(still_pair(v));
    }

    TEST_CASE("rust entries no-op on a frozen array") {
	value v = frozen_pair();
	std::string res;

	__rust_split(&v, "p,q", ",");
	CHECK(still_pair(v));

	__rust_split_whitespace(&v, "p q");
	CHECK(still_pair(v));

	__rust_pop(&res, &v);
	CHECK(res.empty());
	CHECK(still_pair(v));
    }

    TEST_CASE("non-frozen behavior is unchanged by the routing") {
	value v = value::make_array();
	v.array().push_back(value("b"));
	v.array().push_back(value("a"));
	std::string res;

	__php_sort(&v);
	CHECK(v.as_array()[0].as_string() == "a");
	CHECK(v.as_array()[1].as_string() == "b");

	__php_array_pop(&res, &v);
	CHECK(res == "b");
	CHECK(v.as_array().size() == 1);

	value out;
	__php_explode(&out, ",", "p,q,r");
	REQUIRE(out.is_array());
	CHECK(out.as_array().size() == 3);
	CHECK(out.as_array()[2].as_string() == "r");

	__perl_split(&out, ",", "p,q");
	REQUIRE(out.is_array());
	CHECK(out.as_array().size() == 2);

	__rb_rotate(&out, 1);
	CHECK(out.as_array()[0].as_string() == "q");
    }
}
