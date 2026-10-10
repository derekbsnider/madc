// Unit battery for madc::memory::project (split arc 1C; include/madc_memory.h):
// the event stream, its file, and the tools over it, driven through the tool
// registry the MCP server calls. ORACLE: madcide's Nexus records (the ported
// madcide_propose.inc / madcide_nexus.inc / madcide_tests.inc, design
// docs/plans/2026-09-13-nexus-L4-design.md) for every answer's shape; gcc
// -fsyntax-only on the edit candidates (both clean); the runner's
// --report=json for test.run's rows.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <sys/stat.h>
#include <unistd.h>

#include "madc_memory.h"
#include "madc_tools.h"
#include "madc_documents.h"
#include "madcdis/hub.h"
#include "madcdis/verbs.h"		// mutation_context: the document's text
#include "madcdis/world_text.h"	// the one value<->JSON bridge

using nlohmann::json;
using madc::hub::detail::wt_json_to_value;
using madc::hub::detail::wt_value_to_json;
using madc::tools::tier;

namespace {

const char *CALC =
    "struct Point { int x; int y; };\n"		// 1
    "\n"					// 2
    "long add(long a, long b)\n"		// 3
    "{\n"					// 4
    "    return a + b;\n"			// 5
    "}\n"					// 6
    "\n"					// 7
    "int main(void)\n"				// 8
    "{\n"					// 9
    "    return (int)add(2, 3);\n"		// 10
    "}\n";					// 11

const char *SWAPPED = "long add(long a, long b)\n{\n    return b + a;\n}";

// One document, the code-graph tools and the memory's in one registry; a
// call names its caller's tier and actor.
struct bench
{
    madc::hub::world w;
    madc::memory::project memory;
    madc::tools::registry tools;
    madc::hub::entity_id doc;
    explicit bench(const char *path = "/mcp-unit/calc.c", const char *text = CALC)
	: memory(w), doc(madc::document_new(w, path))
    {
	madc::hub::mutation_context(w).text_load(doc, text);
	madc::tools::add_graph_tools(tools);
	memory.add_tools(tools);
    }
    json call(const std::string &name, const json &args, bool &err,
	      tier caller = tier::owner, int64_t actor = -1)
    {
	madc::tools::context c = { w, doc, caller, actor };
	madc::value result;
	err = !tools.call(c, name, wt_json_to_value(args), result);
	return wt_value_to_json(result);
    }
    json node(const char *name)
    {
	bool err = false;
	return call("graph.definition", { { "name", name } }, err)["nodes"][0];
    }
    std::string text() const { return w.text_of(doc)->text(); }
    json record(size_t i) const { return wt_value_to_json(memory.records()[i]); }
};

bool says(const json &r, const char *what)
{
    return r.contains("error") && r["error"].get<std::string>().find(what) != std::string::npos;
}

const json ADD_REF = { { "ref", "symbol" }, { "file", "/mcp-unit/calc.c" },
		       { "kind", "Function" }, { "name", "add" } };

json record_ref(const json &id)
{
    return { { "ref", "record" }, { "id", id } };
}

// A fresh directory under /tmp; the test removes what it made.
std::string temp_dir()
{
    char tmpl[] = "/tmp/madc_memory_XXXXXX";
    char *made = mkdtemp(tmpl);
    return made ? std::string(made) : std::string();
}

} // namespace

