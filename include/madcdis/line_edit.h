#ifndef __MADCDIS_LINE_EDIT_H
#define __MADCDIS_LINE_EDIT_H 1

// madcdis/line_edit.h — the line editor (plan §41.7a, D23): one entry of a
// prompt, edited on the normal screen, as Julia's LineEdit and readline edit
// one. Two pieces, both terminal-free:
//
//   - line_edit, the MODEL: the entry's text and caret, the undo stack and
//     the kill slot, driven by the semantic events of ui_apply_keys (the one
//     keys -> events loop, over the one key owner key_resolver with
//     line_edit_bindings() and an empty focus_state). It never classifies
//     an entry and never finds a completion: Enter and Tab stop the model
//     and ask its HOST (outcome::enter, outcome::complete), which answers
//     through entered() and completed(). So the model knows no session, and
//     the host's classifier runs once per Enter.
//   - line_painter, the BYTES: Julia's full refresh of the entry, relative
//     to the cursor, with no alternate screen and no scroll region. It
//     climbs to the entry's first row erasing each row, writes the prompt
//     and the lines (continuation lines indented to the prompt's width,
//     D22), and places the caret. A line wider than the terminal takes the
//     rows it wraps to. It builds bytes only, as tui_key_bytes does, so the
//     unit battery reads them; a target writes them.
//
// The keys are DATA: line_edit_bindings() binds readline's Emacs defaults
// (Julia's and IPython's defaults share them) to line_action CODES. Meta is
// readline's two ways: a terminal's Alt arrives as Esc and the key in one
// burst, which the parser reads as Alt and the key (`alt+b`), and Esc typed
// before the key is the two-key `esc b`; the table binds both.
//
// Thread contract (.claude/rules/thread-safety.md): plain value objects,
// confined to the thread that owns them (the C++ standard-library
// convention: distinct objects are safe, shared mutation needs
// synchronization). The bindings table is built once and never changes.

#include <cstdint>
#include <deque>
#include <string>
#include <utility>
#include <vector>

#include "madcdis/keys.h"
#include "madcdis/text_buffer.h"	// the one word rule (word_left_in / word_right_in)
#include "madcdis/text_utf16.h"
#include "madcdis/ui_events.h"

namespace madc {
namespace hub {

// ------------------------------------------------------------- the actions
// What a bound key does. The CODE rides the binding (tui_binding::code);
// the model switches on it. The names in line_edit_bindings() are
// readline's, the vocabulary a keys profile will name them by.
enum class line_action : unsigned char
{
    none,
    accept,		// enter: the host says whether the entry is finished
    newline,		// esc enter: a newline, whatever the entry's verdict
    complete,		// tab: indent, or ask the host to complete the word
    backward_char, forward_char, backward_word, forward_word,
    line_start, line_end, previous_line, next_line,
    delete_back, delete_forward, delete_or_end,
    kill_to_end, kill_to_start, kill_space_back, kill_word_back,
    kill_word_forward, yank, transpose, undo,
    cancel, clear_screen,
    search_back, search_forward, abort	// ^R, ^S, ^G (slice 2)
};

// The default keys: readline's Emacs set. Built once; a finalize failure
// would be a defect in this table, which the unit battery pins.
inline const tui_bindings &line_edit_bindings()
{
    static const tui_bindings table = [] {
	static const struct
	{
	    const char *seq;
	    const char *name;
	    line_action action;
	} rows[] = {
	    { "enter",	       "accept-line",	       line_action::accept },
	    { "esc enter",     "insert-newline",       line_action::newline },
	    { "tab",	       "complete",	       line_action::complete },
	    { "left",	       "backward-char",	       line_action::backward_char },
	    { "^b",	       "backward-char",	       line_action::backward_char },
	    { "right",	       "forward-char",	       line_action::forward_char },
	    { "^f",	       "forward-char",	       line_action::forward_char },
	    { "esc b",	       "backward-word",	       line_action::backward_word },
	    { "alt+b",	       "backward-word",	       line_action::backward_word },
	    { "esc f",	       "forward-word",	       line_action::forward_word },
	    { "alt+f",	       "forward-word",	       line_action::forward_word },
	    { "home",	       "beginning-of-line",    line_action::line_start },
	    { "^a",	       "beginning-of-line",    line_action::line_start },
	    { "end",	       "end-of-line",	       line_action::line_end },
	    { "^e",	       "end-of-line",	       line_action::line_end },
	    { "up",	       "previous-line",	       line_action::previous_line },
	    { "^p",	       "previous-line",	       line_action::previous_line },
	    { "down",	       "next-line",	       line_action::next_line },
	    { "^n",	       "next-line",	       line_action::next_line },
	    { "backspace",     "backward-delete-char", line_action::delete_back },
	    { "del",	       "delete-char",	       line_action::delete_forward },
	    { "^d",	       "delete-char-or-eof",   line_action::delete_or_end },
	    { "^k",	       "kill-line",	       line_action::kill_to_end },
	    { "^u",	       "unix-line-discard",    line_action::kill_to_start },
	    { "^w",	       "unix-word-rubout",     line_action::kill_space_back },
	    { "esc backspace", "backward-kill-word",   line_action::kill_word_back },
	    { "esc d",	       "kill-word",	       line_action::kill_word_forward },
	    { "alt+d",	       "kill-word",	       line_action::kill_word_forward },
	    { "^y",	       "yank",		       line_action::yank },
	    { "^t",	       "transpose-chars",      line_action::transpose },
	    { "^_",	       "undo",		       line_action::undo },
	    { "^c",	       "interrupt",	       line_action::cancel },
	    { "^l",	       "clear-screen",	       line_action::clear_screen },
	    { "^r",	       "reverse-search-history", line_action::search_back },
	    { "^s",	       "forward-search-history", line_action::search_forward },
	    { "^g",	       "abort",		       line_action::abort },
	};
	tui_bindings b;
	for ( size_t i = 0; i < sizeof(rows) / sizeof(rows[0]); ++i )
	    b.bind(rows[i].seq, rows[i].name, (int64_t)rows[i].action);
	std::string err;
	b.finalize(err);
	return b;
    }();
    return table;
}

// -------------------------------------------------------------- the layout
// A line's layout (tab stops, control bytes, code-point widths) is
// line_layout / line_width, madcdis/text_utf16.h: the one rule this editor's
// painter and the compiler's diagnostic caret share.

// What the painter shows: the prompt, the entry's lines, and the caret as a
// line and a byte in it.
struct line_view
{
    std::string prompt;
    std::vector<std::string> lines;
    size_t caret_line;
    size_t caret_byte;
    line_view() : caret_line(0), caret_byte(0) {}
};

// ------------------------------------------------------------- the history
// The entries a session took, oldest first (plan §41.7a): what Up recalls
// and Ctrl-R searches. The host owns it across entries, fills it from the
// history file and adds each entry the session takes; the model only reads
// it. A blank entry and a repeat of the newest are not added (Julia's rule,
// measured).
class line_history
{
    std::vector<std::string> _entries;

public:
    enum : size_t { npos = (size_t)-1 };

