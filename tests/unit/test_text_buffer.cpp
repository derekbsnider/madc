// Unit battery for madcdis/text_buffer.h — the piece-table text component
// (Track 7.2 R4; Track 8.1 pulled forward). ORACLE: a plain std::string
// mirror — every mutation is applied to both and the materialized text
// must match byte-for-byte after each step; line/find queries are checked
// against the same mirror.
// Plan: docs/plans/2026-08-24-ui-interaction-rework-and-texteditor.md.

#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN
#include "doctest.h"

thread_local bool madc_verbose = false;
#define DBG(x) do { if(madc_verbose){x;} } while(0)

#include <string>

#include "madcdis/text_buffer.h"

using madc::hub::text_buffer;

// The mirrored mutation pair: piece table and oracle move in lockstep.
struct mirrored
{
    text_buffer b;
    std::string s;

    void load(const std::string &t) { b.load(t); s = t; }
    void insert(size_t off, const std::string &t)
    {
	b.insert(off, t);
	if ( off > s.size() ) off = s.size();
	s.insert(off, t);
    }
    void erase(size_t off, size_t len)
    {
	b.erase(off, len);
	if ( off < s.size() )
	    s.erase(off, len > s.size() - off ? s.size() - off : len);
    }
    void replace(size_t off, size_t len, const std::string &t)
    {
	erase(off, len);
	insert(off, t);
    }
    bool matches() const { return b.text() == s && b.size() == s.size(); }
};

TEST_CASE("piece table — load, insert, erase, replace mirror std::string")
{
    mirrored m;
    m.load("");
    CHECK(m.matches());
    CHECK(m.b.piece_count() == 0u);

    m.insert(0, "hello world");	// insert into empty
    CHECK(m.matches());
    m.insert(5, ",");		// interior split
    CHECK(m.matches());
    CHECK(m.b.text() == "hello, world");
    m.insert(0, ">> ");		// at front (piece boundary)
    CHECK(m.matches());
    m.insert(m.b.size(), "!");	// at end
    CHECK(m.matches());
    m.insert(999, "?");		// past end clamps to append
    CHECK(m.matches());
    CHECK(m.b.text() == ">> hello, world!?");

    m.erase(0, 3);		// whole leading piece
    CHECK(m.matches());
    m.erase(5, 2);		// across a piece boundary
    CHECK(m.matches());
    m.erase(m.b.size() - 1, 5);	// over-long tail erase clamps
    CHECK(m.matches());
    m.erase(2, 0);		// zero-length no-op
    CHECK(m.matches());
    m.erase(999, 4);		// past end no-op
    CHECK(m.matches());

    m.replace(0, 5, "HELLO");
    CHECK(m.matches());

    // The loaded snapshot is never moved: a fresh load resets everything.
    m.load("abc\ndef\n");
    CHECK(m.matches());
    CHECK(m.b.piece_count() == 1u);
}

TEST_CASE("piece table — slice and interior erase splits")
{
    mirrored m;
    m.load("0123456789");
    m.erase(3, 4);		// interior split of the single load piece
    CHECK(m.matches());
    CHECK(m.b.text() == "012789");
    CHECK(m.b.piece_count() == 2u);

    CHECK(m.b.slice(0, 3) == "012");
    CHECK(m.b.slice(2, 3) == "278");	// spans the split
    CHECK(m.b.slice(4, 99) == "89");	// clamps
    CHECK(m.b.slice(99, 1) == "");

    // A long alternating edit sequence stays in lockstep.
    for ( int i = 0; i < 40; ++i )
    {
	m.insert((size_t)(i * 7) % (m.b.size() + 1), "ab");
	CHECK(m.matches());
	m.erase((size_t)(i * 3) % (m.b.size() + 1), (size_t)i % 3);
	CHECK(m.matches());
    }
}

