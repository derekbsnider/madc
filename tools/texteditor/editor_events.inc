// editor_events.inc — madcide's editor core (IDE-4; it moves with madcide):
// the document actions every client of a session drives with zero
// duplicated mutation logic — typing-cadence edits are engine range
// primitives at the caret, save and quit are functions over madc's
// document component (ui::doc_*). What differs per client is only the
// DISPATCH (the bindings table's actions) layered on these functions.
//
// Undo (IDE-2) lives here: every mutating arm checkpoints BEFORE the
// edit, carrying {caret} as the opaque payload, so undo_edit restores
// document, caret, and dirtiness together (a document's saved state rides
// its own history). One coalesced event = one semantic edit = one undo
// step.
//
// Everything here is loop-free and terminal-free (the headless gates drive
// it directly).

long es_int(long w, long es, const char *key, long dflt)
{
    var v;
    ui::get(v, w, es, key);
    if ( v.is_null() )
	return dflt;
    return v.as_integer();
}

bool es_flag(long w, long es, const char *key)
{
    var v;
    ui::get(v, w, es, key);
    if ( v.is_null() )
	return false;
    return v.as_boolean();
}

// Where a program's DATA directory `sub` lives (the one owner: madcide's
// profiles, plugins and headers through their callers). An INSTALLED program's data is its install's: the
// running binary's own directory (madc::compiler_path) anchors the install
// layouts, <exedir>/../share/<product>/<sub> (the Linux/mac package layout),
// then <exedir>/<sub> (the Windows zip: data beside the exe), and they come
// FIRST. The source tree keeps the data beside the file that owns it
// (`src_dir`, the caller's php::dirname(__FILE__)), the answer when the
// running binary has no install layout (madc run from its build tree). It
// is never consulted first: a library compiled from the tree root spells
// __FILE__ relative (tools/madcide/...), so an installed program started
// inside a madc checkout would read that tree's data instead of its own.
// Nothing found keeps the source-tree spelling, so a report names a real
// place to fix.
void resolve_data_dir(var &dir, const char *product, const char *src_dir,
		      const char *sub)
{
    const char *self = madc::compiler_path();
    if ( strlen(self) > 0 )		// "" = the platform probe failed
    {
	var exedir = php::dirname(self);
	var share = format("{}/../share/{}/{}", exedir, product, sub);
	if ( php::file_exists(share) )
	{
	    dir = share;
	    return;
	}
	var beside = format("{}/{}", exedir, sub);
	if ( php::file_exists(beside) )
	{
	    dir = beside;
	    return;
	}
    }
    dir = format("{}/{}", src_dir, sub);
}

// A DOCUMENT is madc's document component (ui::doc_*): its path, its kind
// (stamped from the path when it opens or is renamed — madc's one file-kind
// table; never re-derived from the name at use), whether it is read-only,
// and its saved state (modified is DERIVED: the text's version differs from
// the one last saved). A VIEW text — a panel's transcript ([repl], [build],
// [terminal]), the Help buffer, a git view, an input field, a lens's render
// buffer — is no document: a text under a display path only (bag "path"),
// never looked up on disk, with no kind and no saved state.

// An UNTITLED document has no path: the buffer a launch without a file opens
// (Thonny's <untitled>, JOE's Unnamed). Its text is real and it lexes and
// parses; a save asks for a name first. A view text is named, never untitled.
bool doc_untitled(long w, long doc)
{
    if ( !ui::doc_is(w, doc) )
	return false;
    var p;
    ui::doc_path(p, w, doc);
    return strlen(p.c_str()) == 0;
}

// The path a buffer carries: a document's file (null when untitled), a view
// text's display path. Every "the buffer's path" read asks here.
void buffer_path(var &out, long w, long buf)
{
    if ( !ui::doc_is(w, buf) )
    {
	ui::get(out, w, buf, "path");
	return;
    }
    var p;
    ui::doc_path(p, w, buf);
    var none;
    if ( strlen(p.c_str()) > 0 )
	out = p;
    else
	out = none;
}

// The name a buffer goes by: its path, or `untitled` for a document that has
// none. The unit name a parse, a lens and a REPL run give it, the status
// line's %n and the tab's title; where a FILE is meant, the path is read.
void doc_name(var &out, long w, long doc)
{
    if ( doc_untitled(w, doc) )
    {
	out = "untitled";
	return;
    }
    buffer_path(out, w, doc);
}

// THE empty document under `path`, unmodified — every document either
// editor holds that is not read from its file is minted here (a new file's,
// the untitled buffer's). An empty `path` mints the UNTITLED document, in
// the madc dialect's kind (Save As stamps the real one).
long new_document(long w, const char *path)
{
    long doc = ui::doc_new(w);
    if ( strlen(path) > 0 )
	ui::doc_set_path(w, doc, path);
    return doc;
}

// THE view text named `name` (a display path), empty.
long new_view_text(long w, const char *name)
{
    long t = ui::create(w, "view-buffer");
    ui::text_load(w, t, "");
    ui::set(w, t, "path", name);
    return t;
}

// Open `path` as a document. 0 = the file could not be read (a refused read
// leaves no orphan document in the world).
long setup_document(long w, const char *path, bool ro)
{
    long doc = ui::doc_open(w, path);
    if ( doc != 0 && ro )
	ui::doc_set_read_only(w, doc, true);
    return doc;
}


// True when the document ends in '\n': offset == size is then a caret
// on the EMPTY line past the content lines — the phantom line. The
// caret already reaches it sideways (right, typing, ^K V = bottom); the
// text_buffer counts content lines only, so the navigability rule lives
// here, in the editor model, and every line/column owner below consults
// it — without it vertical motion stops one row short of the bottom.
bool has_phantom_line(long w, long doc)
{
    long total = ui::text_line_count(w, doc);
    if ( total < 1 )
	return false;
    long lst = ui::text_line_start(w, doc, total);
    if ( lst < 0 )
	return false;
    return lst + ui::text_line_len(w, doc, total) < ui::text_size(w, doc);
}

// The 1-based line containing byte offset `off` (1 on an empty document;
// total+1 on the phantom line). ONE indexed engine lookup — the same
// answer the newline count gives: the phantom position (offset == size
// after a terminated last line) is one past the last content line. This
// used to walk text_line_start once per line, and each of those rescanned
// the document: quadratic per call, three calls per composed keystroke.
long line_of(long w, long doc, long off)
{
    return ui::text_line_of(w, doc, off);
}

// Line start for column math: the phantom line starts at the document's
// end; any other absent line answers 0 (the empty document).
long line_start_of(long w, long doc, long line)
{
    long start = ui::text_line_start(w, doc, line);
    if ( start >= 0 )
	return start;
    if ( line > ui::text_line_count(w, doc) && has_phantom_line(w, doc) )
	return ui::text_size(w, doc);
    return 0;
}

