#ifndef __MADCDIS_UI_FOCUS_H
#define __MADCDIS_UI_FOCUS_H 1

// madcdis/ui_focus.h — the ONE focus/navigation owner shared by every ui
// model: the presentation state a frontend keeps between composes (which
// focusable has focus, each choice's live selection) and the navigation
// rules (tab cycles focus — except on a focused edit field that takes it,
// the `tabkey` hint; arrows move a focused choice's selection; enter
// chooses; any other key rides through to the application with the focused
// choice's selection). Moved out of tui_model::apply_keys (2026-09-07, the
// web-provider engine plan Task 2), unchanged: a DOM frontend needs the same
// tab/arrow/enter contract as the grid, so the rule lives one level down
// and both models consume it.
//
// Focusable identity is DISCOVERY ORDER: a model's compose() calls
// begin_compose(), add() once per choice/edit node it walks, end_compose()
// — stable while the application composes the same tree shape (the Phase-1
// contract). The focus slot and the selections survive a compose; a slot
// past the new list resets to 0.
//
// Dependency-free: keys.h + ui_events.h + <map> <vector>.
//
// Thread contract: plain value objects; confined with the model that owns
// them (the C++ standard-library convention).

#include <map>
#include <string>
#include <vector>

#include "madcdis/hub.h"		// name_id
#include "madcdis/keys.h"		// tui_key, tui_keyev
#include "madcdis/ui_events.h"		// tui_event, tui_event_kind

