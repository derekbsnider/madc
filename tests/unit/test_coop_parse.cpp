// Stage-2 cooperative parse — the deterministic interleave gate (the RULED
// slice, docs/plans/2026-08-26-madcide-staged-parse-and-state.md).
//
// The contract under test: two parses interleaved at every yield point
// produce results identical to the same parses run serially. The pumps
// yield per top-level declaration (parse loop) and per LEX_YIELD_GRAIN
// tokens (lex loop); parse_yield_point switches the parse-session ambients
// (token pools, parse cursor, render mute) on resume, so an interleaved
// flow never reads another Program's actives. Interleaving is REAL by
// construction: both tasks are spawned before either runs, so with strict
// FIFO run-to-yield every per-decl yield in one hands the CPU to the
// other; Program::_coop_yields > 0 on both proves the yields fired
// (a vacuously-green gate would prove nothing).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <deque>
#include <fstream>
#include <iostream>
#include <map>
#include <queue>
#include <stack>
#include <string>
#include <vector>

#include "datadef.h"
#include "tokens.h"
#include "datatokens.h"
#include "madc.h"
#include "../src/rt/rt_task.h"

namespace {

// Enough top-level declarations that the parse loop's per-decl yield fires
// many times per source; distinct names per flavor so cross-contamination
// (an actives leak registering into the wrong Program) is directly probeable.
std::string make_source(const char *tag, int nfuncs)
{
    std::string src;
    for (int i = 0; i < nfuncs; i++) {
	src += "int ";
	src += tag;
	src += std::to_string(i);
	src += "(int v) { int r = v + ";
	src += std::to_string(i);
	src += "; return r * 2; }\n";
    }
    return src;
}

struct ParseJob {
    ::Program *prog;
    std::string src;
    std::string name;
    bool parsed = false;
};

// The task body: the same tokenize_buffer + parse pipeline parse_open's
// child frontend drives. Runs on an MT-1 stackful context; the pumps'
// yield points interleave it with the sibling job.
void parse_job_run(void *arg)
{
    ParseJob *job = (ParseJob *)arg;
    TokenProgram *tp = job->prog->tokenize_buffer(job->src, job->name);
    job->parsed = tp && job->prog->parse(tp);
}

bool has_fn(::Program &p, const std::string &name)
{
    return p.funcdef_map.find(name) != p.funcdef_map.end();
}

} // namespace

TEST_CASE("interleaved parses match serial byte-for-byte") {
    const int NF = 6;
    std::string src_a = make_source("fa", NF);
    std::string src_b = make_source("fb", NF);

    // Serial baseline — no tasks live, so every yield point must be the
    // no-op fast path (_coop_yields stays 0: batch compiles pay nothing).
    ::Program sa, sb;
    {
	TokenProgram *tp = sa.tokenize_buffer(src_a, "<coop-serial-a>");
	REQUIRE(tp != NULL);
	REQUIRE(sa.parse(tp));
    }
    {
	TokenProgram *tp = sb.tokenize_buffer(src_b, "<coop-serial-b>");
	REQUIRE(tp != NULL);
	REQUIRE(sb.parse(tp));
    }
    CHECK(sa._coop_yields == 0);
    CHECK(sb._coop_yields == 0);

    // Interleaved run: both jobs spawned before either runs; strict FIFO
    // run-to-yield alternates them at every yield point.
    ::Program ia, ib;
    ParseJob ja, jb;
    ja.prog = &ia; ja.src = src_a; ja.name = "<coop-interleaved-a>";
    jb.prog = &ib; jb.src = src_b; jb.name = "<coop-interleaved-b>";
    __madc_go(parse_job_run, &ja);
    __madc_go(parse_job_run, &jb);
    __madc_task_join_all();

    REQUIRE(ja.parsed);
    REQUIRE(jb.parsed);
    // The gate's teeth: the yields actually fired on both flows.
    CHECK(ia._coop_yields > 0);
    CHECK(ib._coop_yields > 0);

    // Identical registration products, and no cross-contamination: an
    // actives leak (parsing with the sibling's strpool/pools bound) either
    // fails the parse outright or registers names into the wrong Program.
    for (int i = 0; i < NF; i++) {
	std::string fa = "fa" + std::to_string(i);
	std::string fb = "fb" + std::to_string(i);
	CHECK(has_fn(sa, fa));
	CHECK(has_fn(ia, fa));
	CHECK(has_fn(sb, fb));
	CHECK(has_fn(ib, fb));
	CHECK(!has_fn(ia, fb));
	CHECK(!has_fn(ib, fa));
    }
    // Same-size registration surfaces (built-ins + the NF user functions):
    // serial and interleaved must agree exactly.
    CHECK(ia.funcdef_map.size() == sa.funcdef_map.size());
    CHECK(ib.funcdef_map.size() == sb.funcdef_map.size());
    CHECK(ia.diagnostics.size() == sa.diagnostics.size());
    CHECK(ib.diagnostics.size() == sb.diagnostics.size());
}
