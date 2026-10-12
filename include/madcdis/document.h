#ifndef __MADCDIS_DOCUMENT_H
#define __MADCDIS_DOCUMENT_H 1

// madcdis/document.h — the DOCUMENT component (split arc Phase 1A.1; design
// docs/plans/2026-10-09-document-component-design.md): a file buffer in the
// engine. The hub's THIRD component kind, attached sparsely by entity id
// beside the text component (madcdis/text_buffer.h), which holds the text,
// its undo snapshots and its version. This one holds what makes a text a
// FILE: path, kind, read-only, the version last saved (modified is derived
// from it), the parse-handle SLOT with the version it was parsed from, and
// the ANCHORS — byte positions that follow edits (a caret, a mark, a span
// end). Owner rule (2026-10-09): file buffers are madc's, in the process
// that EDITS, never the one that runs user code.
//
// LAYERING: madcdis is the data substrate below the compiler. The component
// STORES; the publics one layer up (src/ns_ui.cpp, src/ns_madc.cpp) do the
// disk I/O, stamp the kind (madc::file_kind_of_path) and open or refresh
// the parse handle (madc::doc_parse). Nothing here names a file format, a
// language or an application.
//
// Also here: the EDIT NOTIFICATION's record and listener (design §2.2). The
// notification hangs on the TEXT component — every world::text_* mutator
// publishes one record, for any entity with text — and is declared here
// because the document is its first consumer (anchors shift on it).
//
// THREAD-SAFETY CONTRACT (.claude/rules/thread-safety.md): plain values,
// confined to the hub world's thread. A listener runs on that thread,
// synchronously, after the mutation, with mutation locked (hub.h).

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

#include "libmadc/value.h"
#include "madc_project.h"	// ProjectTU: a served project's document parses as its TU
#include "madc/bits/ui_enums"	// ui::text_change_kind — the shared enum text

namespace madc {
namespace hub {

typedef uint32_t entity_id;	// hub.h's — restated so this header stands alone
typedef uint32_t anchor_id;	// 0 = none

// ---------------------------------------------------------------- text_change
// ONE record shape for every change of a text (design §2.2, the Neovim
// on_bytes model): a SPLICE is a byte-precise range replacement — the
// bytes [off, off+old_len) became `inserted` (new_len bytes); an insert is
// old_len == 0, an erase new_len == 0, a replace ONE record (never erase +
// insert: an anchor inside a replaced range lands once). LOAD, UNDO and
// REDO are whole-buffer events: off = 0, old_len = the size before, new_len
// = the size after, inserted = NULL — a consumer that must stay exact
// resyncs from the text. `version` is the text's version AFTER the change.
// Rows and columns are the consumer's to derive (text_buffer::line_of);
// there is no second, line-granular record.
struct text_change
{
    typedef ::ui::text_change_kind kind;	// <bits/ui_enums>: one text

    entity_id   entity;
    uint64_t    version;
    kind        what;
    size_t      off;
    size_t      old_len;
    size_t      new_len;
    const std::string *inserted;	// splice only; valid during delivery