TEST_CASE("the stream records a document's edits and replays any seq")
{
    bench b;
    CHECK(b.memory.head() == 0);			// a load is a baseline, not an event
    madc::hub::mutation_context mc(b.w);
    const size_t at = b.text().find("a + b");
    REQUIRE(mc.text_replace(b.doc, at, 5, "b + a"));
    REQUIRE(b.memory.records().size() == 2u);
    CHECK(b.record(0)["kind"] == "checkpoint");		// the baseline, recorded
    CHECK(b.record(0)["text"] == CALC);
    CHECK(b.record(0)["path"] == "/mcp-unit/calc.c");
    CHECK(b.record(1)["kind"] == "splice");
    CHECK(b.record(1)["at"] == at);
    CHECK(b.record(1)["ins"] == "b + a");
    CHECK(b.memory.replay(b.doc, 1) == CALC);
    CHECK(b.memory.replay(b.doc, 2) == b.text());
    CHECK(b.memory.replay(b.doc, 0) == b.text());

    REQUIRE(mc.text_load(b.doc, "int z;\n"));		// followed: a checkpoint
    CHECK(b.record(2)["kind"] == "checkpoint");
    CHECK(b.memory.replay(b.doc, 2).find("b + a") != std::string::npos);
    CHECK(b.memory.replay(b.doc, 3) == "int z;\n");
}

TEST_CASE("a proposal is stored, then accepted through the edit core")
{
    bench b;
    bool err = true;
    json p = b.call("graph.replace", { { "id", b.node("add")["id"] }, { "src", SWAPPED } },
		    err, tier::proposer, 7);
    CHECK_FALSE(err);
    CHECK(p["ok"] == true);
    const json seq = p["proposal"];
    CHECK(b.text() == CALC);				// a proposal changes nothing

    json rows = b.call("graph.proposals", { { "status", "open" } }, err)["rows"];
    REQUIRE(rows.size() == 1u);
    CHECK(rows[0]["seq"] == seq);
    CHECK(rows[0]["verb"] == "graph.replace");
    CHECK(rows[0]["name"] == "add");
    CHECK(rows[0]["actor"] == 7);
    CHECK(says(b.call("graph.proposals", { { "status", "pending" } }, err),
	       "unknown status 'pending'"));

    CHECK(says(b.call("graph.accept", { { "seq", seq } }, err, tier::proposer, 7),
	       "requires editor"));
    json acc = b.call("graph.accept", { { "seq", seq } }, err, tier::editor, 3);
    CHECK_FALSE(err);
    CHECK(acc["ok"] == true);
    CHECK(acc["checks"]["fallback"] == true);		// no test is linked to add
    CHECK(b.text().find("return b + a;") != std::string::npos);

    json full = b.call("graph.proposal", { { "seq", seq } }, err);
    CHECK(full["status"] == "accepted");
    CHECK(full["decision"]["actor"] == 3);
    CHECK(full["decision"]["landed_seq"] == acc["landed_seq"]);
    const json landed = b.record((size_t)acc["landed_seq"].get<int64_t>() - 1);
    CHECK(landed["kind"] == "nodeop");
    CHECK(landed["proposal"] == seq);

    json h = b.call("graph.history", { { "id", b.node("add")["id"] } }, err);
    CHECK_FALSE(err);
    bool nodeop = false, splice = false;
    for ( const json &r : h["rows"] )
    {
	nodeop = nodeop || r["verb"] == "graph.replace";
	splice = splice || r["verb"] == "splice";
    }
    CHECK(nodeop);
    CHECK(splice);
    CHECK(h["complete"] == false);			// past the splice: not derivable

    CHECK(says(b.call("graph.accept", { { "seq", seq } }, err, tier::editor), "is accepted"));
}

TEST_CASE("an accept refuses a target changed since the proposal")
{
    bench b;
    bool err = true;
    json p = b.call("graph.replace", { { "id", b.node("add")["id"] }, { "src", SWAPPED } },
		    err, tier::proposer);
    REQUIRE_FALSE(err);
    b.call("graph.replace", { { "id", b.node("add")["id"] },
			      { "src", "long add(long a, long b)\n{\n    return a + b + 0;\n}" } },
	   err, tier::editor);
    REQUIRE_FALSE(err);
    json r = b.call("graph.accept", { { "seq", p["proposal"] } }, err, tier::editor);
    CHECK(err);
    CHECK(says(r, "changed since seq"));
    CHECK(b.text().find("return a + b + 0;") != std::string::npos);
    CHECK(b.call("graph.proposal", { { "seq", p["proposal"] } }, err)["status"] == "open");
}

