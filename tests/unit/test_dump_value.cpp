// Unit tests for the RUNTIME madc::value walk behind php::print_r and
// php::var_dump (src/rt_dump_value.cpp).
//
// WHY THESE ARE UNIT TESTS AND NOT ONLY A .mad TEST. Two reasons, and both
// matter:
//
//   1. BYTE EXACTNESS. A `.expect` fixture asserts that each of its non-empty
//      lines APPEARS in the output — it cannot see a missing blank line, a
//      wrong order, or a trailing space. print_r's format is made of exactly
//      those things (PHP puts a blank line after a NESTED block's ")" and none
//      after the outermost, and a null element's line ends with a SPACE). Here
//      the whole capture is compared as one string.
//
//   2. KINDS NO SCRIPT CAN BUILD YET. The script surface can produce null,
//      boolean, integer, real, string and array-of-those (`value v = 42;`,
//      php::array_push*). It has no constructor for the `object`, `bytes` or
//      `instance` kinds — those come from a host through the C++ API. The
//      walker must handle all nine, so the gate has to reach them the way a
//      host does. tests/testphpdumpvalue.mad covers the script-reachable ones
//      end-to-end through the compiler.
//
// Every expected string is captured from php-cli 8.3.6 (tmp/or_value2.php, read
// back with cat -A) — never retyped from memory. var_dump keeps madc's one
// documented divergence: it names the value's KIND with madc's own spelling, so
// PHP's int/float/bool become integer/real/boolean, and `object` stays
// distinguishable from `array` where PHP calls both an array. NULL keeps PHP's
// word.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include "libmadc/value.h"

// The dump runtime's own contract, not a public API, so it is not on the
// include path — hence the relative path rather than a second set of extern "C"
// declarations, which could drift from the definitions.
#include "../../src/rt/rt_dump.h"

#include <map>
#include <string>
#include <vector>

using madc::value;

namespace {

// Capture one dump. Read by LENGTH, not strlen: a `bytes` value may contain a
// NUL and the walk writes it through.
std::string dumped(const value &v, int flavor, int depth = 0, bool nested = false)
{
	void *sink = __madc_dump_sink_open();
	REQUIRE(sink != NULL);
	__madc_dump_value(sink, &v, flavor, depth, nested ? 1 : 0);
	CHECK(__madc_dump_sink_failed(sink) == 0);
	std::string out(__madc_dump_sink_text(sink),
			__madc_dump_sink_length(sink));
	__madc_dump_sink_close(sink);
	return out;
}

std::string pr(const value &v, int depth = 0, bool nested = false)
{
	return dumped(v, MADC_DUMP_PRINT_R, depth, nested);
}

std::string vd(const value &v, int depth = 0, bool nested = false)
{
	return dumped(v, MADC_DUMP_VAR_DUMP, depth, nested);
}

// $A = array(1, "two", 3.5, true, null)
value mixed_list()
{
	std::vector<value> e;
	e.push_back(value((int64_t)1));
	e.push_back(value("two"));
	e.push_back(value(3.5));
	e.push_back(value(true));
	e.push_back(value::make_null());
	return value::make_array(e);
}

// $B = array("age" => 36, "name" => "ada", "ok" => true)
value fields()
{
	std::map<std::string, value> m;
	m["age"] = value((int64_t)36);
	m["name"] = value("ada");
	m["ok"] = value(true);
	return value::make_object(m);
}

// $C = array("a" => array(1, 2), "b" => array("x" => array(9)))
value nested_mix()
{
	std::vector<value> a;
	a.push_back(value((int64_t)1));
	a.push_back(value((int64_t)2));

	std::vector<value> nine;
	nine.push_back(value((int64_t)9));
	std::map<std::string, value> x;
	x["x"] = value::make_array(nine);

	std::map<std::string, value> top;
	top["a"] = value::make_array(a);
	top["b"] = value::make_object(x);
	return value::make_object(top);
}

} // namespace

