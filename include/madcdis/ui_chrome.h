#ifndef __MADCDIS_UI_CHROME_H
#define __MADCDIS_UI_CHROME_H 1

// madcdis/ui_chrome.h — the chrome a theme colours, by name: the one table
// the grid (tui_model.h), the page (web_model.h) and the theme owner
// (madc_theme.h) read. Moved out of tui_model.h so a theme reads the names
// without the TUI model (split arc 1B.9).
//
// Thread contract: constants and pure functions.

#include <cstddef>
#include <map>
#include <string>

#include "libmadc/value.h"
#include "madcdis/ui_style.h"

namespace madc {
namespace hub {

// The chrome a theme colours (facelift S2): the dividers, the editor's
// line-number gutter, the caret line's number, and the caret line itself.
// The composer hands their style specs over as the root's `chrome` hint
// object (keys = these names); tui_chrome_of converts each key ONCE.
enum class tui_chrome : unsigned char
{
    divider = 0, gutter, gutter_current, current_line,
    tab, tab_active, statusbar,		// S3: strips and the status bar
    menubar, menu, menu_selected, menu_hot, shadow,	// S4: the menu bar
    toolbar,				// S5: the toolbar row
    dialog, list_selected, field, button_primary,	// S6: floating windows
    count
};
inline bool tui_chrome_of(const std::string &name, tui_chrome &out)
{
    static const char *const names[] = {
	"divider", "gutter", "gutter_current", "current_line",
	"tab", "tab_active", "statusbar",
	"menubar", "menu", "menu_selected", "menu_hot", "shadow",
	"toolbar", "dialog", "list_selected", "field", "button_primary"
    };
    for ( std::size_t i = 0; i < (std::size_t)tui_chrome::count; ++i )
	if ( name == names[i] )
	{
	    out = (tui_chrome)i;
	    return true;
	}
    return false;
}

// A compose's chrome styles from the root's `chrome` hint object
// ({ "<name>": "<style spec>" }): an unknown name or a spec the parser
// refuses leaves that element's default — the gutter dim; the active tab,
// the bars, the menus, the list selection and the primary button reverse;
// the hot letter and the field underlined; the shadow dim on black; the
// rest normal. The ONE reader the grid and the page take their chrome from.
inline void read_chrome_styles(const madc::value &root_hints,
			       ui_style (&out)[(std::size_t)tui_chrome::count])
{
    for ( std::size_t i = 0; i < (std::size_t)tui_chrome::count; ++i )
	out[i] = ui_style::normal();
    out[(std::size_t)tui_chrome::gutter].flags = ui_style::DIM;
    out[(std::size_t)tui_chrome::tab_active] = ui_style::reverse();
    out[(std::size_t)tui_chrome::statusbar] = ui_style::reverse();
    out[(std::size_t)tui_chrome::menubar] = ui_style::reverse();
    out[(std::size_t)tui_chrome::menu] = ui_style::reverse();
    out[(std::size_t)tui_chrome::menu_hot].flags = ui_style::UNDERLINE;
    out[(std::size_t)tui_chrome::shadow].bg = 1;	// black
    out[(std::size_t)tui_chrome::shadow].flags = ui_style::DIM;
    out[(std::size_t)tui_chrome::list_selected] = ui_style::reverse();
    out[(std::size_t)tui_chrome::field].flags = ui_style::UNDERLINE;
    out[(std::size_t)tui_chrome::button_primary] = ui_style::reverse();
    if ( !root_hints.is_object() )
	return;
    const std::map<std::string, madc::value> &ho = root_hints.as_object();
    std::map<std::string, madc::value>::const_iterator ci = ho.find("chrome");
    if ( ci == ho.end() || !ci->second.is_object() )
	return;
    for ( const auto &kv : ci->second.as_object() )
    {
	tui_chrome k;
	ui_style st;
	if ( kv.second.is_string() && tui_chrome_of(kv.first, k)
	  && ui_style_of(kv.second.as_string(), st) )
	    out[(std::size_t)k] = st;
    }
}

} // namespace hub
} // namespace madc

#endif // __MADCDIS_UI_CHROME_H