    text_change()
	: entity(0), version(0), what(kind::splice), off(0), old_len(0), new_len(0),
	  inserted(0) {}
};

// A listener: implemented by whatever observes a world's texts (the
// document component's anchor shift is applied by the world itself, before
// listeners run; a change journal, a colour-dirty tracker, a mirror's
// version check are listeners). Delivered in subscription order; a text
// mutation attempted inside on_text_change is REFUSED (the textlock rule) —
// schedule it.
class world;
class text_listener
{
public:
    virtual ~text_listener() {}
    virtual void on_text_change(world &w, const text_change &c) = 0;
};

// --------------------------------------------------------------------- anchor
// A byte position that follows edits. `right` gravity: an insert AT the
// anchor's offset pushes it after the inserted text (a caret, a span END);
// `left`: it stays before (a selection START, a span START). Ids are stable
// for the document's life; a removed anchor's id is never reused.
struct anchor
{
    anchor_id id;
    size_t    off;
    bool      right;
    anchor() : id(0), off(0), right(false) {}
};

// The ONE shift rule (design §2.3): position `p` across a splice at `off`
// replacing old_len bytes with new_len — a position before `off` is
// unchanged; one inside the replaced range collapses to `off`; one at or past
// its end moves by the size difference; one exactly AT `off` when nothing was
// erased moves iff its gravity is `right`. A document's anchors shift by it;
// a client's own byte positions over any text (ui::anchor_shift) do too.
inline size_t shift_position(size_t p, bool right, size_t off, size_t old_len,
			     size_t new_len)
{
    if ( p < off )
	return p;
    if ( p == off && old_len == 0 )
	return right ? p + new_len : p;
    if ( p < off + old_len )
	return off;
    return p - old_len + new_len;
}

// ------------------------------------------------------------------- document
class document
{
    std::string _path;		// "" = untitled
    int64_t     _kind;		// a <bits/file_kinds> code; stamped by the public
    bool        _read_only;
    uint64_t    _saved_version;	// the text's version when last saved/loaded
    int64_t     _parse_handle;	// 0 = none — an opaque compiler-side handle
    uint64_t    _parse_version;	// the text version the handle was parsed from
    ProjectTU   _parse_tu;	// the TU it parses as (when _has_tu)
    bool        _has_tu;
    int64_t     _parse_standard;	// 0 = its kind's (the family's default)
    int64_t     _parsed_standard;	// what the handle was OPENED as: its standard
    std::string _parsed_name;	// ... and its unit's name
    bool        _parse_busy;	// a parse of it is in flight (it yields)
    std::vector<anchor> _anchors;
    anchor_id   _next_anchor;

public:
    document()
	: _kind(0), _read_only(false), _saved_version(0), _parse_handle(0),
	  _parse_version(0), _has_tu(false), _parse_standard(0), _parsed_standard(0),
	  _parse_busy(false), _next_anchor(1) {}

    // --- identity
    const std::string &path() const { return _path; }
    void set_path(const std::string &p) { _path = p; }
    bool untitled() const { return _path.empty(); }
    int64_t kind() const { return _kind; }
    void set_kind(int64_t k) { _kind = k; }
    bool read_only() const { return _read_only; }
    void set_read_only(bool ro) { _read_only = ro; }

    // --- modified is DERIVED (design §2.4): the text's version differs
    // from the one last saved. mark_saved records "the file on disk IS
    // this version". Undoing back to the saved version reads clean again.
    uint64_t saved_version() const { return _saved_version; }
    void mark_saved(uint64_t version) { _saved_version = version; }
    bool modified(uint64_t version) const { return version != _saved_version; }

    // --- the parse slot (design §2.5): the component stores the handle and
    // the version it describes; madc::doc_parse (the compiler-side public)
    // decides when to refresh — one version compare, never a text compare.
    int64_t parse_handle() const { return _parse_handle; }
    uint64_t parse_version() const { return _parse_version; }
    void set_parse(int64_t handle, uint64_t version)
    {
	_parse_handle = handle;
	_parse_version = version;
    }
    // The TU a served project's document parses as (split arc 1C): its
    // manifest entry's -I, -D and standard, as the --project build compiles
    // it; none (NULL) = the opener's include list. madc::lang reads it.
    const ProjectTU *parse_tu() const { return _has_tu ? &_parse_tu : 0; }
    void set_parse_tu(const ProjectTU &tu)
    {
	_parse_tu = tu;
	_has_tu = true;
    }
    // The standard it parses under: 0 = its kind's, the family's default
    // (the CLI's rule); an application's language choice (an IDE's
    // Language table) names another. madc::lang reads it.
    int64_t parse_standard() const { return _parse_standard; }
    void set_parse_standard(int64_t s) { _parse_standard = s; }
    // What the handle was OPENED as — the standard and the unit's name: the
    // door reopens it when either moved since (a new standard, a Save As).
    int64_t parsed_standard() const { return _parsed_standard; }
    const std::string &parsed_name() const { return _parsed_name; }
    void set_parse_opened(int64_t standard, const std::string &name)
    {
	_parsed_standard = standard;
	_parsed_name = name;
    }
    // A parse yields between slices, so a second caller can arrive while
    // one runs: it waits for this to clear (madc::lang's door owns both).
    bool parse_busy() const { return _parse_busy; }
    void set_parse_busy(bool b) { _parse_busy = b; }