TEST_CASE("piece table — line model: terminated, trailing, empty")
{
    text_buffer b;
    b.load("");
    CHECK(b.line_count() == 0u);

    b.load("one\ntwo\nthree\n");	// fully terminated
    CHECK(b.line_count() == 3u);
    size_t off = 0, len = 0;
    REQUIRE(b.line_span(1, off, len));
    CHECK(b.slice(off, len) == "one");
    REQUIRE(b.line_span(3, off, len));
    CHECK(b.slice(off, len) == "three");
    CHECK_FALSE(b.line_span(4, off, len));
    CHECK_FALSE(b.line_span(0, off, len));

    b.load("one\ntwo");			// trailing unterminated span
    CHECK(b.line_count() == 2u);
    REQUIRE(b.line_span(2, off, len));
    CHECK(b.slice(off, len) == "two");

    b.load("\n\n");			// empty lines are lines
    CHECK(b.line_count() == 2u);
    REQUIRE(b.line_span(1, off, len));
    CHECK(len == 0u);

    // Lines survive edits that cross the piece structure.
    b.load("aa\nbb\ncc\n");
    b.replace(3, 2, "BXB");		// "bb" -> "BXB" splits pieces
    CHECK(b.text() == "aa\nBXB\ncc\n");
    CHECK(b.line_count() == 3u);
    REQUIRE(b.line_span(2, off, len));
    CHECK(b.slice(off, len) == "BXB");
}

TEST_CASE("piece table — find at and after an offset")
{
    text_buffer b;
    b.load("the cat sat on the mat");
    CHECK(b.find(0, "the") == 0u);
    CHECK(b.find(1, "the") == 15u);
    CHECK(b.find(16, "the") == text_buffer::npos);
    CHECK(b.find(0, "dog") == text_buffer::npos);
    CHECK(b.find(0, "") == text_buffer::npos);
    // Across edited piece boundaries.
    b.replace(4, 3, "CAT");
    CHECK(b.find(0, "CAT sat") == 4u);
}

TEST_CASE("piece table — checkpoint/undo: pieces-vector snapshots + payload")
{
    text_buffer b;
    std::string oracle = "one\ntwo\n";
    b.load(oracle);
    CHECK(b.history_depth() == 0u);

    // Checkpoint BEFORE each mutation (the editor cadence); the payload
    // is opaque — a caret here.
    b.checkpoint(madc::value((int64_t)0));
    b.insert(4, "TWO-");			// "one\nTWO-two\n"
    b.checkpoint(madc::value((int64_t)4));
    b.erase(0, 4);				// "TWO-two\n"
    CHECK(b.text() == "TWO-two\n");
    CHECK(b.history_depth() == 2u);

    madc::value meta;
    REQUIRE(b.undo(meta));			// back to post-insert
    CHECK(b.text() == "one\nTWO-two\n");
    CHECK(meta.as_integer() == 4);
    CHECK(b.line_count() == 2u);		// derived queries see the restore

    REQUIRE(b.undo(meta));			// back to the load state
    CHECK(b.text() == oracle);
    CHECK(meta.as_integer() == 0);
    CHECK(!b.undo(meta));			// history empty: refused

    // Undone state is fully live: edits after an undo work and can be
    // checkpointed again (the add buffer is append-only — old snapshots
    // never dangled while newer edits appended).
    b.checkpoint(madc::value((int64_t)7));
    b.insert(b.size(), "three\n");
    CHECK(b.text() == "one\ntwo\nthree\n");
    REQUIRE(b.undo(meta));
    CHECK(b.text() == oracle);
    CHECK(meta.as_integer() == 7);

    // load() replaces the sources: history dies with them.
    b.checkpoint(madc::value((int64_t)1));
    b.load("fresh");
    CHECK(b.history_depth() == 0u);
    CHECK(!b.undo(meta));
}