    // True when the entry joined the ring (the host then writes its record).
    bool add(const std::string &entry)
    {
	if ( entry.find_first_not_of(" \t\n") == std::string::npos )
	    return false;
	if ( !_entries.empty() && _entries.back() == entry )
	    return false;
	_entries.push_back(entry);
	return true;
    }
    size_t size() const { return _entries.size(); }
    const std::string &at(size_t i) const { return _entries[i]; }

    // Prefix recall (Up / Down): the newest entry before `from` that starts
    // with `prefix`, or the oldest after it. npos when there is none.
    size_t prefix_before(const std::string &prefix, size_t from) const
    {
	for ( size_t i = from > _entries.size() ? _entries.size() : from; i > 0; --i )
	    if ( _entries[i - 1].compare(0, prefix.size(), prefix) == 0 )
		return i - 1;
	return npos;
    }
    size_t prefix_after(const std::string &prefix, size_t from) const
    {
	for ( size_t i = from + 1; i < _entries.size(); ++i )
	    if ( _entries[i].compare(0, prefix.size(), prefix) == 0 )
		return i;
	return npos;
    }

    // Incremental search (Ctrl-R / Ctrl-S): the newest entry at or before
    // `from` that holds `needle` (its last occurrence), or the oldest at or
    // after it (its first). `at` gets the occurrence's byte. npos when none.
    size_t holding_before(const std::string &needle, size_t from,
			  size_t &at) const
    {
	if ( _entries.empty() )
	    return npos;
	for ( size_t i = (from >= _entries.size() ? _entries.size() - 1 : from) + 1;
	      i > 0; --i )
	{
	    size_t p = _entries[i - 1].rfind(needle);
	    if ( p != std::string::npos )
	    {
		at = p;
		return i - 1;
	    }
	}
	return npos;
    }
    size_t holding_after(const std::string &needle, size_t from,
			 size_t &at) const
    {
	for ( size_t i = from; i < _entries.size(); ++i )
	{
	    size_t p = _entries[i].find(needle);
	    if ( p != std::string::npos )
	    {
		at = p;
		return i;
	    }
	}
	return npos;
    }
};

// The history FILE's record (Julia's repl_history.jl, measured): a time
// line, a mode line naming the prompt the entry was typed at (D22's
// standard name), then every line of the entry after a tab, so a line of an
// entry can never begin a record.
inline std::string history_record(const std::string &time_utc,
				  const std::string &mode,
				  const std::string &entry)
{
    std::string r = "# time: " + time_utc + "\n# mode: " + mode + "\n";
    size_t begin = 0;
    for (;;)
    {
	size_t nl = entry.find('\n', begin);
	r += '\t';
	r.append(entry, begin, (nl == std::string::npos ? entry.size() : nl) - begin);
	r += '\n';
	if ( nl == std::string::npos )
	    break;
	begin = nl + 1;
    }
    return r;
}

// A history file's records, in file order: (mode, entry) pairs. A record
// starts at its time line; lines after a tab are its entry's; other lines
// are skipped, so a damaged record costs only itself.
inline std::vector<std::pair<std::string, std::string> >
history_records(const std::string &file)
{
    std::vector<std::pair<std::string, std::string> > out;
    bool open = false, has_line = false;
    size_t begin = 0;
    while ( begin < file.size() )
    {
	size_t nl = file.find('\n', begin);
	size_t end = nl == std::string::npos ? file.size() : nl;
	std::string line = file.substr(begin, end - begin);
	begin = end + 1;
	if ( !line.empty() && line[line.size() - 1] == '\r' )
	    line.erase(line.size() - 1);
	if ( line.compare(0, 7, "# time:") == 0 )
	{
	    out.push_back(std::make_pair(std::string(), std::string()));
	    open = true;
	    has_line = false;
	}
	else if ( open && line.compare(0, 8, "# mode: ") == 0 )
	    out.back().first = line.substr(8);
	else if ( open && !line.empty() && line[0] == '\t' )
	{
	    if ( has_line )
		out.back().second += '\n';
	    out.back().second += line.substr(1);
	    has_line = true;
	}
    }
    return out;
}

// --------------------------------------------------------------- the model
class line_edit
{
public:
    // Why step() stopped.
    enum class outcome : unsigned char
    {
	idle,		// the queued input is used up: repaint, read more keys
	enter,		// Enter: classify text() and answer entered()
	complete,	// Tab on a word: answer completed()
	dropped,	// Ctrl-C: the entry is dropped
	ended		// Ctrl-D on an empty entry: the input ended
    };
    // The host's answer to outcome::enter.
    enum class verdict : unsigned char
    {
	taken,		// the entry is final: it is over
	incomplete,	// a newline at the caret
	extendable	// a newline; Enter on the blank last line then takes it
    };

