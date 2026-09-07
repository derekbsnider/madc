// Unit battery for madcdis/keys.h — the ONE key owner shared by every ui
// model (the web-provider engine plan, Task 1): key spelling in both
// directions, the bindings table's whole-table validation and JOE chord
// conventions, and the chord resolver (key_resolver) — standing alone, and
// riding tui_model::apply_keys (the grid model consumes the resolver; the
// coalescing of a printable run AROUND a chord is the model's §7.5 contract
// and is asserted through the model on purpose).
// Plan: docs/plans/2026-09-07-web-provider-engine-plan.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>
#include <vector>

#include "madcdis/keys.h"
#include "madcdis/tui_model.h"	// the consumer, for the through-the-model chord cases

using madc::hub::tui_key;
using madc::hub::tui_keyev;
using madc::hub::tui_event;
using madc::hub::tui_event_kind;
using madc::hub::tui_model;
using madc::hub::tui_bindings;
using madc::hub::tui_key_name;
using madc::hub::tui_key_from_name;
using madc::hub::key_step;
using madc::hub::key_resolver;

TEST_CASE("key spelling — one owner, both directions")
{
    CHECK(tui_key_name(tui_keyev(tui_key::ctrl, 'k')) == "^k");
    CHECK(tui_key_name(tui_keyev(tui_key::ch, 's')) == "s");
    CHECK(tui_key_name(tui_keyev(tui_key::ch, ' ')) == "space");
    CHECK(tui_key_name(tui_keyev(tui_key::pgup)) == "pgup");

    tui_keyev k;
    REQUIRE(tui_key_from_name("^s", k));
    CHECK(k.kind == tui_key::ctrl);
    CHECK(k.ch == 's');
    REQUIRE(tui_key_from_name("^S", k));	// generous in, canonical out
    CHECK(k.ch == 's');
    REQUIRE(tui_key_from_name("q", k));
    CHECK(k.kind == tui_key::ch);
    CHECK(k.ch == 'q');
    REQUIRE(tui_key_from_name("space", k));
    CHECK(k.kind == tui_key::ch);
    CHECK(k.ch == ' ');
    REQUIRE(tui_key_from_name("home", k));
    CHECK(k.kind == tui_key::home);
    CHECK(!tui_key_from_name("", k));
    CHECK(!tui_key_from_name("^!", k));
    CHECK(!tui_key_from_name("nosuch", k));

    // Punctuation controls round-trip like letters do.
    CHECK(tui_key_name(tui_keyev(tui_key::ctrl, '_')) == "^_");
    CHECK(tui_key_name(tui_keyev(tui_key::ctrl, '^')) == "^^");
    REQUIRE(tui_key_from_name("^_", k));
    CHECK(k.kind == tui_key::ctrl);
    CHECK(k.ch == '_');
    REQUIRE(tui_key_from_name("^^", k));
    CHECK(k.ch == '^');
    REQUIRE(tui_key_from_name("^\\", k));
    CHECK(k.ch == '\\');
    REQUIRE(tui_key_from_name("^]", k));
    CHECK(k.ch == ']');
}

TEST_CASE("bindings — build validation is loud and whole-table")
{
    tui_bindings b;
    CHECK(!b.bind("^k nosuchkey", "x"));	// unknown spelling
    CHECK(!b.bind("", "x"));
    CHECK(b.bind("^K S", "save"));		// normalizes to "^k s"
    CHECK(b.bind("^k q", "quit"));
    std::string err;
    CHECK(b.finalize(err));
    CHECK(b.bound("^k s"));
    CHECK(b.action_of("^k s") == "save");
    CHECK(b.prefix("^k"));
    CHECK(!b.bound("^k"));

    tui_bindings head;				// printable-headed: refused
    CHECK(head.bind("g g", "goto"));
    CHECK(!head.finalize(err));
    CHECK(err.find("printable-headed") != std::string::npos);

    tui_bindings shadow;			// prefix conflict: refused
    CHECK(shadow.bind("^k", "block"));
    CHECK(shadow.bind("^k s", "save"));
    CHECK(!shadow.finalize(err));
    CHECK(err.find("shadows") != std::string::npos);

    // JOE's OTHER chord convention: a ctrl+letter CONTINUATION is the
    // letter (^K ^Z == ^K Z — users keep ctrl held). Both spellings
    // canonicalize to one slot; ctrl+punctuation continuations keep
    // their ctrl form (^K ^_ is not ^K _).
    tui_bindings ctrlcont;
    CHECK(ctrlcont.bind("^k ^z", "shell"));
    CHECK(ctrlcont.bind("^k ^_", "special"));
    CHECK(ctrlcont.finalize(err));
    CHECK(ctrlcont.bound("^k z"));
    CHECK(ctrlcont.action_of("^k z") == "shell");
    CHECK(ctrlcont.bound("^k ^_"));
    CHECK(!ctrlcont.bound("^k _"));
    CHECK(tui_bindings::cont_spelling(tui_keyev(tui_key::ctrl, 'z')) == "z");
    CHECK(tui_bindings::cont_spelling(tui_keyev(tui_key::ctrl, '_')) == "^_");
    CHECK(tui_bindings::cont_spelling(tui_keyev(tui_key::ch, 'Z')) == "z");
}

