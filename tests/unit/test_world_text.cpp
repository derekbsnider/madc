// Unit battery for madcdis/world_text.h — Track 7 Phase 1 S4: the tagged
// world format (parse/emit), world binding (apply/extract), and the
// SourceAdapter contract with line locators. Doc-level round-trip is the
// substrate half of gate G2 (the transcript-level proof lands in S5).
// Plan: docs/plans/2026-08-20-track7-phase1-text-adventure.md (S4).

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <cstdio>
#include <string>
#include <vector>

#include "madcdis/world_text.h"

using madc::hub::world;
using madc::hub::world_doc;
using madc::hub::world_doc_parse;
using madc::hub::world_doc_emit;
using madc::hub::world_doc_apply;
using madc::hub::world_doc_extract;
using madc::hub::world_text_adapter;
using madc::hub::entity_id;
using madc::hub::name_id;

static const char *SAMPLE =
    "# the pilot's miniature world\n"
    "%world 1\n"
    "%entity twisty-passage\n"
    "  title = Twisty Passage\n"
    "  desc = You are in a maze of twisty little passages, all alike.\n"
    "%entity brass-lantern\n"
    "  short = a brass lantern\n"
    "  fuel = 50\n"
    "  lit = false\n"
    "  portable = true\n"
    "%entity dead-end\n"
    "  title = Dead End\n"
    "%entity hero\n"
    "%link brass-lantern in twisty-passage\n"
    "%link hero in twisty-passage\n"
    "%link twisty-passage exit dead-end north\n"
    "%link dead-end exit twisty-passage south\n"
    "%verb take\n"
    "%verb edit key=builder refusal=Only a builder may reshape the world.\n"
    "%verb smite domain=wizardry min=3 refusal=You lack the wizardry.\n"
    "%require inspect key=builder refusal=Only a builder sees the bones.\n";

TEST_CASE("world_doc — parse recovers every declaration with kinds and lines")
{
    world_doc d;
    std::string err;
    REQUIRE(world_doc_parse(SAMPLE, d, err));
    CHECK(err.empty());
    CHECK(d.version == 1);
    REQUIRE(d.entities.size() == 4u);
    CHECK(d.entities[0].name == "twisty-passage");
    CHECK(d.entities[0].line == 3u);
    REQUIRE(d.entities[1].props.size() == 4u);
    CHECK(d.entities[1].props[1].first == "fuel");
    CHECK(d.entities[1].props[1].second.as_integer() == 50);
    CHECK(d.entities[1].props[2].second.as_boolean() == false);
    CHECK(d.entities[0].props[1].second.as_string()
	  == "You are in a maze of twisty little passages, all alike.");
    REQUIRE(d.links.size() == 4u);
    CHECK(d.links[2].key == "north");
    CHECK(d.links[0].key.empty());
    REQUIRE(d.verbs.size() == 3u);
    CHECK(d.verbs[1].keys.size() == 1u);
    CHECK(d.verbs[1].keys[0] == "builder");
    CHECK(d.verbs[1].refusal == "Only a builder may reshape the world.");
    CHECK(d.verbs[2].domain == "wizardry");
    CHECK(d.verbs[2].min_level == 3);
    REQUIRE(d.requires_.size() == 1u);
    CHECK(d.requires_[0].name == "inspect");
}

TEST_CASE("world_doc — apply binds entities, kinds, and keyed links")
{
    world_doc d;
    std::string err;
    REQUIRE(world_doc_parse(SAMPLE, d, err));
    world w;
    REQUIRE(world_doc_apply(d, w, err));
    CHECK(w.entity_count() == 4u);

    entity_id room = w.find("twisty-passage");
    entity_id lamp = w.find("brass-lantern");
    entity_id dead = w.find("dead-end");
    REQUIRE(room != 0u);
    REQUIRE(lamp != 0u);
    CHECK(w.get(lamp)->bag.as_object().at("fuel").as_integer() == 50);
    CHECK(w.get(lamp)->bag.as_object().at("portable").as_boolean());
    name_id rel_exit = w.intern("exit");
    name_id north = w.intern("north");
    CHECK(w.target(room, rel_exit, north) == dead);
    CHECK(w.sources(room, w.intern("in")).size() == 2u);	// lamp + hero
}

