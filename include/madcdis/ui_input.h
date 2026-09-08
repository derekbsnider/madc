#ifndef __MADCDIS_UI_INPUT_H
#define __MADCDIS_UI_INPUT_H 1

// madcdis/ui_input.h — the ONE keys → semantic-events adapter every ui
// model runs: bound sequences resolve FIRST through the key owner (a
// pending chord consumes every key until it completes, misses, or esc
// cancels it — resize/wake alone pass through); then printable runs
// coalesce into ONE text event (design §7.5 — five key events never become
// five domain transactions); resize/wake report themselves; every other
// key goes to the focus owner (tab cycles; arrows navigate a focused
// choice; enter chooses; the rest reach the application as a key event
// carrying the focused choice's selection). Moved out of tui_model::
// apply_keys (2026-09-07, the web-provider engine plan Task 3): the grid
// model runs it over the terminal's keys, the DOM model over the page's —
// the same keys become the same events by construction, not by a second
// copy of the loop.
//
// Dependency-free: keys.h + ui_focus.h + <string> <vector>.
//
// Thread contract: a pure function over the caller's owners — confined
// with the model that holds them (the C++ standard-library convention).

#include <string>
#include <vector>

#include "madcdis/keys.h"
#include "madcdis/ui_events.h"
#include "madcdis/ui_focus.h"

namespace madc {
namespace hub {

inline std::vector<tui_event> ui_apply_keys(key_resolver &keys_owner,
					    focus_state &focus_owner,
					    const std::vector<tui_keyev> &keys)
{
    std::vector<tui_event> out;
    std::string run;
    for ( size_t i = 0; i < keys.size(); ++i )
    {
	const tui_keyev &k = keys[i];
	// The key owner FIRST: a pending chord consumes the key; a bound
	// head fires or opens a chord; everything else is passthrough.
	key_step step = keys_owner.step(k);
	if ( step.k == key_step::kind::passthrough && k.kind == tui_key::ch )
	{
	    run += k.ch;
	    continue;
	}
	if ( !run.empty() )
	{
	    tui_event e;
	    e.kind = tui_event_kind::text;
	    e.text = run;
	    out.push_back(e);
	    run.clear();
	}
	if ( step.k == key_step::kind::pending
	     || step.k == key_step::kind::cancelled )
	{
	    // A chord STARTED, extended or cancelled: the pending prefix is
	    // visible state — a status line echoing it (JOE's %k) needs a
	    // repaint event to show it live, or to clear it.
	    tui_event e;
	    e.kind = tui_event_kind::focus;
	    out.push_back(e);
	    continue;
	}
	if ( step.k == key_step::kind::action )
	{
	    tui_event e;
	    e.kind = tui_event_kind::action;
	    e.action_name = step.action_name;
	    e.seq = step.seq;
	    out.push_back(e);
	    continue;
	}
	if ( step.k == key_step::kind::transparent )
	{
	    // A resize or wake mid-chord passes through without disturbing
	    // the pending prefix.
	    tui_event e;
	    e.kind = k.kind == tui_key::resize ? tui_event_kind::resize
					      : tui_event_kind::wake;
	    out.push_back(e);
	    continue;
	}
	if ( k.kind == tui_key::resize )
	{
	    tui_event e;
	    e.kind = tui_event_kind::resize;
	    out.push_back(e);
	    continue;
	}
	if ( k.kind == tui_key::wake )
	{
	    tui_event e;
	    e.kind = tui_event_kind::wake;
	    out.push_back(e);
	    continue;
	}
	// The focus owner: tab cycles, arrows move a focused choice's
	// selection, enter chooses; any other key is the application's
	// and rides through with the focused choice's live selection.
	tui_event e;
	focus_owner.navigate(k, e);
	out.push_back(e);
    }
    if ( !run.empty() )
    {
	tui_event e;
	e.kind = tui_event_kind::text;
	e.text = run;
	out.push_back(e);
    }
    return out;
}

} // namespace hub
} // namespace madc

#endif // __MADCDIS_UI_INPUT_H
