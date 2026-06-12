// Unit tests for madc::value (libmadc public embedding API).
//
// This binary intentionally links against madc_value.o only — the
// public value type must not pull in parser/compiler internals.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "libmadc/value.h"

#include <map>
#include <stdexcept>
#include <string>
#include <vector>

using madc::value;

TEST_SUITE("madc::value") {

    TEST_CASE("default constructor is null") {
	value v;
	CHECK(v.is_null());
	CHECK(v.type() == value::kind::null);
    }

    TEST_CASE("nullptr_t constructor is null") {
	value v(nullptr);
	CHECK(v.is_null());
    }

    TEST_CASE("boolean roundtrip") {
	value t(true);
	value f(false);
	CHECK(t.is_boolean());
	CHECK(t.as_boolean() == true);
	CHECK(f.as_boolean() == false);
    }

    TEST_CASE("integer roundtrip") {
	value v(int64_t(42));
	CHECK(v.is_integer());
	CHECK(v.as_integer() == 42);
    }

    TEST_CASE("real roundtrip") {
	value v(3.14);
	CHECK(v.is_real());
	CHECK(v.as_real() == doctest::Approx(3.14));
    }

    TEST_CASE("string roundtrip from c_str, std::string, and rvalue") {
	value a("hello");
	std::string s("world");
	value b(s);
	value c(std::string("rvalue"));
	CHECK(a.is_string());
	CHECK(a.as_string() == "hello");
	CHECK(b.as_string() == "world");
	CHECK(c.as_string() == "rvalue");
    }

    TEST_CASE("string from null c_str produces empty string") {
	const char *p = NULL;
	value v(p);
	CHECK(v.is_string());
	CHECK(v.as_string().empty());
    }

    TEST_CASE("bytes roundtrip") {
	std::vector<uint8_t> raw{0x01, 0x02, 0xff};
	value v = value::make_bytes(raw);
	CHECK(v.is_bytes());
	CHECK(v.as_bytes() == raw);
    }

    TEST_CASE("array make_array empty and populated") {
	value empty = value::make_array();
	CHECK(empty.is_array());
	CHECK(empty.as_array().empty());

	std::vector<value> seed;
	seed.push_back(value(int64_t(1)));
	seed.push_back(value(int64_t(2)));
	value a = value::make_array(std::move(seed));
	REQUIRE(a.as_array().size() == 2);
	CHECK(a.as_array()[0].as_integer() == 1);
	CHECK(a.as_array()[1].as_integer() == 2);
    }

    TEST_CASE("array() mutator appends in place") {
	value a = value::make_array();
	a.array().push_back(value("first"));
	a.array().push_back(value("second"));
	REQUIRE(a.as_array().size() == 2);
	CHECK(a.as_array()[0].as_string() == "first");
	CHECK(a.as_array()[1].as_string() == "second");
    }

    TEST_CASE("object roundtrip and field access") {
	value o = value::make_object();
	o.object()["user_id"] = value(int64_t(123));
	o.object()["name"]    = value("alice");
	REQUIRE(o.as_object().count("user_id") == 1);
	CHECK(o.as_object().at("user_id").as_integer() == 123);
	CHECK(o.as_object().at("name").as_string() == "alice");
    }

    TEST_CASE("nested array of objects") {
	value root = value::make_array();
	for ( int i = 0; i < 3; ++i )
	{
	    value entry = value::make_object();
	    entry.object()["i"] = value(int64_t(i));
	    root.array().push_back(std::move(entry));
	}
	REQUIRE(root.as_array().size() == 3);
	CHECK(root.as_array()[0].as_object().at("i").as_integer() == 0);
	CHECK(root.as_array()[2].as_object().at("i").as_integer() == 2);
    }

    TEST_CASE("copy is deep — mutating copy does not change source") {
	value src = value::make_array();
	src.array().push_back(value(int64_t(10)));

	value dst = src;
	dst.array().push_back(value(int64_t(20)));

	CHECK(src.as_array().size() == 1);
	CHECK(dst.as_array().size() == 2);
	CHECK(src.as_array()[0].as_integer() == 10);
    }

    TEST_CASE("move leaves source in null state") {
	value src = value::make_array();
	src.array().push_back(value(int64_t(7)));
	value dst = std::move(src);
	CHECK(src.is_null());
	REQUIRE(dst.is_array());
	CHECK(dst.as_array()[0].as_integer() == 7);
    }

    TEST_CASE("type accessors throw on mismatch") {
	value v(int64_t(1));
	CHECK_THROWS_AS(v.as_string(), std::runtime_error);
	CHECK_THROWS_AS(v.as_real(),   std::runtime_error);
	CHECK_THROWS_AS(v.as_array(),  std::runtime_error);
    }

    TEST_CASE("equality across all kinds") {
	CHECK(value() == value());
	CHECK(value(true) == value(true));
	CHECK(value(true) != value(false));
	CHECK(value(int64_t(5)) == value(int64_t(5)));
	CHECK(value(int64_t(5)) != value(int64_t(6)));
	CHECK(value(1.5) == value(1.5));
	CHECK(value("x") == value("x"));
	CHECK(value("x") != value("y"));
	CHECK(value::make_bytes({1,2,3}) == value::make_bytes({1,2,3}));
	CHECK(value::make_bytes({1,2,3}) != value::make_bytes({1,2,4}));
	CHECK(value::make_array() == value::make_array());

	value o1 = value::make_object();
	o1.object()["a"] = value(int64_t(1));
	value o2 = value::make_object();
	o2.object()["a"] = value(int64_t(1));
	value o3 = value::make_object();
	o3.object()["a"] = value(int64_t(2));
	CHECK(o1 == o2);
	CHECK(o1 != o3);
    }

    TEST_CASE("equality is false across different kinds") {
	CHECK(value(int64_t(1)) != value(1.0));
	CHECK(value("1")        != value(int64_t(1)));
	CHECK(value()           != value(false));
    }

    TEST_CASE("kind_name covers every enumerator") {
	CHECK(std::string(value::kind_name(value::kind::null))    == "null");
	CHECK(std::string(value::kind_name(value::kind::boolean)) == "boolean");
	CHECK(std::string(value::kind_name(value::kind::integer)) == "integer");
	CHECK(std::string(value::kind_name(value::kind::real))    == "real");
	CHECK(std::string(value::kind_name(value::kind::string))  == "string");
	CHECK(std::string(value::kind_name(value::kind::bytes))   == "bytes");
	CHECK(std::string(value::kind_name(value::kind::array))   == "array");
	CHECK(std::string(value::kind_name(value::kind::object))  == "object");
    }

    TEST_CASE("array() on freshly-constructed array works without explicit reset") {
	value a = value::make_array();
	CHECK(a.array().empty());
	a.array().push_back(value(int64_t(99)));
	CHECK(a.as_array()[0].as_integer() == 99);
    }

    TEST_CASE("object() on a null value vivifies an empty object") {
	value v;
	CHECK(v.is_null());
	v.object()["k"] = value(int64_t(1));
	CHECK(v.is_object());
	CHECK(v.as_object().at("k").as_integer() == 1);
    }

    TEST_CASE("array() on a null value vivifies an empty array") {
	value v;
	CHECK(v.is_null());
	v.array().push_back(value(int64_t(7)));
	CHECK(v.is_array());
	CHECK(v.as_array().size() == 1);
    }

    TEST_CASE("mutating accessors still throw on non-null kind mismatch") {
	value v(int64_t(1));
	CHECK_THROWS_AS(v.object(), std::runtime_error);
	CHECK_THROWS_AS(v.array(),  std::runtime_error);
	value o = value::make_object();
	CHECK_THROWS_AS(o.array(),  std::runtime_error);
	value a = value::make_array();
	CHECK_THROWS_AS(a.object(), std::runtime_error);
    }

    TEST_CASE("a vivified value equals its make_ factory twin") {
	value vo;
	vo.object();
	CHECK(vo == value::make_object());
	value va;
	va.array();
	CHECK(va == value::make_array());
    }
}

