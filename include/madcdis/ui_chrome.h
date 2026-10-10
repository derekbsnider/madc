#ifndef __MADCDIS_UI_CHROME_H
#define __MADCDIS_UI_CHROME_H 1

// madcdis/ui_chrome.h — the chrome a theme colours, by name: the one table
// the grid (tui_model.h) and the theme owner (madc_theme.h) read. Moved out
// of tui_model.h so a theme reads the names without the TUI model (split
// arc 1B.9).
//
// Thread contract: constants and a pure function.

#include <cstddef>
#include <string>

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

} // namespace hub
} // namespace madc

#endif // __MADCDIS_UI_CHROME_H
