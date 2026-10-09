/* conpty_host — a Windows pseudo-console (ConPTY, what Windows Terminal hosts
 * a console program in) around one command, with its terminal bytes on this
 * process's stdin / stdout. scripts/tui_win_golden.py reaches the owner's box
 * through WSL, whose interop hands a Win32 program PIPES, never a console;
 * this host turns those pipes back into the terminal a user's window gives.
 *
 *   conpty_host.exe COLS ROWS PROGRAM [ARG...]
 *
 * Exits with the program's exit status. Cross-built on the container:
 *   x86_64-w64-mingw32-gcc -O2 -o tmp/conpty_host.exe scripts/conpty_host.c -lshell32
 * Test tooling only — madc ships no part of it.
 */
#define _WIN32_WINNT 0x0A00
#include <windows.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* The container's mingw-w64 headers predate ConPTY: the three entry points
 * and the attribute are declared here and resolved from kernel32 at run time. */
typedef VOID *HPCON;
#ifndef PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE
#define PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE 0x00020016
#endif
typedef HRESULT (WINAPI *create_pc_fn)(COORD, HANDLE, HANDLE, DWORD, HPCON *);
typedef VOID (WINAPI *close_pc_fn)(HPCON);

static HANDLE pc_in;	/* the pseudo-console's input (we write) */
static HANDLE pc_out;	/* its output (we read) */

static DWORD WINAPI pump_in(LPVOID unused)
{
	HANDLE src = GetStdHandle(STD_INPUT_HANDLE);
	char buf[4096];
	DWORD n, w;
	(void)unused;
	while (ReadFile(src, buf, sizeof buf, &n, NULL) && n > 0)
		if (!WriteFile(pc_in, buf, n, &w, NULL))
			break;
	return 0;
}

static DWORD WINAPI pump_out(LPVOID unused)
{
	HANDLE dst = GetStdHandle(STD_OUTPUT_HANDLE);
	char buf[8192];
	DWORD n, w;
	(void)unused;
	while (ReadFile(pc_out, buf, sizeof buf, &n, NULL) && n > 0)
		if (!WriteFile(dst, buf, n, &w, NULL))
			break;
	return 0;
}

/* The command line: each argument quoted when it carries a space or quote
 * (the arguments this host is given are plain relative paths and words). */
static wchar_t *command_line(int argc, wchar_t **argv)
{
	size_t len = 1;
	int i;
	for (i = 0; i < argc; i++)
		len += 2 * wcslen(argv[i]) + 3;
	wchar_t *cmd = calloc(len, sizeof *cmd);
	for (i = 0; i < argc; i++) {
		int quote = wcspbrk(argv[i], L" \t\"") != NULL;
		if (i)
			wcscat(cmd, L" ");
		if (quote)
			wcscat(cmd, L"\"");
		wcscat(cmd, argv[i]);
		if (quote)
			wcscat(cmd, L"\"");
	}
	return cmd;
}

int main(void)
{
	int argc;
	wchar_t **argv = CommandLineToArgvW(GetCommandLineW(), &argc);
	if (!argv || argc < 4) {
		fprintf(stderr, "usage: conpty_host COLS ROWS PROGRAM [ARG...]\n");
		return 2;
	}
	COORD size = { (SHORT)_wtoi(argv[1]), (SHORT)_wtoi(argv[2]) };

	HANDLE in_read, out_write;
	if (!CreatePipe(&in_read, &pc_in, NULL, 0) || !CreatePipe(&pc_out, &out_write, NULL, 0)) {
		fprintf(stderr, "conpty_host: CreatePipe failed (%lu)\n", GetLastError());
		return 2;
	}
	HMODULE k32 = GetModuleHandleW(L"kernel32.dll");
	create_pc_fn CreatePseudoConsole = (create_pc_fn)(void (*)(void))GetProcAddress(k32, "CreatePseudoConsole");
	close_pc_fn ClosePseudoConsole = (close_pc_fn)(void (*)(void))GetProcAddress(k32, "ClosePseudoConsole");
	if (!CreatePseudoConsole || !ClosePseudoConsole) {
		fprintf(stderr, "conpty_host: this Windows has no ConPTY\n");
		return 2;
	}
	HPCON hpc;
	HRESULT hr = CreatePseudoConsole(size, in_read, out_write, 0, &hpc);
	if (FAILED(hr)) {
		fprintf(stderr, "conpty_host: CreatePseudoConsole failed (0x%08lx)\n", (unsigned long)hr);
		return 2;
	}
	/* The pseudo-console holds its own copies. */
	CloseHandle(in_read);
	CloseHandle(out_write);

	STARTUPINFOEXW si;
	SIZE_T attr_size = 0;
	memset(&si, 0, sizeof si);
	si.StartupInfo.cb = sizeof si;
	/* This host's own stdio are pipes; without this the child inherits them
	 * instead of the pseudo-console's handles. */
	si.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	InitializeProcThreadAttributeList(NULL, 1, 0, &attr_size);
	si.lpAttributeList = malloc(attr_size);
	if (!InitializeProcThreadAttributeList(si.lpAttributeList, 1, 0, &attr_size)
	    || !UpdateProcThreadAttribute(si.lpAttributeList, 0, PROC_THREAD_ATTRIBUTE_PSEUDOCONSOLE,
					  hpc, sizeof hpc, NULL, NULL)) {
		fprintf(stderr, "conpty_host: attribute list failed (%lu)\n", GetLastError());
		return 2;
	}

	HANDLE out_thread = CreateThread(NULL, 0, pump_out, NULL, 0, NULL);
	CreateThread(NULL, 0, pump_in, NULL, 0, NULL);

	PROCESS_INFORMATION pi;
	wchar_t *cmd = command_line(argc - 3, argv + 3);
	if (!CreateProcessW(NULL, cmd, NULL, NULL, FALSE, EXTENDED_STARTUPINFO_PRESENT,
			    NULL, NULL, &si.StartupInfo, &pi)) {
		fprintf(stderr, "conpty_host: CreateProcess failed (%lu)\n", GetLastError());
		ClosePseudoConsole(hpc);
		return 2;
	}
	WaitForSingleObject(pi.hProcess, INFINITE);
	DWORD rc = 0;
	GetExitCodeProcess(pi.hProcess, &rc);
	/* Closing the pseudo-console ends its output; the pump drains it first
	 * (ClosePseudoConsole waits on an undrained pipe). */
	ClosePseudoConsole(hpc);
	WaitForSingleObject(out_thread, 2000);
	return (int)rc;
}