TEST_CASE("a proposer withdraws only its own; an editor rejects with a reason")
{
    bench b;
    bool err = true;
    json main_id = b.node("main")["id"];
    json p = b.call("graph.delete", { { "id", main_id } }, err, tier::proposer, 7);
    REQUIRE_FALSE(err);
    CHECK(says(b.call("graph.withdraw", { { "seq", p["proposal"] } }, err, tier::proposer, 8),
	       "is not yours"));
    b.call("graph.withdraw", { { "seq", p["proposal"] } }, err, tier::proposer, 7);
    CHECK_FALSE(err);
    CHECK(b.call("graph.proposal", { { "seq", p["proposal"] } }, err)["status"] == "withdrawn");
    CHECK(says(b.call("graph.accept", { { "seq", p["proposal"] } }, err, tier::editor),
	       "is withdrawn"));

    json q = b.call("graph.delete", { { "id", main_id } }, err, tier::proposer, 7);
    REQUIRE_FALSE(err);
    b.call("graph.reject", { { "seq", q["proposal"] }, { "reason", "main stays" } }, err,
	   tier::editor);
    CHECK_FALSE(err);
    json full = b.call("graph.proposal", { { "seq", q["proposal"] } }, err);
    CHECK(full["status"] == "rejected");
    CHECK(full["decision"]["reason"] == "main stays");
    CHECK(b.text() == CALC);
    CHECK(says(b.call("graph.proposal", { { "seq", 999 } }, err), "no proposal 999"));
}

TEST_CASE("records and links: create, link, context, explain, close")
{
    bench b;
    bool err = true;
    json d = b.call("nexus.create", { { "rkind", "decision" },
				      { "fields", { { "title", "keep add pure" } } } },
		    err, tier::proposer, 7);
    REQUIRE_FALSE(err);
    const json id = d["id"];
    CHECK(says(b.call("nexus.create", { { "rkind", "wish" } }, err, tier::proposer),
	       "unknown record kind 'wish'"));
    CHECK(says(b.call("nexus.create", { { "rkind", "task" } }, err, tier::observer),
	       "requires proposer"));

    b.call("nexus.link", { { "from", record_ref(id) }, { "rel", "affects" }, { "to", ADD_REF } },
	   err, tier::proposer);
    CHECK_FALSE(err);
    CHECK(says(b.call("nexus.link", { { "from", record_ref(id) }, { "rel", "likes" },
				      { "to", ADD_REF } }, err, tier::proposer),
	       "unknown relation 'likes'"));
    CHECK(says(b.call("nexus.link", { { "from", 5 }, { "rel", "affects" }, { "to", ADD_REF } },
		      err, tier::proposer), "from needs a ref object"));

    json ctx = b.call("nexus.context", { { "ref", ADD_REF } }, err);
    CHECK_FALSE(err);
    bool linked = false, compiled = false;
    for ( const json &r : ctx["rows"] )
    {
	linked = linked || (r["provenance"] == "record" && r.contains("record")
			    && r["record"]["id"] == id && r["rel"] == "affects");
	compiled = compiled || (r["provenance"] == "compiler" && r["node"]["name"] == "add");
    }
    CHECK(linked);
    CHECK(compiled);

    json ex = b.call("nexus.explain", { { "ref", ADD_REF } }, err);
    CHECK_FALSE(err);
    CHECK(ex["what"]["node"]["name"] == "add");
    CHECK(ex["what"]["provenance"] == "compiler");
    CHECK(ex["depends"]["callers"] == 1);			// main calls add
    REQUIRE(ex["constraints"].size() == 1u);
    CHECK(ex["constraints"][0]["title"] == "keep add pure");
    REQUIRE(ex["why"].size() == 1u);
    CHECK(ex["why"][0]["record"]["rkind"] == "decision");
    bool parseable = false;
    for ( const json &l : ex["layers"] )
	parseable = parseable || l == "parseable";
    CHECK(parseable);

    json p = b.call("graph.replace", { { "id", b.node("add")["id"] }, { "src", SWAPPED } },
		    err, tier::proposer);
    REQUIRE_FALSE(err);
    REQUIRE(p["constraints"].size() == 1u);		// the decision informs the proposal
    CHECK(p["constraints"][0]["id"] == id);

    b.call("nexus.update", { { "id", id }, { "fields", { { "body", "no side effects" } } } },
	   err, tier::proposer);
    CHECK_FALSE(err);
    json rec = b.call("nexus.record", { { "id", id } }, err);
    CHECK(rec["fields"]["title"] == "keep add pure");	// an update merges
    CHECK(rec["fields"]["body"] == "no side effects");
    CHECK(rec["state"] == "open");

    b.call("nexus.close", { { "id", id }, { "state", "superseded" } }, err, tier::proposer);
    CHECK_FALSE(err);
    CHECK(b.call("nexus.records", json::object(), err)["rows"].empty());	// not live
    CHECK(b.call("nexus.records", { { "state", "superseded" } }, err)["rows"].size() == 1u);

    b.call("nexus.unlink", { { "from", record_ref(id) }, { "rel", "affects" }, { "to", ADD_REF } },
	   err, tier::proposer);
    CHECK_FALSE(err);
    for ( const json &r : b.call("nexus.context", { { "ref", ADD_REF } }, err)["rows"] )
	CHECK_FALSE(r.contains("rel"));
}