// ---- the ONE text-mutation owner + the ONE anchor registry ------------
// Every editor insert/erase routes through ed_text_insert / ed_text_erase,
// never ui::text_* directly: the es bag carries BYTE-ANCHORED things (the
// themed highlight spans, the selection markers, and — with presence — the
// other clients' carets) that must SHIFT with the text they point into.
// Without it every keystroke above a span repaints the document one byte
// off per character (the madcide "typing spaces re-shades everything"
// defect), and a marker set before a keystroke goes stale.
//
// The shift is ONE mechanism (client-server arc V3a, the anchor registry;
// gate check-one-anchor-owner.sh): the engine's anchor rule, ui::anchor_shift
// (the rule a document's own anchors follow; split arc 1D), is the ONE splice
// primitive over a single byte position, and shift_anchors applies it to
// every registered anchor in ONE pass per splice. A second hand-rolled shift
// (a `mark + n` under a positional guard) is exactly the drift the gate bans
// — it lived in block_copy until V3a.

// Reanchor one es scalar anchor by name across a splice at `off` replacing
// old_len bytes with new_len (skip when the key is absent; an unset marker,
// negative, passes through) — by the anchors' ONE rule, ui::anchor_shift.
// Right-gravity: a marker sticks to the text after it, as the live caret
// does.
void shift_es_offset(long w, long es, const char *key, long off, long old_len,
		     long new_len)
{
    var v;
    ui::get(v, w, es, key);
    if ( v.is_null() )
	return;
    long p = v.as_integer();
    long np = ui::anchor_shift(p, off, old_len, new_len, true);
    if ( np != p )
	ui::set(w, es, key, np);
}

// Shift the anchors carried on ONE es viewing `doc` across a splice at `off`
// replacing old_len bytes with new_len: the themed highlight ranges (an
// insert at a range's start stays outside it, one at its end does not
// extend it) and the selection markers. `with_caret` shifts the es "caret"
// too — for a PEER (presence) es whose caret is passive; the EDITING es
// passes false, its caret set by the edit call site (a paste lands after
// the clip, a cut on its start), never a passive shift.
void shift_es_anchors(long w, long es, long doc, long off, long old_len,
		      long new_len, bool with_caret)
{
    var hs;
    ui::get(hs, w, es, "hspans");
    if ( !hs.is_null() && php::count(hs) > 0 )
    {
	var out = {};
	for ( var r : hs )
	{
	    long s = ui::anchor_shift(r["s"].as_integer(), off, old_len, new_len, true);
	    long e = ui::anchor_shift(r["e"].as_integer(), off, old_len, new_len, false);
	    if ( e > s )
	    {
		// The row is the converter's (spans_to_hspans): carry it
		// WHOLE and move only its geometry. A field list here is a
		// second copy of the row shape — when slice 3.1 added a
		// field the web read, such a list silently dropped it and
		// killed the web's colour on the first keystroke
		// (tests/testidespanshift.mad). Today the row is {s, e, c};
		// this code does not know that, by design.
		var nr = r;
		nr["s"] = s;
		nr["e"] = e;
		out.push(nr);
	    }
	}
	ui::set(w, es, "hspans", out);
    }
    if ( with_caret )
	shift_es_offset(w, es, "caret", off, old_len, new_len);
    shift_es_offset(w, es, "mark", off, old_len, new_len);
    shift_es_offset(w, es, "bend", off, old_len, new_len);
}

// The anchor registry: shift EVERY byte-anchored thing across an edit in one
// pass. The EDITING es (the one the mutation ran on) shifts its themed
// ranges + selection but NOT its caret — each edit call site sets that as
// the edit's own result. Every OTHER es registered as viewing `doc` (another
// CLIENT's window — client-server V3c presence) is passive: its caret AND
// selection shift, so one edit moves the other windows' places in the same
// pass instead of leaving them stale (the compose-time clamp was a lossy
// stand-in). A peer that has since navigated to another document is skipped
// — its "caret" now anchors into THAT document.
// ---- live colour's record of what changed (madcide colour_catch_up) ----
// Per client state bag, since that client's colour last caught up: the
// document the edits were in (cdirty_doc; 0 = none), the byte range they
// touched in its CURRENT text (cdirty_lo..cdirty_hi), whether any inserted
// or erased text held a character that opens or closes a multi-line token
// (cdirty_multi), and whether the whole text changed at once (cdirty_all:
// undo, redo). The editor itself reads none of it; a client with colour does
// (with the text's own version, ui::text_version).

// A character that can open or close a token spanning lines: a block
// comment's `/` `*`, a string's `"` (a raw string), a line continuation's `\`.
bool multiline_char(char c)
{
    return c == '/' || c == '*' || c == '"' || c == '\\';
}

bool multiline_text(const char *s)
{
    for ( long i = 0; s[i] != 0; i = i + 1 )
	if ( multiline_char(s[i]) )
	    return true;
    return false;
}

// True when the `n` bytes at `off` of `doc` hold a multiline_char — read
// line by line, before an erase removes them.
bool multiline_range(long w, long doc, long off, long n)
{
    long end = off + n;
    long last = ui::text_line_count(w, doc);
    for ( long line = line_of(w, doc, off); n > 0 && line <= last; line = line + 1 )
    {
	long ls = line_start_of(w, doc, line);
	if ( ls < 0 || ls >= end )
	    break;
	var l;
	ui::text_line(l, w, doc, line);
	if ( l.is_null() )
	    break;
	const char *s = l.c_str();
	long len = strlen(s);
	for ( long k = 0; k < len; k = k + 1 )
	    if ( ls + k >= off && ls + k < end && multiline_char(s[k]) )
		return true;
    }
    return false;
}

// Record a splice at `off` replacing old_len bytes with new_len on `es`'s
// colour record: the range already recorded moves with the edit (its start
// stays before an insert at it, its end follows one), then grows to cover
// the new text.
void colour_dirty(long w, long es, long doc, long off, long old_len, long new_len,
		  bool multi)
{
    long lo = -1;
    long hi = -1;
    if ( es_int(w, es, "cdirty_doc", 0) == doc )
    {
	lo = ui::anchor_shift(es_int(w, es, "cdirty_lo", -1), off, old_len, new_len, false);
	hi = ui::anchor_shift(es_int(w, es, "cdirty_hi", -1), off, old_len, new_len, true);
    }
    long end = off + new_len;
    if ( lo < 0 || off < lo )
	lo = off;
    if ( hi < 0 || end > hi )
	hi = end;
    ui::set(w, es, "cdirty_doc", doc);
    ui::set(w, es, "cdirty_lo", lo);
    ui::set(w, es, "cdirty_hi", hi);
    if ( multi )
	ui::set(w, es, "cdirty_multi", true);
}

// The whole of `doc` changed at once (undo, redo): every client viewing it
// re-colours all of it.
void colour_dirty_all(long w, long es, long doc)
{
    if ( es != 0 )
    {
	ui::set(w, es, "cdirty_doc", doc);
	ui::set(w, es, "cdirty_all", true);
    }
    var reg;
    ui::get(reg, w, doc, "anchor_es");
    if ( reg.is_null() )
	return;
    for ( var r : reg )
    {
	long peer = r.as_integer();
	if ( peer == es || es_int(w, peer, "adoc", -1) != doc )
	    continue;
	ui::set(w, peer, "cdirty_doc", doc);
	ui::set(w, peer, "cdirty_all", true);
    }
}

