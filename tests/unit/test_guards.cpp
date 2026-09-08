// Unit tests for madc_guards — the ONE parser of the resource-guard knobs
// (MADC_MEM_LIMIT / MADC_CPU_LIMIT / madc.ini mem-limit / cpu-limit): off |
// 0 | auto | a whole number (owner ruling 2026-09-07, KG Decision
// resource_guards_default_off). Arming itself is process state the
// reducers gate (tests/testguardsoff.mad under the runner's auto).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;	// the prologue every tests/unit file uses
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>
#include "madc_guards.h"

TEST_CASE("guard knob: off, 0, auto, a number")
{
	MadcGuardKnob k;
	REQUIRE(madc_guard_knob_parse("off", k));
	CHECK(k.mode == MadcGuardMode::off);
	REQUIRE(madc_guard_knob_parse("0", k));
	CHECK(k.mode == MadcGuardMode::off);
	REQUIRE(madc_guard_knob_parse("auto", k));
	CHECK(k.mode == MadcGuardMode::auto_);
	REQUIRE(madc_guard_knob_parse(" AUTO ", k));	// case and blanks are syntax
	CHECK(k.mode == MadcGuardMode::auto_);
	REQUIRE(madc_guard_knob_parse("4096", k));
	CHECK(k.mode == MadcGuardMode::fixed);
	CHECK(k.value == 4096u);
	REQUIRE(madc_guard_knob_parse("Off", k));
	CHECK(k.mode == MadcGuardMode::off);
}

TEST_CASE("guard knob: anything else is refused, never truncated")
{
	MadcGuardKnob k;
	CHECK(!madc_guard_knob_parse("", k));
	CHECK(!madc_guard_knob_parse("8G", k));
	CHECK(!madc_guard_knob_parse("-5", k));
	CHECK(!madc_guard_knob_parse("4096MB", k));
	CHECK(!madc_guard_knob_parse("on", k));
	CHECK(!madc_guard_knob_parse("1.5", k));
}
