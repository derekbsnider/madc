// madc_console.cpp — madc::console_attach (include/madc/ns_madc): a Windows
// GUI image's standard streams onto its parent's console (plan
// docs/plans/2026-10-03-chthonia-windows-macos.md §7a). Its own unit because
// it needs <windows.h>, which ns_madc.cpp cannot include (the `interface`
// macro collides with the module map). Every other platform's program keeps
// the terminal it was started from.
//
// Thread contract: called once, from main, before any thread starts — it
// rebinds the process's standard streams (the CRT's and Win32's).

#include <stdio.h>

#if defined(_WIN32)
#include <windows.h>
#include <io.h>

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