namespace madc {
namespace hub {

struct focusable
{
    enum class kind : unsigned char { choice, edit };
    kind k;
    size_t option_count;			// choice: how many options
    std::vector<name_id> option_actions;	// choice: first action each
    std::vector<int64_t> option_codes;		// choice: the `code` hint each
						// (0 = none) — the application's
						// own enum for the option
    bool takes_tab;				// edit: the field's own key (the
						// `tabkey` hint — a terminal, a
						// REPL input): focused, tab is
						// the application's, not a cycle
    focusable() : k(kind::choice), option_count(0), takes_tab(false) {}
};

// The menu bar (TUI facelift S4): the root `menu` hint's menus as a grid
// shows them — a row is a command (its action id + code, the application's
// enum) or a separator; `hot` is the letter that picks it (menu_hotkeys),
// 0 = none. The application's data; the focus owner keeps which menu is open
// and which row is lit.
struct menu_row
{
    std::string id;		// the action name (the menu data's command id)
    int64_t code;		// the action's code (0 = none)
    std::string title;
    bool enabled, sep;
    char hot;			// lower-case ASCII, 0 = none
    menu_row() : code(0), enabled(true), sep(false), hot(0) {}
};
struct menu_col
{
    std::string title;
    char hot;
    std::vector<menu_row> rows;
    menu_col() : hot(0) {}
};

// The hotkey letters (Turbo Vision's highlighted letters): each title's
// first ASCII letter or digit no earlier title in the same list took — the
// bar's menus as one list, each dropdown's command rows as another. A title
// with every letter taken gets none (its row is still reachable by arrows).
inline char menu_hotkey_of(const std::string &title, std::string &taken)
{
    for ( size_t i = 0; i < title.size(); ++i )
    {
	unsigned char c = (unsigned char)title[i];
	if ( c >= 'A' && c <= 'Z' )
	    c = (unsigned char)(c - 'A' + 'a');
	if ( !((c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')) )
	    continue;
	if ( taken.find((char)c) != std::string::npos )
	    continue;
	taken += (char)c;
	return (char)c;
    }
    return 0;
}
inline void menu_hotkeys(std::vector<menu_col> &menus)
{
    std::string bar;
    for ( size_t m = 0; m < menus.size(); ++m )
    {
	menus[m].hot = menu_hotkey_of(menus[m].title, bar);
	std::string rows;
	for ( size_t i = 0; i < menus[m].rows.size(); ++i )
	    if ( !menus[m].rows[i].sep )
		menus[m].rows[i].hot = menu_hotkey_of(menus[m].rows[i].title, rows);
    }
}

class focus_state
{
    std::vector<focusable> _focusables;
    size_t _focus;
    std::map<size_t, size_t> _selection;	// per choice slot
    std::vector<menu_col> _menus;		// the bar (S4), from the last compose
    size_t _menu_open;				// the open menu; npos = closed
    size_t _menu_row;				// its lit row
    size_t _menu_anchor;			// where the model drops it (an
						// opaque column; npos = under
						// its bar title)

    static bool menu_row_selectable(const menu_row &r)
	{ return !r.sep && r.enabled; }
    // The open menu's first selectable row at or after `from` going `step`
    // (+1 / -1), wrapping; npos when it has none.
    size_t menu_row_from(size_t from, int step) const
    {
	const std::vector<menu_row> &rows = _menus[_menu_open].rows;
	size_t n = rows.size();
	for ( size_t k = 0; k < n; ++k )
	{
	    size_t i = (from + n + (size_t)(step * (long)k)) % n;
	    if ( menu_row_selectable(rows[i]) )
		return i;
	}
	return std::string::npos;
    }
    void menu_show(size_t m)
    {
	_menu_open = m;
	_menu_anchor = std::string::npos;
	_menu_row = _menus[m].rows.empty() ? 0 : menu_row_from(0, 1);
	if ( _menu_row == std::string::npos )
	    _menu_row = 0;
    }
    // A key's letter for the hotkey tests: a plain or Alt-held printable,
    // lower-cased; 0 for anything else.
    static char menu_letter(const tui_keyev &k)
    {
	if ( k.kind != tui_key::ch || (k.mods & ~key_mod_bits(::ui::key_mod::alt)) != 0 )
	    return 0;
	char c = k.ch;
	if ( c >= 'A' && c <= 'Z' )
	    c = (char)(c - 'A' + 'a');
	return c;
    }

public:
    focus_state() : _focus(0), _menu_open(std::string::npos), _menu_row(0),
		    _menu_anchor(std::string::npos) {}

    // compose() calls these in discovery order (the Phase-1 identity rule):
    // the list is rebuilt, focus and selections are kept.
    void begin_compose() { _focusables.clear(); }
    size_t add(const focusable &f)
    {
	_focusables.push_back(f);
	return _focusables.size() - 1;
    }
    void end_compose()
    {
	if ( _focus >= _focusables.size() )
	    _focus = 0;
    }

    size_t count() const { return _focusables.size(); }
    const std::vector<focusable> &focusables() const { return _focusables; }
    size_t focus() const { return _focus; }
    // The autofocus hint (focus:1 on a choice or edit node): arrows/enter
    // land there without a tab cycle. Set by compose as it discovers the
    // node — the slot it names may be the one about to be added.
    void set_focus(size_t slot) { _focus = slot; }
    bool has_focus_slot() const { return _focus < _focusables.size(); }
    const focusable &focused() const { return _focusables[_focus]; }

    // A choice's live selection: 0 when never moved, clamped to the option
    // count the last compose discovered (a shrunken menu keeps a valid row).
    size_t selection_of(size_t slot) const
    {
	std::map<size_t, size_t>::const_iterator it = _selection.find(slot);
	size_t sel = it == _selection.end() ? 0 : it->second;
	if ( slot < _focusables.size()
	  && _focusables[slot].k == focusable::kind::choice
	  && _focusables[slot].option_count > 0
	  && sel >= _focusables[slot].option_count )
	    sel = _focusables[slot].option_count - 1;
	return sel;
    }

    // The focused slot is a choice with options — arrows and enter are
    // the widget's.
    bool on_choice() const
    {
	return _focus < _focusables.size()
	    && _focusables[_focus].k == focusable::kind::choice
	    && _focusables[_focus].option_count > 0;
    }

    // The focused slot is an edit field that takes tab (a shell's or a
    // REPL's completion key) — tab rides through to the application; the
    // field's own binding hands the keyboard back.
    bool on_tab_field() const
    {
	return _focus < _focusables.size()
	    && _focusables[_focus].k == focusable::kind::edit
	    && _focusables[_focus].takes_tab;
    }

    // The navigation step for a key the key owner passed through: fills
    // `e` and returns true when the key was consumed (focus or a selection
    // moved — a focus event says "repaint"; or a choose fired). False = the
    // application's key: `e` is the key event, carrying the focused
    // choice's live selection when on_choice() (keys the widget does not
    // consume — ins/del — act on the focused row without the selection
    // ever leaving the model; presentation state stays here).
    bool navigate(const tui_keyev &k, tui_event &e)
    {
	const bool choice = on_choice();
	if ( k.kind == tui_key::tab && _focusables.size() > 1 && !on_tab_field() )
	{
	    _focus = (_focus + 1) % _focusables.size();
	    e.kind = tui_event_kind::focus;
	    return true;
	}
	if ( choice && (k.kind == tui_key::left || k.kind == tui_key::up
		     || k.kind == tui_key::right || k.kind == tui_key::down) )
	{
	    size_t n = _focusables[_focus].option_count;
	    size_t sel = selection_of(_focus);
	    if ( k.kind == tui_key::left || k.kind == tui_key::up )
		sel = (sel + n - 1) % n;
	    else
		sel = (sel + 1) % n;
	    _selection[_focus] = sel;
	    e.kind = tui_event_kind::focus;
	    return true;
	}
	if ( choice && k.kind == tui_key::enter )
	{
	    size_t sel = selection_of(_focus);
	    e.kind = tui_event_kind::choose;
	    e.option = sel;
	    e.action = _focusables[_focus].option_actions[sel];
	    e.action_code = sel < _focusables[_focus].option_codes.size()
			    ? _focusables[_focus].option_codes[sel] : 0;
	    return true;
	}
	e.kind = tui_event_kind::key;
	e.key = k.kind;
	e.ch = k.ch;
	e.mods = k.mods;
	if ( choice )
	{
	    e.choice_focused = true;
	    e.option = selection_of(_focus);
	}
	return false;
    }

    // ---- the menu bar (TUI facelift S4) ----------------------------------
    // compose() installs the bar from the root's `menu` hint each time; a
    // bar that shrank past the open menu closes it, the lit row re-settles.
    void set_menus(const std::vector<menu_col> &menus)
    {
	_menus = menus;
	if ( _menu_open != std::string::npos )
	{
	    if ( _menu_open >= _menus.size() )
		_menu_open = std::string::npos;
	    else if ( _menu_row >= _menus[_menu_open].rows.size()
		      || !menu_row_selectable(_menus[_menu_open].rows[_menu_row]) )
	    {
		size_t r = _menus[_menu_open].rows.empty() ? std::string::npos
							   : menu_row_from(0, 1);
		_menu_row = r == std::string::npos ? 0 : r;
	    }
	}
    }
    const std::vector<menu_col> &menus() const { return _menus; }
    bool menu_is_open() const { return _menu_open != std::string::npos; }
    size_t open_menu() const { return _menu_open; }
    size_t menu_lit_row() const { return _menu_row; }
    size_t menu_anchor() const { return _menu_anchor; }

    // The APPLICATION opens a menu by its title (a toolbar button's arrow —
    // ui::menu_open, facelift S5), dropped at `anchor` (the model's column;
    // npos = under its bar title). False = no menu has that title.
    bool menu_open_titled(const std::string &title, size_t anchor)
    {
	for ( size_t m = 0; m < _menus.size(); ++m )
	    if ( _menus[m].title == title )
	    {
		menu_show(m);
		_menu_anchor = anchor;
		return true;
	    }
	return false;
    }

    // A key no binding took (the key owner passed it through) that OPENS the
    // bar: F10 opens the first menu, Alt and a menu's letter that menu. `e`
    // is a focus event (repaint). False = not the bar's key.
    bool menu_opens(const tui_keyev &k, tui_event &e)
    {
	if ( _menus.empty() || menu_is_open() )
	    return false;
	if ( k.kind == tui_key::fkey && k.ch == 10 && k.mods == 0 )
	{
	    menu_show(0);
	    e.kind = tui_event_kind::focus;
	    return true;
	}
	if ( k.kind == tui_key::ch && k.mods == key_mod_bits(::ui::key_mod::alt) )
	{
	    char c = menu_letter(k);
	    for ( size_t m = 0; c && m < _menus.size(); ++m )
		if ( _menus[m].hot == c )
		{
		    menu_show(m);
		    e.kind = tui_event_kind::focus;
		    return true;
		}
	}
	return false;
    }

    // An open bar takes EVERY key (the modal dropdown): left/right move
    // between menus, up/down between selectable rows (a separator or a
    // disabled row is never lit), Enter or a row's letter chooses — the SAME
    // action event a bound chord produces (the row's id + code), the bar
    // closing; Alt and a menu's letter switches menus; Esc or F10 closes.
    // A disabled row is never chosen. Every other key is swallowed (a focus
    // event: repaint).
    void menu_key(const tui_keyev &k, tui_event &e)
    {
	e.kind = tui_event_kind::focus;
	const size_t nm = _menus.size();
	switch ( k.kind )
	{
	    case tui_key::esc:
		_menu_open = std::string::npos;
		return;
	    case tui_key::fkey:
		if ( k.ch == 10 )
		    _menu_open = std::string::npos;
		return;
	    case tui_key::left:
		menu_show((_menu_open + nm - 1) % nm);
		return;
	    case tui_key::right:
		menu_show((_menu_open + 1) % nm);
		return;
	    case tui_key::up:
	    case tui_key::down:
	    {
		if ( _menus[_menu_open].rows.empty() )
		    return;
		int step = k.kind == tui_key::down ? 1 : -1;
		size_t n = _menus[_menu_open].rows.size();
		size_t r = menu_row_from((_menu_row + n + (size_t)step) % n, step);
		if ( r != std::string::npos )
		    _menu_row = r;
		return;
	    }
	    case tui_key::enter:
		menu_choose(_menu_row, e);
		return;
	    default:
		break;
	}
	char c = menu_letter(k);
	if ( !c )
	    return;
	if ( k.mods )				// Alt and a letter: another menu
	{
	    for ( size_t m = 0; m < nm; ++m )
		if ( _menus[m].hot == c )
		{
		    menu_show(m);
		    return;
		}
	}
	const std::vector<menu_row> &rows = _menus[_menu_open].rows;
	for ( size_t i = 0; i < rows.size(); ++i )
	    if ( !rows[i].sep && rows[i].hot == c )
	    {
		menu_choose(i, e);
		return;
	    }
    }
    // Choose row `i` of the open menu: an enabled command row closes the bar
    // and becomes the action event; anything else leaves it open.
    bool menu_choose(size_t i, tui_event &e)
    {
	if ( !menu_is_open() || i >= _menus[_menu_open].rows.size()
	  || !menu_row_selectable(_menus[_menu_open].rows[i]) )
	    return false;
	const menu_row &row = _menus[_menu_open].rows[i];
	e.kind = tui_event_kind::action;
	e.action_name = row.id;
	e.action_code = row.code;
	_menu_open = std::string::npos;
	return true;
    }

    // A pointing gesture PICKED an option (madcide polish P2, the list
    // dialogs): a click on the row `index` of the choice at `slot`, or the
    // dialog's primary button on the live selection (`index` < 0). Focus
    // moves to that choice, the selection to that row, and `e` is the SAME
    // choose event Enter produces (option + its action) — one contract for
    // keyboard and pointer; the widget's selection never leaves this owner.
    // False = not a choice with options (nothing chosen, `e` untouched).
    bool choose(size_t slot, long index, tui_event &e)
    {
	if ( slot >= _focusables.size()
	  || _focusables[slot].k != focusable::kind::choice
	  || _focusables[slot].option_count == 0 )
	    return false;
	_focus = slot;
	size_t n = _focusables[slot].option_count;
	size_t sel = index < 0 ? selection_of(slot)
			       : ((size_t)index < n ? (size_t)index : n - 1);
	_selection[slot] = sel;
	e.kind = tui_event_kind::choose;
	e.option = sel;
	e.action = _focusables[slot].option_actions[sel];
	e.action_code = sel < _focusables[slot].option_codes.size()
			? _focusables[slot].option_codes[sel] : 0;
	return true;
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_UI_FOCUS_H
