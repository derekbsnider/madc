#!/bin/bash
# headerless_suite.sh — run a packed artifact's suite with NO headers on disk.
#
#   bash scripts/headerless_suite.sh [test-glob ...]
#   MADC_HEADERLESS_PROFILE=win64 bash scripts/headerless_suite.sh
#
# WHY THIS LANE EXISTS. Serving the standard headers out of the artifact's own
# frozen corpus — so a machine with no compiler installation can still compile
# — is a headline madc feature, in all six cells: {Linux, Windows, macOS} x
# {C++ standard library, C library}. Every OTHER lane runs where the headers
# happen to be on disk, so when the forest fails to serve a unit, live parse
# silently rescues it from /usr/include and the suite stays green. The failure
# is INVISIBLE. Under wine there is a second source of the same blindness:
# wine maps / to Z:, so the build container's headers are reachable from the
# PE too.
#
# That blindness has already cost real defects. Task #58 was filed when the
# promise broke on a real Windows box, then fixed days later by unrelated
# work, and NEITHER event was noticed by any automated lane. The first run of
# this lane found 132 failures on Linux, including a frozen /usr/include/
# features.h whose own #include of <stdc-predef.h> was not in the corpus.
#
# MECHANISM. A private mount namespace (`unshare -rm`; no root, no privileges)
# with an empty tmpfs over every include directory the relevant compilers
# would search. The absence is REAL — not a flag madc could ignore, not an
# include-path override some other search arm could route around. Nothing
# outside the namespace is touched and the masks die with the process.
#
# The masked set is DISCOVERED, by asking each compiler where it searches
# (`cc -E -Wp,-v`), never hard-coded: a toolchain version bump must not
# silently shrink the mask, because a shrunk mask is a vacuum and vacuums
# read as green.
#
# NEGATIVE CONTROL — why this lane can be trusted. A lane asserting an absence
# must prove the absence bit, or a mask that failed to apply reports GREEN for
# the worst possible reason: everything served from the disk we believed we
# had hidden. So before any test runs, inside the same namespace:
#   (a) the profile's real compiler MUST fail to find <stdio.h>; and
#   (b) it must fail with a missing-header diagnostic specifically; and
#   (c) that same compiler MUST still compile a header-free TU.
# (b) and (c) are not padding. The prototype for this script masked a
# directory that also held cc1, so gcc "failed" — on `cannot execute cc1`.
# A one-sided control would have accepted that as proof the mask worked.
#
# PROFILES. One implementation, one table; a third artifact is a table row.
#   native  bin/madc-release              (Linux, libstdc++ + glibc)
#   win64   the packed PE under wine      (Windows, MinGW libstdc++ + UCRT)
# macOS needs a different masking primitive (no unshare; a sandbox profile
# denying SDK reads) and is deliberately not faked here — an unfaithful mask
# would be worse than no lane.
#
# Fixtures: tests/<base>.headerless_skip marks a test structurally out of
# scope — the `--no-embedded-headers` and _realhdr families exist precisely to
# exercise the on-disk path. Same generic domain convention as win64/wine64
# (.claude/rules/test-fixtures.md); the runner needs no change and there is no
# per-test branch here.
set -e
cd "$(dirname "$0")/.."

PROFILE="${MADC_HEADERLESS_PROFILE:-native}"
case "$PROFILE" in
native)
	PROFILE_BIN="bin/madc-release"
	PROFILE_WRAPPER=""
	PROFILE_DOMAINS="headerless"
	PROFILE_CC="gcc"
	;;
win64)
	PROFILE_BIN="bin/madc-release-x86-64-windows.exe"
	PROFILE_WRAPPER="wine"
	# BOTH domains on top of headerless: a wine run of the win64 binary is
	# already two domains (see remote_build.sh's wine stage), and this lane
	# adds a third rather than replacing them.
	PROFILE_DOMAINS="headerless win64 wine64"
	PROFILE_CC="x86_64-w64-mingw32-gcc"
	;;
*)
	echo "headerless_suite: unknown profile '$PROFILE' (native|win64)" >&2
	exit 1
	;;
esac

MADC_BIN="${MADC_BIN:-$PROFILE_BIN}"
if [ ! -f "$MADC_BIN" ]; then
	echo "headerless_suite: $MADC_BIN missing — build the packed artifact first" >&2
	echo "  (this lane tests the PACKED binary; bin/madc carries no forest)" >&2
	exit 1
fi