TEST_SUITE("value dump: scalar kinds") {

    // print_r of a scalar at TOP LEVEL is the value alone: no type, no newline.
    // print_r(null) and print_r(false) are both the EMPTY string in PHP.
    TEST_CASE("print_r renders each scalar kind as PHP does") {
	CHECK(pr(value::make_null()) == "");
	CHECK(pr(value(true)) == "1");
	CHECK(pr(value(false)) == "");
	CHECK(pr(value((int64_t)42)) == "42");
	CHECK(pr(value((int64_t)-7)) == "-7");
	CHECK(pr(value(3.5)) == "3.5");
	CHECK(pr(value("hi")) == "hi");
	CHECK(pr(value("")) == "");
    }

    // var_dump names the value's KIND. NULL keeps PHP's word.
    TEST_CASE("var_dump names the kind and terminates its own line") {
	CHECK(vd(value::make_null()) == "NULL\n");
	CHECK(vd(value(true)) == "boolean(true)\n");
	CHECK(vd(value(false)) == "boolean(false)\n");
	CHECK(vd(value((int64_t)42)) == "integer(42)\n");
	CHECK(vd(value(3.5)) == "real(3.5)\n");
	CHECK(vd(value("hi")) == "string(2) \"hi\"\n");
	CHECK(vd(value("")) == "string(0) \"\"\n");
    }

    // PHP's double->string is %.14G plus a forced decimal point in an exponent
    // mantissa: 1.0E+25 where C's %G gives 1E+25. One formatter, both flavors.
    TEST_CASE("a real uses PHP's own float format") {
	CHECK(pr(value(1.0e25)) == "1.0E+25");
	CHECK(vd(value(1.0e25)) == "real(1.0E+25)\n");
	CHECK(pr(value(0.1)) == "0.1");
    }

    // The byte payload is written by COUNT, so a NUL inside it is emitted and
    // does not end the value. That is what separates __madc_dump_raw from a
    // NUL-terminated primitive.
    TEST_CASE("bytes are written by length, NUL included") {
	static const char raw[] = { 'A', '\0', 'B' };
	value b = value::make_bytes(raw, sizeof raw);
	REQUIRE(b.type() == value::kind::bytes);
	CHECK(pr(b) == std::string(raw, sizeof raw));
	CHECK(vd(b) == std::string("bytes(3) \"", 10)
		       + std::string(raw, sizeof raw) + "\"\n");
    }

    // An opaque typed instance reports the identity it HAS. The type id is a
    // number because no type-id -> name registry exists at run time yet; saying
    // "Object" and stopping would be a quiet guess.
    TEST_CASE("an instance reports its type id and payload size") {
	value i = value::make_instance(4242, 24, NULL);
	REQUIRE(i.type() == value::kind::instance);
	CHECK(pr(i) == "instance#4242");
	CHECK(vd(i) == "instance(24) #4242\n");
    }
}

TEST_SUITE("value dump: aggregate kinds") {

    TEST_CASE("print_r frames a mixed list exactly as PHP does") {
	CHECK(pr(mixed_list()) ==
	      "Array\n"
	      "(\n"
	      "    [0] => 1\n"
	      "    [1] => two\n"
	      "    [2] => 3.5\n"
	      "    [3] => 1\n"
	      "    [4] => \n"          // a null element: the trailing space is PHP's
	      ")\n");
    }

    TEST_CASE("var_dump frames a mixed list with madc's kind words") {
	CHECK(vd(mixed_list()) ==
	      "array(5) {\n"
	      "  [0]=>\n"
	      "  integer(1)\n"
	      "  [1]=>\n"
	      "  string(3) \"two\"\n"
	      "  [2]=>\n"
	      "  real(3.5)\n"
	      "  [3]=>\n"
	      "  boolean(true)\n"
	      "  [4]=>\n"
	      "  NULL\n"
	      "}\n");
    }

    // The object kind is a string-keyed map, which IS a PHP associative array —
    // so print_r frames it as "Array" and spells the keys bare, exactly as PHP
    // does for one. Keys come out in KEY order (the backing is a std::map)
    // where PHP preserves insertion order; the oracle was written in key order
    // so that difference stays visible rather than hidden by a reordering.
    TEST_CASE("print_r renders the object kind as PHP's associative array") {
	CHECK(pr(fields()) ==
	      "Array\n"
	      "(\n"
	      "    [age] => 36\n"
	      "    [name] => ada\n"
	      "    [ok] => 1\n"
	      ")\n");
    }

    // var_dump keeps `object` distinct from `array`, which PHP cannot: PHP calls
    // an associative array an array. The keys are quoted, as PHP quotes a
    // string key and not an index.
    TEST_CASE("var_dump keeps object distinct from array") {
	CHECK(vd(fields()) ==
	      "object(3) {\n"
	      "  [\"age\"]=>\n"
	      "  integer(36)\n"
	      "  [\"name\"]=>\n"
	      "  string(3) \"ada\"\n"
	      "  [\"ok\"]=>\n"
	      "  boolean(true)\n"
	      "}\n");
    }

    // The nesting rules: print_r steps 8 per level, puts the child's type word
    // on the "=> " line, and follows a NESTED block's ")" with a blank line —
    // which the OUTERMOST block does not get.
    TEST_CASE("print_r nests with PHP's 8-step and blank line") {
	CHECK(pr(nested_mix()) ==
	      "Array\n"
	      "(\n"
	      "    [a] => Array\n"
	      "        (\n"
	      "            [0] => 1\n"
	      "            [1] => 2\n"
	      "        )\n"
	      "\n"
	      "    [b] => Array\n"
	      "        (\n"
	      "            [x] => Array\n"
	      "                (\n"
	      "                    [0] => 9\n"
	      "                )\n"
	      "\n"
	      "        )\n"
	      "\n"
	      ")\n");
    }

    // The top level here is an OBJECT kind, so var_dump says `object(2)` where
    // the oracle says `array(2)` — PHP has one word for both and madc keeps them
    // apart. print_r above agrees with PHP byte for byte, because print_r frames
    // an associative array exactly like a list.
    TEST_CASE("var_dump nests with a 2-step and no blank line") {
	CHECK(vd(nested_mix()) ==
	      "object(2) {\n"
	      "  [\"a\"]=>\n"
	      "  array(2) {\n"
	      "    [0]=>\n"
	      "    integer(1)\n"
	      "    [1]=>\n"
	      "    integer(2)\n"
	      "  }\n"
	      "  [\"b\"]=>\n"
	      "  object(1) {\n"
	      "    [\"x\"]=>\n"
	      "    array(1) {\n"
	      "      [0]=>\n"
	      "      integer(9)\n"
	      "    }\n"
	      "  }\n"
	      "}\n");
    }

    TEST_CASE("an empty aggregate still frames itself") {
	CHECK(pr(value::make_array()) == "Array\n(\n)\n");
	CHECK(vd(value::make_array()) == "array(0) {\n}\n");
	CHECK(pr(value::make_object()) == "Array\n(\n)\n");
	CHECK(vd(value::make_object()) == "object(0) {\n}\n");
    }
}

