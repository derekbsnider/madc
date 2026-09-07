#ifndef __MADCDIS_WEB_MODEL_H
#define __MADCDIS_WEB_MODEL_H 1

// madcdis/web_model.h — the level-3 renderer's MODEL (the web target,
// docs/plans/2026-09-06-ui-web-target-and-madcide-gui.md §3.2): the
// Level-1 pair (model + target) repeated at the TREE level instead of the
// grid level. The same uinode tree tui_model composes onto a cell grid
// becomes keyed DOM OPERATIONS the embedded page applies (JSON, through the
// repo's one JSON owner — json.hpp, the value<->JSON bridge's library); the
// page's input comes back as the SAME semantic tui_event objects the grid
// model emits, through the SAME owners: keys resolve in key_resolver
// (madcdis/keys.h), focus moves in focus_state (madcdis/ui_focus.h), and
// the keys → events loop is ui_apply_keys (madcdis/ui_input.h). No key →
// action logic exists in the page: it posts raw key spellings in the TUI
// vocabulary (tui_key_name) and printable runs; the engine decides.
//
// DOM OPERATIONS. compose() returns the full keyed tree every cycle (the
// compose+diff cadence the TUI runs) as ONE JSON array:
//   {"op":"root"}                                          reset marker
//   {"op":"node","key":"0.2","parent":"0","class":"heading",
//    "label":"...","text":"..."}                           create-or-update
//   {"op":"node","key":"0.3","parent":"0","class":"edit","nlines":2,
//    "lines":[{"t":"line text","s":[[start,len,"keyword"],...]}, ...],
//    "caret":{"line":3,"col":7},"sel":[[l,c],[l,c]]|null,
//    "tabwidth":8,"focus":true}
//   {"op":"node","key":"0.4","parent":"0","class":"choice",
//    "opts":["Save","Quit"],"sel":1,"list":false,"focus":true}
//   {"op":"end"}                                           prune unvisited keys
// Keys are node PATHS in the composed tree (child indices joined by '.',
// the root is "0"), so the applier reconciles by key: an unchanged key
// updates in place; keys absent after "end" are removed. The page WRAPS
// content itself (no wrap_text here — rows/cols are viewport FACTS for the
// application's compose, as on the TUI); offsets in the edit node are BYTE
// offsets turned into line/col, as the grid model reads them.
//
// THE EDIT NODE IS INCREMENTAL (the thin-client shape — Neovim's grid_line
// redraws "a continuous part of a row" and the rest "should remain
// unchanged"; xi-editor rewrites the frontend's line cache with copy/ins
// runs): the model keeps the rows it last emitted per edit key — its diff
// BASIS, as tui_model keeps the painted grid — and each compose sends
// work proportional to what changed, never the document:
//   "lines":[rows]   the full line-DOM: this key's FIRST paint, or the
//                    first after reset_surface() (ui::refresh) / a resync
//   "patch":{"at":i,"del":d,"ins":[rows]}
//                    rows [i, i+d) of what the page holds are replaced by
//                    `ins` — ONE splice from the common prefix to the
//                    common suffix (one edited line = at:i del:1 ins:1)
//   neither          no line changed (a caret move, a resize, a wake)
//   "nlines":N       ALWAYS: the row count the page must hold after this
//                    op — the page checks its own count against it and
//                    posts {"kind":"resync"} on disagreement
// The caret / sel / focus fields ride every edit op; the page re-renders
// only the lines whose caret or selection state changed. A key that
// leaves the tree drops its basis (the page prunes the element the same
// way), so the two sides forget together.
//
// INPUT. The page posts ONE JSON object per event through the target's
// bound callback:
//   {"kind":"key","key":"^k"}              a key in tui_key_name spelling
//   {"kind":"text","text":"abc"}           a printable run (the page
//                                          coalesces an `input` event)
//   {"kind":"resize","rows":40,"cols":120} viewport FACTS in text cells
//   {"kind":"snapshot","text":"..."}       the test seam's DOM text: kept
//                                          for last_snapshot(), reported
//                                          as ONE snapshot event (text)
//   {"kind":"resync"}                      the page's line-DOM disagrees
//                                          with the model's basis (a render
//                                          the platform dropped before the
//                                          page loaded, a lost eval): every
//                                          basis is forgotten and ONE focus
//                                          event asks the application to
//                                          recompose — the next compose
//                                          paints every edit node in full
// apply_input() turns each into zero or more tui_event objects; a malformed
// or unknown object yields none and never throws.
//
// Thread contract: a plain value object; confined with the frontend that
// owns it (the C++ standard-library convention).