# The wineserver must already be running OUTSIDE the namespace. If wine had to
# start one from inside, that server would inherit the masked view and outlive
# this run, poisoning every later normal wine lane.
if [ "$PROFILE_WRAPPER" = "wine" ] && [ "${MADC_HEADERLESS_INSIDE:-0}" != "1" ]; then
	WINEDEBUG=-all wineserver -p || true
fi

if [ "${MADC_HEADERLESS_INSIDE:-0}" != "1" ]; then
	if ! unshare -rm true 2>/dev/null; then
		echo "headerless_suite: this host cannot create a user+mount namespace" >&2
		echo "  (unshare -rm failed; the lane needs it to make the absence real)" >&2
		exit 1
	fi
	MADC_HEADERLESS_INSIDE=1 exec unshare -rm "$0" "$@"
fi

# --- Discover every include directory the relevant compilers search. ---------
# Asked, never listed. The host toolchain is masked even in the win64 profile:
# a real Windows box has no /usr/include either, and wine would otherwise let
# the PE reach it through Z:.
search_dirs() {
	command -v "$1" >/dev/null 2>&1 || return 0
	echo | "$1" -E -Wp,-v -x c - 2>&1 >/dev/null \
		| sed -n '/search starts here:/,/End of search list/p' \
		| sed -n 's/^ \(\/.*\)$/\1/p'
}

MASK_DIRS=()
while IFS= read -r d; do
	[ -d "$d" ] || continue
	for seen in ${MASK_DIRS[@]+"${MASK_DIRS[@]}"}; do
		[ "$seen" = "$d" ] && continue 2
	done
	MASK_DIRS+=("$d")
done < <( { search_dirs gcc; search_dirs "$PROFILE_CC"; \
	    printf '%s\n' /usr/include /usr/local/include; } )

if [ "${#MASK_DIRS[@]}" -lt 2 ]; then
	echo "headerless_suite: discovered only ${#MASK_DIRS[@]} include dir(s) to mask —" >&2
	echo "  refusing to run, because a near-empty mask cannot prove an absence." >&2
	exit 1
fi

for d in "${MASK_DIRS[@]}"; do
	mount -t tmpfs none "$d"
done

# --- Negative control: prove the mask bit, all three directions. -------------
CTL=$(mktemp -d)
trap 'rm -rf "$CTL"' EXIT
printf '#include <stdio.h>\nint main(void){ return 0; }\n' > "$CTL/needs_header.c"
printf 'int main(void){ return 0; }\n'                     > "$CTL/no_header.c"

if "$PROFILE_CC" -fsyntax-only "$CTL/needs_header.c" 2>"$CTL/err"; then
	echo "headerless_suite: CONTROL FAILED — $PROFILE_CC still found <stdio.h>." >&2
	echo "  The absence is not real, so a green run here would prove nothing." >&2
	printf '  masked: %s\n' "${MASK_DIRS[@]}" >&2
	exit 1
fi
if ! grep -q 'No such file or directory' "$CTL/err"; then
	echo "headerless_suite: CONTROL FAILED — $PROFILE_CC failed, but NOT on a" >&2
	echo "  missing header:" >&2
	sed 's/^/    /' "$CTL/err" >&2
	echo "  A failure for an unrelated reason (a masked cc1, a broken PATH)" >&2
	echo "  would masquerade as a working mask." >&2
	exit 1
fi
if ! "$PROFILE_CC" -fsyntax-only "$CTL/no_header.c" 2>"$CTL/err2"; then
	echo "headerless_suite: CONTROL FAILED — $PROFILE_CC cannot compile a" >&2
	echo "  header-free TU here, so the mask broke the toolchain rather than" >&2
	echo "  just hiding headers:" >&2
	sed 's/^/    /' "$CTL/err2" >&2
	exit 1
fi

echo "headerless_suite: profile=$PROFILE bin=$MADC_BIN"
echo "  mask verified — ${#MASK_DIRS[@]} include roots empty; $PROFILE_CC fails on"
echo "  <stdio.h> and succeeds without it."
echo

# --- The suite, with this profile's skip fixtures active. --------------------
export MADC_BIN
export MADC_SKIP_EXT="$PROFILE_DOMAINS${MADC_SKIP_EXT:+ $MADC_SKIP_EXT}"
[ -n "$PROFILE_WRAPPER" ] && export MADC_WRAPPER="$PROFILE_WRAPPER"
export WINEDEBUG=-all
exec bash scripts/run_tests.sh "$@"