// A splice's anchors and colour record (at `off`, old_len bytes replaced by
// new_len), on the editing client (0: none — the edit was no client's) and
// every peer viewing the document; `multi` = the erased or inserted text
// held a multiline_char.
void shift_anchors(long w, long es, long doc, long off, long old_len, long new_len,
		   bool multi)
{
    if ( es != 0 )
    {
	shift_es_anchors(w, es, doc, off, old_len, new_len, false);
	colour_dirty(w, es, doc, off, old_len, new_len, multi);
    }
    var reg;
    ui::get(reg, w, doc, "anchor_es");
    if ( reg.is_null() )
	return;
    for ( var r : reg )
    {
	long peer = r.as_integer();
	if ( peer == es || es_int(w, peer, "adoc", -1) != doc )
	    continue;
	shift_es_anchors(w, peer, doc, off, old_len, new_len, true);
	colour_dirty(w, peer, doc, off, old_len, new_len, multi);
    }
}

// Register `es` as a client viewing `doc`: the presence roster the anchor
// registry shifts and compose draws (client-server V3c). Idempotent; on the
// first join it deals the es its round-robin presence colour SLOT from a
// monotonic per-document counter (a client that later leaves does not
// renumber the others). Records the viewed doc so a shift/draw skips a peer
// that has navigated away. init_view_es calls it for every client — open()'s
// first window and each viewwindow spawn.
void es_view_doc(long w, long es, long doc)
{
    ui::set(w, es, "adoc", doc);
    var reg;
    ui::get(reg, w, doc, "anchor_es");
    if ( reg.is_null() )
	reg = {};
    for ( var r : reg )
	if ( r.as_integer() == es )
	    return;
    long dealt = es_int(w, doc, "presence_dealt", 0);
    ui::set(w, es, "presence_colour", dealt % 8);
    ui::set(w, doc, "presence_dealt", dealt + 1);
    reg.push(es);
    ui::set(w, doc, "anchor_es", reg);
}

// ---- the change log's record kinds (split arc 1D) -------------------------
// The change log is madc's project memory (madc::tool_host: head / records /
// replay); a record's KIND is text only on its wire line. The ONE reader
// converts it here and every consumer switches on the code — the
// enum-boundary rule (Nexus L4b). ckOTHER = an annotation kind the memory's
// tools own (a nodeop, a proposal, a record …).
enum clog_kind : unsigned char { ckNONE = 0, ckSPLICE, ckCHECKPOINT, ckOTHER };

long clog_kind_of(var &rec)
{
    var k = rec["kind"];
    if ( k.is_null() )
	return ckNONE;
    if ( k == "splice" )
	return ckSPLICE;
    if ( k == "checkpoint" )
	return ckCHECKPOINT;
    return ckOTHER;
}

// One keyed READ of a map that may still be null or the empty container (`{}`
// is an empty ARRAY; a keyed read before the first keyed write throws). out
// stays null for a missing key, and the map is left as it was: the carrier's
// subscript CREATES the slot it reads (madarray_key_slot: access vivifies), so
// the missing key is asked first — a validating read of a manifest must not
// add `"version": null` to it. (Promoted from madcide_past.inc, Nexus L4c.)
void keyed_get(var &out, var &map, const char *key)
{
    if ( map.is_object() && php::array_key_exists(key, map) )
	out = map[key];
}

// ---- one mutation path (split arc 1D, item 17) --------------------------
// Every change to a text — a document's or a view's; this editor's verbs,
// madc's ex core, a tool over MCP — reaches madcide as the session's EDIT
// NOTIFICATION (ui::doc_subscribe), and only here does madcide's per-client
// state follow it: the anchor registry, the colour record, the text
// revision (a text no client views has none of it to move). A verb of
// this editor names the client it runs for (ed_editing_es) and, for an
// erase, whether the erased text held a multi-line character (read before
// it went); a change no client of this editor made treats every viewer as
// a peer and an erase as possibly multi-line (the conservative recolour).
long ed_editing_es = 0;
bool ed_erase_multi = true;

void ed_text_changed(long w, long doc, var &c)
{
    long es = ed_editing_es;
    if ( c["kind"] != ui::text_change_kind::splice )
    {
	colour_dirty_all(w, es, doc);	// a load, an undo, a redo: all of it
	return;
    }
    long off = c["off"].as_integer();
    long del = c["old_len"].as_integer();
    var ins = c["inserted"];
    long n = strlen(ins.c_str());
    bool multi = (del > 0 && (es != 0 ? ed_erase_multi : true)) || multiline_text(ins.c_str());
    if ( del > 0 || n > 0 )
	shift_anchors(w, es, doc, off, del, n, multi);
}

// The worlds whose edit notification ed_text_changed already follows: one
// subscription per world, however many clients (editor states) it holds.
var ed_followed_worlds;

void ed_follow_edits(long w)
{
    if ( !ed_followed_worlds.is_null() )
	for ( var f : ed_followed_worlds )
	    if ( f.as_integer() == w )
		return;
    ed_followed_worlds[] = w;
    ui::doc_subscribe(w, ed_text_changed);
}

void ed_text_insert(long w, long es, long doc, long off, const char *s)
{
    long was = ed_editing_es;
    ed_editing_es = es;
    ui::text_insert(w, doc, off, s);
    ed_editing_es = was;
}

void ed_text_erase(long w, long es, long doc, long off, long n)
{
    long was = ed_editing_es;
    bool multi_was = ed_erase_multi;
    ed_editing_es = es;
    ed_erase_multi = multiline_range(w, doc, off, n);
    ui::text_erase(w, doc, off, n);
    ed_editing_es = was;
    ed_erase_multi = multi_was;
}

// A caret on `line` keeping column `col` when the line is long enough —
// the vertical-movement rule. A target past the content lines lands on
// the phantom line when there is one (down/pgdn reach the bottom; a
// too-big go-to-line lands at the end, JOE's rule) and clamps otherwise.
long caret_to_line(long w, long doc, long line, long col)
{
    long total = ui::text_line_count(w, doc);
    if ( line < 1 )
	line = 1;
    if ( total > 0 && line > total )
    {
	if ( has_phantom_line(w, doc) )
	    return ui::text_size(w, doc);
	line = total;
    }
    long start = ui::text_line_start(w, doc, line);
    if ( start < 0 )
	return 0;
    long len = ui::text_line_len(w, doc, line);
    if ( col > len )
	col = len;
    return start + col;
}

// Is buffer `buf` read-only? A document's own flag (ui::doc_read_only), or a
// view text's (bag "read_only": the help, a git view). The ONE rule: the
// typing gate, save and the menus' state all read it.
bool buffer_read_only(long w, long buf)
{
    if ( ui::doc_is(w, buf) )
	return ui::doc_read_only(w, buf);
    var ro;
    ui::get(ro, w, buf, "read_only");
    return !ro.is_null() && ro.as_boolean();
}

// The read-only verdict on the client's SUBJECT buffer — the actor's active
// document ("adoc"), else the world's one "document" — and its refusal
// prose: `editable` and its `reason`. False = no subject buffer
// (`editable` stays true). The one reader: derive_access stores it on the
// bag; a client whose display is read-only over a file that is not
// (madcide's menus over a lens that edits through to the file) asks it
// directly.
bool access_verdict(bool &editable, var &reason, long w, long es)
{
    editable = true;
    reason = "";
    long doc = es_int(w, es, "adoc", 0);
    if ( doc == 0 )
	doc = ui::entity_by_name(w, "document");
    if ( doc == 0 )
	return false;
    if ( buffer_read_only(w, doc) )
    {
	editable = false;
	reason = "Document is read-only.";
    }
    return true;
}