TEST_CASE("key_resolver — the chord state machine stands alone")
{
    tui_bindings b;
    REQUIRE(b.bind("^k s", "save"));
    REQUIRE(b.bind("^q", "quit"));
    std::string err;
    REQUIRE(b.finalize(err));
    key_resolver r;
    r.set_bindings(b);
    // A printable with no chord pending is the model's business: the
    // resolver classifies it passthrough and touches no state.
    CHECK(r.step(tui_keyev(tui_key::ch, 'a')).k == key_step::kind::passthrough);
    CHECK(r.pending().empty());
    // An unbound non-printable with no chord pending: passthrough too.
    CHECK(r.step(tui_keyev(tui_key::tab)).k == key_step::kind::passthrough);
    key_step s1 = r.step(tui_keyev(tui_key::ctrl, 'k'));
    CHECK(s1.k == key_step::kind::pending);
    CHECK(r.pending() == "^k");
    key_step s2 = r.step(tui_keyev(tui_key::resize));
    CHECK(s2.k == key_step::kind::transparent);
    CHECK(r.pending() == "^k");
    CHECK(r.step(tui_keyev(tui_key::wake)).k == key_step::kind::transparent);
    CHECK(r.pending() == "^k");
    key_step s3 = r.step(tui_keyev(tui_key::ch, 'S'));	// case-insensitive continuation
    CHECK(s3.k == key_step::kind::action);
    CHECK(s3.action_name == "save");
    CHECK(s3.seq == "^k s");
    CHECK(r.pending().empty());
    key_step s4 = r.step(tui_keyev(tui_key::ctrl, 'q'));
    CHECK(s4.k == key_step::kind::action);
    CHECK(s4.action_name == "quit");
    CHECK(s4.seq == "^q");
    r.step(tui_keyev(tui_key::ctrl, 'k'));
    CHECK(r.step(tui_keyev(tui_key::esc)).k == key_step::kind::cancelled);
    CHECK(r.pending().empty());
    r.step(tui_keyev(tui_key::ctrl, 'k'));
    key_step miss = r.step(tui_keyev(tui_key::ch, 'z'));
    CHECK(miss.k == key_step::kind::action);
    CHECK(miss.action_name.empty());
    CHECK(miss.seq == "^k z");
    CHECK(r.pending().empty());
    // A profile swap abandons the chord in flight with its table.
    r.step(tui_keyev(tui_key::ctrl, 'k'));
    CHECK(r.pending() == "^k");
    r.set_bindings(tui_bindings());
    CHECK(r.pending().empty());
    CHECK(r.bindings().empty());
    // With no table installed every key is passthrough — the pre-bindings
    // adapter's byte-identical behaviour rides this.
    CHECK(r.step(tui_keyev(tui_key::ctrl, 'k')).k == key_step::kind::passthrough);
    CHECK(r.step(tui_keyev(tui_key::esc)).k == key_step::kind::passthrough);
}