    explicit line_edit(const std::string &prompt)
	: _prompt(prompt), _caret(0), _killing(false), _enters(0),
	  _extendable(false), _pasted(false), _final(false), _tabs(0),
	  _goal(npos), _clear(false), _history(0), _recall(npos),
	  _searching(false), _search_back(true), _search_failed(false),
	  _match(npos), _match_at(0), _saved_caret(0) {}

    // The history Up recalls and Ctrl-R searches (slice 2): the host's,
    // read here and never changed. Null: no history.
    void set_history(const line_history *h) { _history = h; }

    // A new entry: the text empties and its undo history with it. The kill
    // slot stays (readline's), and so does input queued past the last
    // entry: the rest of a paste is the next entry's.
    void start()
    {
	_text.clear();
	_caret = 0;
	_undo.clear();
	_killing = false;
	_enters = 0;
	_extendable = false;
	_pasted = false;
	_final = false;
	_tabs = 0;
	_goal = npos;
	_clear = false;
	_listed.clear();
	_recall = npos;
	_searching = false;
    }

    // Is an incremental search (Ctrl-R / Ctrl-S) in progress?
    bool searching() const { return _searching; }

    // Queue one read's events (ui_apply_keys's output).
    void feed(const std::vector<tui_event> &events)
    {
	_queue.insert(_queue.end(), events.begin(), events.end());
    }

    // Use queued input until the host must answer (enter, complete), the
    // entry ends (dropped, ended), or nothing is left (idle).
    outcome step()
    {
	while ( !_queue.empty() )
	{
	    tui_event &e = _queue.front();
	    if ( _searching )
	    {
		if ( search_event(e) )
		    _queue.pop_front();
		continue;
	    }
	    if ( e.kind == tui_event_kind::text )
	    {
		// A line break in text is pasted (a typed one is the enter
		// key): it acts as Enter, without the three-Enter rule.
		size_t nl = e.text.find('\n');
		if ( nl != 0 )
		    insert_run(e.text.substr(0, nl));
		if ( nl == std::string::npos )
		{
		    _queue.pop_front();
		    continue;
		}
		e.text.erase(0, nl + 1);
		if ( e.text.empty() )
		    _queue.pop_front();
		return ask_enter(true);
	    }
	    tui_event ev = e;
	    _queue.pop_front();
	    if ( ev.kind != tui_event_kind::action || ev.action_code == 0 )
		continue;	// an unbound key, a chord's echo, a resize
	    outcome o = apply((line_action)ev.action_code);
	    if ( o != outcome::idle )
		return o;
	}
	return outcome::idle;
    }