TEST_CASE("test records: discover, candidates, and test.run's refusals and verdicts")
{
    bench b;
    bool err = false;
    CHECK(says(b.call("test.run", { { "ids", json::array() } }, err, tier::editor),
	       "no testable layer"));

    const std::string dir = temp_dir();
    REQUIRE_FALSE(dir.empty());
    const std::string unit = dir + "/unit";
    REQUIRE(mkdir(unit.c_str(), 0700) == 0);
    const char *const files[] = { "/a.mad", "/b.mad", "/b.helper", "/unit/c.cpp" };
    for ( const char *f : files )
	REQUIRE(madc::document_write_file(dir + f, "\n", false));

    json d = b.call("test.discover", { { "dir", dir } }, err, tier::proposer);
    CHECK_FALSE(err);
    CHECK(d["discovered"] == 2);				// b.mad is b.helper's unit
    CHECK(d["created"] == 2);
    d = b.call("test.discover", { { "dir", dir } }, err, tier::proposer);
    CHECK(d["created"] == 0);
    CHECK(d["updated"] == 2);

    json mads = b.call("test.list", { { "family", "mad" } }, err)["rows"];
    REQUIRE(mads.size() == 1u);
    CHECK(mads[0]["name"] == "a");
    json units = b.call("test.list", { { "family", "unit" } }, err)["rows"];
    REQUIRE(units.size() == 1u);
    CHECK(units[0]["name"] == "c");

    json cand = b.call("test.candidates", { { "ref", ADD_REF } }, err);
    CHECK(cand["fallback"] == true);
    CHECK(cand["suite"]["count"] == 1);
    b.call("nexus.link", { { "from", record_ref(mads[0]["id"]) }, { "rel", "tests" },
			   { "to", ADD_REF } }, err, tier::proposer);
    cand = b.call("test.candidates", { { "ref", ADD_REF } }, err);
    CHECK(cand["fallback"] == false);
    REQUIRE(cand["tests"].size() == 1u);
    CHECK(cand["tests"][0]["id"] == mads[0]["id"]);

    // An external record: its command, the placeholders substituted.
    json pass = b.call("nexus.create",
		       { { "rkind", "test" },
			 { "fields", { { "name", "base" }, { "family", "external" },
				       { "command", "test {base} = /mcp-unit/calc" } } } },
		       err, tier::proposer);
    REQUIRE_FALSE(err);
    CHECK(says(b.call("test.run", { { "ids", json::array({ pass["id"] }) }, { "target", "nowhere" } }, err,
		      tier::editor), "no node offers 'nowhere'"));
    CHECK(says(b.call("test.run", { { "ids", json::array({ units[0]["id"] }) } }, err, tier::editor),
	       "no runner for family unit"));
    CHECK(says(b.call("test.run", { { "ids", json::array({ pass["id"] }) } }, err, tier::proposer),
	       "requires editor"));
    json run = b.call("test.run", { { "ids", json::array({ pass["id"] }) } }, err, tier::editor);
    CHECK_FALSE(err);
    CHECK(run["ran"] == 1);
    CHECK(run["passed"] == 1);
    CHECK(run["synced"] == false);				// the file is not on disk
    CHECK(run["rows"][0]["result"] == "pass");

    json fail = b.call("nexus.create",
		       { { "rkind", "test" },
			 { "fields", { { "name", "never" }, { "family", "external" },
				       { "command", "false" } } } },
		       err, tier::proposer);
    run = b.call("test.run", { { "ids", json::array({ fail["id"] }) } }, err, tier::editor);
    CHECK(run["failed"] == 1);
    CHECK(run["rows"][0]["result"] == "fail");
    CHECK(run["rows"][0]["exit"] == 1);

    json res = b.call("test.results", { { "test", pass["id"] } }, err);
    CHECK(res["rows"].size() == 1u);
    CHECK(res["latest"][std::to_string(pass["id"].get<int64_t>())]["result"] == "pass");

    for ( const char *f : files )
	std::remove((dir + f).c_str());
    rmdir(unit.c_str());
    rmdir(dir.c_str());
}