#include <algorithm>
#include <map>
#include <set>
#include <string>
#include <vector>

#include "json.hpp"

#include "madcdis/keys.h"
#include "madcdis/ui_events.h"
#include "madcdis/ui_focus.h"
#include "madcdis/ui_input.h"
#include "madcdis/uinode.h"

namespace madc {
namespace hub {

// The role → DOM class spelling: the standard vocabulary by identity; a
// role outside it is structure only ("node") — as the grid model treats
// an unknown role (children carry it).
inline const char *web_class_of(const roles &r, name_id role)
{
    if ( role == r.heading )   return "heading";
    if ( role == r.content )   return "content";
    if ( role == r.list )      return "list";
    if ( role == r.item )      return "item";
    if ( role == r.action )    return "action";
    if ( role == r.status )    return "status";
    if ( role == r.group )     return "group";
    if ( role == r.separator ) return "separator";
    if ( role == r.choice )    return "choice";
    if ( role == r.edit )      return "edit";
    return "node";
}

// A byte offset in a multi-line text → 0-based line and byte column
// (clamped into the text; the page renders tabs, so columns stay bytes).
inline void web_line_col(const std::string &text, long off,
			 size_t &line, size_t &col)
{
    if ( off < 0 )
	off = 0;
    if ( (size_t)off > text.size() )
	off = (long)text.size();
    line = 0;
    size_t line_start = 0;
    for ( size_t i = 0; i < (size_t)off; ++i )
	if ( text[i] == '\n' )
	{
	    ++line;
	    line_start = i + 1;
	}
    col = (size_t)off - line_start;
}

class web_model
{
    key_resolver _keys;			// the ONE chord/key owner
    focus_state _focus;			// the ONE focus/navigation owner
    size_t _rows, _cols;		// last reported viewport facts
    std::string _snapshot;		// last snapshot text (test seam)

    // One line of an edit node's line-DOM as the model last emitted it:
    // the text and its clipped spans [start-in-line, len, class]. The
    // per-key vector of these is the diff basis (see the header).
    struct span_ref
    {
	long a, len;
	std::string cls;
	bool operator==(const span_ref &o) const
	{
	    return a == o.a && len == o.len && cls == o.cls;
	}
    };
    struct edit_row
    {
	std::string t;
	std::vector<span_ref> s;
	bool operator==(const edit_row &o) const { return t == o.t && s == o.s; }
    };
    std::map<std::string, std::vector<edit_row> > _basis;	// edit key -> rows
    std::set<std::string> _seen;		// edit keys this compose visited

    // A node op that carries a focus flag, patched after end_compose()
    // (the flag reads the CLAMPED focus, as the grid paints the caret).
    struct slot_op { size_t op; size_t slot; };

    static const long default_tab_stop = 8;

    // One highlight-span row of an edit node's hints["spans"], validated
    // as the grid model validates it (byte range non-empty, a class name).
    // The web reads the SEMANTIC class `cls` (the portable fact the span
    // carries) and renders it as the CSS class `c-<cls>` — its own styling
    // vocabulary (page.css / the @gui theme). The sibling `c` field is the
    // TUI's JOE style spec, which the web ignores; a row with only `c`
    // (a TUI-only styling) carries no `cls` and is skipped here.
    struct doc_span { long start, end; std::string cls; };
    static bool span_before(const doc_span &a, const doc_span &b)
    {
	return a.start < b.start;
    }