    // outcome::enter: must the host take the entry as it stands, without
    // asking (Julia's third Enter in a row at the end of an incomplete
    // entry; Enter on the blank last line of an extendable one, D11)?
    bool enter_is_final() const { return _final; }

    // The host's answer to outcome::enter.
    void entered(verdict v)
    {
	if ( v == verdict::taken )
	    return;
	edit();
	insert_text("\n");
	if ( !_pasted )
	    ++_enters;
	_extendable = v == verdict::extendable;
    }

    // The host's answer to outcome::complete: the word starts at byte
    // `start` and these are its candidates (sorted, no duplicates). One is
    // inserted; several insert their longest common prefix, and a second
    // Tab in a row lists them (take_listed). None: nothing (Julia).
    void completed(size_t start, const std::vector<std::string> &candidates)
    {
	unsigned repeats = _tabs++;
	if ( candidates.empty() || start > _caret )
	    return;
	const std::string word = _text.substr(start, _caret - start);
	if ( candidates.size() == 1 )
	{
	    replace_word(start, candidates[0]);
	    _tabs = 0;
	    return;
	}
	std::string prefix = candidates[0];
	for ( size_t i = 1; i < candidates.size(); ++i )
	{
	    size_t n = 0;
	    while ( n < prefix.size() && n < candidates[i].size()
		    && prefix[n] == candidates[i][n] )
		++n;
	    prefix.erase(n);
	}
	if ( prefix.size() > word.size()
	     && prefix.compare(0, word.size(), word) == 0 )
	    replace_word(start, prefix);
	else if ( repeats > 0 )
	    _listed = candidates;
    }

    const std::string &text() const { return _text; }
    size_t caret() const { return _caret; }
    const std::string &prompt() const { return _prompt; }

    // The painter's view, with the caret where it is or at the end. During a
    // search it is readline's one line: the search prompt with its query,
    // then the matched entry, the caret at the match.
    line_view view() const
    {
	if ( _searching )
	    return make_view(search_shown(), search_caret(), search_prompt());
	return make_view(_text, _caret, _prompt);
    }
    line_view view_at_end() const
    {
	if ( _searching )
	    return make_view(search_shown(), search_shown().size(),
			     search_prompt());
	return make_view(_text, _text.size(), _prompt);
    }

    // Screen requests since the last take: a clear (Ctrl-L), and the
    // candidates a second Tab listed.
    bool take_clear()
    {
	bool c = _clear;
	_clear = false;
	return c;
    }
    std::vector<std::string> take_listed()
    {
	std::vector<std::string> l;
	l.swap(_listed);
	return l;
    }

private:
    enum : size_t { npos = (size_t)-1 };
    struct snapshot
    {
	std::string text;
	size_t caret;
    };

    std::string _prompt;
    std::string _text;
    size_t _caret;
    std::vector<snapshot> _undo;
    std::string _kill;
    bool _killing;		// the last action killed: the next kill joins it
    unsigned _enters;		// typed Enters in a row, each answered by a newline
    bool _extendable;		// the last verdict was extendable
    bool _pasted;		// the pending enter is a pasted line break
    bool _final;		// the pending enter takes the entry as it stands
    unsigned _tabs;		// Tabs in a row before the pending one
    size_t _goal;		// the column Up / Down keep (npos: none yet)
    bool _clear;
    std::vector<std::string> _listed;
    std::deque<tui_event> _queue;
    const line_history *_history;
    size_t _recall;		// the recalled entry (npos: none)
    std::string _live;		// what was typed before recall or search began
    std::string _prefix;	// the text before the caret when recall began
    bool _searching, _search_back, _search_failed;
    std::string _query, _last_query;
    size_t _match, _match_at;	// the matched entry, and the match in it
    std::string _saved;		// the entry as it was when the search began
    size_t _saved_caret;