// The typing gate AND its refusal prose, derived ONCE from the
// enumerated affordances: the read-only rule lives in the shared
// availability check; the editor stores its verdict and its reason
// (verb `c`'s enumerated state) on the editor-state bag instead of
// re-implementing or re-spelling either.
void derive_access(long w, long es)
{
    bool editable = true;
    var reason;
    if ( access_verdict(editable, reason, w, es) )
	ui::set(w, es, "ro_msg", reason.c_str());
    ui::set(w, es, "editable", editable);
}

// A typing-cadence edit was refused: the message is the CHECK's own
// reason, read back off the bag — one rule, one spelling.
void refuse_edit(long w, long es)
{
    var ro;
    ui::get(ro, w, es, "ro_msg");
    ui::set(w, es, "msg", ro.c_str());
}

// THE client: an editor-state bag over `doc` — the launch window's and every
// further window's (spawn_view_es) — its caret at the top, no selection,
// the access verdict derived (the world's verbs already bound) and the
// world's edits followed.
long new_editor_state(long w, long doc)
{
    long es = ui::create(w, "editor-state");
    ui::set(w, es, "caret", 0);
    ui::set(w, es, "mark", -1);
    ui::set(w, es, "adoc", doc);	// the subject-document rule's key
    derive_access(w, es);
    ed_follow_edits(w);
    return es;
}

// The editor session the headless gates open: the document + its
// editor-state (madcide's launch opens its document through its own open
// rule, open_buffer_doc, then new_editor_state). 0 = the file could not be
// read.
long setup_editor(long w, const char *path, bool ro)
{
    long doc = setup_document(w, path, ro);
    if ( doc == 0 )
	return 0;
    new_editor_state(w, doc);
    return doc;
}

// Save: the buffer written back to its file (ui::doc_save — the write and
// "the file on disk IS this" in one); a read-only buffer, one with no file
// and a failed write refuse. The answer is the status message; the session
// goes on.
bool buffer_write(long w, long es, long doc)
{
    var p;
    ui::doc_path(p, w, doc);
    if ( buffer_read_only(w, doc) )
	ui::set(w, es, "msg", "Document is read-only.");
    else if ( strlen(p.c_str()) == 0 )
	ui::set(w, es, "msg", "No path.");
    else if ( !ui::doc_save(w, doc) )
	ui::set(w, es, "msg", "Write failed.");
    else
	ui::set(w, es, "msg", format("Wrote {} bytes.", ui::text_size(w, doc)));
    return true;
}

// Quit: refused while the buffer has unsaved changes, unless `discard`
// (§7.3's modified rule). False = the session ends.
bool buffer_quit(long w, long es, long doc, bool discard)
{
    if ( !discard && ui::doc_modified(w, doc) )
    {
	ui::set(w, es, "msg", "Unsaved changes — w to save, q! to discard.");
	return true;
    }
    ui::set(w, es, "msg", "Bye.");
    return false;
}

void enter_search(long w, long es)
{
    ui::set(w, es, "searching", true);
    ui::set(w, es, "search", "");
}

// THE clamp-a-caret-to-a-document rule — one owner (IDE-9e windows
// carry per-window carets that go stale when the other window edits;
// their restore and snapshot both ride this).
long clamp_caret_to(long w, long doc, long caret)
{
    long size = ui::text_size(w, doc);
    if ( caret > size )
	caret = size;
    if ( caret < 0 )
	caret = 0;
    return caret;
}

// The ES caret clamped to the live document — every event application
// starts here.
long clamped_caret(long w, long es, long doc)
{
    return clamp_caret_to(w, doc, es_int(w, es, "caret", 0));
}

// The {caret} payload rule — ONE spelling; checkpoint, undo and redo all
// ride it, so every history entry restores the caret that belonged to it.
// A document's history carries its anchors and its saved state with it
// (ui::doc_checkpoint: modified is derived, never carried); a view text's
// (an input field) is the plain text history.
void edit_meta(var &m, long w, long es, long doc, long caret)
{
    m = { "caret": caret };
}

// A payload no client of this editor wrote (madc's ex core, a tool over
// MCP) carries no caret: the caret stays, clamped by the next event.
void restore_meta(long w, long es, long doc, var &m)
{
    var c;
    keyed_get(c, m, "caret");
    if ( !c.is_null() )
	ui::set(w, es, "caret", c.as_integer());
}

// One undo step (IDE-2): checkpoint BEFORE a mutation with the
// interaction state that belongs to that moment.
void edit_checkpoint(long w, long es, long doc, long caret)
{
    var m;
    edit_meta(m, w, es, doc, caret);
    if ( ui::doc_is(w, doc) )
	ui::doc_checkpoint(w, doc, m);
    else
	ui::text_checkpoint(w, doc, m);
}

// Undo/redo (madcide v2 grew redo): the CURRENT payload rides to the
// opposite stack, so walking either direction restores the caret that
// belonged to the text being restored. The notification recolours.
bool history_step(long w, long es, long doc, bool undo)
{
    var now;
    edit_meta(now, w, es, doc, clamped_caret(w, es, doc));
    var m;
    long was = ed_editing_es;
    ed_editing_es = es;
    bool moved = false;
    if ( ui::doc_is(w, doc) )
	moved = undo ? ui::doc_undo(m, w, doc, now) : ui::doc_redo(m, w, doc, now);
    else
	moved = undo ? ui::text_undo(m, w, doc, now) : ui::text_redo(m, w, doc, now);
    ed_editing_es = was;
    if ( !moved )
    {
	ui::set(w, es, "msg", undo ? "Nothing to undo." : "Nothing to redo.");
	return false;
    }
    restore_meta(w, es, doc, m);
    return true;
}

bool undo_edit(long w, long es, long doc)
{
    return history_step(w, es, doc, true);
}

bool redo_edit(long w, long es, long doc)
{
    return history_step(w, es, doc, false);
}

// ---- the selection (plan §41.11a step 3e) -------------------------------
// THE selection rule, one owner (the edit node's highlight, a peer's
// presence highlight, madcide's block commands and the clipboard all read
// it): the block runs [mark, bend-else-caret]; under a TWO-POINT
// personality (the es "selblock" flag: JOE's ^K B / ^K K) a mark without
// an end is no block yet. `r` answers {lo, hi}; false = no selection.
bool selection_range(var &r, long w, long es, long caret)
{
    long mark = es_int(w, es, "mark", -1);
    long bend = es_int(w, es, "bend", -1);
    long e = bend >= 0 ? bend : caret;
    if ( bend < 0 && es_flag(w, es, "selblock") )
	e = mark;
    long lo = mark < e ? mark : e;
    long hi = mark < e ? e : mark;
    var b = { "lo": lo, "hi": hi };
    r = b;
    return mark >= 0 && lo < hi;
}

