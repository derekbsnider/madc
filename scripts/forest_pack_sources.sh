#!/bin/bash
# forest_pack_sources.sh — the pack's SOURCE ARM list: every system header a
# platform's packed madc serves to a compile with no headers on disk.
#
#   bash scripts/forest_pack_sources.sh linux <c++-include-root>  > LIST
#   MADC_WIN_PACK_CFLAGS='-D_UCRT ...' \
#   bash scripts/forest_pack_sources.sh windows <cc> <c++> <c++-include-root> \
#        <crt-roots-file>  > LIST
#   bash scripts/forest_pack_sources.sh darwin <c++-include-root> \
#        <c-include-name> <c-include-source>  > LIST
#
# Owner law (2026-10-13): the pack holds ALL system headers, never a subset
# grown one header at a time as tests happen to include them. The list is
# DERIVED, never hand-kept: on Linux it is every header file the C library's,
# the kernel headers' and the C++ standard library's development packages
# install — the package manager's own file lists, asked through the package
# that owns a known header of each (dpkg, else rpm). scripts/forest_pack.sh
# hands it to the freeze (--freeze-sources=), which packs each file's exact
# text as a source-only unit, and to forest_pack_gate.sh (--units-from), which
# fails the pack when any listed header is not a unit.
#
# On Windows (owner 2026-10-13) the C set is the Linux EQUIVALENTS — every
# glibc header name the target's C library (mingw-w64/UCRT) also ships — plus
# the Microsoft C runtime's own headers (<crt-roots-file>,
# scripts/forest_pack_crt_windows.txt), and what those headers include, never
# windows.h and the Windows API; the core Windows headers the frozen list
# already names stay units through the grove. The
# C++ standard library is every file under its include root, as on Linux,
# plus the C headers its standard headers reach.
#
# On macOS the core C names are madc's own embedded darwin prelude (one
# flattened umbrella, scripts/gen_darwin_prelude.sh), and the umbrella keeps
# every #define of Apple's cdefs and availability headers — so the REST of the
# C library is served as raw header text on top of it, exactly as a Mac with
# an SDK already reads <netdb.h>: every file of the staged open-licensed libc
# tree (<c-include-source>), each named as the hosted binary's search list
# spells the SDK (<c-include-name>; a Mac with the SDK reads the real file
# there first). Those lines read `name<TAB>file`. The C++ standard library is
# every file under the pinned libc++ include root.
#
# Every path is spelled as the compiler's search list spells it (search dir +
# include name): a packed madc forms that same string and looks the unit up
# by it. gcc canonicalizes a system header's dependency path by default;
# -fno-canonical-system-headers keeps the search-list spelling.
#
# The compiler's own include directory (stddef.h, stdarg.h, the x86
# intrinsics) is not listed: madc serves its own versions of the freestanding
# headers (include/madc/), and the intrinsics are gcc's builtins.
#
# One path per line, sorted, unique. Exit 1 with a reason when a family
# cannot be resolved — a source arm that silently lacks a family is the
# defect this list exists to close.
#
# See docs/plans/2026-10-13-pack-all-system-headers.md.
set -u
set -o pipefail		# a compiler that fails mid-pipeline fails the list

die() { echo "forest_pack_sources: $*" >&2; exit 1; }

# owner_files <path>: every regular file the package owning <path> installs
# under /usr/include, one per line.
owner_files() {
	local probe="$1" pkg
	if command -v dpkg > /dev/null 2>&1 && pkg=$(dpkg -S "$probe" 2>/dev/null | head -1) \
	   && [ -n "$pkg" ]; then
		dpkg -L "${pkg%%: *}"
	elif command -v rpm > /dev/null 2>&1 && pkg=$(rpm -qf --qf '%{NAME}\n' "$probe" 2>/dev/null | head -1) \
	   && [ -n "$pkg" ]; then
		rpm -ql "$pkg"
	else
		return 1
	fi | while IFS= read -r f; do
		case "$f" in /usr/include/*) [ -f "$f" ] && echo "$f" ;; esac
	done
}

# deps <compiler> [flags...]: the dependencies of the translation unit on
# stdin, one path per line, in the search list's spelling.
deps() {
	"$@" -M -MT x -fno-canonical-system-headers - | tr ' \\' '\n\n' | grep '^/'
}

# public_names: every header name the host's C library package installs,
# relative to its include dir (the multiarch dir folded in) — the Linux set
# another target's equivalents are named from.
public_names() {
	owner_files /usr/include/stdio.h | sed -e 's,^/usr/include/,,' \
		-e 's,^[^/]*-linux-gnu[^/]*/,,' | grep '\.h$'
}