    // Code point steps: a caret never stops inside a UTF-8 sequence.
    size_t prev_cp(size_t i) const
    {
	if ( i == 0 )
	    return 0;
	--i;
	while ( i > 0 && ((unsigned char)_text[i] & 0xC0) == 0x80 )
	    --i;
	return i;
    }
    size_t next_cp(size_t i) const
    {
	if ( i >= _text.size() )
	    return _text.size();
	uint32_t cp = 0;
	return i + utf8_decode_at(_text, i, cp);
    }
    size_t line_begin(size_t i) const
    {
	size_t nl = i == 0 ? std::string::npos : _text.rfind('\n', i - 1);
	return nl == std::string::npos ? 0 : nl + 1;
    }
    size_t line_finish(size_t i) const
    {
	size_t nl = _text.find('\n', i);
	return nl == std::string::npos ? _text.size() : nl;
    }
    // The display column of byte `i` of the line that starts at `begin`.
    size_t column_in_line(size_t begin, size_t i) const
    {
	std::vector<size_t> col;
	line_layout(_text.substr(begin, line_finish(begin) - begin),
		    line_width(_prompt), col);
	return col[i - begin];
    }
    // The byte of the line starting at `begin` nearest the column `goal`
    // from the left: the first code point that starts at or past it.
    size_t byte_at_column(size_t begin, size_t goal) const
    {
	std::vector<size_t> col;
	std::string line = _text.substr(begin, line_finish(begin) - begin);
	line_layout(line, line_width(_prompt), col);
	for ( size_t b = 0; b < line.size(); ++b )
	    if ( col[b] >= goal
		 && ((unsigned char)line[b] & 0xC0) != 0x80 )
		return begin + b;
	return begin + line.size();
    }

    static line_view make_view(const std::string &text, size_t caret,
			       const std::string &prompt)
    {
	line_view v;
	v.prompt = prompt;
	size_t begin = 0;
	for (;;)
	{
	    size_t nl = text.find('\n', begin);
	    size_t end = nl == std::string::npos ? text.size() : nl;
	    if ( caret >= begin && caret <= end )
	    {
		v.caret_line = v.lines.size();
		v.caret_byte = caret - begin;
	    }
	    v.lines.push_back(text.substr(begin, end - begin));
	    if ( nl == std::string::npos )
		break;
	    begin = nl + 1;
	}
	return v;
    }

    // ---- history recall (Up / Down), plan §41.7a slice 2
    // Up on the first line: the newest older entry that starts with the
    // text before the caret (both precedents), the caret staying after
    // that prefix (Julia). The prefix and what was typed are kept while Up
    // and Down go on, and Down past the newest restores what was typed.
    void recall_older()
    {
	if ( !_history )
	    return;
	if ( _recall == npos )
	{
	    _live = _text;
	    _prefix = _text.substr(0, _caret);
	}
	size_t i = _history->prefix_before(_prefix,
				_recall == npos ? _history->size() : _recall);
	if ( i == line_history::npos )
	    return;
	_recall = i;
	_text = _history->at(i);
	_caret = _prefix.size();
    }
    void recall_newer()
    {
	if ( !_history || _recall == npos )
	    return;
	size_t i = _history->prefix_after(_prefix, _recall);
	if ( i == line_history::npos )
	{
	    _text = _live;
	    _recall = npos;
	}
	else
	{
	    _recall = i;
	    _text = _history->at(i);
	}
	_caret = _prefix.size() <= _text.size() ? _prefix.size() : _text.size();
    }

    // ---- incremental search (Ctrl-R / Ctrl-S), plan §41.7a slice 2
    // readline's one-line form. Typing extends the query and searches on
    // from the match; Ctrl-R / Ctrl-S step to the next older / newer entry
    // holding it (an empty query takes the last one); Enter leaves the
    // match in the entry, unrun (Julia); Ctrl-G or Ctrl-C restores the
    // entry; any other key keeps the match and then acts.
    void begin_search(bool back)
    {
	_searching = true;
	_search_back = back;
	_search_failed = false;
	_query.clear();
	_match = npos;
	_match_at = 0;
	_saved = _text;
	_saved_caret = _caret;
    }
    // Search from the match (`again`: past it) for the query.
    void search(bool again)
    {
	if ( !_history || _query.empty() )
	{
	    _search_failed = false;
	    return;
	}
	size_t at = 0, i;
	if ( _search_back )
	{
	    if ( again && _match == 0 )
		i = line_history::npos;
	    else
		i = _history->holding_before(_query,
		    _match == npos ? _history->size()
				   : (again ? _match - 1 : _match), at);
	}
	else
	    i = _match == npos ? line_history::npos
		: _history->holding_after(_query,
					  again ? _match + 1 : _match, at);
	if ( i == line_history::npos )
	{
	    _search_failed = true;
	    return;
	}
	_search_failed = false;
	_match = i;
	_match_at = at;
    }
    // The search ends with the match in the entry, or with the entry as
    // it was.
    void end_search(bool keep)
    {
	_searching = false;
	if ( !_query.empty() )
	    _last_query = _query;
	if ( keep && _match != npos )
	{
	    _live = _saved;
	    _prefix.clear();
	    _recall = _match;	// Up and Down go on from the match
	    _text = _history->at(_match);
	    _caret = _match_at;
	}
	else
	{
	    _text = _saved;
	    _caret = _saved_caret;
	}
    }
    const std::string &search_shown() const
    {
	return _match != npos ? _history->at(_match) : _saved;
    }
    size_t search_caret() const
    {
	return _match != npos ? _match_at : _saved_caret;
    }
    std::string search_prompt() const
    {
	return std::string(_search_failed ? "(failed " : "(")
	    + (_search_back ? "reverse-i-search" : "i-search") + ")'"
	    + _query + "': ";
    }
    // One queued event while searching. False when the event stays queued
    // for the editor (the search ended on it, and it acts next).
    bool search_event(tui_event &e)
    {
	if ( e.kind == tui_event_kind::text )
	{
	    // A pasted line break ends the search as Enter does; the rest
	    // of the paste is typed into the entry.
	    size_t nl = e.text.find('\n');
	    _query += e.text.substr(0, nl);
	    search(false);
	    if ( nl == std::string::npos )
		return true;
	    e.text.erase(0, nl + 1);
	    end_search(true);
	    return e.text.empty();
	}
	if ( e.kind != tui_event_kind::action || e.action_code == 0 )
	    return true;
	switch ( (line_action)e.action_code )
	{
	    case line_action::search_back:
	    case line_action::search_forward:
		_search_back = (line_action)e.action_code
			       == line_action::search_back;
		if ( _query.empty() )
		{
		    _query = _last_query;
		    search(false);
		}
		else
		    search(true);
		return true;
	    case line_action::delete_back:
		if ( !_query.empty() )
		{
		    // The shorter query searches again from the newest entry
		    // (backward) or from the match (forward).
		    size_t i = _query.size() - 1;
		    while ( i > 0 && ((unsigned char)_query[i] & 0xC0) == 0x80 )
			--i;
		    _query.erase(i);
		    if ( _search_back )
			_match = npos;
		    search(false);
		}
		return true;
	    case line_action::accept:
		end_search(true);
		return true;
	    case line_action::abort:
	    case line_action::cancel:
		end_search(false);
		return true;
	    default:
		end_search(true);
		return false;	// the key acts on the entry
	}
    }

