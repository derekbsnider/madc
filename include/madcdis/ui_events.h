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
// The event and pointer-phase vocabularies are the shared enum text
// (include/madc/bits/ui_enums, via keys.h): ui::event_kind and
// ui::pointer_phase under the engine's names. What the application receives
// are SEMANTIC units, never raw terminal events — the target/model pair owns
// which keys become navigation (consumed there, re-render signalled) and
// which reach the application; the enumerators are documented in the
// fragment, once.
typedef ::ui::event_kind tui_event_kind;

// The steps of a pointing-device gesture (tui_event_kind::pointer): a
// press places, a drag extends, a release ends — the shared text's
// ui::pointer_phase.
typedef ::ui::pointer_phase pointer_phase;

// The phase's name at the boundary (the event object's `phase`, the
// page's posted input) — ONE spelling owner, both directions.
inline const char *pointer_phase_name(pointer_phase p)
{
    switch ( p )
    {
	case pointer_phase::down: return "down";
	case pointer_phase::drag: return "drag";
	case pointer_phase::up:   return "up";
    }
    return "down";
}
inline bool pointer_phase_from_name(const std::string &s, pointer_phase &p)
{
    if ( s == "down" ) { p = pointer_phase::down; return true; }
    if ( s == "drag" ) { p = pointer_phase::drag; return true; }
    if ( s == "up" )   { p = pointer_phase::up;   return true; }
    return false;
}

// The UI LEVEL (the shared text's ui::level — ordered by requirement, OWNER
// 2026-09-09) and its name at the boundaries (the capabilities manifest's
// `ui.levels`, ui::open's refusal, a command line's spelling) — ONE spelling
// owner, both directions; the enum is contiguous ui::NONE..ui::GFX3D.
typedef ::ui::level ui_level;
inline const char *ui_level_name(ui_level l)
{
    switch ( l )
    {
	case ::ui::NONE:  return "none";
	case ::ui::LINE:  return "line";
	case ::ui::TUI:   return "tui";
	case ::ui::WEB:   return "web";
	case ::ui::GUI:   return "gui";
	case ::ui::GFX2D: return "gfx2d";
	case ::ui::GFX3D: return "gfx3d";
    }
    return "none";
}
inline bool ui_level_from_name(const std::string &s, ui_level &l)
{
    for ( int i = ::ui::NONE; i <= ::ui::GFX3D; ++i )
	if ( s == ui_level_name((ui_level)i) )
	{
	    l = (ui_level)i;
	    return true;
	}
    return false;
}

struct tui_event
{
    tui_event_kind kind;
    std::string	   text;	// text: the run; snapshot: the page's text
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
    pointer_phase  phase;	// pointer: the gesture step
    long	   offset;	// pointer: BYTE offset in the edit node's text;
				// -1 = the press named no text position (the
				// node itself — a window's header)
    entity_id	   subject;	// pointer: the entity the node projects (0 = none)
    long	   tag;		// pointer: the application tag the node's
				// hints carried (`tag`), echoed as data —
				// a composer's own identity for the node
				// (madcide: the window index); -1 = none

    tui_event() : kind(tui_event_kind::none), key(tui_key::none), ch(0),
		  option(0), choice_focused(false), action(0),
		  phase(pointer_phase::down), offset(0), subject(0), tag(-1) {}
};

} // namespace hub
} // namespace madc

#endif // __MADCDIS_UI_EVENTS_H