TEST_CASE("world_doc — round-trip: emit(extract(apply(parse(T)))) is stable")
{
    world_doc d;
    std::string err;
    REQUIRE(world_doc_parse(SAMPLE, d, err));
    world w;
    REQUIRE(world_doc_apply(d, w, err));

    world_doc out = world_doc_extract(w);
    out.verbs = d.verbs;		// the session layer merges its decls
    out.requires_ = d.requires_;
    std::string emitted = world_doc_emit(out);

    // A second trip through parse/apply/extract/emit is byte-identical —
    // the format is a fixed point after one canonicalization.
    world_doc d2;
    REQUIRE(world_doc_parse(emitted, d2, err));
    world w2;
    REQUIRE(world_doc_apply(d2, w2, err));
    world_doc out2 = world_doc_extract(w2);
    out2.verbs = d2.verbs;
    out2.requires_ = d2.requires_;
    CHECK(world_doc_emit(out2) == emitted);

    // And the second world answers the same questions as the first.
    CHECK(w2.entity_count() == w.entity_count());
    CHECK(w2.get(w2.find("brass-lantern"))->bag.as_object().at("fuel").as_integer() == 50);
    CHECK(w2.target(w2.find("twisty-passage"), w2.intern("exit"),
		    w2.intern("north")) == w2.find("dead-end"));
}

TEST_CASE("world_doc — loud errors name the line and the reason")
{
    world_doc d;
    std::string err;
    CHECK_FALSE(world_doc_parse("%entity x\n", d, err));	// no %world
    CHECK(err.find("version") != std::string::npos);

    world_doc d2;
    CHECK_FALSE(world_doc_parse("%world 1\n%bogus x\n", d2, err));
    CHECK(err.find("line 2") != std::string::npos);

    world_doc d3;
    CHECK_FALSE(world_doc_parse("%world 1\nfuel = 3\n", d3, err));
    CHECK(err.find("no open %entity") != std::string::npos);

    world_doc d4;
    std::string dup = "%world 1\n%entity a\n%entity a\n";
    REQUIRE(world_doc_parse(dup, d4, err));
    world w;
    CHECK_FALSE(world_doc_apply(d4, w, err));
    CHECK(err.find("duplicate") != std::string::npos);

    world_doc d5;
    std::string badlink = "%world 1\n%entity a\n%link a in nowhere\n";
    REQUIRE(world_doc_parse(badlink, d5, err));
    world w2;
    CHECK_FALSE(world_doc_apply(d5, w2, err));
    CHECK(err.find("nowhere") != std::string::npos);
}

TEST_CASE("world_text_adapter — the SourceAdapter contract with locators")
{
    // Write the sample to a scratch file the adapter can read.
    std::string path = "/tmp/madc_test_world_text.world";
    FILE *f = fopen(path.c_str(), "w");
    REQUIRE(f != (FILE *)0);
    fputs(SAMPLE, f);
    fclose(f);

    world_text_adapter adapter;
    madc::DataSource src(path);
    CHECK(adapter.can_read(src));

    std::vector<madc::ExtractedRecordType> types;
    REQUIRE(adapter.discover_types(src, types));
    REQUIRE(types.size() == 4u);
    CHECK(types[0].name() == "entity");

    std::vector<madc::ExtractedRecord> ents;
    madc::error err;
    REQUIRE(adapter.extract(src, "entity", ents, &err));
    REQUIRE(ents.size() == 4u);
    CHECK(ents[1].record.as_object().at("name").as_string() == "brass-lantern");
    CHECK(ents[1].record.as_object().at("props").as_object()
	      .at("fuel").as_integer() == 50);
    CHECK(ents[1].locator.locator_kind == madc::SourceLocator::kind::line_range);
    CHECK(ents[1].locator.line_start == 6u);	// "%entity brass-lantern"
    CHECK(ents[1].locator.line_count == 5u);	// decl + four props

    std::vector<madc::ExtractedRecord> links;
    REQUIRE(adapter.extract(src, "link", links, &err));
    REQUIRE(links.size() == 4u);
    CHECK(links[2].record.as_object().at("key").as_string() == "north");

    std::vector<madc::ExtractedRecord> reqs;
    REQUIRE(adapter.extract(src, "require", reqs, &err));
    REQUIRE(reqs.size() == 1u);
    CHECK(reqs[0].record.as_object().at("name").as_string() == "inspect");

    std::vector<madc::ExtractedRecord> none;
    CHECK_FALSE(adapter.extract(src, "no-such-family", none, &err));
    CHECK_FALSE(err.message.empty());

    remove(path.c_str());
}
