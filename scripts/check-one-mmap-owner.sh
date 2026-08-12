#!/bin/bash
# GATE — one file-mapping owner (Windows lane W1).
#
# The rule: mmap-family calls (mmap/munmap/mprotect/madvise), their MAP_/
# PROT_ flag tokens, and the <sys/mman.h> include belong to
# madc::detail::map_file_readonly in src/madc_posix_io.cpp. Consumers (the
# cir_freeze forest image today; the madcdis arena/id_table frozen-segment
# plans tomorrow) call the owner, so the Win32 backend
# (CreateFileMapping/MapViewOfFile) lands in ONE file.
#
# Side benefit the owner locks in: MIR's mir-code-alloc.h redefines
# MAP_FAILED, which forced cir_freeze.cpp into an #undef + (void *)-1
# comparison; the owner file includes no MIR headers and compares against
# the real macro. A new raw mmap in a MIR-including TU would re-grow that
# wart — this gate is why it can't.
#
# Marker note: call-shaped uses + specific flag tokens only, `//`/`/*`
# comment tails stripped. MADC_MAP_FIELD / MADC_MAP_KEY_FIELD (madcdis
# mapper macros) share the MAP_ letters but not the tokens — no match.
set -u
cd "$(dirname "$0")/.."

pat='\b(mmap|munmap|mprotect|madvise)[[:space:]]*\(|\bMAP_(PRIVATE|SHARED|ANONYMOUS|ANON|FIXED|FAILED)\b|\bPROT_(READ|WRITE|EXEC|NONE)\b|#[[:space:]]*include[[:space:]]*<sys/mman\.h>'
hits=$(grep -rnE --include='*.cpp' --include='*.h' "$pat" \
    src/ include/ tests/unit/ \
  | sed -E 's_(//|/\*).*__' \
  | grep -E "$pat" \
  | grep -v '^src/madc_posix_io\.cpp:' )
n=$(printf '%s' "$hits" | grep -c . )

if [ "$n" -ne 0 ]; then
	echo "one-mmap-owner gate: $n raw mapping use(s) outside the owner."
	echo "Use madc::detail::map_file_readonly (src/madc_posix_io.h) — the Win32"
	echo "backend lands behind that owner only."
	printf '%s\n' "$hits" | sed 's/^/  /'
	exit 1
fi

echo "one-mmap-owner gate: GREEN — src/madc_posix_io.cpp is the only mapping site."
exit 0