    // Rows come out sorted by start (stable: rows with one start keep
    // their order) — the sweep in edit_lines relies on it. The composers
    // already hand them over in source order, so the sort is a contract,
    // not a cost.
    static void read_spans(const madc::value &hints, std::vector<doc_span> &out)
    {
	if ( !hints.is_object() )
	    return;
	const std::map<std::string, madc::value> &ho = hints.as_object();
	std::map<std::string, madc::value>::const_iterator hi = ho.find("spans");
	if ( hi == ho.end() || !hi->second.is_array() )
	    return;
	for ( const madc::value &row : hi->second.as_array() )
	{
	    if ( !row.is_object() )
		continue;
	    doc_span ds;
	    ds.start = hint_of(row, "s", -1);
	    ds.end = hint_of(row, "e", -1);
	    const std::map<std::string, madc::value> &ro = row.as_object();
	    std::map<std::string, madc::value>::const_iterator ci = ro.find("cls");
	    if ( ds.start < 0 || ds.end <= ds.start
	      || ci == ro.end() || !ci->second.is_string()
	      || ci->second.as_string().empty() )
		continue;
	    ds.cls = ci->second.as_string();
	    out.push_back(ds);
	}
	std::stable_sort(out.begin(), out.end(), span_before);
    }

    // The edit node's text as the line-DOM: one row per line, each with
    // the spans that overlap it ([start-in-line, len, class]). ONE sweep
    // over start-sorted spans: a cursor admits a span into the ACTIVE set
    // on the line that holds its start, and the set drops it after the
    // line that holds its end — O(lines + spans + spans that cross a line
    // boundary). The earlier form tested every span against every line;
    // on a 4557-line, 4841-span document that was 22M comparisons — 140 ms
    // of a 165 ms render — per keystroke (measured 2026-09-07).
    static std::vector<edit_row> edit_rows(const std::string &text,
					   const std::vector<doc_span> &spans)
    {
	std::vector<edit_row> rows;
	size_t ls = 0;
	size_t next = 0;		// the first span not yet admitted
	std::vector<size_t> active;	// admitted spans still reaching ahead
	for ( size_t i = 0; i <= text.size(); ++i )
	{
	    if ( i < text.size() && text[i] != '\n' )
		continue;
	    const size_t le = i;		// [ls, le) is one line
	    edit_row row;
	    row.t = text.substr(ls, le - ls);
	    while ( next < spans.size() && (size_t)spans[next].start < le )
		active.push_back(next++);
	    size_t keep = 0;
	    for ( size_t k = 0; k < active.size(); ++k )
	    {
		const doc_span &sp = spans[active[k]];
		size_t a = (size_t)sp.start;
		size_t b = (size_t)sp.end;
		if ( a < ls )
		    a = ls;
		if ( b > le )
		    b = le;
		if ( a < b )
		{
		    span_ref sr;
		    sr.a = (long)(a - ls);
		    sr.len = (long)(b - a);
		    sr.cls = sp.cls;
		    row.s.push_back(sr);
		}
		// A span reaching past this line's newline has more to
		// give on the next line; one ending at or before it is done.
		if ( (size_t)sp.end > le + 1 )
		    active[keep++] = active[k];
	    }
	    active.resize(keep);
	    rows.push_back(row);
	    ls = i + 1;
	}
	return rows;
    }

    // Rows [from, to) as the page's row objects {"t": text, "s": [...]}.
    static nlohmann::json rows_json(const std::vector<edit_row> &rows,
				    size_t from, size_t to)
    {
	nlohmann::json out = nlohmann::json::array();
	for ( size_t i = from; i < to && i < rows.size(); ++i )
	{
	    nlohmann::json row = nlohmann::json::object();
	    row["t"] = rows[i].t;
	    nlohmann::json s = nlohmann::json::array();
	    for ( size_t k = 0; k < rows[i].s.size(); ++k )
		s.push_back(nlohmann::json::array(
		    { rows[i].s[k].a, rows[i].s[k].len, rows[i].s[k].cls }));
	    row["s"] = s;
	    out.push_back(row);
	}
	return out;
    }

    // The edit node's line-DOM against this key's basis: the full rows
    // when there is none, else ONE splice from the common prefix to the
    // common suffix — absent when nothing changed. The rows become the
    // new basis either way.
    void emit_edit_lines(nlohmann::json &op, const std::string &key,
			 std::vector<edit_row> &rows)
    {
	op["nlines"] = (long)rows.size();
	std::map<std::string, std::vector<edit_row> >::iterator bi = _basis.find(key);
	if ( bi == _basis.end() )
	    op["lines"] = rows_json(rows, 0, rows.size());
	else
	{
	    const std::vector<edit_row> &old = bi->second;
	    const size_t n0 = old.size(), n1 = rows.size();
	    size_t p = 0;
	    while ( p < n0 && p < n1 && old[p] == rows[p] )
		++p;
	    size_t q = 0;
	    while ( q < n0 - p && q < n1 - p && old[n0 - 1 - q] == rows[n1 - 1 - q] )
		++q;
	    if ( p < n0 || p < n1 )
	    {
		nlohmann::json patch = nlohmann::json::object();
		patch["at"] = (long)p;
		patch["del"] = (long)(n0 - p - q);
		patch["ins"] = rows_json(rows, p, n1 - q);
		op["patch"] = patch;
	    }
	}
	_seen.insert(key);
	_basis[key].swap(rows);
    }

