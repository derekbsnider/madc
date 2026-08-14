#!/bin/bash
# GATE — one host dynamic-library seam (Windows lane W1).
#
# The rule: every dlopen/dlsym/dlclose/dlerror-family use in the madc host
# goes through the madc_dl seam (include/madc_dl.h, src/madc_dl.cpp). Call
# sites name the concept (madcdl_open_global / madcdl_sym_default / ...);
# only the seam touches <dlfcn.h> for these calls. A raw call added anywhere
# else is a site the Win32 backend cannot reach — the exact scattered-#ifdef
# failure mode the seam exists to prevent.
#
# Marker note: matches CALL-shaped uses (`dlopen(`, `dlsym(`, `dladdr(`,
# ...), any RTLD_ flag token, the Dl_info type, and the <dlfcn.h> include
# itself, with `//`- and `/*`-comment tails stripped first so prose mentions
# of the POSIX names don't count. `madc_dlopen` (the script-facing builtin
# thunks in parser.cpp — no word boundary before `dl`), `madcdl_*` and
# `MadcDlInfo` (the seam itself) do not match.
#
# EXCLUDED, deliberately:
#   src/madc_dl.cpp                  — the seam's own POSIX backend
#   tests/unit/test_native_shared.cpp — simulates a THIRD-PARTY C host
#     dlopen'ing a madc-emitted .so (RTLD_DEEPBIND isolation); the test's
#     whole point is the raw external-consumer view, not the madc host.
#   include/madc/**                  — the TARGET's own surface, plus
#   src/embedded_headers.cpp           its GENERATED mirror (every byte of
#     that file is produced verbatim from include/madc/** by
#     scripts/gen_embedded_headers.sh; nothing is hand-written there).
#     This is a SCOPE statement, not a hole: the gate's rule is about the
#     HOST's call sites, and these two paths contain no host code by
#     construction. include/madc/posix/dlfcn.h is the live case — mingw-w64
#     ships no <dlfcn.h>, so that header declares the names for COMPILED
#     USER PROGRAMS and src/rt/rt_posix_dl.c implements them (L3, strict
#     C11 over Win32). It deliberately does NOT route through madcdl_*:
#     that seam is madc's own C++, and an emitted-C program links
#     libmadc_rt.a alone, so the forward would be an unresolved symbol
#     (docs/plans/2026-08-13-posix-target-surface.md §3).
#     Verify after changing either line: drop a raw `dlopen(` into a host
#     .cpp and confirm this gate still exits 1.
set -u
cd "$(dirname "$0")/.."

pat='\bdl(open|sym|close|error|addr)[[:space:]]*\(|\bRTLD_[A-Z]+|\bDl_info\b|#[[:space:]]*include[[:space:]]*<dlfcn\.h>'
hits=$(grep -rnE --include='*.cpp' --include='*.h' "$pat" \
    src/ include/ tests/unit/ \
  | sed -E 's_(//|/\*).*__' \
  | grep -E "$pat" \
  | grep -v '^src/madc_dl\.cpp:' \
  | grep -v '^tests/unit/test_native_shared\.cpp:' \
  | grep -v '^include/madc/' \
  | grep -v '^src/embedded_headers\.cpp:' )
n=$(printf '%s' "$hits" | grep -c . )

if [ "$n" -ne 0 ]; then
	echo "one-dl-owner gate: $n raw dl-family call(s) outside the madc_dl seam."
	echo "Use madcdl_open_global/open_local/open_self/probe_loaded/sym/sym_default/close/error/addr"
	echo "(include/madc_dl.h) instead — the Win32 backend lands behind that seam only."
	printf '%s\n' "$hits" | sed 's/^/  /'
	exit 1
fi

echo "one-dl-owner gate: GREEN — src/madc_dl.cpp is the only dl-family call site."
exit 0