TEST_CASE("piece table — redo: two stacks, checkpoint clears redo")
{
    text_buffer b;
    b.load("one\n");
    b.checkpoint(madc::value((int64_t)0));
    b.insert(4, "two\n");			// "one\ntwo\n"
    b.checkpoint(madc::value((int64_t)4));
    b.insert(8, "three\n");			// "one\ntwo\nthree\n"
    CHECK(b.redo_depth() == 0u);

    // Each undo pairs the document being LEFT with the caret live on it
    // (now_meta), so redo restores both together.
    madc::value meta;
    REQUIRE(b.undo(meta, madc::value((int64_t)8)));
    CHECK(b.text() == "one\ntwo\n");
    CHECK(meta.as_integer() == 4);
    CHECK(b.redo_depth() == 1u);

    REQUIRE(b.undo(meta, madc::value((int64_t)4)));
    CHECK(b.text() == "one\n");
    CHECK(meta.as_integer() == 0);
    CHECK(b.redo_depth() == 2u);
    CHECK(!b.undo(meta, madc::value((int64_t)0)));	// refused undo
    CHECK(b.redo_depth() == 2u);			// disturbs nothing

    REQUIRE(b.redo(meta, madc::value((int64_t)0)));
    CHECK(b.text() == "one\ntwo\n");
    CHECK(meta.as_integer() == 4);
    CHECK(b.history_depth() == 1u);

    REQUIRE(b.redo(meta, madc::value((int64_t)4)));
    CHECK(b.text() == "one\ntwo\nthree\n");
    CHECK(meta.as_integer() == 8);
    CHECK(!b.redo(meta, madc::value((int64_t)8)));	// redo empty: refused

    // Redo pushed onto undo: the round trip walks back again.
    REQUIRE(b.undo(meta, madc::value((int64_t)8)));
    CHECK(b.text() == "one\ntwo\n");
    CHECK(b.redo_depth() == 1u);

    // A checkpoint is a new edit branch: redo dies.
    b.checkpoint(madc::value((int64_t)4));
    CHECK(b.redo_depth() == 0u);
    b.insert(4, "TWO-");			// "one\nTWO-two\n"
    REQUIRE(b.undo(meta, madc::value((int64_t)9)));
    CHECK(b.redo_depth() == 1u);

    // The legacy one-argument undo is destructive: no capture, so any
    // redo entries are stale and die with it.
    madc::value m2;
    REQUIRE(b.undo(m2));
    CHECK(b.text() == "one\n");
    CHECK(b.redo_depth() == 0u);

    // load() clears BOTH stacks.
    b.checkpoint(madc::value((int64_t)0));
    b.insert(0, "x");
    b.undo(meta, madc::value((int64_t)1));
    CHECK(b.redo_depth() == 1u);
    b.load("fresh");
    CHECK(b.history_depth() == 0u);
    CHECK(b.redo_depth() == 0u);
}

TEST_CASE("piece table — word motion: JOE ^Z/^X duals over [A-Za-z0-9_]")
{
    text_buffer b;
    b.load("int foo_1 = bar(2);\n");
    // words: "int" [0,3)  "foo_1" [4,9)  "bar" [12,15)  "2" [16,17)

    CHECK(b.word_right(0) == 3u);	// from a word: its end
    CHECK(b.word_right(3) == 9u);	// from a gap: end of the NEXT word
    CHECK(b.word_right(5) == 9u);	// mid-word: end of the current word
    CHECK(b.word_right(9) == 15u);
    CHECK(b.word_right(15) == 17u);
    CHECK(b.word_right(17) == 20u);	// no word left: clamps to the end
    CHECK(b.word_right(20) == 20u);
    CHECK(b.word_right(99) == 20u);	// past end clamps

    CHECK(b.word_left(20) == 16u);	// start of the previous word
    CHECK(b.word_left(16) == 12u);
    CHECK(b.word_left(12) == 4u);
    CHECK(b.word_left(6) == 4u);	// mid-word: start of the current word
    CHECK(b.word_left(4) == 0u);
    CHECK(b.word_left(0) == 0u);

    // Across edited piece boundaries, same answers as the flat string.
    b.replace(4, 5, "qux");		// "int qux = bar(2);\n"
    CHECK(b.word_right(3) == 7u);
    CHECK(b.word_left(10) == 4u);
}
