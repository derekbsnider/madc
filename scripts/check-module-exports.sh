#!/bin/bash
# check-module-exports.sh — a madc module exports no symbol of the library it
# links statically: madcgit no libgit2 symbol, madcmark no cmark-gfm symbol.
#
# Each module links its dependency statically (src/madcgit.mk,
# src/madcmark.mk), and madc loads modules RTLD_GLOBAL (src/madc_dl.cpp): an
# exported dependency symbol would enter the process's global scope beside
# any copy of that library a program loads itself, and a script's dlsym
# fallback would bind to the module's copy. A module's own C API is its
# madc<name>_* (include/madc/madcgit.h, include/madc/madcmark.h).
#
# The MODULES table below is the data: a module, its dependency's symbol
# pattern, and the images a target's build leaves in the tree (an absent
# image is skipped):
#   lib/lib<module>.so                         ELF: nm -D
#   lib/lib<module>.dylib, lib/<module>/*-macos/lib<module>.dylib
#                                              Mach-O: llvm-nm -g
#   bin/<module>.dll                           PE: objdump -p, the export table
# An image present with no tool to read it FAILS: a gate that cannot look
# must not pass.
#
# Negative control: a synthetic shared object for this host exporting git_x
# must FAIL; the same with git_x hidden must PASS — else the reader or the
# pattern is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

# module|dependency|the dependency's exported-symbol pattern
MODULES="madcgit|libgit2|^(git|giterr)_
madcmark|cmark-gfm|^(cmark_|CMARK_|houdini_|_scan_|_ext_scan_|create_[a-z]+_extension$|normalize_map_label$)"
FORBIDDEN='^(git|giterr)_'	# the control's pattern (madcgit's row)

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
	echo "$2: $(printf '%s\n' "$out" | wc -l) $DEPENDENCY symbol(s) exported"
	return 1
}

# --- negative control --------------------------------------------------------
tmpd=$(mktemp -d)
fail_control() {
	rm -rf "$tmpd"
	echo "check-module-exports: CONTROL FAILED — $1" >&2
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
DEPENDENCY=libgit2
check "$ckind" "$tmpd/bad.so" >/dev/null; [ $? -eq 1 ] || fail_control "an exported git_x passed"
check "$ckind" "$tmpd/good.so" >/dev/null || fail_control "a hidden git_x failed"
rm -rf "$tmpd"

# --- the tree ---------------------------------------------------------------
status=0
seen=0
while IFS='|' read -r module DEPENDENCY FORBIDDEN; do
	[ -n "$module" ] || continue
	for row in "elf lib/lib$module.so" "macho lib/lib$module.dylib" \
		   "macho lib/$module/arm64-macos/lib$module.dylib" \
		   "macho lib/$module/x86-64-macos/lib$module.dylib" "pe bin/$module.dll"; do
		kind=${row%% *}
		f=${row#* }
		[ -f "$f" ] || continue
		seen=$((seen + 1))
		check "$kind" "$f" >&2
		case $? in
		0) ;;
		1) status=1
		   echo "  -> $module links $DEPENDENCY hidden: src/$module.mk (--exclude-libs on ELF and PE, -load_hidden on Mach-O)" >&2 ;;
		*) echo "check-module-exports: cannot read $f's exports (no $kind symbol tool)" >&2; status=1 ;;
		esac
	done
done <<EOT
$MODULES
EOT
[ $status -eq 0 ] || exit 1
if [ $seen -eq 0 ]; then
	echo "check-module-exports: OK (no module image built; nothing to check)"
else
	echo "check-module-exports: OK ($seen module image(s), none exporting its dependency's symbols)"
fi
