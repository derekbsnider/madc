// Unit battery for madcdis/document.h and the world's edit notification
// (split arc Phase 1A.1; design docs/plans/2026-10-09-document-component-
// design.md). ORACLE: the design's stated rules — one record per change
// with the byte-precise shape, versions that only grow, the anchor shift
// rule case by case, modified derived from the saved version, undo
// restoring anchors and the saved state, refusals (read-only, a mutation
// inside a delivery), delivery order, and a plain text (no document)
// publishing too.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>
#include <vector>

#include "madcdis/hub.h"

using madc::hub::world;
using madc::hub::entity_id;
using madc::hub::anchor_id;
using madc::hub::document;
using madc::hub::text_change;
using madc::hub::text_listener;

// A listener that records every delivery (and, optionally, tries to edit
// from inside one — the textlock case).
struct recorder : text_listener
{
    std::vector<text_change> seen;
    std::vector<std::string> inserted;	// copied: the pointer dies with delivery
    bool try_edit;
    bool edit_admitted;
    recorder() : try_edit(false), edit_admitted(true) {}
    void on_text_change(world &w, const text_change &c)
    {
	seen.push_back(c);
	inserted.push_back(c.inserted ? *c.inserted : std::string());
	if ( try_edit )
	    edit_admitted = w.text_insert(c.entity, 0, "X");
    }
};

static entity_id open_doc(world &w, const std::string &text,
			  const std::string &path = "a.c")
{
    entity_id e = w.create("document");
    REQUIRE(w.doc_attach(e, path, 33, text));	// 33: any kind code
    return e;
}

TEST_CASE("every mutation publishes one byte-precise record and a new version")
{
    world w;
    recorder r;
    w.text_subscribe(&r);
    entity_id e = open_doc(w, "hello world");
    // attach = one load record
    REQUIRE(r.seen.size() == 1u);
    CHECK(r.seen[0].what == text_change::kind::load);
    CHECK(r.seen[0].old_len == 0u);
    CHECK(r.seen[0].new_len == 11u);
    uint64_t v0 = w.text_version(e);
    CHECK(v0 >= 1u);

    CHECK(w.text_insert(e, 5, ","));
    REQUIRE(r.seen.size() == 2u);
    CHECK(r.seen[1].what == text_change::kind::splice);
    CHECK(r.seen[1].off == 5u);
    CHECK(r.seen[1].old_len == 0u);
    CHECK(r.seen[1].new_len == 1u);
    CHECK(r.inserted[1] == ",");
    CHECK(r.seen[1].version == v0 + 1);
    CHECK(w.text_version(e) == v0 + 1);

    CHECK(w.text_erase(e, 0, 6));	// "hello," -> "world"
    REQUIRE(r.seen.size() == 3u);
    CHECK(r.seen[2].off == 0u);
    CHECK(r.seen[2].old_len == 6u);
    CHECK(r.seen[2].new_len == 0u);
    CHECK(w.text_of(e)->text() == " world");

    CHECK(w.text_replace(e, 1, 5, "there"));	// ONE record, not two
    REQUIRE(r.seen.size() == 4u);
    CHECK(r.seen[3].off == 1u);
    CHECK(r.seen[3].old_len == 5u);
    CHECK(r.seen[3].new_len == 5u);
    CHECK(r.inserted[3] == "there");
    CHECK(w.text_of(e)->text() == " there");
    CHECK(w.text_version(e) == v0 + 3);

    // A no-op is admitted and publishes nothing, moves no version.
    CHECK(w.text_insert(e, 0, ""));
    CHECK(w.text_erase(e, 0, 0));
    CHECK(w.text_replace(e, 2, 0, ""));
    CHECK(r.seen.size() == 4u);
    CHECK(w.text_version(e) == v0 + 3);

    // A load is a whole-buffer record with both sizes.
    CHECK(w.text_load(e, "xy"));
    REQUIRE(r.seen.size() == 5u);
    CHECK(r.seen[4].what == text_change::kind::load);
    CHECK(r.seen[4].old_len == 6u);
    CHECK(r.seen[4].new_len == 2u);
    CHECK(r.seen[4].inserted == (const std::string *)0);
    w.text_unsubscribe(&r);
}