// A selection made the GUI way — Shift with a motion, Select all, a pointer
// drag — sets the es "selgui" flag beside the markers: typing, Backspace,
// Delete, Enter and a paste REPLACE it, and a motion without Shift drops
// it. A block the block keys make (JOE's ^K B / ^K K, pico's mark) leaves
// the flag unset and keeps its own rules.
bool selection_gui(long w, long es)
{
    return es_flag(w, es, "selgui");
}

void select_clear(long w, long es)
{
    ui::set(w, es, "mark", -1);
    ui::set(w, es, "bend", -1);
    ui::set(w, es, "selgui", false);
}

// Light [anchor, here] as a GUI selection, through the SAME markers the
// block keys set: the mark, plus the end under the two-point personality
// (selection_range's rule reads both).
void select_span(long w, long es, long anchor, long here)
{
    if ( anchor == here )
    {
	select_clear(w, es);
	return;
    }
    ui::set(w, es, "mark", anchor);
    ui::set(w, es, "bend", es_flag(w, es, "selblock") ? here : -1);
    ui::set(w, es, "selgui", true);
}

// Was Shift held on the key (or the bound chord's last key) the event
// carries? (ui::key_mod bits in its `mods`.)
bool ev_shift(var &ev)
{
    var m;
    keyed_get(m, ev, "mods");
    return !m.is_null() && (m.as_integer() & (long)ui::key_mod::shift) != 0;
}

// A caret motion's selection rule, run once the caret moved from `before`:
// Shift held extends the GUI selection from its anchor (the caret the
// first shifted motion started from); a motion without Shift drops a GUI
// selection and leaves a block alone.
void motion_select(long w, long es, long before, var &ev)
{
    long now = es_int(w, es, "caret", before);
    if ( ev_shift(ev) )
    {
	long mark = es_int(w, es, "mark", -1);
	select_span(w, es, selection_gui(w, es) && mark >= 0 ? mark : before, now);
    }
    else if ( selection_gui(w, es) )
	select_clear(w, es);
}

// Typing over a GUI selection: the selection's text is replaced by `text`
// ("" = deleted) as ONE undo step, the caret after it. False = no GUI
// selection (nothing changed: the caller's own edit applies).
bool selection_replace(long w, long es, long doc, long caret, const char *text)
{
    var r;
    if ( !selection_gui(w, es) || !selection_range(r, w, es, caret) )
	return false;
    var t = text;
    long lo = r["lo"].as_integer();
    long hi = r["hi"].as_integer();
    select_clear(w, es);
    edit_checkpoint(w, es, doc, caret);
    ed_text_erase(w, es, doc, lo, hi - lo);
    if ( strlen(t.c_str()) > 0 )
	ed_text_insert(w, es, doc, lo, t.c_str());
    ui::set(w, es, "caret", lo + strlen(t.c_str()));
    return true;
}

// ---- line-wise edits (Thonny's Edit ▸ Indent, Dedent, Toggle comment) ----
// ONE owner for an edit applied to every line the selection touches: the
// span rule, each line's edit, and the pass that applies them (one undo
// step; the last line first, so the lines above keep their offsets; the
// caret carried through ui::anchor_shift; a GUI selection re-lit over the
// lines' whole text).
enum line_edit_op : unsigned char { leINDENT = 0, leDEDENT, leCOMMENT };

// The lines the byte range [lo, hi) touches — ONE rule (a selection's lines
// here, a proposal's hunk in madcide_mcp.inc): `r` answers {first, last},
// 1-based; a range ending at a line's START leaves that line out (it reached
// up to the line, not into it); an empty range is the line it stands on.
void range_lines(var &r, long w, long doc, long lo, long hi)
{
    long end = hi > lo ? hi - 1 : hi;
    var b = { "first": line_of(w, doc, lo), "last": line_of(w, doc, end) };
    r = b;
}

// The lines a line-wise edit covers: the selection's (range_lines), else
// the caret's line (the phantom line counts: the caret can stand on it).
void edit_line_span(var &r, long w, long es, long doc, long caret)
{
    var s;
    if ( selection_range(s, w, es, caret) )
	range_lines(r, w, doc, s["lo"].as_integer(), s["hi"].as_integer());
    else
	range_lines(r, w, doc, caret, caret);
}

// A GUI selection that crosses a line: Tab indents its lines instead of
// replacing it (IDLE, Thonny, VS Code).
bool selection_spans_lines(long w, long es, long doc, long caret)
{
    var s;
    if ( !selection_gui(w, es) || !selection_range(s, w, es, caret) )
	return false;
    return line_of(w, doc, s["lo"].as_integer()) != line_of(w, doc, s["hi"].as_integer());
}

// The blanks (spaces and tabs) a text starts with — the ONE leading-blank
// scanner: a line's indent here, a layout line's nesting depth
// (madcide_core.inc), the indent an MCP fragment inherits (madcide_mcp.inc).
long blank_run(const char *s)
{
    long k = 0;
    while ( s[k] == ' ' || s[k] == '\t' )
	k = k + 1;
    return k;
}

// One indent level as the editor types it: Edit ▸ Indent's insertion, the
// Tab key's text, and the level autoindent adds after a block opener. The
// buffer's own, es "indentunit" (madcide primes it per buffer: the file's
// indentation, else its language's — autoindent_prime); unset = a tab.
void indent_unit(var &out, long w, long es)
{
    ui::get(out, w, es, "indentunit");
    if ( out.is_null() || strlen(out.c_str()) == 0 )
	out = "\t";
}

// The leading spaces one Dedent removes when the line does not start with a
// tab: the indent unit's width when it is spaces, else the tab width.
long dedent_spaces(long w, long es)
{
    var u;
    indent_unit(u, w, es);
    if ( u.c_str()[0] == ' ' )
	return strlen(u.c_str());
    long tabw = es_int(w, es, "tabwidth", 8);
    if ( tabw < 1 )
	tabw = 8;
    return tabw;
}

// A line of nothing but blanks (a CRLF file's '\r' counted as its end).
bool blank_line(const char *s)
{
    long k = blank_run(s);
    return s[k] == 0 || (s[k] == '\r' && s[k + 1] == 0);
}

// The edit `op` makes to line `n`: {at, del, ins} — erase `del` bytes at
// `at`, then insert `ins` there (0 and "" = the line is left alone).
// Indent inserts the indent unit a typed Tab inserts, on a line that is not
// empty; dedent removes one leading tab, else up to one unit's (or, the
// unit a tab, one tab width's) leading spaces (dedent_spaces); the
// comment toggle inserts `prefix` and a space at column 0 of a non-blank
// line, or with `strip` removes the prefix after the line's blanks and the
// one space that follows it.
void line_edit_of(var &e, long w, long es, long doc, long n, long op,
		  const char *prefix, bool strip)
{
    var l;
    ui::text_line(l, w, doc, n);
    const char *s = l.c_str();
    long at = line_start_of(w, doc, n);
    long del = 0;
    var ins = "";
    switch ( op )
    {
	case leINDENT:
	    if ( s[0] != 0 )
		indent_unit(ins, w, es);
	    break;
	case leDEDENT:
	    if ( s[0] == '\t' )
		del = 1;
	    else
	    {
		long most = dedent_spaces(w, es);
		while ( del < most && s[del] == ' ' )
		    del = del + 1;
	    }
	    break;
	case leCOMMENT:
	    if ( blank_line(s) )
		break;
	    if ( !strip )
		ins = format("{} ", prefix);
	    else
	    {
		long k = blank_run(s);
		long pl = strlen(prefix);
		at = at + k;
		del = pl;
		if ( s[k + pl] == ' ' )
		    del = pl + 1;
	    }
	    break;
    }
    var r = { "at": at, "del": del, "ins": ins };
    e = r;
}

