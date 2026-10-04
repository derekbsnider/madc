#!/bin/bash
# check-madcgit-exports.sh — the madcgit module exports no libgit2 symbol.
#
# Every target links libgit2 statically into the module (src/madcgit.mk), and
# madc loads modules RTLD_GLOBAL (src/madc_dl.cpp): an exported git_* would
# enter the process's global scope beside any libgit2 a program loads itself,
# and a script's dlsym fallback would bind to the module's minimal read-only
# copy. The module's own C API is madcgit_* (include/madc/madcgit.h).
#
# Checks every module image present in the tree (a target's build puts its
# image there; an absent one is named and skipped):
#   lib/libmadcgit.so                          ELF: nm -D
#   lib/libmadcgit.dylib, lib/madcgit/*-macos/libmadcgit.dylib
#                                              Mach-O: llvm-nm -g
#   bin/madcgit.dll                            PE: objdump -p, the export table
# An image present with no tool to read it FAILS: a gate that cannot look
# must not pass.
#
# Negative control: a synthetic shared object for this host exporting git_x
# must FAIL; the same with git_x hidden must PASS — else the reader or the
# pattern is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

FORBIDDEN='^(git|giterr)_'

first_tool() {
	local t
	for t in "$@"; do
		command -v "$t" >/dev/null 2>&1 && { echo "$t"; return 0; }
	done
	return 1
}

# exports KIND FILE — the image's exported (defined, global) symbol names, one
# per line, Mach-O's leading underscore removed. Nonzero when it cannot read.
exports() {
	local kind="$1" f="$2" tool
	case "$kind" in
	elf)
		tool=$(first_tool nm llvm-nm-18 llvm-nm) || return 1
		"$tool" -D --defined-only "$f" | awk 'NF >= 3 { print $3 }'
		;;
	macho)
		tool=$(first_tool llvm-nm-18 llvm-nm nm) || return 1
		"$tool" -g --defined-only "$f" | awk 'NF >= 3 { sub(/^_/, "", $3); print $3 }'
		;;
	pe)
		tool=$(first_tool x86_64-w64-mingw32-objdump llvm-objdump-18 llvm-objdump objdump) || return 1
		"$tool" -p "$f" | awk '/\[Ordinal\/Name Pointer\] Table/ { t = 1; next }
			t && /^[[:space:]]*$/ { t = 0 }
			t { sub(/^.*\] /, ""); print }'
		;;
	esac
}

# check KIND FILE — prints each forbidden export; 0 = clean, 1 = forbidden
# exports, 2 = the image could not be read.
check() {
	local out
	out=$(exports "$1" "$2") || return 2
	out=$(printf '%s\n' "$out" | grep -E "$FORBIDDEN")
	[ -z "$out" ] && return 0
	printf '%s\n' "$out" | sed "s|^|$2: |" | head -5
	echo "$2: $(printf '%s\n' "$out" | wc -l) libgit2 symbol(s) exported"
	return 1
}

# --- negative control --------------------------------------------------------
tmpd=$(mktemp -d)
fail_control() {
	rm -rf "$tmpd"
	echo "check-madcgit-exports: CONTROL FAILED — $1" >&2
	exit 2
}
printf 'int git_x(void) { return 1; }\nint madcgit_y(void) { return git_x(); }\n' > "$tmpd/bad.c"
printf '__attribute__((visibility("hidden"))) int git_x(void) { return 1; }\nint madcgit_y(void) { return git_x(); }\n' > "$tmpd/good.c"
case "$(uname -s)" in
Darwin) ckind=macho; cflags=(-dynamiclib) ;;
*)      ckind=elf;   cflags=(-shared -fPIC) ;;
esac
cc "${cflags[@]}" -o "$tmpd/bad.so" "$tmpd/bad.c" 2>/dev/null || fail_control "cc could not build the control"
cc "${cflags[@]}" -o "$tmpd/good.so" "$tmpd/good.c" 2>/dev/null || fail_control "cc could not build the control"
check "$ckind" "$tmpd/bad.so" >/dev/null; [ $? -eq 1 ] || fail_control "an exported git_x passed"
check "$ckind" "$tmpd/good.so" >/dev/null || fail_control "a hidden git_x failed"
rm -rf "$tmpd"

# --- the tree ---------------------------------------------------------------
status=0
seen=0
for row in "elf lib/libmadcgit.so" "macho lib/libmadcgit.dylib" \
	   "macho lib/madcgit/arm64-macos/libmadcgit.dylib" \
	   "macho lib/madcgit/x86-64-macos/libmadcgit.dylib" "pe bin/madcgit.dll"; do
	kind=${row%% *}
	f=${row#* }
	[ -f "$f" ] || continue
	seen=$((seen + 1))
	check "$kind" "$f" >&2
	case $? in
	0) ;;
	1) status=1 ;;
	*) echo "check-madcgit-exports: cannot read $f's exports (no $kind symbol tool)" >&2; status=1 ;;
	esac
done
if [ $status -ne 0 ]; then
	echo "  -> the module links libgit2 hidden: src/madcgit.mk (--exclude-libs on ELF and PE, -load_hidden on Mach-O)" >&2
	exit 1
fi
if [ $seen -eq 0 ]; then
	echo "check-madcgit-exports: OK (no madcgit image built; nothing to check)"
else
	echo "check-madcgit-exports: OK ($seen madcgit image(s), no libgit2 symbol exported)"
fi