TEST_SUITE("value dump: nesting inside a generated walk") {

    // The case a struct member of type `value` produces: the GENERATED walk
    // emits the key line and passes the depth it framed, and the runtime walk
    // picks the columns up from there. Byte-compared against the same value
    // nested one level in PHP.
    TEST_CASE("a value at depth 1 continues the enclosing frame") {
	CHECK(pr(mixed_list(), 1, true) ==
	      "Array\n"
	      "        (\n"
	      "            [0] => 1\n"
	      "            [1] => two\n"
	      "            [2] => 3.5\n"
	      "            [3] => 1\n"
	      "            [4] => \n"
	      "        )\n"
	      "\n");
	CHECK(vd(mixed_list(), 1, true) ==
	      "  array(5) {\n"
	      "    [0]=>\n"
	      "    integer(1)\n"
	      "    [1]=>\n"
	      "    string(3) \"two\"\n"
	      "    [2]=>\n"
	      "    real(3.5)\n"
	      "    [3]=>\n"
	      "    boolean(true)\n"
	      "    [4]=>\n"
	      "    NULL\n"
	      "  }\n");
    }

    // A scalar value at depth > 0 ends its ENTRY with a newline; at top level it
    // does not (print_r(42) is exactly "42").
    TEST_CASE("a nested scalar ends its entry") {
	CHECK(pr(value((int64_t)42), 1, true) == "42\n");
	CHECK(pr(value::make_null(), 1, true) == "\n");
	CHECK(vd(value((int64_t)42), 1, true) == "  integer(42)\n");
    }
}

TEST_SUITE("value dump: the *RECURSION* format") {

    // PHP marks a CYCLE, and only a cycle: a value that merely appears twice is
    // printed twice in full (oracle tmp/or_value.php, "$twice" vs "$cyc"). The
    // walk therefore guards with an ANCESTOR stack, not a visited set.
    //
    // A cycle is UNCONSTRUCTIBLE today, so the detection cannot be exercised:
    // madc::value owns its children through unique_ptr<vector<value>> /
    // unique_ptr<map<string,value>> with value semantics, so pushing a value
    // into its own array deep-COPIES it — the graph is a tree by construction.
    // What is gated here is the FORMAT, which is the part that can silently
    // drift: print_r's marker sits on its own line indented by exactly ONE
    // space at EVERY depth (verified at depths 1 and 2), with no "(" block and
    // no trailing blank line, while var_dump's sits at the value column.
    // The guard itself becomes reachable when the refcounted-cell backing lands
    // (include/libmadc/value.h: "their cell representation arrives with the
    // madcdis pool work"), which is when aliasing — and so a cycle — can exist.
    TEST_CASE("print_r's marker keeps PHP's one-space line") {
	void *sink = __madc_dump_sink_open();
	REQUIRE(sink != NULL);
	__madc_dump_pr_recursion(sink, "Array");
	CHECK(std::string(__madc_dump_sink_text(sink)) == "Array\n *RECURSION*\n");
	__madc_dump_sink_close(sink);
    }

    TEST_CASE("var_dump's marker sits at the value column") {
	void *sink = __madc_dump_sink_open();
	REQUIRE(sink != NULL);
	__madc_dump_vd_recursion(sink, 4);
	CHECK(std::string(__madc_dump_sink_text(sink)) == "    *RECURSION*\n");
	__madc_dump_sink_close(sink);
    }
}

