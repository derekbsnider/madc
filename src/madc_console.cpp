// madc_console.cpp — the process's standard streams and the terminal they
// reach. madc::console_attach (include/madc/ns_madc): a Windows GUI image's
// standard streams onto its parent's console (plan
// docs/plans/2026-10-03-chthonia-windows-macos.md §7a). madc::colour_reaches
// (include/madc_console.h): whether a stream shows colour — the one question
// every coloured output asks (split arc Phase 0.5). Its own unit because it
// needs <windows.h>, which ns_madc.cpp cannot include (the `interface` macro
// collides with the module map). Every other platform's program keeps the
// terminal it was started from.
//
// Thread contract: console_attach is called once, from main, before any
// thread starts — it rebinds the process's standard streams (the CRT's and
// Win32's). The colour contract is madc_console.h's.

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <iostream>

#include "madc_console.h"

#if !defined(_WIN32)
#include <unistd.h>
#endif

#if defined(_WIN32)
#include <windows.h>
#include <io.h>
#endif

namespace {

// The buffers the C++ runtime gave the process's standard streams, read
// before any capture or tee swaps a stream's rdbuf (<iostream> above
// constructs the streams first). A stream on one of them writes that
// descriptor; any other buffer is the host's (a capture, a tee, a string).
std::streambuf *const process_stdout_buf = std::cout.rdbuf();
std::streambuf *const process_stderr_buf = std::cerr.rdbuf();
std::streambuf *const process_stdlog_buf = std::clog.rdbuf();

std::atomic<unsigned char> colour_choice(
    (unsigned char)madc::colour_when::automatic);

// The descriptor `os` writes, -1 when it is not a standard stream's.
int standard_descriptor(const std::ostream &os)
{
    std::streambuf *buf = os.rdbuf();
    if ( buf == process_stdout_buf )
	return 1;
    if ( buf == process_stderr_buf || buf == process_stdlog_buf )
	return 2;
    return -1;
}

// gcc's auto: descriptor `fd` is a terminal that shows colour, and NO_COLOR
// (no-color.org) does not ask for none.
bool terminal_shows_colour(int fd)
{
    const char *no_colour = getenv("NO_COLOR");
    if ( no_colour && *no_colour )
	return false;
#if defined(_WIN32)
    // A console in VT mode (Windows 10 1809+): the mode is switched on when
    // it is off — what the terminal target does for its own output; a
    // console that refuses it shows no SGR. Never the legacy console-cell
    // colour API (owner 2026-08-27).
    if ( !_isatty(fd) )
	return false;
    HANDLE h = (HANDLE)_get_osfhandle(fd);
    DWORD mode = 0;
    if ( h == INVALID_HANDLE_VALUE || !GetConsoleMode(h, &mode) )
	return false;
    return (mode & ENABLE_VIRTUAL_TERMINAL_PROCESSING) != 0
	|| SetConsoleMode(h, mode | ENABLE_VIRTUAL_TERMINAL_PROCESSING);
#else
    const char *term = getenv("TERM");
    return isatty(fd) && term && strcmp(term, "dumb") != 0;
#endif
}

} // namespace

namespace madc {

void set_colour_when(colour_when when)
{
    colour_choice.store((unsigned char)when);
}

bool parse_colour_when(const char *text, colour_when &when)
{
    static const struct { const char *name; colour_when code; } names[] = {
	{ "auto", colour_when::automatic },
	{ "always", colour_when::always },
	{ "never", colour_when::never },
    };
    for ( const auto &n : names )
	if ( text && strcmp(text, n.name) == 0 )
	{
	    when = n.code;
	    return true;
	}
    return false;
}

bool colour_reaches(const std::ostream &os)
{
    switch ( (colour_when)colour_choice.load() )
    {
	case colour_when::always:    return true;
	case colour_when::never:     return false;
	case colour_when::automatic: break;
    }
    int fd = standard_descriptor(os);
    return fd >= 0 && terminal_shows_colour(fd);
}

size_t terminal_columns()
{
    const char *env = getenv("COLUMNS");
    const long n = env ? atol(env) : 0;
    return n > 0 ? (size_t)n : 80;
}

} // namespace madc

#if defined(_WIN32)

namespace {

// A standard handle the parent gave the process: a console, a file or a
// pipe. A GUI image started from a console without redirection has none.
bool std_handle_given(DWORD which)
{
    HANDLE h = GetStdHandle(which);
    return h != NULL && h != INVALID_HANDLE_VALUE
	&& GetFileType(h) != FILE_TYPE_UNKNOWN;
}

// One standard stream onto the attached console: the CRT stream reopened on
// the console device, its descriptor at the stream's own number (0, 1, 2 —
// what a write(2)-level caller uses), and the Win32 standard handle set to
// it (the TUI target reads GetStdHandle). The device opens for reading as
// well as writing: GetConsoleMode, SetConsoleMode and
// GetConsoleScreenBufferInfo — the terminal target's console test and grid
// setup — need GENERIC_READ on an output handle, and a "w" open has none.
void bind_console_stream(FILE *stream, int fd, DWORD which, const char *dev,
			 const char *mode)
{
    if ( !freopen(dev, mode, stream) )
	return;
    int got = _fileno(stream);
    if ( got >= 0 && got != fd )
	_dup2(got, fd);
    SetStdHandle(which, (HANDLE)_get_osfhandle(fd));
}

} // namespace

namespace madc {

bool console_attach()
{
    if ( GetConsoleWindow() != NULL )
	return true;			// a console image, or attached already
    // Which streams the parent gave the process — asked before attaching,
    // so a redirected stream stays where the parent sent it.
    bool in = std_handle_given(STD_INPUT_HANDLE);
    bool out = std_handle_given(STD_OUTPUT_HANDLE);
    bool err = std_handle_given(STD_ERROR_HANDLE);
    if ( !AttachConsole(ATTACH_PARENT_PROCESS) )
	return false;
    if ( !in )
	bind_console_stream(stdin, 0, STD_INPUT_HANDLE, "CONIN$", "r");
    if ( !out )
	bind_console_stream(stdout, 1, STD_OUTPUT_HANDLE, "CONOUT$", "w+");
    if ( !err )
	bind_console_stream(stderr, 2, STD_ERROR_HANDLE, "CONOUT$", "w+");
    return true;
}

} // namespace madc

#else

namespace madc {

bool console_attach()
{
    return true;
}

} // namespace madc

#endif
