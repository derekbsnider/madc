#ifndef __LIBMADC_SYSINFO_H
#define __LIBMADC_SYSINFO_H 1

// madc::sys — the system object (Python `sys` convention; task #91,
// docs/plans/2026-07-22-madc-sys-object.md). ONE host-side instance
// serves every lane: scripts declare it via the embedded <ns_madc>
// header (include it like Python's `import sys`) and resolve the
// Itanium symbol _ZN4madc3sysE mangled-direct (cpp-first-api.md);
// JIT and native mains populate argv/path through the injected
// __madc_sys_init(argc, argv) call (madc_mir_backend.cpp).
//
// LAYOUT CONTRACT: the script-side declaration in include/madc/ns_madc
// mirrors this struct field-for-field (madc::value members lower to
// _Alignas(alignof(madc::value)) long[] buffers of the same size).
// Append-only: new members go at the END, in BOTH files.

#include "value.h"

namespace madc {

struct SysInfo
{
    value argv;                  // script-mutable; [0] = script path
    value path;                  // script-mutable; future import/eval search seed
    const char *const platform;  // immutable fact: "linux" / "darwin" / ...
    const char *const version;   // immutable fact: MADC_VERSION_STR
    const char *const hostname;  // immutable fact: eager gethostname()
};

extern SysInfo sys;

// (Re)populate sys.argv / sys.path from the program arguments —
// idempotent (clears first). The facts initialize at load time.
void sys_populate_args(int argc, char **argv);

} // namespace madc

#endif // __LIBMADC_SYSINFO_H
