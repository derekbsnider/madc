#ifndef __MADC_CRASH_H
#define __MADC_CRASH_H 1

#include <cstddef>

// The crash surface: fault-reporter installation plus the raw fd-2 writers
// the resource-guard handlers share. It lives in its own TU because the
// Win32 arm needs <windows.h>, whose winnt.h declares a TokenType
// enumerator (TOKEN_INFORMATION_CLASS) that collides with madc's core
// TokenType — windows.h and tokens.h can never meet in one TU.

// Robust unbuffered write to stderr (async-signal-safe on POSIX).
void madc_crash_write(const char *data, std::size_t size);

// snprintf-result-shaped variant: clamps a formatted length to capacity.
void madc_crash_write_formatted(const char *data, int length,
				std::size_t capacity);

// Install the platform fault reporter: POSIX = sigaction handlers on an
// alternate stack; Win32 = SetUnhandledExceptionFilter. Prints the signal /
// exception name + fault address + a JIT-aware backtrace, then lets the
// default consequence happen (core dump / WER / the real exit status).
void madc_install_crash_handler(void);

#endif // __MADC_CRASH_H