TEST_CASE("anchors shift by the one rule, for every gravity and position")
{
    world w;
    entity_id e = open_doc(w, "0123456789");
    anchor_id before = w.anchor_add(e, 2, false);
    anchor_id at_l = w.anchor_add(e, 5, false);
    anchor_id at_r = w.anchor_add(e, 5, true);
    anchor_id inside = w.anchor_add(e, 7, true);
    anchor_id after = w.anchor_add(e, 9, false);
    CHECK(w.anchor_of(e, 99) == (const madc::hub::anchor *)0);

    // insert at 5 (old_len 0): before unchanged; AT 5 moves iff right
    CHECK(w.text_insert(e, 5, "ab"));
    CHECK(w.anchor_of(e, before)->off == 2u);
    CHECK(w.anchor_of(e, at_l)->off == 5u);
    CHECK(w.anchor_of(e, at_r)->off == 7u);
    CHECK(w.anchor_of(e, inside)->off == 9u);
    CHECK(w.anchor_of(e, after)->off == 11u);

    // replace [4, 10) with "Z": before unchanged; 5, 7, 9 inside -> 4;
    // 11 past the end -> 11 - 6 + 1 = 6
    CHECK(w.text_replace(e, 4, 6, "Z"));
    CHECK(w.anchor_of(e, before)->off == 2u);
    CHECK(w.anchor_of(e, at_l)->off == 4u);
    CHECK(w.anchor_of(e, at_r)->off == 4u);
    CHECK(w.anchor_of(e, inside)->off == 4u);
    CHECK(w.anchor_of(e, after)->off == 6u);

    // erase [0, 3): before (2) inside -> 0; the rest move by -3
    CHECK(w.text_erase(e, 0, 3));
    CHECK(w.anchor_of(e, before)->off == 0u);
    CHECK(w.anchor_of(e, at_l)->off == 1u);
    CHECK(w.anchor_of(e, after)->off == 3u);

    // a load clamps
    CHECK(w.text_load(e, "ab"));
    CHECK(w.anchor_of(e, after)->off == 2u);

    CHECK(w.anchor_move(e, after, 1));
    CHECK(w.anchor_of(e, after)->off == 1u);
    CHECK(w.anchor_remove(e, after));
    CHECK(!w.anchor_remove(e, after));
    CHECK(w.anchor_of(e, after) == (const madc::hub::anchor *)0);
    // an anchor past the end clamps on add
    anchor_id far = w.anchor_add(e, 100, true);
    CHECK(w.anchor_of(e, far)->off == 2u);
    // no document, no anchors
    entity_id plain = w.create("x");
    w.text_load(plain, "abc");
    CHECK(w.anchor_add(plain, 1, true) == 0u);
}