TEST_CASE("chords — a ctrl-held continuation completes the chord (JOE)")
{
    tui_bindings b;
    b.bind("^k z", "shell");
    std::string err;
    REQUIRE(b.finalize(err));
    tui_model m;
    m.set_bindings(b);
    std::vector<tui_keyev> keys;
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::ctrl, 'z'));	// ctrl still held
    std::vector<tui_event> ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::focus);	// chord opened: repaint (%k)
    CHECK(ev[1].kind == tui_event_kind::action);
    CHECK(ev[1].action_name == "shell");
    CHECK(ev[1].seq == "^k z");

    // A three-key chord repaints on the open AND on each extension —
    // the %k echo grows live ("^k", then "^k e") before the action.
    tui_bindings b3;
    b3.bind("^k e c", "deep");
    REQUIRE(b3.finalize(err));
    tui_model m3;
    m3.set_bindings(b3);
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::ch, 'e'));
    keys.push_back(tui_keyev(tui_key::ch, 'c'));
    ev = m3.apply_keys(keys);
    REQUIRE(ev.size() == 3u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::focus);
    CHECK(ev[2].kind == tui_event_kind::action);
    CHECK(ev[2].action_name == "deep");
    CHECK(ev[2].seq == "^k e c");
}

static tui_bindings joe_table()
{
    tui_bindings b;
    b.bind("^k s", "save");
    b.bind("^k q", "quit");
    b.bind("^s", "search");	// a single-key binding rides the same table
    std::string err;
    REQUIRE(b.finalize(err));
    return b;
}

TEST_CASE("chords — resolve, coalesce around, miss, cancel, persist")
{
    tui_model m;
    m.set_bindings(joe_table());

    // A printable run flushes BEFORE the chord fires; the chord OPENING
    // emits a focus (repaint) event — the pending prefix is visible
    // state (JOE's %k echo); the chord's own printable continuation
    // never joins a text run.
    std::vector<tui_keyev> keys;
    keys.push_back(tui_keyev(tui_key::ch, 'a'));
    keys.push_back(tui_keyev(tui_key::ch, 'b'));
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::ch, 's'));
    keys.push_back(tui_keyev(tui_key::ch, 'c'));
    std::vector<tui_event> ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 4u);
    CHECK(ev[0].kind == tui_event_kind::text);
    CHECK(ev[0].text == "ab");
    CHECK(ev[1].kind == tui_event_kind::focus);
    CHECK(ev[2].kind == tui_event_kind::action);
    CHECK(ev[2].action_name == "save");
    CHECK(ev[2].seq == "^k s");
    CHECK(ev[3].kind == tui_event_kind::text);
    CHECK(ev[3].text == "c");

    // Chord continuations are letter-case-insensitive (JOE's ^K S == ^K s):
    // a shifted continuation matches the same binding.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::ch, 'S'));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::action);
    CHECK(ev[1].action_name == "save");
    CHECK(ev[1].seq == "^k s");

    // Single-key binding fires directly.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 's'));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::action);
    CHECK(ev[0].action_name == "search");
    CHECK(ev[0].seq == "^s");

    // An unbound completion reports the miss: empty action, the seq.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::ch, 'z'));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::action);
    CHECK(ev[1].action_name == "");
    CHECK(ev[1].seq == "^k z");

    // esc cancels a pending chord — the cancel repaints too (the %k
    // echo must clear); the next key is ordinary.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::esc));
    keys.push_back(tui_keyev(tui_key::ch, 'x'));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 3u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::focus);
    CHECK(m.pending_chord() == "");
    CHECK(ev[2].kind == tui_event_kind::text);
    CHECK(ev[2].text == "x");

    // A resize passes through mid-chord and the chord still completes —
    // across apply_keys BATCHES (pending is adapter state, and the
    // start-focus event leaves it readable for the %k seat).
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::resize));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::resize);
    CHECK(m.pending_chord() == "^k");
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ch, 'q'));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::action);
    CHECK(ev[0].action_name == "quit");

    // A wake (stage-2: cooperative tasks drained) has resize's exact
    // transparency: it passes through mid-chord without disturbing the
    // pending prefix, and the chord still completes.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ctrl, 'k'));
    keys.push_back(tui_keyev(tui_key::wake));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 2u);
    CHECK(ev[0].kind == tui_event_kind::focus);
    CHECK(ev[1].kind == tui_event_kind::wake);
    CHECK(m.pending_chord() == "^k");
    keys.clear();
    keys.push_back(tui_keyev(tui_key::ch, 'q'));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::action);
    CHECK(ev[0].action_name == "quit");
    // Outside a chord: one wake in, one wake event out.
    keys.clear();
    keys.push_back(tui_keyev(tui_key::wake));
    ev = m.apply_keys(keys);
    REQUIRE(ev.size() == 1u);
    CHECK(ev[0].kind == tui_event_kind::wake);
}
