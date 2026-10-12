#ifndef __MADCDIS_UI_SGR_H
#define __MADCDIS_UI_SGR_H 1

// madcdis/ui_sgr.h — a terminal's LAST step from a style (split arc 1B.9):
// the style-to-SGR bytes and the colour depth the terminal shows. THE
// owner: the TUI's paint (src/ui_term.cpp) and the REPL prompt's marks both
// write through it. Defined in src/ui_term.cpp.
//
// Thread contract: pure functions over their arguments (the depth reads the
// environment).

#include <string>

#include "madcdis/ui_style.h"

namespace madc {
namespace hub {

// The bytes that take a terminal from style `from` to style `to` at
// `depth`, appended to `out` (VT-102 ANSI, JOE parity): any change to
// normal is \x1b[0m; any other change resets, then sets `to`'s whole list
// (attributes 1/2/3/4/5/7, fg 30+c, bg 40+c, an exact colour at the depth
// instead); pure inverse from normal is \x1b[7m.
void sgr(std::string &out, ui_style from, ui_style to, ui_colour_depth depth);

// The colour depth this terminal shows: COLORTERM=truecolor|24bit or Windows
// Terminal (WT_SESSION) is 24-bit, a TERM naming 256color the xterm palette,
// anything else the 8/16-colour index.
ui_colour_depth terminal_colour_depth();

} // namespace hub
} // namespace madc

#endif // __MADCDIS_UI_SGR_H
