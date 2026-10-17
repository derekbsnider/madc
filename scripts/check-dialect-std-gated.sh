#!/bin/bash
# GATE — the madc dialect surface is gated on --std=madc (owner 2026-10-17:
# "we do need to make sure that the dialect builtin registration is gated on
# --std=madc"; "the entire madc:: namespace is gated to --std=madc").
# docs/plans/2026-10-17-dialect-builtin-registration.md, "Gated on --std=madc".
#
# The rule: ONE predicate (Program::dialect_surface_enabled, reading the
# LanguageStd enum) answers "is the dialect on" for every registration path.
# Under every other standard — each C and C++ standard, gnu variants included
# — a dialect name (php::, madc::value, ui::) resolves to nothing: the
# program's own namespaces are its alone. Under --std=madc they resolve.
#
# Sides:
#   1. every strict standard refuses each dialect program;
#   2. --std=madc compiles each one (the programs are well-formed: a strict
#      refusal is the gate's, not a broken program's);
#   3. detector control: a strict C++ program that declares its OWN
#      `namespace php` compiles — a strict resolution is visible to this gate.
#   4. the table serves the dialect (R4c): --std=madc's --show-stats counts
#      names the generated table built; a strict compile reports no table.
set -u
cd "$(dirname "$0")/.."

MADC_BIN="${MADC_BIN:-bin/madc}"
if [ ! -x "$MADC_BIN" ]; then
	echo "check-dialect-std-gated: $MADC_BIN not built" >&2
	exit 2
fi
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

printf 'int main() { return php::strtoupper("hi")[0] == 72 ? 0 : 1; }\n' > "$tmp/php.mad"
printf 'int main() { madc::value v; return 0; }\n' > "$tmp/value.mad"
printf 'int main() { return (int)ui::TUI - 2; }\n' > "$tmp/ui.mad"
printf 'namespace php { inline unsigned long strlen(const char *s) { unsigned long n = 0; while (s[n]) n++; return n; } }\nint main() { return (int)php::strlen("ab") - 2; }\n' > "$tmp/own.mad"

compiles() {
	"$MADC_BIN" "--std=$1" -c -o "$tmp/out.o" "$2" > /dev/null 2>&1
}

fail=0
for s in c89 c99 c11 c17 c23 gnu89 gnu99 gnu11 gnu17 gnu23 \
	 c++98 c++11 c++14 c++17 c++20 c++23 gnu++98 gnu++11 gnu++14 gnu++17 gnu++20 gnu++23; do
	for p in php value ui; do
		if compiles "$s" "$tmp/$p.mad"; then
			echo "check-dialect-std-gated: --std=$s RESOLVED the dialect ($p.mad):"
			cat "$tmp/$p.mad"
			fail=1
		fi
	done
done
for p in php value ui; do
	if ! compiles madc "$tmp/$p.mad"; then
		echo "check-dialect-std-gated: --std=madc REFUSED $p.mad (the dialect must resolve):"
		"$MADC_BIN" --std=madc -c -o "$tmp/out.o" "$tmp/$p.mad" 2>&1 | head -5
		fail=1
	fi
done
if ! compiles c++17 "$tmp/own.mad"; then
	echo "check-dialect-std-gated: DETECTOR CONTROL FAILED — a strict program's own namespace php did not compile:"
	"$MADC_BIN" --std=c++17 -c -o "$tmp/out.o" "$tmp/own.mad" 2>&1 | head -5
	fail=1
fi
table_line() {
	"$MADC_BIN" "--std=$1" --show-stats -c -o "$tmp/out.o" "$2" 2>&1 >/dev/null | grep "dialect table"
}
if ! table_line madc "$tmp/php.mad" | grep -Eq " [1-9][0-9]* of [0-9]+ names built"; then
	echo "check-dialect-std-gated: --std=madc built no dialect table name for php.mad:"
	table_line madc "$tmp/php.mad"
	fail=1
fi
if table_line c++17 "$tmp/own.mad" | grep -q .; then
	echo "check-dialect-std-gated: --std=c++17 reported a dialect table"
	fail=1
fi
if [ "$fail" -ne 0 ]; then
	echo "Owner: Program::dialect_surface_enabled (include/madc.h) and every registration it gates."
	exit 1
fi
echo "check-dialect-std-gated: OK — 22 strict standards refuse php::, madc::value and ui::; --std=madc resolves them from the table"
