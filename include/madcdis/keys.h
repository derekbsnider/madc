#ifndef __MADCDIS_KEYS_H
#define __MADCDIS_KEYS_H 1

// madcdis/keys.h — the ONE key owner shared by every ui model: the key
// vocabulary (tui_key / tui_keyev), its spelling in both directions
// (tui_key_name / tui_key_from_name — what a tui_event's `key` carries to
// the script and what a bindings table's sequences are written in), the
// bindings table (key sequences -> action names, DATA installed per
// profile) and the chord resolver (key_resolver — the pending-chord state
// machine every model hands its non-printable keys to FIRST). Factored out
// of tui_model.h (2026-09-07, the web-provider engine plan Task 1) so the
// grid model and the DOM model run ONE implementation of chord resolution
// and key spelling; keys resolve natively, never in a page's JavaScript.
//
// Dependency-free: <map> <set> <string> <vector> only. The terminal byte
// -> key adapter (tui_keyparse) stays in tui_model.h — it is terminal
// input, not key semantics.
//
// Thread contract: plain value objects; confined with the model that owns
// them (the C++ standard-library convention — concurrent reads safe,
// distinct objects safe, shared mutation needs synchronization).

#include <map>
#include <set>
#include <string>
#include <vector>

#include "madc/bits/ui_enums"	// ui::key — the shared enum text

namespace madc {
namespace hub {

// ------------------------------------------------------------------- the keys
// The key vocabulary is ONE text shared with the dialect: include/madc/bits/
// ui_enums defines `ui::key` (plain C++11, no includes) for the engine AND
// for <ns_ui>, so script code compares `ev["key_code"] == ui::key::down`
// against the very enumerators this header switches on — no second copy to
// drift. tui_key is that enum under the engine's name.
typedef ::ui::key tui_key;

struct tui_keyev
{
    tui_key kind;
    char    ch;
    tui_keyev() : kind(tui_key::none), ch(0) {}
    explicit tui_keyev(tui_key k, char c = 0) : kind(k), ch(c) {}
};

// The ONE key-spelling owner, both directions (ids/enums inside, names at
// the value boundary): what a tui_event's `key` field carries to the
// script, and what a bindings table's sequences are written in. Control
// chords spell "^"+letter; a printable spells as itself ("space" for the
// blank, which cannot stand alone in a space-separated sequence).
inline std::string tui_key_name(const tui_keyev &k)
{
    switch ( k.kind )
    {
	case tui_key::ch:
	    return k.ch == ' ' ? std::string("space") : std::string(1, k.ch);
	case tui_key::ctrl:	 return std::string("^") + k.ch;
	case tui_key::enter:	 return "enter";
	case tui_key::tab:	 return "tab";
	case tui_key::backspace: return "backspace";
	case tui_key::esc:	 return "esc";
	case tui_key::up:	 return "up";
	case tui_key::down:	 return "down";
	case tui_key::left:	 return "left";
	case tui_key::right:	 return "right";
	case tui_key::home:	 return "home";
	case tui_key::end:	 return "end";
	case tui_key::pgup:	 return "pgup";
	case tui_key::pgdn:	 return "pgdn";
	case tui_key::del:	 return "del";
	case tui_key::ins:	 return "ins";
	default:		 return "";
    }
}

// Spelling -> key. Generous on input (an upper-case letter after "^"
// lowers), canonical on output via tui_key_name. False = not a spelling.
inline bool tui_key_from_name(const std::string &name, tui_keyev &out)
{
    if ( name.empty() )
	return false;
    if ( name.size() == 2 && name[0] == '^' )
    {
	char c = name[1];
	if ( c >= 'A' && c <= 'Z' )
	    c = (char)(c - 'A' + 'a');
	if ( (c < 'a' || c > 'z') && c != '\\' && c != ']' && c != '^'
		&& c != '_' )
	    return false;
	out = tui_keyev(tui_key::ctrl, c);
	return true;
    }
    if ( name.size() == 1 && name[0] >= 0x20 && name[0] <= 0x7e )
    {
	out = tui_keyev(tui_key::ch, name[0]);
	return true;
    }
    static const struct { const char *n; tui_key k; } named[] = {
	{ "space", tui_key::ch }, { "enter", tui_key::enter },
	{ "tab", tui_key::tab }, { "backspace", tui_key::backspace },
	{ "esc", tui_key::esc }, { "up", tui_key::up },
	{ "down", tui_key::down }, { "left", tui_key::left },
	{ "right", tui_key::right }, { "home", tui_key::home },
	{ "end", tui_key::end }, { "pgup", tui_key::pgup },
	{ "pgdn", tui_key::pgdn }, { "del", tui_key::del },
	{ "ins", tui_key::ins },
    };
    for ( size_t i = 0; i < sizeof(named) / sizeof(named[0]); ++i )
	if ( name == named[i].n )
	{
	    out = tui_keyev(named[i].k, named[i].k == tui_key::ch ? ' ' : 0);
	    return true;
	}
    return false;
}

// ------------------------------------------------------------- the bindings
// Key sequences -> action names: DATA, installed per profile (owner
// 2026-08-25 — the JOE/WordStar ^K-chord ruling; a profile swap is a new
// table, never a second hardcoded map). A sequence is space-separated key
// spellings ("^k s"), any length. Validation is loud and whole-table at
// finalize(): a sequence must START with a non-printable key (a printable
// head would swallow typing), and no bound sequence may be a proper
// prefix of another (deterministic resolution — JOE's ^K is only ever a
// prefix). Canonical spellings are the map keys, so lookups and the seq
// reported on events agree byte-for-byte.
class tui_bindings
{
    std::map<std::string, std::string> _actions;
    std::set<std::string> _prefixes;

public:
    // SEQUENCE spelling: tui_key_name with printable LETTERS lowered —
    // chords are letter-case-insensitive (JOE's ^K S == ^K s convention;
    // the shift state of a chord continuation never distinguishes
    // bindings). Key EVENTS keep tui_key_name's exact spelling.
    static std::string seq_spelling(const tui_keyev &k)
    {
	if ( k.kind == tui_key::ch && k.ch >= 'A' && k.ch <= 'Z' )
	    return std::string(1, (char)(k.ch - 'A' + 'a'));
	return tui_key_name(k);
    }