// Apply `op` to the span's lines (`prefix` = the line comment leCOMMENT
// toggles): gated on the editable verdict; the toggle strips when every
// non-blank line already carries the prefix, else adds it. False = nothing
// changed (no undo step; the buffer keeps its dirtiness).
bool edit_lines(long w, long es, long doc, long caret, long op, const char *prefix)
{
    if ( !es_flag(w, es, "editable") )
    {
	refuse_edit(w, es);
	return false;
    }
    var span;
    edit_line_span(span, w, es, doc, caret);
    long first = span["first"].as_integer();
    long last = span["last"].as_integer();
    bool strip = false;
    if ( op == leCOMMENT )
    {
	long seen = 0;
	strip = true;
	for ( long n = first; n <= last; n = n + 1 )
	{
	    var l;
	    ui::text_line(l, w, doc, n);
	    if ( blank_line(l.c_str()) )
		continue;
	    seen = seen + 1;
	    if ( strncmp(l.c_str() + blank_run(l.c_str()), prefix, strlen(prefix)) != 0 )
		strip = false;
	}
	if ( seen == 0 )
	    return false;
    }
    var edits = {};
    for ( long n = last; n >= first; n = n - 1 )
    {
	var e;
	line_edit_of(e, w, es, doc, n, op, prefix, strip);
	if ( e["del"].as_integer() > 0 || strlen(e["ins"].c_str()) > 0 )
	    edits.push(e);
    }
    if ( php::count(edits) == 0 )
	return false;
    // A GUI selection is re-lit over the lines it covered, its caret end
    // kept: through to the next line's start when it reached it, else to
    // the last line's end.
    var sel;
    bool gui = selection_gui(w, es) && selection_range(sel, w, es, caret);
    bool whole = gui && line_of(w, doc, sel["hi"].as_integer()) > last;
    bool back = gui && caret == sel["lo"].as_integer();
    edit_checkpoint(w, es, doc, caret);
    long c = caret;
    for ( var e : edits )
    {
	long at = e["at"].as_integer();
	long del = e["del"].as_integer();
	if ( del > 0 )
	{
	    ed_text_erase(w, es, doc, at, del);
	    c = ui::anchor_shift(c, at, del, 0, true);
	}
	long n = strlen(e["ins"].c_str());
	if ( n > 0 )
	{
	    ed_text_insert(w, es, doc, at, e["ins"].c_str());
	    c = ui::anchor_shift(c, at, 0, n, true);
	}
    }
    ui::set(w, es, "caret", c);
    if ( gui )
    {
	long lo = line_start_of(w, doc, first);
	long hi = line_start_of(w, doc, last) + ui::text_line_len(w, doc, last);
	if ( whole )
	    hi = line_start_of(w, doc, last + 1);
	ui::set(w, es, "caret", back ? lo : hi);
	select_span(w, es, back ? hi : lo, back ? lo : hi);
    }
    return true;
}

// ---- the clipboard: interaction state on the editor-state bag ("clip").
// THE cut rule, one owner (vised's ^K, madcide's Cut and JOE's ^K Y): the
// selection's text goes to the clipboard and the markers clear; an
// editable buffer loses the text, the caret keeping its place in what
// remains (the cut point when it sat inside or at the end); read-only, the
// text is only copied. False = no selection (nothing changed: the caller
// spells its own refusal).
bool cut_selection(long w, long es, long doc, long caret, bool editable)
{
    var r;
    if ( !selection_range(r, w, es, caret) )
	return false;
    long lo = r["lo"].as_integer();
    long hi = r["hi"].as_integer();
    var t;
    ui::text(t, w, doc);
    ui::set(w, es, "clip", perl::substr(t, lo, hi - lo));
    select_clear(w, es);
    if ( !editable )
    {
	var ro;
	ui::get(ro, w, es, "ro_msg");
	var m = format("{} (Copied.)", ro);
	ui::set(w, es, "msg", m.c_str());
	return true;
    }
    edit_checkpoint(w, es, doc, caret);
    ed_text_erase(w, es, doc, lo, hi - lo);
    long nc = caret;
    if ( nc > hi )
	nc = nc - (hi - lo);
    else if ( nc > lo )
	nc = lo;
    ui::set(w, es, "caret", nc);
    return true;
}

// Copy the selection's text to the clipboard; the selection stays. False =
// no selection.
bool copy_selection(long w, long es, long doc, long caret)
{
    var r;
    if ( !selection_range(r, w, es, caret) )
	return false;
    long lo = r["lo"].as_integer();
    long hi = r["hi"].as_integer();
    var t;
    ui::text(t, w, doc);
    ui::set(w, es, "clip", perl::substr(t, lo, hi - lo));
    return true;
}

// Insert `text` at the caret (gated), replacing a GUI selection; the caret
// lands after it. An empty text is a silent no-op.
void paste_text(long w, long es, long doc, long caret, const char *text0)
{
    var text = text0;
    if ( strlen(text.c_str()) == 0 )
	return;
    if ( !es_flag(w, es, "editable") )
    {
	refuse_edit(w, es);
	return;
    }
    if ( selection_replace(w, es, doc, caret, text.c_str()) )
	return;
    edit_checkpoint(w, es, doc, caret);
    ed_text_insert(w, es, doc, caret, text.c_str());
    ui::set(w, es, "caret", caret + strlen(text.c_str()));
}

// Insert the clipboard at the caret (paste_text's rules).
void paste_clip(long w, long es, long doc, long caret)
{
    var c;
    ui::get(c, w, es, "clip");
    if ( c.is_null() )
	return;
    paste_text(w, es, doc, caret, c.c_str());
}

// ---- autoindent (Thonny / VS Code / JOE's ^T I) ----------------------
// On by default; es "autoindent" false turns it off (madcide primes it from
// the "autoindent" setting each event, autoindent_prime). The language's
// block delimiters arrive the same way: es "indentopen" (a line ending in
// one opens a level) and "indentclose" (one typed first on a blank-indented
// line closes a level) — unset = none, the line's indentation is copied.
bool autoindent_on(long w, long es)
{
    var v;
    ui::get(v, w, es, "autoindent");
    return v.is_null() || v.as_boolean();
}

// True when `c` is one of es `key`'s characters.
bool es_char_in(long w, long es, const char *key, char c)
{
    var set;
    ui::get(set, w, es, key);
    if ( set.is_null() || c == 0 )
	return false;
    return strchr(set.c_str(), c) != NULL;
}