    void walk(const roles &r, const uinode &n, const std::string &key,
	      const std::string &parent, nlohmann::json &ops,
	      std::vector<slot_op> &slots)
    {
	nlohmann::json op = nlohmann::json::object();
	op["op"] = "node";
	op["key"] = key;
	op["parent"] = parent;
	op["class"] = web_class_of(r, n.role);
	// Slice 3 layout hints (madcide GUI): `region` docks a node into a
	// workbench slot (rail/sidebar/editor/panel/statusbar), `popup`
	// floats it as an overlay, `tabs` asks the editor group for a tab
	// strip. Additive op fields the page places by; a node without the
	// hint carries none — byte-identical to the pre-slice-3 ops (the
	// test_web_model negative control). The class stays the ROLE; the
	// region is a SEPARATE data attribute, never a new role.
	{
	    std::string region = hint_str(n.hints, "region");
	    if ( !region.empty() )
		op["region"] = region;
	    if ( hint_of(n.hints, "popup", 0) )
		op["popup"] = true;
	    if ( hint_of(n.hints, "tabs", 0) )
		op["tabs"] = true;
	    // The @gui theme (slice 3 Task 4): the root's `theme` hint is a
	    // bag of CSS custom-property name -> value strings; emit them so
	    // the page applies them as `--name` variables. String values only
	    // (a colour / font spec); other kinds are ignored.
	    if ( n.hints.is_object() )
	    {
		const std::map<std::string, madc::value> &ho = n.hints.as_object();
		std::map<std::string, madc::value>::const_iterator ti = ho.find("theme");
		if ( ti != ho.end() && ti->second.is_object() )
		{
		    nlohmann::json theme = nlohmann::json::object();
		    const std::map<std::string, madc::value> &tv = ti->second.as_object();
		    for ( std::map<std::string, madc::value>::const_iterator vi = tv.begin();
			  vi != tv.end(); ++vi )
			if ( vi->second.is_string() )
			    theme[vi->first] = vi->second.as_string();
		    if ( !theme.empty() )
			op["theme"] = theme;
		}
	    }
	}
	bool recurse = true;
	if ( n.role == r.heading )
	{
	    // The bar: label left, content right — the page lays it out.
	    op["label"] = prose::text_of(n.label);
	    op["text"] = prose::text_of(n.content);
	}
	else if ( n.role == r.status || n.role == r.content
	       || n.role == r.item )
	{
	    op["text"] = node_text(n);
	    // The status bar as items (slice 3 Task 5): a status node whose
	    // hints carry {items:{left,right}} renders a justified item bar;
	    // the page prefers items when present, else the single `text`.
	    if ( n.role == r.status && n.hints.is_object() )
	    {
		const std::map<std::string, madc::value> &ho = n.hints.as_object();
		std::map<std::string, madc::value>::const_iterator ii = ho.find("items");
		if ( ii != ho.end() && ii->second.is_object() )
		{
		    const std::map<std::string, madc::value> &iv = ii->second.as_object();
		    nlohmann::json items = nlohmann::json::object();
		    std::map<std::string, madc::value>::const_iterator l = iv.find("left");
		    std::map<std::string, madc::value>::const_iterator rr = iv.find("right");
		    if ( l != iv.end() && l->second.is_string() )
			items["left"] = l->second.as_string();
		    if ( rr != iv.end() && rr->second.is_string() )
			items["right"] = rr->second.as_string();
		    op["items"] = items;
		}
	    }
	}
	else if ( n.role == r.action )
	{
	    op["label"] = prose::text_of(n.label);
	}
	else if ( n.role == r.list )
	{
	    if ( !n.label.is_null() )
		op["label"] = prose::text_of(n.label);
	}
	else if ( n.role == r.choice )
	{
	    // A menu: the node's children are its OPTIONS — one focusable,
	    // the same navigation/choose semantics as the grid's bar; the
	    // list:1 hint asks the page for the popup-list presentation,
	    // focus:1 is the autofocus hint. Options consumed — no generic
	    // child recursion (the grid does the same).
	    size_t slot = _focus.count();
	    focusable f;
	    f.k = focusable::kind::choice;
	    f.option_count = n.children.size();
	    for ( size_t i = 0; i < n.children.size(); ++i )
		f.option_actions.push_back(n.children[i].actions.empty()
					   ? (name_id)0
					   : n.children[i].actions[0]);
	    _focus.add(f);
	    if ( hint_of(n.hints, "focus", 0) )
		_focus.set_focus(slot);
	    if ( !n.label.is_null() )
		op["label"] = prose::text_of(n.label);
	    nlohmann::json opts = nlohmann::json::array();
	    for ( size_t i = 0; i < n.children.size(); ++i )
		opts.push_back(node_text(n.children[i]));
	    op["opts"] = opts;
	    op["sel"] = (long)_focus.selection_of(slot);
	    op["list"] = hint_of(n.hints, "list", 0) != 0;
	    slot_op so;
	    so.op = ops.size();
	    so.slot = slot;
	    slots.push_back(so);
	    recurse = false;
	}
	else if ( n.role == r.edit )
	{
	    // The editable region: the SAME hints the grid model reads
	    // (caret / sel_start / sel_end byte offsets, tabwidth, rows,
	    // focus, spans rows {s, e, cls}) — rendered as the line-DOM.
	    size_t slot = _focus.count();
	    if ( hint_of(n.hints, "focus", 0) )
		_focus.set_focus(slot);
	    focusable f;
	    f.k = focusable::kind::edit;
	    _focus.add(f);
	    const std::string text = prose::text_of(n.content);
	    long caret = hint_of(n.hints, "caret", 0);
	    long sel_start = hint_of(n.hints, "sel_start", -1);
	    long sel_end = hint_of(n.hints, "sel_end", -1);
	    long tabw = hint_of(n.hints, "tabwidth", default_tab_stop);
	    if ( tabw < 1 )
		tabw = 1;
	    if ( tabw > 16 )
		tabw = 16;
	    long rows = hint_of(n.hints, "rows", 0);
	    std::vector<doc_span> spans;
	    read_spans(n.hints, spans);
	    std::vector<edit_row> doc_rows = edit_rows(text, spans);
	    emit_edit_lines(op, key, doc_rows);
	    size_t line, col;
	    web_line_col(text, caret, line, col);
	    op["caret"] = nlohmann::json{ {"line", (long)line}, {"col", (long)col} };
	    if ( sel_start >= 0 && sel_end > sel_start )
	    {
		size_t l0, c0, l1, c1;
		web_line_col(text, sel_start, l0, c0);
		web_line_col(text, sel_end, l1, c1);
		op["sel"] = nlohmann::json::array({
		    nlohmann::json::array({ (long)l0, (long)c0 }),
		    nlohmann::json::array({ (long)l1, (long)c1 }) });
	    }
	    else
		op["sel"] = nullptr;
	    op["tabwidth"] = tabw;
	    if ( rows > 0 )
		op["rows"] = rows;
	    slot_op so;
	    so.op = ops.size();
	    so.slot = slot;
	    slots.push_back(so);
	}
	// group / separator / unknown: structure only — children carry it.
	ops.push_back(op);
	if ( !recurse )
	    return;
	for ( size_t i = 0; i < n.children.size(); ++i )
	    walk(r, n.children[i], key + "." + std::to_string(i), key, ops,
		 slots);
    }

public:
    web_model() : _rows(24), _cols(80) {}

