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
# Marker note: matches CALL-shaped uses (`dlopen(`, `dlsym(`, ...) and any
# RTLD_ flag token, with `//`- and `/*`-comment tails stripped first so prose
# mentions of the POSIX names don't count. `madc_dlopen` (the script-facing
# builtin thunks in parser.cpp — no word boundary before `dl`) and
# `madcdl_*` (the seam itself) do not match.
#
# NOT yet covered (extends when the dladdr slice of W1 lands): dladdr /
# Dl_info and the <dlfcn.h> includes still needed for them.
#
# EXCLUDED, deliberately:
#   src/madc_dl.cpp                  — the seam's own POSIX backend
#   tests/unit/test_native_shared.cpp — simulates a THIRD-PARTY C host
#     dlopen'ing a madc-emitted .so (RTLD_DEEPBIND isolation); the test's
#     whole point is the raw external-consumer view, not the madc host.
set -u
cd "$(dirname "$0")/.."

hits=$(grep -rnE --include='*.cpp' --include='*.h' \
    '\bdl(open|sym|close|error)[[:space:]]*\(|\bRTLD_[A-Z]+' \
    src/ include/ tests/unit/ \
  | sed -E 's_(//|/\*).*__' \
  | grep -E '\bdl(open|sym|close|error)[[:space:]]*\(|\bRTLD_[A-Z]+' \
  | grep -v '^src/madc_dl\.cpp:' \
  | grep -v '^tests/unit/test_native_shared\.cpp:' )
n=$(printf '%s' "$hits" | grep -c . )

if [ "$n" -ne 0 ]; then
	echo "one-dl-owner gate: $n raw dl-family call(s) outside the madc_dl seam."
	echo "Use madcdl_open_global/open_local/open_self/probe_loaded/sym/sym_default/close/error"
	echo "(include/madc_dl.h) instead — the Win32 backend lands behind that seam only."
	printf '%s\n' "$hits" | sed 's/^/  /'
	exit 1
fi

echo "one-dl-owner gate: GREEN — src/madc_dl.cpp is the only dl-family call site."
exit 0
