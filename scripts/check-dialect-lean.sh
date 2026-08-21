#!/bin/bash
# check-dialect-lean.sh — the dialect-lean gate (.claude/rules/dialect-lean.md).
#
# OWNER LAW (2026-08-21): the --std=madc surface — the auto-include prelude
# fragments included — must not depend on C++ system header parsing, and
# nothing the dialect or polyglot functionality defines depends on
# std::string. This gate holds that line statically:
#
#   1. No dialect fragment may contain a C++ system #include (any
#      <header> without a .h suffix — <string>, <vector>, <iostream>...).
#   2. Every std::string mention in a fragment must sit inside the
#      stdlib-guard conditional
#      `#if defined(_GLIBCXX_STRING) || defined(_LIBCPP_STRING)`
#      (the <ns_madc> convention: interop conveniences exist only when
#      the TU already paid for <string>).
#
# Fragment set: the extensionless files under include/madc/ plus
# include/madc/bits/* — the auto-include-served dialect surface. The
# *.h twins (ns_php.h, stddef.h, ...) are C/C++-interop headers and are
# out of scope here.
#
# The gate self-tests: a synthetic bad fragment must FAIL the scan
# (negative control), else the gate itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

scan_fragment() {
	# $1 = file; prints violations, returns 0 when clean.
	awk '
		BEGIN { depth = 0; guard_depth = 0; bad = 0 }
		# Comments are prose, not surface — strip // tails before the
		# content tests (guard lines are never commented).
		{ sub(/\/\/.*$/, "") }
		/^[ \t]*#[ \t]*if/ {
			depth++
			if ($0 ~ /defined[ \t]*\([ \t]*_GLIBCXX_STRING[ \t]*\)/ \
			 && $0 ~ /defined[ \t]*\([ \t]*_LIBCPP_STRING[ \t]*\)/ \
			 && guard_depth == 0)
				guard_depth = depth
			next
		}
		/^[ \t]*#[ \t]*endif/ {
			if (guard_depth == depth) guard_depth = 0
			if (depth > 0) depth--
			next
		}
		/^[ \t]*#[ \t]*include[ \t]*<[^.>]*>/ {
			print FILENAME ":" FNR ": C++ system include in a dialect fragment: " $0
			bad = 1
			next
		}
		/std::string/ {
			if (guard_depth == 0) {
				print FILENAME ":" FNR ": std::string outside the stdlib guard: " $0
				bad = 1
			}
		}
		END { exit bad }
	' "$1"
}

# --- negative control: the scanner must fail a synthetic bad fragment ---
ctrl_dir=$(mktemp -d)
trap 'rm -rf "$ctrl_dir"' EXIT
printf '#include <string>\nnamespace x { std::string f(); }\n' \
	> "$ctrl_dir/bad_fragment"
if scan_fragment "$ctrl_dir/bad_fragment" > /dev/null 2>&1; then
	echo "check-dialect-lean: NEGATIVE CONTROL FAILED — the scanner" >&2
	echo "accepted a fragment with a bare <string> include; the gate" >&2
	echo "is broken." >&2
	exit 2
fi

# --- the real scan ---
rc=0
for f in include/madc/* include/madc/bits/*; do
	[ -f "$f" ] || continue
	case "$f" in
		*.h) continue ;;    # C/C++-interop headers: out of scope
	esac
	if ! scan_fragment "$f"; then
		rc=1
	fi
done

if [ "$rc" -ne 0 ]; then
	echo "check-dialect-lean: FAIL — see .claude/rules/dialect-lean.md" >&2
	exit 1
fi
echo "check-dialect-lean: OK"
exit 0