TEST_SUITE("value dump: the sink itself") {

    // A NULL sink prints to stdout; a real one captures. Both go through one
    // writer, so this checks the capture side reports itself honestly.
    TEST_CASE("an empty capture is an empty string, not a null pointer") {
	void *sink = __madc_dump_sink_open();
	REQUIRE(sink != NULL);
	// Through a named local: doctest's expression decomposition cannot bind
	// a `const char *` rvalue for its lhs capture.
	const char *txt = __madc_dump_sink_text(sink);
	CHECK(txt != NULL);
	CHECK(std::string(txt) == "");
	CHECK(__madc_dump_sink_length(sink) == 0);
	CHECK(__madc_dump_sink_failed(sink) == 0);
	__madc_dump_sink_close(sink);
    }

    // The length is the binary-safe reader: strlen would stop at the NUL.
    TEST_CASE("the length sees past an embedded NUL") {
	void *sink = __madc_dump_sink_open();
	REQUIRE(sink != NULL);
	__madc_dump_raw(sink, "a\0b", 3);
	CHECK(__madc_dump_sink_length(sink) == 3);
	__madc_dump_sink_close(sink);
    }

    // A dump of a broken value must SAY so rather than print a plausible empty
    // aggregate. A null pointer is the reachable-from-C form of that.
    //
    // The wording is "madc dump", not "madc::value dump": the GENERATED pointer
    // walk reports its own failures (an ancestor stack that would not grow)
    // through the same primitive, __madc_dump_fail, so the spelling has one
    // owner in rt_dump.c instead of one per walk.
    TEST_CASE("a null value pointer reports itself") {
	void *sink = __madc_dump_sink_open();
	REQUIRE(sink != NULL);
	__madc_dump_value(sink, NULL, MADC_DUMP_PRINT_R, 0, 0);
	CHECK(std::string(__madc_dump_sink_text(sink))
	      == "[madc dump failed: null value pointer]\n");
	__madc_dump_sink_close(sink);
    }

    // The ancestor stack is SHARED with the generated pointer walk, so a value
    // walk must leave it exactly as it found it. A leak here would make the rest
    // of an enclosing struct dump report a false *RECURSION*.
    TEST_CASE("the shared ancestor stack is balanced across a value walk") {
	madc::value v = nested_mix();
	void *sink = __madc_dump_sink_open();
	REQUIRE(sink != NULL);
	// Occupy one slot, dump, and confirm the SAME address is still reported
	// as on-path afterwards (1 = pushed would mean the walk had popped it).
	int outer = 7;
	CHECK(__madc_dump_anc_push(&outer, 999u) == 1);
	__madc_dump_value(sink, &v, MADC_DUMP_PRINT_R, 0, 0);
	CHECK(__madc_dump_anc_push(&outer, 999u) == 0);
	__madc_dump_anc_pop();
	__madc_dump_anc_pop();
	// ...and with the stack now empty it is pushable again.
	CHECK(__madc_dump_anc_push(&outer, 999u) == 1);
	__madc_dump_anc_pop();
	__madc_dump_sink_close(sink);
    }

    // The TAG is what keeps `struct T { int v; int *p; }` with `p = &t.v` from
    // reporting a false cycle: &t and &t.v are the SAME address, and only the
    // pointee type tells them apart.
    TEST_CASE("the same address under two tags is not a cycle") {
	int x = 0;
	CHECK(__madc_dump_anc_push(&x, 1u) == 1);
	CHECK(__madc_dump_anc_push(&x, 2u) == 1);	// different type: not a cycle
	CHECK(__madc_dump_anc_push(&x, 1u) == 0);	// same type: a cycle
	__madc_dump_anc_pop();
	__madc_dump_anc_pop();
	// Popped off the path, the address is reachable again — a STACK, not a
	// visited set. This is the property PHP's own output demands.
	CHECK(__madc_dump_anc_push(&x, 1u) == 1);
	__madc_dump_anc_pop();
    }
}