    // Before a change: one undo step. A change ends a recall: the
    // recalled entry is now what is typed.
    void edit()
    {
	_recall = npos;
	snapshot s;
	s.text = _text;
	s.caret = _caret;
	_undo.push_back(s);
    }
    void insert_text(const std::string &s)
    {
	_text.insert(_caret, s);
	_caret += s.size();
    }
    // A typed or pasted run: one edit (§7.5's coalescing).
    void insert_run(const std::string &run)
    {
	if ( run.empty() )
	    return;
	_enters = 0;
	_tabs = 0;
	_killing = false;
	_goal = npos;
	edit();
	insert_text(run);
    }
    void erase_range(size_t from, size_t to)
    {
	if ( to <= from )
	    return;
	edit();
	_text.erase(from, to - from);
	_caret = from;
    }
    void kill_range(size_t from, size_t to, bool backward)
    {
	if ( to <= from )
	    return;
	std::string s = _text.substr(from, to - from);
	if ( _killing )
	    _kill = backward ? s + _kill : _kill + s;
	else
	    _kill = s;
	erase_range(from, to);
    }
    void replace_word(size_t start, const std::string &with)
    {
	edit();
	_text.replace(start, _caret - start, with);
	_caret = start + with.size();
    }
    size_t word_left(size_t i) const { return text_buffer::word_left_in(_text, i); }
    size_t word_right(size_t i) const { return text_buffer::word_right_in(_text, i); }

    outcome ask_enter(bool pasted)
    {
	_pasted = pasted;
	size_t last = line_begin(_text.size());
	bool on_last = _caret >= last;
	bool last_blank = true;
	for ( size_t i = last; i < _text.size(); ++i )
	    if ( !text_buffer::space_byte(_text[i]) )
		last_blank = false;
	_final = (!pasted && _caret == _text.size() && _enters >= 2)
	      || (_extendable && on_last && last_blank && last > 0);
	return outcome::enter;
    }

    outcome tab()
    {
	size_t begin = line_begin(_caret);
	bool blank = true;
	for ( size_t i = begin; i < _caret; ++i )
	    if ( !text_buffer::space_byte(_text[i]) )
		blank = false;
	if ( !blank )
	    return outcome::complete;
	// Only whitespace before the caret on its line: indent to the next
	// multiple of four columns (Julia's width on a continuation line).
	size_t origin = line_width(_prompt);
	size_t at = column_in_line(begin, _caret) - origin;
	_tabs = 0;
	edit();
	insert_text(std::string(4 - at % 4, ' '));
	return outcome::idle;
    }