    // CONTINUATION spelling (every key after the head): JOE's other
    // chord convention — the ctrl state of a continuation never
    // distinguishes bindings either (^K ^Z == ^K Z; users keep ctrl
    // held), so a ctrl+letter continuation spells as the bare letter.
    // Ctrl+punctuation (^_ ^^ ^] ^\) has no letter form and stays
    // itself. bind() canonicalization and the model's pending-chord
    // extension both ride this — one owner, both directions.
    static std::string cont_spelling(const tui_keyev &k)
    {
	if ( k.kind == tui_key::ctrl && k.ch >= 'a' && k.ch <= 'z' )
	    return std::string(1, k.ch);
	return seq_spelling(k);
    }

    bool empty() const { return _actions.empty(); }
    void clear() { _actions.clear(); _prefixes.clear(); }

    // Parse + canonicalize one sequence; false (table untouched) on a
    // spelling that is not a key.
    bool bind(const std::string &seq, const std::string &action)
    {
	std::string canon;
	size_t i = 0;
	while ( i < seq.size() )
	{
	    while ( i < seq.size() && seq[i] == ' ' )
		++i;
	    size_t j = i;
	    while ( j < seq.size() && seq[j] != ' ' )
		++j;
	    if ( j == i )
		break;
	    tui_keyev k;
	    if ( !tui_key_from_name(seq.substr(i, j - i), k) )
		return false;
	    if ( canon.empty() )
		canon += seq_spelling(k);
	    else
	    {
		canon += ' ';
		canon += cont_spelling(k);
	    }
	    i = j;
	}
	if ( canon.empty() )
	    return false;
	_actions[canon] = action;
	return true;
    }

    // Whole-table validation + the prefix set. False leaves the table
    // unusable by contract; `err` names the offending sequence.
    bool finalize(std::string &err)
    {
	_prefixes.clear();
	for ( std::map<std::string, std::string>::const_iterator it
		= _actions.begin(); it != _actions.end(); ++it )
	{
	    const std::string &seq = it->first;
	    tui_keyev head;
	    tui_key_from_name(seq.substr(0, seq.find(' ')), head);
	    if ( head.kind == tui_key::ch )
	    {
		err = "printable-headed sequence: " + seq;
		return false;
	    }
	    for ( size_t sp = seq.find(' '); sp != std::string::npos;
		  sp = seq.find(' ', sp + 1) )
	    {
		std::string prefix = seq.substr(0, sp);
		if ( _actions.count(prefix) )
		{
		    err = "sequence shadows a shorter binding: " + seq;
		    return false;
		}
		_prefixes.insert(prefix);
	    }
	}
	return true;
    }

