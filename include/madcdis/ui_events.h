#ifndef __MADCDIS_UI_EVENTS_H
#define __MADCDIS_UI_EVENTS_H 1

// madcdis/ui_events.h — the ONE semantic-event vocabulary every ui model
// emits and every ui frontend's application receives (tui_event_kind,
// tui_event). Moved out of tui_model.h (2026-09-07, the web-provider engine
// plan Task 2) so the grid model, the DOM model and the shared focus owner
// (madcdis/ui_focus.h) speak one event type: a key on the terminal and the
// same key in a window become the SAME tui_event.
//
// Thread contract: plain value objects; confined with the model that emits
// them (the C++ standard-library convention).

#include <string>

#include "madcdis/hub.h"	// name_id
#include "madcdis/keys.h"	// tui_key

namespace madc {
namespace hub {

// ----------------------------------------------------------------- the events
// What the application receives: SEMANTIC units, never raw terminal
// events. The target/model pair owns which keys become navigation
// (consumed here, re-render signalled) and which reach the application.
enum class tui_event_kind : unsigned char
{
    none = 0,
    text,	// a coalesced printable run — one semantic insertion
    key,	// a non-printable key for the application to interpret
    choose,	// enter on the focused choice's selected option
    focus,	// focus or menu selection moved: recompose and repaint
    resize,	// the surface changed size: recompose and repaint
    action,	// a bound key sequence completed (empty name = unbound miss)
    wake	// cooperative background tasks drained: recompose (the
		// application re-checks its pending state, e.g. a spawned
		// parse's completion)
};

struct tui_event
{
    tui_event_kind kind;
    std::string	   text;	// text: the run
    tui_key	   key;		// key: which one (ctrl -> `ch`)
    char	   ch;
    size_t	   option;	// choose: 0-based option index;
				// key: the focused choice's 0-based selection
				// (valid only when choice_focused)
    bool	   choice_focused; // key: a focused choice existed — `option`
				// carries its selection (read-only presentation
				// state, the tui_pending precedent), so the
				// application can act on the focused row for
				// keys the widget does not consume (ins/del)
    name_id	   action;	// choose: the option's first action; 0 = none
    std::string	   action_name;	// action: the bound name ("" = unbound)
    std::string	   seq;		// action: the canonical sequence spelling

    tui_event() : kind(tui_event_kind::none), key(tui_key::none), ch(0),
		  option(0), choice_focused(false), action(0) {}
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_UI_EVENTS_H