TEST_CASE("a bound stream restores from its file; a torn line is skipped")
{
    const std::string dir = temp_dir();
    REQUIRE_FALSE(dir.empty());
    const std::string path = dir + "/calc.events";
    bool err = true;
    {
	bench b;
	std::string why;
	REQUIRE(b.memory.bind(path, why));
	b.call("nexus.create", { { "rkind", "task" }, { "fields", { { "title", "t1" } } } }, err,
	       tier::proposer);
	CHECK(b.memory.head() == 1);
    }
    REQUIRE(madc::document_write_file(path, "{\"seq\":9,\"kind\":", true));	// a crash mid-append
    {
	bench b;
	std::string why;
	REQUIRE(b.memory.bind(path, why));
	CHECK(b.memory.head() == 1);
	json rows = b.call("nexus.records", json::object(), err)["rows"];
	REQUIRE(rows.size() == 1u);
	CHECK(rows[0]["fields"]["title"] == "t1");
	json t2 = b.call("nexus.create", { { "rkind", "task" } }, err, tier::proposer);
	CHECK(t2["id"] == 2);
    }
    {
	bench b;
	std::string why;
	REQUIRE(b.memory.bind(path, why));
	CHECK(b.memory.head() == 2);				// the record after the torn line
	CHECK(b.call("nexus.records", json::object(), err)["rows"].size() == 2u);
    }
    {
	bench b;
	std::string why;
	b.call("nexus.create", { { "rkind", "task" } }, err, tier::proposer);
	CHECK_FALSE(b.memory.bind(path, why));			// this session logged already
	CHECK(why.find("already logged") != std::string::npos);
    }
    std::remove(path.c_str());
    rmdir(dir.c_str());
}

TEST_CASE("a tool needing a layer the asset lacks refuses, naming it")
{
    bench b("/mcp-unit/notes.txt", "just words\n");
    bool err = false;
    json r = b.call("nexus.explain", { { "ref", ADD_REF } }, err);
    CHECK(err);
    CHECK(says(r, "no parseable layer for kind"));
    b.call("nexus.records", json::object(), err);	// managed: a file's asset
    CHECK_FALSE(err);
}