// --- The 32-byte madc_value C ABI + cell runtime (madc_api.h /
// madc_value_cell.h). Header-only struct + the cell impl in madc_value.o —
// still no parser/compiler internals in this binary.
#include "madc_api.h"
#include "madc_value_cell.h"
#include <cstddef>
#include <cstdlib>

TEST_SUITE("madc_value 32-byte ABI") {

    TEST_CASE("layout is pinned ABI") {
        CHECK(sizeof(madc_value) == 32);
        CHECK(alignof(madc_value) == 16);
        CHECK(offsetof(madc_value, type_id) == 0);
        CHECK(offsetof(madc_value, flags) == 4);
        CHECK(offsetof(madc_value, size) == 8);
        CHECK(MADC_VALUE_NULL == MADC_TYPEID_INVALID);
        CHECK(MADC_VALUE_BOOLEAN == MADC_TYPEID_BOOL);
        CHECK(MADC_VALUE_INTEGER == MADC_TYPEID_INT64);
        CHECK(MADC_VALUE_REAL == MADC_TYPEID_DOUBLE);
        CHECK(MADC_VALUE_STRING == MADC_TYPEID_TEXT);
        CHECK(MADC_VF_HEAP == 1u);
        CHECK(MADC_VF_INLINE_TEXT == 2u);
        CHECK(MADC_VF_TYPE_LOCKED == 4u);
        CHECK(MADC_VF_TYPE_COERCE == 8u);
        CHECK(MADC_VF_NULLABLE == 16u);
        CHECK(MADC_VF_CONST == 32u);
    }

    TEST_CASE("cell runtime: retain/release/saturation") {
        void *p = madc_cell_alloc(8);
        REQUIRE(p != (void *)NULL);
        CHECK(madc_cell_refcount(p) == 1);
        madc_cell_retain(p);
        CHECK(madc_cell_refcount(p) == 2);
        madc_cell_release(p);
        CHECK(madc_cell_refcount(p) == 1);
        madc_cell_release(p);                    // frees; do not touch p after
        void *q = madc_cell_alloc(4);
        REQUIRE(q != (void *)NULL);
        madc_cell_of(q)->refcount = MADC_CELL_PERMANENT - 1;
        madc_cell_retain(q);                     // saturates
        CHECK(madc_cell_refcount(q) == MADC_CELL_PERMANENT);
        madc_cell_release(q);                    // permanent: no-op, no free
        CHECK(madc_cell_refcount(q) == MADC_CELL_PERMANENT);
        std::free(madc_cell_of(q));              // test cleanup of permanent cell
    }
}