windows() {
	local cc="$1" cxx="$2" root="$3" crt="$4" cdir triple name tu flags out
	[ -d "$root" ] || die "the C++ include root '$root' is not a directory"
	[ -f "$crt" ] || die "no C runtime roots file '$crt'"
	command -v "$cc" > /dev/null 2>&1 || die "no C compiler '$cc'"
	command -v "$cxx" > /dev/null 2>&1 || die "no C++ compiler '$cxx'"
	read -r -a flags <<<"${MADC_WIN_PACK_CFLAGS:-}"
	# The C library's include dir: the first search dir holding <stdio.h>.
	cdir=$("$cc" -E -Wp,-v -x c - < /dev/null 2>&1 | sed -n 's,^ \(/.*\)$,\1,p' \
		| while IFS= read -r d; do [ -f "$d/stdio.h" ] && { echo "$d"; break; }; done)
	[ -n "$cdir" ] || die "$cc names no search dir holding <stdio.h>"
	triple=$("$cxx" -dumpmachine) || die "$cxx -dumpmachine failed"
	out=$(
		find "$root" -type f
		tu=
		while IFS= read -r name; do
			[ -f "$cdir/$name" ] || continue
			tu="$tu#include <$name>"$'\n'
		done < <(public_names | LC_ALL=C sort -u)
		[ -n "$tu" ] || die "no glibc header name has a $cdir equivalent"
		# The C runtime's own headers: every one named must exist.
		while IFS= read -r name; do
			case "$name" in ""|\#*) continue ;; esac
			[ -f "$cdir/$name" ] || die "$crt names $name, absent from $cdir"
			tu="$tu#include <$name>"$'\n'
		done < "$crt"
		printf '%s' "$tu" | deps "$cc" "${flags[@]}" -x c \
			|| die "$cc could not read the C equivalents"
		for name in $(cd "$root" && find . -maxdepth 1 -type f ! -name '*.*' | sed 's,^\./,,'); do
			printf '#include <%s>\n' "$name"
		done | deps "$cxx" "${flags[@]}" -std=gnu++23 -nostdinc++ \
			-isystem "$root" -isystem "$root/$triple" -isystem "$root/backward" -x c++ \
			|| die "$cxx could not read the C++ standard headers"
	) || exit 1
	# Only the two trees: the compiler's own dir is madc's to serve (above).
	printf '%s\n' "$out" | awk -v a="$root/" -v b="$cdir/" \
		'index($0, a) == 1 || index($0, b) == 1' | LC_ALL=C sort -u
}

darwin() {
	local root="$1" cname="$2" csrc="$3" out cout
	[ -d "$root" ] || die "the C++ include root '$root' is not a directory"
	[ -d "$csrc" ] || die "the C include source '$csrc' is not a directory"
	out=$(find "$root" -type f) || exit 1
	[ -n "$out" ] || die "'$root' holds no header"
	# -L: the staged tree links its files into place.
	cout=$(cd "$csrc" && find -L . -type f | sed 's,^\./,,') || exit 1
	[ -n "$cout" ] || die "'$csrc' holds no header"
	{
		printf '%s\n' "$out"
		printf '%s\n' "$cout" | while IFS= read -r rel; do
			printf '%s/%s\t%s/%s\n' "$cname" "$rel" "$csrc" "$rel"
		done
	} | LC_ALL=C sort -u
}

linux() {
	local cxx="$1" probe out
	[ -d "$cxx" ] || die "the C++ include root '$cxx' is not a directory"
	out=$(
		for probe in /usr/include/stdio.h /usr/include/linux/limits.h "$cxx/vector"; do
			[ -e "$probe" ] || die "no $probe on this host"
			owner_files "$probe" || die "no package owns $probe (neither dpkg nor rpm answered)"
		done
	) || exit 1
	[ -n "$out" ] || die "the package lists named no header"
	printf '%s\n' "$out" | LC_ALL=C sort -u
}

USAGE="usage: $0 linux <c++-include-root> | windows <cc> <c++> <c++-include-root> <crt-roots-file> | darwin <c++-include-root> <c-include-name> <c-include-source>"
case "${1:-}" in
linux)	[ $# -eq 2 ] || die "$USAGE"; linux "$2" ;;
windows) [ $# -eq 5 ] || die "$USAGE"; windows "$2" "$3" "$4" "$5" ;;
darwin)	[ $# -eq 4 ] || die "$USAGE"; darwin "$2" "$3" "$4" ;;
*)	die "$USAGE" ;;
esac