    void set_bindings(const tui_bindings &b) { _keys.set_bindings(b); }
    const std::string &pending_chord() const { return _keys.pending(); }
    size_t rows() const { return _rows; }
    size_t cols() const { return _cols; }
    const std::string &last_snapshot() const { return _snapshot; }
    const std::vector<focusable> &focusables() const { return _focus.focusables(); }
    size_t focus_slot() const { return _focus.focus(); }
    size_t selection_of(size_t slot) const { return _focus.selection_of(slot); }

    // The composed tree as DOM ops JSON (see the header comment). Contract
    // as the grid model's: compose() before apply_input() — events are
    // interpreted against the focusables the last compose discovered;
    // recompose after any focus/resize/text event.
    std::string compose(const roles &r, const uinode &tree)
    {
	nlohmann::json ops = nlohmann::json::array();
	ops.push_back(nlohmann::json{ {"op", "root"} });
	std::vector<slot_op> slots;
	_focus.begin_compose();
	_seen.clear();
	walk(r, tree, "0", "", ops, slots);
	_focus.end_compose();
	// An edit key that left the tree drops its basis — the page prunes
	// the element after "end" by the same rule, so both sides forget.
	for ( std::map<std::string, std::vector<edit_row> >::iterator bi = _basis.begin();
	      bi != _basis.end(); )
	    if ( _seen.count(bi->first) )
		++bi;
	    else
		_basis.erase(bi++);
	for ( size_t i = 0; i < slots.size(); ++i )
	    ops[slots[i].op]["focus"] = slots[i].slot == _focus.focus();
	ops.push_back(nlohmann::json{ {"op", "end"} });
	return ops.dump();
    }