    outcome apply(line_action a)
    {
	bool kill = a == line_action::kill_to_end
	    || a == line_action::kill_to_start
	    || a == line_action::kill_space_back
	    || a == line_action::kill_word_back
	    || a == line_action::kill_word_forward;
	bool joins = _killing;
	_killing = false;
	if ( a != line_action::accept )
	    _enters = 0;
	if ( a != line_action::complete )
	    _tabs = 0;
	if ( a != line_action::previous_line && a != line_action::next_line )
	{
	    _goal = npos;
	    _recall = npos;	// the recalled entry is now what is typed
	}
	if ( kill )
	    _killing = joins;
	switch ( a )
	{
	    case line_action::accept:
		return ask_enter(false);
	    case line_action::newline:
		edit();
		insert_text("\n");
		break;
	    case line_action::complete:
		return tab();
	    case line_action::backward_char:
		_caret = prev_cp(_caret);
		break;
	    case line_action::forward_char:
		_caret = next_cp(_caret);
		break;
	    case line_action::backward_word:
		_caret = word_left(_caret);
		break;
	    case line_action::forward_word:
		_caret = word_right(_caret);
		break;
	    case line_action::line_start:
		_caret = line_begin(_caret);
		break;
	    case line_action::line_end:
		_caret = line_finish(_caret);
		break;
	    case line_action::previous_line:
	    {
		size_t begin = line_begin(_caret);
		if ( begin == 0 )
		{
		    recall_older();	// the first line: history
		    _goal = npos;
		    break;
		}
		if ( _goal == npos )
		    _goal = column_in_line(begin, _caret);
		_caret = byte_at_column(line_begin(begin - 1), _goal);
		break;
	    }
	    case line_action::next_line:
	    {
		size_t end = line_finish(_caret);
		if ( end == _text.size() )
		{
		    recall_newer();	// the last line: history
		    _goal = npos;
		    break;
		}
		if ( _goal == npos )
		    _goal = column_in_line(line_begin(_caret), _caret);
		_caret = byte_at_column(end + 1, _goal);
		break;
	    }
	    case line_action::delete_back:
		erase_range(prev_cp(_caret), _caret);
		break;
	    case line_action::delete_or_end:
		if ( _text.empty() )
		    return outcome::ended;
		erase_range(_caret, next_cp(_caret));
		break;
	    case line_action::delete_forward:
		erase_range(_caret, next_cp(_caret));
		break;
	    case line_action::kill_to_end:
	    {
		// At the end of a line the line break goes (Emacs's C-k).
		size_t end = line_finish(_caret);
		kill_range(_caret, end == _caret ? next_cp(_caret) : end,
			   false);
		_killing = true;
		break;
	    }
	    case line_action::kill_to_start:
		kill_range(line_begin(_caret), _caret, true);
		_killing = true;
		break;
	    case line_action::kill_space_back:
	    {
		size_t i = _caret;
		while ( i > 0 && text_buffer::space_byte(_text[i - 1]) )
		    --i;
		while ( i > 0 && !text_buffer::space_byte(_text[i - 1]) )
		    --i;
		kill_range(i, _caret, true);
		_killing = true;
		break;
	    }
	    case line_action::kill_word_back:
		kill_range(word_left(_caret), _caret, true);
		_killing = true;
		break;
	    case line_action::kill_word_forward:
		kill_range(_caret, word_right(_caret), false);
		_killing = true;
		break;
	    case line_action::yank:
		if ( !_kill.empty() )
		{
		    edit();
		    insert_text(_kill);
		}
		break;
	    case line_action::transpose:
	    {
		// readline's transpose-chars, by code point and within the
		// line: at the line's end the two before the caret swap;
		// elsewhere the one before and the one at the caret swap and
		// the caret moves past them. Nothing at the line's start.
		size_t begin = line_begin(_caret);
		size_t end = line_finish(_caret);
		if ( _caret == begin )
		    break;
		size_t mid = _caret == end ? prev_cp(_caret) : _caret;
		if ( mid == begin )
		    break;
		size_t lo = prev_cp(mid);
		size_t hi = next_cp(mid);
		std::string a1 = _text.substr(lo, mid - lo);
		std::string b1 = _text.substr(mid, hi - mid);
		edit();
		_text.replace(lo, hi - lo, b1 + a1);
		_caret = hi;
		break;
	    }
	    case line_action::undo:
		if ( !_undo.empty() )
		{
		    _text = _undo.back().text;
		    _caret = _undo.back().caret;
		    _undo.pop_back();
		}
		break;
	    case line_action::cancel:
		return outcome::dropped;
	    case line_action::clear_screen:
		_clear = true;
		break;
	    case line_action::search_back:
	    case line_action::search_forward:
		begin_search(a == line_action::search_back);
		break;
	    case line_action::abort:	// outside a search: nothing to abort
	    case line_action::none:
	    default:
		break;
	}
	return outcome::idle;
    }
};

// ------------------------------------------------------------- the painter
// The bytes that show an entry (Julia's full refresh). Only relative moves:
// the entry is wherever the cursor was when it began.
class line_painter
{
    size_t _rows;	// the rows the last paint used (0: nothing painted)
    size_t _caret_row;	// the row, from the entry's first, it left the cursor on

