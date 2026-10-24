#ifndef __MADC_CONSOLE_H
#define __MADC_CONSOLE_H 1

// madc_console.h — the process's standard streams and the terminal they
// reach (src/madc_console.cpp): whether a stream shows colour.
//
// THREAD-SAFETY CONTRACT: set_colour_when is an atomic store (the CLI sets
// it once, before any output); colour_reaches reads that, the environment
// and the descriptor's terminal state, and is safe to call concurrently.

#include <cstddef>
#include <iosfwd>

namespace madc {

// When output is coloured — gcc's -fdiagnostics-color=WHEN.
enum class colour_when : unsigned char { automatic, always, never };

void set_colour_when(colour_when when);
// The option's text (`auto`, `always`, `never`) as its code; false: none.
bool parse_colour_when(const char *text, colour_when &when);

// Colour reaches `os`: always under `always`, never under `never`; else
// (gcc's auto) `os` writes the process's own stdout or stderr buffer, that
// descriptor is a terminal that shows colour (TERM set and not "dumb"; on
// Windows a console in VT mode), and NO_COLOR is unset or empty. A string
// stream, a capture or a tee never gets colour on its own.
bool colour_reaches(const std::ostream &os);

// `seq` (an SGR sequence) when `colour`, else nothing.
inline const char *sgr(bool colour, const char *seq)
{
    return colour ? seq : "";
}

// The width laid-out text wraps to (a diagnostic's source echo, the help):
// $COLUMNS when it holds a positive number, else 80. The one rule; the TUI's
// full-screen grid asks the terminal itself (ui_term.cpp).
size_t terminal_columns();

} // namespace madc

#endif
