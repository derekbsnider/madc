#ifndef __MADCDIS_UI_FOCUS_H
#define __MADCDIS_UI_FOCUS_H 1

// madcdis/ui_focus.h — the ONE focus/navigation owner shared by every ui
// model: the presentation state a frontend keeps between composes (which
// focusable has focus, each choice's live selection) and the navigation
// rules (tab cycles focus; arrows move a focused choice's selection; enter
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
    focusable() : k(kind::choice), option_count(0) {}
};

class focus_state
{
    std::vector<focusable> _focusables;
    size_t _focus;
    std::map<size_t, size_t> _selection;	// per choice slot

public:
    focus_state() : _focus(0) {}

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
	if ( k.kind == tui_key::tab && _focusables.size() > 1 )
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
	    return true;
	}
	e.kind = tui_event_kind::key;
	e.key = k.kind;
	e.ch = k.ch;
	if ( choice )
	{
	    e.choice_focused = true;
	    e.option = selection_of(_focus);
	}
	return false;
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
	return true;
    }
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_UI_FOCUS_H