    static void csi(std::string &out, size_t n, char final_byte)
    {
	out += "\x1b[";
	out += std::to_string(n);
	out += final_byte;
    }

public:
    line_painter() : _rows(0), _caret_row(0) {}

    // Forget the last paint: the next one starts on the cursor's row.
    void reset()
    {
	_rows = 0;
	_caret_row = 0;
    }

    // An entry's start, after output the host did not write: `cols` blanks
    // and a return leave the cursor at the start of a fresh row, the one it
    // was on when that row was empty and the next one otherwise. A terminal
    // wraps at the margin, so a row that already held text (`printf("x")`)
    // pushes the blanks onto the next row; an empty one holds them all.
    static std::string fresh_row(size_t cols)
    {
	std::string out(cols ? cols : 1, ' ');
	out += "\r\x1b[0K";
	return out;
    }

    std::string paint(const line_view &v, size_t cols)
    {
	if ( cols == 0 )
	    cols = 1;
	std::string out;
	// Down to the last row painted, then up to the first, erasing each.
	if ( _rows > 0 )
	{
	    size_t down = _rows - 1 - _caret_row;
	    if ( down )
		csi(out, down, 'B');
	    for ( size_t r = 0; r < _rows; ++r )
	    {
		out += "\r\x1b[0K";
		if ( r + 1 < _rows )
		    csi(out, 1, 'A');
	    }
	}
	else
	    out += "\r\x1b[0K";
	const size_t origin = line_width(v.prompt);
	size_t row = 0, end_row = 0;
	size_t caret_row = 0, caret_col = origin;
	std::vector<size_t> col;
	for ( size_t i = 0; i < v.lines.size(); ++i )
	{
	    const bool last = i + 1 == v.lines.size();
	    if ( i == 0 )
		out += v.prompt;
	    else
	    {
		out += "\r\n";
		out.append(origin, ' ');
	    }
	    out += line_layout(v.lines[i], origin, col);
	    const size_t w = col.back();
	    const bool exact = w > 0 && w % cols == 0;
	    if ( i == v.caret_line )
	    {
		size_t c = col[v.caret_byte < v.lines[i].size()
			       ? v.caret_byte : v.lines[i].size()];
		if ( c == w && exact && !last )
		{
		    // At the end of a line that fills its last row: the
		    // cursor cannot pass the margin, so it sits on the
		    // last cell.
		    caret_row = row + c / cols - 1;
		    caret_col = cols - 1;
		}
		else
		{
		    caret_row = row + c / cols;
		    caret_col = c % cols;
		}
	    }
	    if ( last )
	    {
		// A last line that fills its row leaves the cursor pending
		// at the margin: a line break makes the row after it real.
		if ( exact )
		    out += "\r\n";
		end_row = row + w / cols;
		row = end_row + 1;
	    }
	    else
		row += exact ? w / cols : w / cols + 1;
	}
	if ( end_row > caret_row )
	    csi(out, end_row - caret_row, 'A');
	out += '\r';
	if ( caret_col )
	    csi(out, caret_col, 'C');
	_rows = row;
	_caret_row = caret_row;
	return out;
    }

    // The entry is over: repaint it with the caret at its end, write `tail`
    // there (Ctrl-C's "^C"), and start a new row for what follows.
    std::string finish(line_view v, size_t cols, const std::string &tail)
    {
	if ( !v.lines.empty() )
	{
	    v.caret_line = v.lines.size() - 1;
	    v.caret_byte = v.lines.back().size();
	}
	std::string out = paint(v, cols);
	out += tail;
	out += "\r\n";
	reset();
	return out;
    }

    // Candidates in columns, down then across (readline's and ls's order),
    // each padded to the widest plus two; the last column needs no padding.
    static std::string list(const std::vector<std::string> &items, size_t cols)
    {
	std::string out;
	if ( items.empty() )
	    return out;
	size_t widest = 0;
	for ( size_t i = 0; i < items.size(); ++i )
	{
	    size_t w = line_width(items[i]);
	    if ( w > widest )
		widest = w;
	}
	const size_t cell = widest + 2;
	size_t per_row = (cols + 2) / cell;
	if ( per_row == 0 )
	    per_row = 1;
	const size_t rows = (items.size() + per_row - 1) / per_row;
	for ( size_t r = 0; r < rows; ++r )
	{
	    for ( size_t c = 0; c < per_row; ++c )
	    {
		size_t idx = c * rows + r;
		if ( idx >= items.size() )
		    break;
		out += items[idx];
		if ( (c + 1) * rows + r < items.size() )
		    out.append(cell - line_width(items[idx]), ' ');
	    }
	    out += "\r\n";
	}
	return out;
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_LINE_EDIT_H