// What Enter inserts at `at`: a newline, then with autoindent the blanks
// that start `at`'s line (only those before `at`), plus one indent unit when
// the last non-blank before `at` on that line is a block opener.
void enter_text(var &out, long w, long es, long doc, long at)
{
    out = "\n";
    if ( !autoindent_on(w, es) )
	return;
    long n = line_of(w, doc, at);
    var l;
    ui::text_line(l, w, doc, n);
    if ( l.is_null() )
	return;					// the phantom line: nothing to copy
    const char *s = l.c_str();
    long upto = at - line_start_of(w, doc, n);
    if ( upto > (long)strlen(s) )
	upto = strlen(s);
    long b = blank_run(s);
    if ( b > upto )
	b = upto;
    var ind = perl::substr(l, 0, b);
    long k = upto - 1;
    while ( k >= b && (s[k] == ' ' || s[k] == '\t' || s[k] == '\r') )
	k = k - 1;
    if ( k >= b && es_char_in(w, es, "indentopen", s[k]) )
    {
	var u;
	indent_unit(u, w, es);
	ind = format("{}{}", ind, u);
    }
    out = format("\n{}", ind);
}

// The electric close: `t`, a single block closer typed where only blanks
// precede the caret on its line, first takes one level off that line
// (Edit ▸ Dedent's rule, line_edit_of). Answers the caret after the erase;
// `caret` itself when nothing changed.
long close_dedent(long w, long es, long doc, long caret, const char *t)
{
    if ( !autoindent_on(w, es) || strlen(t) != 1 || !es_char_in(w, es, "indentclose", t[0]) )
	return caret;
    long n = line_of(w, doc, caret);
    long start = line_start_of(w, doc, n);
    var l;
    ui::text_line(l, w, doc, n);
    if ( l.is_null() || caret == start || blank_run(l.c_str()) < caret - start )
	return caret;
    var e;
    line_edit_of(e, w, es, doc, n, leDEDENT, "", false);
    long del = e["del"].as_integer();
    if ( del <= 0 )
	return caret;
    ed_text_erase(w, es, doc, start, del);
    return ui::anchor_shift(caret, start, del, 0, true);
}

// The kind=="text" arm both editors share: search-pattern append while
// searching, else a gated insert at the caret.
void edit_text_run(long w, long es, long doc, long caret, var &ev)
{
    var t = format("{}", ev["text"]);
    if ( es_flag(w, es, "searching") )
    {
	var s;
	ui::get(s, w, es, "search");
	var pat = format("{}{}", s, t);
	ui::set(w, es, "search", pat.c_str());
    }
    else if ( !es_flag(w, es, "editable") )
	refuse_edit(w, es);
    else if ( !selection_replace(w, es, doc, caret, t.c_str()) )
    {
	edit_checkpoint(w, es, doc, caret);
	long at = close_dedent(w, es, doc, caret, t.c_str());
	ed_text_insert(w, es, doc, at, t.c_str());
	ui::set(w, es, "caret", at + strlen(t.c_str()));
    }
}

// THE find rule, one seat: the stored es "search" pattern from caret+1
// with wrap to the top; "Not found." or the caret moves. vised's search
// enter and madcide's find prompt / ^L find-next all repeat it.
void do_find(long w, long es, long doc, long caret)
{
    var pat;
    ui::get(pat, w, es, "search");
    if ( pat.is_null() || strlen(pat.c_str()) == 0 )
    {
	ui::set(w, es, "msg", "No search pattern.");
	return;
    }
    long hit = ui::text_find(w, doc, caret + 1, pat.c_str());
    if ( hit < 0 )
	hit = ui::text_find(w, doc, 0, pat.c_str());
    if ( hit < 0 )
	ui::set(w, es, "msg", "Not found.");
    else
	ui::set(w, es, "caret", hit);
}

// Search-mode key handling; every key is consumed while searching.
bool search_key(long w, long es, long doc, long caret, var &ev)
{
    var k = ev["key_code"];		// ui::key — the enumerator, not the name
    if ( k == ui::key::enter )
    {
	var pat;
	ui::get(pat, w, es, "search");
	ui::set(w, es, "searching", false);
	if ( strlen(pat.c_str()) == 0 )
	    return true;	// silent on empty (the pinned transcript)
	do_find(w, es, doc, caret);
	return true;
    }
    if ( k == ui::key::esc )
    {
	ui::set(w, es, "searching", false);
	return true;
    }
    if ( k == ui::key::backspace )
    {
	var s;
	ui::get(s, w, es, "search");
	long n = strlen(s.c_str());
	if ( n > 0 )
	    ui::set(w, es, "search", perl::substr(s, 0, n - 1));
	return true;
    }
    return true;	// other keys are inert while searching
}

// ---- shared projection pieces (one rule, one seat; scoped-dupaudit
// consolidation — vised and madcide compose the same document nodes) ----

// The heading bar: the caller's label + the badge rule (read-only wins
// over modified).
void compose_heading(var &head, long w, long es, long doc,
		     const char *label)
{
    var h = { "role": "heading", "label": label };
    if ( !es_flag(w, es, "editable") )
	h["content"] = "[read-only]";
    else if ( ui::doc_modified(w, doc) )
	h["content"] = "[modified]";
    head = h;
}

// The edit window bound to the document: caret + selection as hints
// ({caret, sel_start, sel_end} byte offsets) — selection_range's rule: the
// selection runs [mark, bend-else-caret], except under a TWO-POINT
// personality (the es "selblock" flag, derived from the loaded profile's
// vocabulary: JOE's ^K B / ^K K), where a mark WITHOUT an end lights
// nothing; an editor that never sets `bend` or the flag (vised) keeps the
// live mark..caret rule.
void compose_edit_node(var &ed, long w, long es, long doc, long caret)
{
    var e = { "role": "edit" };
    var t;
    ui::text(t, w, doc);
    e["content"] = t;
    e["subject"] = doc;		// the entity the node projects (uinode's
				// edit contract) — a pointer event names it
    var hints = { "caret": caret };
    var sr;
    if ( selection_range(sr, w, es, caret) )
    {
	hints["sel_start"] = sr["lo"];
	hints["sel_end"] = sr["hi"];
    }
    // Presence (client-server V3c): the OTHER clients viewing this document
    // draw as carets in their dealt presence colour SLOT. Each es registered
    // on the doc that still views it (skip this one) contributes its caret
    // and selection; the anchor registry keeps them current across edits, so
    // a peer caret points at the same text it did before.
    var reg;
    ui::get(reg, w, doc, "anchor_es");
    if ( !reg.is_null() )
    {
	var pres = {};
	for ( var r : reg )
	{
	    long peer = r.as_integer();
	    if ( peer == es || es_int(w, peer, "adoc", -1) != doc )
		continue;
	    long pcaret = es_int(w, peer, "caret", -1);
	    if ( pcaret < 0 )
		continue;
	    var pr = { "caret": pcaret, "colour": es_int(w, peer, "presence_colour", 0) };
	    var psr;
	    if ( selection_range(psr, w, peer, pcaret) )
	    {
		pr["sel_start"] = psr["lo"];
		pr["sel_end"] = psr["hi"];
	    }
	    pres.push(pr);
	}
	if ( php::count(pres) > 0 )
	    hints["presence"] = pres;
    }
    e["hints"] = hints;
    ed = e;
}

