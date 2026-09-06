#!/bin/bash
# GATE — a copied token keeps its origin.
#
# The rule: a parser-side copy of an EXISTING token — a template pattern, a
# default argument, a call argument, a substitution run — is made with
# TokenBase::clone_origin(), never bare clone(). clone() constructs a fresh
# token, and TokenBase's constructor stamps it with the CURRENT parse
# position, so a copied header token became "user code" at its use site: the
# builder's root/library split then emitted an unselected libc++ basic_string
# constructor instance unconditionally, and its body could not link (darwin
# dispatch #8, 2026-09-04). Two earlier fixes had patched around the same
# stamping by redirecting _parse_* across a clone loop (class-template
# instantiation, lazy pattern capture) — the rule lived in N places, and the
# member-template injection had none of them.
#
# The lexer's macro expansion is the ONE legitimate bare clone(): an expanded
# token's location IS the expansion site (gcc's primary location), so
# src/lexer.cpp is exempt by name. Everything else under src/ is scanned;
# comments mentioning clone() (no `->`/`.` receiver) do not count.
set -u
cd "$(dirname "$0")/.."
scan() {
	grep -nE '(->|\.)clone\(\)' "$@" 2>/dev/null \
	  | grep -vE 'clone_origin\(\)' \
	  | grep -vE '^[^:]+:[0-9]+:[[:space:]]*//' || true
}
files=$(ls src/*.cpp | grep -v '/lexer\.cpp$')
# shellcheck disable=SC2086
hits=$(scan $files)
n=$(printf '%s' "$hits" | grep -c .)
echo "check-clone-origin: $n bare clone() token copies outside the lexer (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$hits" | sed 's/^/  /' | head -20
	echo "  -> copy with clone_origin() (include/tokens.h): the copy is the same source text from the same place" >&2
	exit 1
fi
# Negative control: a bare copy must be caught, a clone_origin() copy must not.
tmp=$(mktemp "${TMPDIR:-/tmp}/clone-origin.XXXXXX")
printf 'out.push_back(t->clone());\n' > "$tmp"
if [ -z "$(scan "$tmp")" ]; then
	echo "check-clone-origin: SELFTEST FAILED — a bare clone() copy passed" >&2
	rm -f "$tmp"; exit 1
fi
printf 'out.push_back(t->clone_origin());\n// clone() stamps the parse position\n' > "$tmp"
if [ -n "$(scan "$tmp")" ]; then
	echo "check-clone-origin: SELFTEST FAILED — a clone_origin() copy or a comment was flagged" >&2
	rm -f "$tmp"; exit 1
fi
rm -f "$tmp"
echo "GREEN — every parser-side token copy keeps its origin (clone_origin); the lexer's macro-expansion clone() is the one exempt site"