    // Forget what the page holds: the NEXT compose paints every edit node
    // in full (ui::refresh — the grid model's painted-grid reset).
    void reset_surface() { _basis.clear(); }

    // One posted input object → zero or more semantic events, through the
    // shared adapter — the page's keys become the grid's events.
    std::vector<tui_event> apply_input(const std::string &json)
    {
	std::vector<tui_event> none;
	nlohmann::json j = nlohmann::json::parse(json, nullptr, false);
	if ( j.is_discarded() || !j.is_object() )
	    return none;
	nlohmann::json::const_iterator ki = j.find("kind");
	if ( ki == j.end() || !ki->is_string() )
	    return none;
	const std::string kind = ki->get<std::string>();
	std::vector<tui_keyev> keys;
	if ( kind == "key" )
	{
	    nlohmann::json::const_iterator it = j.find("key");
	    tui_keyev k;
	    if ( it == j.end() || !it->is_string()
	      || !tui_key_from_name(it->get<std::string>(), k) )
		return none;
	    keys.push_back(k);
	}
	else if ( kind == "text" )
	{
	    nlohmann::json::const_iterator it = j.find("text");
	    if ( it == j.end() || !it->is_string() )
		return none;
	    const std::string t = it->get<std::string>();
	    for ( size_t i = 0; i < t.size(); ++i )
		keys.push_back(tui_keyev(tui_key::ch, t[i]));
	}
	else if ( kind == "resize" )
	{
	    nlohmann::json::const_iterator ri = j.find("rows");
	    nlohmann::json::const_iterator ci = j.find("cols");
	    if ( ri == j.end() || ci == j.end() || !ri->is_number_integer()
	      || !ci->is_number_integer() || ri->get<long>() <= 0
	      || ci->get<long>() <= 0 )
		return none;
	    _rows = (size_t)ri->get<long>();
	    _cols = (size_t)ci->get<long>();
	    keys.push_back(tui_keyev(tui_key::resize));
	}
	else if ( kind == "resync" )
	{
	    // The page found its line-DOM out of step with the basis (a
	    // render the platform dropped before the page loaded, a lost
	    // eval): forget the bases and ask for a recompose — a focus
	    // event, the application's "repaint" signal on every target.
	    reset_surface();
	    tui_event e;
	    e.kind = tui_event_kind::focus;
	    none.push_back(e);
	    return none;
	}
	else if ( kind == "snapshot" )
	{
	    // Not a key: the page's rendered text (the test seam) — kept for
	    // last_snapshot() and reported as ONE snapshot event carrying it.
	    nlohmann::json::const_iterator it = j.find("text");
	    if ( it == j.end() || !it->is_string() )
		return none;
	    _snapshot = it->get<std::string>();
	    tui_event e;
	    e.kind = tui_event_kind::snapshot;
	    e.text = _snapshot;
	    none.push_back(e);
	    return none;
	}
	else
	    return none;
	return ui_apply_keys(_keys, _focus, keys);
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_WEB_MODEL_H
