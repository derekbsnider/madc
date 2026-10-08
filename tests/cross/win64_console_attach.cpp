// win64_console_attach.cpp — madc::console_attach (src/madc_console.cpp) in a
// Windows GUI image (-mwindows), linked against that source and run from cmd
// on a real Windows console by scripts/win_console_lane.sh.
//
//   win64_console_attach.exe none|given LOG
//
// `none` clears the three standard handles first: cmd in a console window
// starts a GUI program with none, so console_attach reopens each stream on
// the console device. `given` keeps what the parent passed. Then the
// terminal target's console test (src/ui_term.cpp: GetConsoleMode on the
// standard input and output handles; stderr beside them) is appended to LOG
// as one line:
//
//   none attach=1 in=1 out=1 err=1
//
// Oracle: the same program built with gcc's -mwindows and a hand-written
// AttachConsole + freopen("CONOUT$", "w+") answers 1 for every stream; with
// "w" (madc before this fixture) stdout answers 0 — GetConsoleMode needs
// GENERIC_READ on the handle (ERROR_ACCESS_DENIED).
#include <windows.h>
#include <stdio.h>
#include <string.h>

namespace madc {
bool console_attach();
}

static int console_mode_ok(DWORD which)
{
    DWORD mode;
    return GetConsoleMode(GetStdHandle(which), &mode) ? 1 : 0;
}

int main(int argc, char **argv)
{
    if ( argc < 3 )
	return 2;
    bool none = strcmp(argv[1], "none") == 0;
    if ( none )
    {
	SetStdHandle(STD_INPUT_HANDLE, NULL);
	SetStdHandle(STD_OUTPUT_HANDLE, NULL);
	SetStdHandle(STD_ERROR_HANDLE, NULL);
    }
    int attached = madc::console_attach() ? 1 : 0;
    int in = console_mode_ok(STD_INPUT_HANDLE);
    int out = console_mode_ok(STD_OUTPUT_HANDLE);
    int err = console_mode_ok(STD_ERROR_HANDLE);
    FILE *log = fopen(argv[2], "a");
    if ( !log )
	return 3;
    fprintf(log, "%s attach=%d in=%d out=%d err=%d\n", argv[1], attached, in, out, err);
    fclose(log);
    return 0;
}