TEST_CASE("modified is derived from the saved version; undo restores anchors and the saved state")
{
    world w;
    entity_id e = open_doc(w, "abc");
    CHECK(!w.doc_modified(e));
    anchor_id caret = w.anchor_add(e, 3, true);

    madc::value app = madc::value::make_object();
    app.object()["caret"] = madc::value((int64_t)3);
    CHECK(w.doc_checkpoint(e, app));
    CHECK(w.text_insert(e, 3, "def"));
    CHECK(w.doc_modified(e));
    CHECK(w.anchor_of(e, caret)->off == 6u);
    uint64_t v_after = w.text_version(e);

    madc::value back;
    madc::value now = madc::value::make_object();
    now.object()["caret"] = madc::value((int64_t)6);
    CHECK(w.doc_undo(e, back, now));
    CHECK(w.text_of(e)->text() == "abc");
    CHECK(back.is_object());
    CHECK(back.as_object().find("caret")->second.as_integer() == 3);
    CHECK(w.anchor_of(e, caret)->off == 3u);	// restored, not shifted
    CHECK(!w.doc_modified(e));			// back at the saved version
    CHECK(w.text_version(e) == v_after + 1);	// an undo is a NEW version

    CHECK(w.doc_redo(e, back));
    CHECK(w.text_of(e)->text() == "abcdef");
    CHECK(w.doc_modified(e));
    CHECK(w.anchor_of(e, caret)->off == 6u);	// the redo entry carried the live anchors
    // ... and the application's payload the undo was given for the state
    // it left: a redo hands back the caret that belonged to it
    REQUIRE(back.is_object());
    CHECK(back.as_object().find("caret")->second.as_integer() == 6);
    CHECK(!w.doc_redo(e, back));		// nothing more

    // save, then edit, then undo past the save: modified again
    CHECK(w.doc_mark_saved(e));
    CHECK(!w.doc_modified(e));
    CHECK(w.doc_checkpoint(e, madc::value()));
    CHECK(w.text_erase(e, 0, 1));
    CHECK(w.doc_modified(e));
    CHECK(w.doc_undo(e, back));
    CHECK(!w.doc_modified(e));
    // the document's path / kind / ring
    CHECK(w.doc_of(e)->path() == "a.c");
    CHECK(w.doc_of(e)->kind() == 33);
    CHECK(w.doc_set_path(e, "b.cpp", 65));
    CHECK(w.doc_of(e)->kind() == 65);
    CHECK(w.docs().size() == 1u);
    CHECK(w.docs()[0] == e);
    CHECK(w.doc_set_parse(e, 7, w.text_version(e)));
    CHECK(w.doc_of(e)->parse_handle() == 7);
    CHECK(w.doc_detach(e));
    CHECK(!w.has_doc(e));
    CHECK(!w.has_text(e));
    CHECK(w.docs().empty());
}

TEST_CASE("refusals: a read-only document, a mutation inside a delivery, a second attach")
{
    world w;
    entity_id e = open_doc(w, "abc");
    CHECK(!w.doc_attach(e, "x", 1, "y"));	// already a document
    CHECK(w.doc_set_read_only(e, true));
    CHECK(!w.text_insert(e, 0, "z"));
    CHECK(!w.text_erase(e, 0, 1));
    CHECK(!w.text_replace(e, 0, 1, "z"));
    CHECK(w.text_of(e)->text() == "abc");
    CHECK(w.text_load(e, "reloaded"));	// a reload is admitted
    CHECK(w.doc_set_read_only(e, false));

    recorder r;
    r.try_edit = true;
    w.text_subscribe(&r);
    CHECK(w.text_insert(e, 0, "Q"));
    CHECK(r.seen.size() == 1u);
    CHECK(!r.edit_admitted);		// the inner edit was refused
    CHECK(w.text_of(e)->text() == "Qreloaded");	// and the outer one intact
    CHECK(!w.publishing());
    w.text_unsubscribe(&r);
    CHECK(!w.text_unsubscribe(&r));
}

TEST_CASE("listeners run in subscription order; a plain text publishes too")
{
    world w;
    recorder a, b;
    w.text_subscribe(&a);
    w.text_subscribe(&b);
    entity_id plain = w.create("log");
    CHECK(w.text_load(plain, ""));
    CHECK(w.text_insert(plain, 0, "line\n"));
    REQUIRE(a.seen.size() == 2u);
    REQUIRE(b.seen.size() == 2u);
    CHECK(a.seen[1].entity == plain);
    CHECK(a.inserted[1] == "line\n");
    CHECK(w.text_version(plain) == 2u);
    // the raw text history publishes whole-buffer records too
    madc::value m;
    CHECK(w.text_checkpoint(plain, madc::value()));
    CHECK(w.text_insert(plain, 0, "x"));
    CHECK(w.text_undo(plain, m));
    REQUIRE(a.seen.size() == 4u);
    CHECK(a.seen[3].what == text_change::kind::undo);
    CHECK(a.seen[3].old_len == 6u);
    CHECK(a.seen[3].new_len == 5u);
    w.text_unsubscribe(&a);
    w.text_unsubscribe(&b);
}