    // --- anchors (design §2.3)
    anchor_id anchor_add(size_t off, bool right)
    {
	anchor a;
	a.id = _next_anchor++;
	a.off = off;
	a.right = right;
	_anchors.push_back(a);
	return a.id;
    }
    // The anchor, or NULL when absent (the only way to read one).
    const anchor *anchor_of(anchor_id id) const
    {
	for ( size_t i = 0; i < _anchors.size(); ++i )
	    if ( _anchors[i].id == id )
		return &_anchors[i];
	return 0;
    }
    bool anchor_move(anchor_id id, size_t off)
    {
	for ( size_t i = 0; i < _anchors.size(); ++i )
	    if ( _anchors[i].id == id )
	    {
		_anchors[i].off = off;
		return true;
	    }
	return false;
    }
    bool anchor_remove(anchor_id id)
    {
	for ( size_t i = 0; i < _anchors.size(); ++i )
	    if ( _anchors[i].id == id )
	    {
		_anchors.erase(_anchors.begin() + i);
		return true;
	    }
	return false;
    }
    size_t anchor_count() const { return _anchors.size(); }

    // Every anchor across a splice at `off` replacing old_len bytes with
    // new_len, by the one rule (shift_position).
    void shift(size_t off, size_t old_len, size_t new_len)
    {
	for ( size_t i = 0; i < _anchors.size(); ++i )
	    _anchors[i].off = shift_position(_anchors[i].off, _anchors[i].right,
					     off, old_len, new_len);
    }
    // A whole-buffer change (load): every anchor clamps to the new size.
    void clamp(size_t size)
    {
	for ( size_t i = 0; i < _anchors.size(); ++i )
	    if ( _anchors[i].off > size )
		_anchors[i].off = size;
    }

    // --- the checkpoint payload (design §2.4): the document rides its
    // anchors and whether it was CLEAN (the text's version at the
    // checkpoint was the saved one) inside the text's history entry, so
    // undo restores the document AND its anchors AND its clean state —
    // "clean" rather than the saved NUMBER, because an undo is a new
    // version: restoring a clean snapshot re-marks that new version as the
    // saved one; a dirty snapshot stays dirty. The application's own
    // payload sits under "app"; the component never reads it.
    madc::value checkpoint_meta(const madc::value &app,
				uint64_t text_version) const
    {
	madc::value m = madc::value::make_object();
	m.object()["app"] = app;
	m.object()["clean"] = madc::value(!modified(text_version));
	madc::value rows = madc::value::make_array();
	for ( size_t i = 0; i < _anchors.size(); ++i )
	{
	    madc::value row = madc::value::make_array();
	    row.array().push_back(madc::value((int64_t)_anchors[i].id));
	    row.array().push_back(madc::value((int64_t)_anchors[i].off));
	    row.array().push_back(madc::value(_anchors[i].right));
	    rows.array().push_back(row);
	}
	m.object()["anchors"] = rows;
	return m;
    }
    // Restore from a payload checkpoint_meta made, the text now at
    // `new_version`; hands "app" back. A clean snapshot makes the new
    // version the saved one; a dirty one leaves the saved version where it
    // was (the text differs from it). An anchor added since the checkpoint
    // keeps its id and clamps; one the payload names that was removed since
    // is NOT resurrected (its id is gone from the live set — a removed
    // anchor's owner has let go).
    void restore_meta(const madc::value &m, madc::value &app_out,
		      uint64_t new_version)
    {
	app_out = madc::value();
	if ( !m.is_object() )
	    return;
	const std::map<std::string, madc::value> &o = m.as_object();
	std::map<std::string, madc::value>::const_iterator it = o.find("app");
	if ( it != o.end() )
	    app_out = it->second;
	it = o.find("clean");
	if ( it != o.end() && it->second.is_boolean() && it->second.as_boolean() )
	    _saved_version = new_version;
	it = o.find("anchors");
	if ( it != o.end() && it->second.is_array() )
	{
	    const std::vector<madc::value> &rows = it->second.as_array();
	    for ( size_t i = 0; i < rows.size(); ++i )
	    {
		if ( !rows[i].is_array() || rows[i].as_array().size() < 2 )
		    continue;
		const std::vector<madc::value> &r = rows[i].as_array();
		anchor_move((anchor_id)r[0].as_integer(),
			    (size_t)r[1].as_integer());
	    }
	}
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_DOCUMENT_H