// The status line's priority rule: prompt (madcide's one prompt mode —
// vised never sets it) > search prompt > transient message > "Ln N,
// Col M" + the caller's idle suffix (vised's key legend; empty for
// madcide).
void compose_status(var &status, long w, long es, long doc, long caret,
		    const char *idle_suffix)
{
    var st = { "role": "status" };
    var msg;
    ui::get(msg, w, es, "msg");
    var pr;
    ui::get(pr, w, es, "prompt");
    if ( !pr.is_null() && strlen(pr.c_str()) > 0 )
    {
	var pi;
	ui::get(pi, w, es, "pinput");
	st["content"] = format("{}: {}", pr, pi);
    }
    else if ( es_flag(w, es, "searching") )
    {
	var s;
	ui::get(s, w, es, "search");
	st["content"] = format("Find: {}", s);
    }
    else if ( !msg.is_null() && strlen(msg.c_str()) > 0 )
	st["content"] = msg;
    else
    {
	long line = line_of(w, doc, caret);
	long start = line_start_of(w, doc, line);
	st["content"] = format("Ln {}, Col {}{}", line, caret - start + 1,
			       idle_suffix);
    }
    status = st;
}

// The Save/Find/Quit menu — the SAME w/f/q actions both editors
// dispatch to the same script verbs (criterion 4's choice tree).
void compose_menu(var &menu)
{
    var save = { "role": "item", "content": "Save" };
    var sa = {};
    sa.push("w");
    save["actions"] = sa;
    var find = { "role": "item", "content": "Find" };
    var fa = {};
    fa.push("f");
    find["actions"] = fa;
    var quit = { "role": "item", "content": "Quit" };
    var qa = {};
    qa.push("q");
    quit["actions"] = qa;
    var mn = { "role": "choice" };
    var opts = {};
    opts.push(save);
    opts.push(find);
    opts.push(quit);
    mn["children"] = opts;
    menu = mn;
}

// Where a caret motion key moves the caret from `caret`; -1 = not a
// motion key (the line keys move by line, pgup / pgdn by ten).
long motion_target(long w, long doc, long caret, var &k)
{
    long size = ui::text_size(w, doc);
    if ( k == ui::key::left )
	return caret > 0 ? caret - 1 : caret;
    if ( k == ui::key::right )
	return caret < size ? caret + 1 : caret;
    long line = line_of(w, doc, caret);
    long start = line_start_of(w, doc, line);
    long col = caret - start;
    if ( k == ui::key::home )
	return start;
    if ( k == ui::key::end )
    {
	long len = ui::text_line_len(w, doc, line);
	if ( len < 0 )
	    len = 0;
	return start + len;
    }
    if ( k == ui::key::up )
	return caret_to_line(w, doc, line - 1, col);
    if ( k == ui::key::down )
	return caret_to_line(w, doc, line + 1, col);
    if ( k == ui::key::pgup )
	return caret_to_line(w, doc, line - 10, col);
    if ( k == ui::key::pgdn )
	return caret_to_line(w, doc, line + 10, col);
    return -1;
}

// Caret movement and the direct edit keys (backspace/del/enter). True =
// the key was one of ours; false = the dispatcher's key, not handled. A
// motion with Shift extends the GUI selection (motion_select); Backspace,
// Delete and Enter replace one.
bool edit_key(long w, long es, long doc, long caret, var &ev)
{
    var k = ev["key_code"];		// ui::key — the enumerator, not the name
    bool editable = es_flag(w, es, "editable");
    long size = ui::text_size(w, doc);
    long to = motion_target(w, doc, caret, k);
    if ( to >= 0 )
    {
	ui::set(w, es, "caret", to);
	motion_select(w, es, caret, ev);
	return true;
    }
    if ( k == ui::key::backspace )
    {
	if ( !editable )
	    refuse_edit(w, es);
	else if ( selection_replace(w, es, doc, caret, "") )
	    return true;
	else if ( caret > 0 )
	{
	    edit_checkpoint(w, es, doc, caret);
	    ed_text_erase(w, es, doc, caret - 1, 1);
	    ui::set(w, es, "caret", caret - 1);
	}
	return true;
    }
    if ( k == ui::key::del )
    {
	if ( !editable )
	    refuse_edit(w, es);
	else if ( selection_replace(w, es, doc, caret, "") )
	    return true;
	else if ( caret < size )
	{
	    edit_checkpoint(w, es, doc, caret);
	    ed_text_erase(w, es, doc, caret, 1);
	}
	return true;
    }
    if ( k == ui::key::enter )
    {
	if ( !editable )
	{
	    refuse_edit(w, es);
	    return true;
	}
	// the newline lands where a selection starts, else at the caret
	var sel;
	long at = caret;
	if ( selection_gui(w, es) && selection_range(sel, w, es, caret) )
	    at = sel["lo"].as_integer();
	var nl;
	enter_text(nl, w, es, doc, at);
	if ( !selection_replace(w, es, doc, caret, nl.c_str()) )
	{
	    edit_checkpoint(w, es, doc, caret);
	    ed_text_insert(w, es, doc, caret, nl.c_str());
	    ui::set(w, es, "caret", caret + strlen(nl.c_str()));
	}
	return true;
    }
    if ( k == ui::key::tab )
    {
	// Tab over a GUI selection that crosses a line indents its lines
	// (IDLE, Thonny, VS Code: the GUI rule beside "typing replaces it").
	if ( selection_spans_lines(w, es, doc, caret) )
	{
	    edit_lines(w, es, doc, caret, leINDENT, "");
	    return true;
	}
	// Otherwise Tab types one indent unit at the caret — a tab byte
	// (JOE, pico, vim's insert mode), or the buffer's spaces (Thonny's
	// four in Python): the one typed-text owner inserts it (B55).
	var unit;
	indent_unit(unit, w, es);
	var te = { "event": "text", "event_code": ui::event_kind::text, "text": unit };
	edit_text_run(w, es, doc, caret, te);
	return true;
    }
    return false;	// not an edit-core key — the dispatcher's business
}

// The pointer arm — ONE caret model for mouse and keyboard. `ev` is the
// engine's pointer event ({phase_code, offset}: a BYTE offset in this document
// the target's hit test resolved to). A press places the caret and drops
// any live selection; a drag lights [press, here] as a GUI selection
// (select_span: the SAME markers the block keys set, so typing replaces
// it); a release is the last placement. `selectable` false (madcide's displayed views:
// stored-space markers are meaningless there) keeps the gesture to caret
// placement. The press offset parks on the es as "anchor" for the drag
// that follows it; the release drops it.
void edit_pointer(long w, long es, long doc, var &ev, bool selectable)
{
    var ph = ev["phase_code"];		// ui::pointer_phase
    long off = clamp_caret_to(w, doc, ev["offset"].as_integer());
    ui::set(w, es, "caret", off);
    if ( ph == ui::pointer_phase::down )
    {
	select_clear(w, es);
	ui::set(w, es, "anchor", off);
	return;
    }
    long anchor = es_int(w, es, "anchor", -1);
    if ( anchor < 0 )
	return;		// a drag whose press landed elsewhere: caret only
    if ( selectable )
	select_span(w, es, anchor, off);	// a GUI selection (anchor == off: none)
    else
	select_clear(w, es);
    if ( ph == ui::pointer_phase::up )
	ui::set(w, es, "anchor", -1);
}
