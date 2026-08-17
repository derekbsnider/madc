#!/bin/bash
# GATE — one mingw ANSI-stdio interposer map (Windows lane W1).
#
# The rule: on a mingw host built with __USE_MINGW_ANSI_STDIO, the
# printf/scanf family must resolve to the host's statically bound
# __mingw_* implementations (UCRT doesn't export the names; PE ld
# excludes the mingw runtime archives from --export-all-symbols), and
# the name->address list for that lives in EXACTLY ONE place:
# third_party/mir/mir-mingw-stdio.h. Both default-scope resolvers
# (c2m's import_resolver, madc's madcdl_sym_default) consult it via
# mir_mingw_ansi_stdio_lookup(). A second hand-rolled __mingw_ table is
# the divergence bug this gate exists to prevent.
#
# Marker: any __mingw_ token in code, `//`- and `/*`-comment tails
# stripped first so prose mentions don't count. The owner header is
# excluded. (`__MINGW32__` does not match — different case/spelling.)
set -u
cd "$(dirname "$0")/.."

pat='__mingw_'
hits=$(grep -rnE --include='*.c' --include='*.cpp' --include='*.h' --include='*.S' "$pat" \
    src/ include/ tests/ third_party/mir/ \
  | sed -E 's_(//|/\*).*__' \
  | grep -E "$pat" \
  | grep -v '^third_party/mir/mir-mingw-stdio\.h:' )
n=$(printf '%s' "$hits" | grep -c . )

if [ "$n" -ne 0 ]; then
	echo "one-mingw-stdio-map gate: $n __mingw_ reference(s) outside the owner map."
	echo "The ANSI-stdio interposer name->address list lives ONLY in"
	echo "third_party/mir/mir-mingw-stdio.h — resolvers call mir_mingw_ansi_stdio_lookup()."
	printf '%s\n' "$hits" | sed 's/^/  /'
	exit 1
fi

echo "one-mingw-stdio-map gate: GREEN — mir-mingw-stdio.h is the only __mingw_ site."
exit 0