    // The INVERSE: the sequence a client shows beside a command (a menu
    // item's accelerator). An action may be bound to several sequences;
    // the one shown is the fewest-keys, then shortest, then first in
    // spelling order — deterministic, and a single key beats a chord.
    // Empty when the action is unbound.
    std::string seq_for_action(const std::string &action) const
    {
	std::string best;
	size_t best_keys = 0;
	for ( std::map<std::string, std::string>::const_iterator it
		= _actions.begin(); it != _actions.end(); ++it )
	{
	    if ( it->second != action )
		continue;
	    size_t keys = 1;
	    for ( size_t i = 0; i < it->first.size(); ++i )
		if ( it->first[i] == ' ' )
		    ++keys;
	    if ( best.empty() || keys < best_keys
	      || (keys == best_keys && it->first.size() < best.size()) )
	    {
		best = it->first;
		best_keys = keys;
	    }
	}
	return best;
    }

    bool bound(const std::string &canon_seq) const
	{ return _actions.count(canon_seq) != 0; }
    bool prefix(const std::string &canon_seq) const
	{ return _prefixes.count(canon_seq) != 0; }
    const std::string &action_of(const std::string &canon_seq) const
    {
	static const std::string none;
	std::map<std::string, std::string>::const_iterator it
	    = _actions.find(canon_seq);
	return it == _actions.end() ? none : it->second;
    }
};

// ------------------------------------------------------------- the resolver
// One key through the installed bindings — the ONE owner of pending-chord
// state. A model hands EVERY key here first: a pending chord consumes the
// key (extends, completes, misses, or esc cancels it — resize/wake alone
// pass through untouched); with no chord pending a bound head fires, a
// prefix head opens a chord, and everything else is `passthrough` — the
// model's own rules apply (printable runs coalesce there, §7.5; tab/arrows/
// enter navigate there). A printable with no chord pending never touches
// state. With no table installed every key is passthrough, so a model with
// no bindings behaves byte-identically to the pre-bindings adapter.
struct key_step
{
    enum class kind : unsigned char
    {
	passthrough,	// not bound, no chord pending: the model's own rules apply
	pending,	// a chord started or extended: repaint (a status line echoes it)
	cancelled,	// esc cancelled a pending chord: repaint (the echo clears)
	action,		// a bound sequence completed: action_name (empty = a miss), seq
	transparent	// resize/wake mid-chord: pass through, the chord untouched
    };
    kind k;
    std::string action_name, seq;
    key_step() : k(kind::passthrough) {}
};

class key_resolver
{
    tui_bindings _bindings;	// the installed profile
    std::string _pending;	// chord so far (canonical)

public:
    // Install a finalized bindings table (a profile swap is a new table);
    // any chord in flight is abandoned with its profile.
    void set_bindings(const tui_bindings &b)
    {
	_bindings = b;
	_pending.clear();
    }
    const tui_bindings &bindings() const { return _bindings; }
    const std::string &pending() const { return _pending; }

    key_step step(const tui_keyev &k)
    {
	key_step s;
	if ( !_pending.empty() )
	{
	    if ( k.kind == tui_key::resize || k.kind == tui_key::wake )
	    {
		s.k = key_step::kind::transparent;
		return s;
	    }
	    if ( k.kind == tui_key::esc )
	    {
		_pending.clear();
		s.k = key_step::kind::cancelled;
		return s;
	    }
	    std::string candidate = _pending + " "
				  + tui_bindings::cont_spelling(k);
	    if ( _bindings.prefix(candidate) )
	    {
		_pending = candidate;
		s.k = key_step::kind::pending;
		return s;
	    }
	    s.k = key_step::kind::action;
	    s.action_name = _bindings.action_of(candidate);
	    s.seq = candidate;
	    _pending.clear();
	    return s;
	}
	if ( k.kind == tui_key::ch )
	    return s;		// printable runs are the model's (§7.5)
	if ( !_bindings.empty() && k.kind != tui_key::resize
	     && k.kind != tui_key::wake )
	{
	    std::string head = tui_bindings::seq_spelling(k);
	    if ( _bindings.bound(head) )
	    {
		s.k = key_step::kind::action;
		s.action_name = _bindings.action_of(head);
		s.seq = head;
		return s;
	    }
	    if ( _bindings.prefix(head) )
	    {
		_pending = head;
		s.k = key_step::kind::pending;
		return s;
	    }
	}
	return s;
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_KEYS_H
