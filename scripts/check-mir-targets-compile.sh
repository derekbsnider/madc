#!/bin/bash
# check-mir-targets-compile.sh — the in-tree MIR compiles for EVERY target it
# supports (x86-64, aarch64, ppc64, riscv64, s390x), not only for the build
# host's.
#
# A build compiles only its host's target: mir-gen.c includes ONE
# mir-gen-<target>.c, chosen by mir-target.h. The others are never compiled
# here, so a change to them (or to the selection) can break a target no lane
# builds. This gate selects each target with clang's --target and
# syntax-checks every MIR source whose code depends on the target (the files
# naming a MIR_TARGET_IS_* selector), using the host's C headers.
#
# Each target must select exactly ONE MIR target (mir-target.h once defined
# selectors for x86-64 and aarch64 only, so ppc64, riscv64 and s390x hosts hit
# "#error undefined or unsupported generation target").
# Negative control: a source that errors only under __s390x__ must FAIL the
# s390x check and pass the x86-64 one.
# A clang without a target's backend skips that target, loudly.
#
#   scripts/check-mir-targets-compile.sh [MIRDIR]
set -u
cd "$(dirname "$0")/.." || exit 2
mir=${1:-third_party/mir}

TARGETS="x86_64-linux-gnu aarch64-linux-gnu powerpc64le-linux-gnu riscv64-linux-gnu s390x-linux-gnu"

if ! command -v clang > /dev/null 2>&1; then
	echo "check-mir-targets-compile: SKIP — no clang on this host (it carries every target's backend)"
	exit 0
fi
multiarch=$(gcc -print-multiarch 2> /dev/null)
[ -n "$multiarch" ] || multiarch=x86_64-linux-gnu

mkdir -p tmp
ctl=$(mktemp -d tmp/mir-targets-ctl.XXXXXX) || exit 2
trap 'rm -rf "$ctl"' EXIT
# The host's glibc headers under another target ask for the 32-bit stubs.
mkdir -p "$ctl/inc/gnu"
: > "$ctl/inc/gnu/stubs-32.h"

# Syntax-checks $2 (a C file) for target $1 with the MIR sources' includes;
# prints the errors.
check() {
	clang --target="$1" -fsyntax-only -std=gnu11 -w -I"$mir" -I"$mir/c2mir" \
		-isystem "$ctl/inc" -isystem "/usr/include/$multiarch" -isystem /usr/include \
		"$2" 2>&1 | grep -E "error" | head -5
}

printf '#if defined(__s390x__)\n#error s390x only\n#endif\nint x;\n' > "$ctl/s390x_only.c"
if [ -z "$(check s390x-linux-gnu "$ctl/s390x_only.c")" ]; then
	echo "check-mir-targets-compile: CONTROL FAILED — an s390x-only error went undetected" >&2
	exit 1
fi
if [ -n "$(check x86_64-linux-gnu "$ctl/s390x_only.c")" ]; then
	echo "check-mir-targets-compile: CONTROL FAILED — the x86-64 check saw the s390x-only error" >&2
	exit 1
fi

# Exactly one MIR target per triple.
printf '#include "mir-target.h"\n#if MIR_TARGET_IS_X86_64 + MIR_TARGET_IS_AARCH64 + MIR_TARGET_IS_PPC64 + MIR_TARGET_IS_S390X + MIR_TARGET_IS_RISCV64 != 1\n#error not exactly one MIR target selected\n#endif\nint x;\n' \
	> "$ctl/one_target.c"

# The translation units: a file another MIR source #includes (a target's
# mir-gen-<target>.c, mir-macho.c, ...) is checked inside its includer.
included=$(cd "$mir" && grep -h -o -E '#include "[^"]+\.c"' -- *.c c2mir/*.c \
	| sed -e 's|.*"\(.*\)"|\1|' -e 's|.*/||' | sort -u)
srcs=$(cd "$mir" && grep -l "MIR_TARGET_IS_" -- *.c c2mir/*.c \
	| while read -r f; do printf '%s\n' "$included" | grep -qxF "${f##*/}" || echo "$f"; done)
if [ -z "$srcs" ]; then
	echo "check-mir-targets-compile: no MIR translation unit names a MIR_TARGET_IS_* selector — the marker is stale (a null is a broken gate, not a pass)" >&2
	exit 1
fi
fail=0 checked=""
for t in $TARGETS; do
	if ! clang --target="$t" -x c -fsyntax-only /dev/null 2> /dev/null; then
		echo "check-mir-targets-compile: SKIP $t — this clang has no backend for it"
		continue
	fi
	out=$(check "$t" "$ctl/one_target.c")
	for f in $srcs; do
		[ -n "$out" ] && break
		out=$(check "$t" "$mir/$f")
		[ -n "$out" ] && out="$f: $out"
	done
	if [ -n "$out" ]; then
		echo "check-mir-targets-compile: MIR does not compile for $t —" >&2
		printf '  %s\n' "$out" >&2
		fail=1
	else
		checked="$checked $t"
	fi
done
[ "$fail" = 0 ] || exit 1
echo "check-mir-targets-compile: OK (MIR compiles for:$checked; one target each; control both ways)"
