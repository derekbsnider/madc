#ifndef __MADC_THEME_H
#define __MADC_THEME_H 1

// madc_theme.h — a colour THEME: a style per highlight class and per piece
// of chrome (split arc 1B.9, design
// docs/plans/2026-10-10-prompt-colour-design.md). The one owner of madc's
// built-in default and of the `.theme` text madcide's profiles are written
// in; the REPL prompt colours through it (madc.ini's `theme` key names
// another), and madcide's own reader moves onto it.
//
// Thread contract: a theme is a plain value, immutable once read; the
// default is a function's constant (C++11 static initialization).

#include <string>

#include "highlight_class.h"
#include "madcdis/ui_chrome.h"
#include "madcdis/ui_style.h"

namespace madc {

struct theme
{
    // A style per highlight class: normal (plain) unless the theme names one.
    hub::ui_style syntax[highlight_class_count];
    // A style per piece of chrome, where the theme names one; a piece it
    // leaves unnamed keeps the renderer's own default.
    hub::ui_style chrome[(std::size_t)hub::tui_chrome::count];
    bool chrome_named[(std::size_t)hub::tui_chrome::count];

    theme() : chrome_named() {}
    const hub::ui_style &of(HighlightClass c) const { return syntax[(std::size_t)c]; }
};

// madc's one built-in theme: madcide's default.theme, JOE 4.6 parity —
// keyword, control, type and typename bold; string and number cyan; comment
// green; directive blue; include and escape bold cyan; the rest plain; no
// chrome named.
const theme &theme_default();

// The theme a `.theme` text spells: one `name spec` per line — a
// highlight_class_name or a chrome name (tui_chrome_of), then ui_style_of's
// spec words — `#` comments and blank lines skipped, `@scope` lines skipped
// (an IDE's). A class the text leaves unnamed takes its parent's style:
// control and directive keyword's, typename type's, namespace typename's,
// include and escape string's. A theme is the whole scheme, never laid over
// another; a text with no lines colours nothing. False, with `err` naming
// the line, for a name neither vocabulary has or a spec ui_style_of cannot
// read; `out` is then unchanged.
bool theme_read(const std::string &text, theme &out, std::string &err);

} // namespace madc

#endif // __MADC_THEME_H
