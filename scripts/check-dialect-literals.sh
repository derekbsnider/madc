#!/bin/bash
# check-dialect-literals.sh — the dialect object-literal gate
# (.claude/rules/dialect-literals.md).
#
# In madc-dialect PRODUCTION code (the shipped tools), build an object with
# an object literal — `var x = { "k": v, ... };` — never by declaring a bare
# `var x;` and then filling it field-by-field with string-literal keys
# (`x["k"] = v;`). Imperative key-assignment is for MUTATING an object that
# already exists; spelling a literal one statement at a time is the degraded
# form this gate forbids.
#
# The gate flags ONLY the unambiguous regression: a bare `var NAME;`
# (uninitialized) immediately followed — skipping blank/comment-only lines —
# by `NAME["<string literal>"] = ...`. It does NOT flag:
#   - a mutation of an already-initialized `var NAME = { ... };`
#   - a computed/dynamic key (`NAME[expr] = ...`, e.g. NAME[key.c_str()])
#   - an integer/array index (`NAME[0] = ...`)
# so the language's legitimate imperative mutation stays available.
#
# Scope: the production dialect sources under tools/ (*.inc, *.mad). Tests
# that deliberately exercise imperative mutation as a language feature are
# out of scope by design.
#
# The gate self-tests both directions (negative control): a synthetic bad
# file must FAIL the scan and a synthetic good file must PASS it, else the
# gate itself is broken and we fail loudly.

set -u
cd "$(dirname "$0")/.." || exit 2

scan_file() {
	# $1 = file; prints violations, returns nonzero when any found.
	awk '
		BEGIN { pending = 0; bad = 0 }
		{
			line = $0
			if (pending) {
				t = line; sub(/^[ \t]+/, "", t)
				if (t == "" || t ~ /^\/\//) { next }  # skip blank/comment
				if (t ~ ("^" pvar "\\[\"[^\"]*\"\\][ \t]*=[^=]")) {
					printf "%s:%d: `var %s;` built field-by-field — use an object literal `var %s = { ... };`\n", FILENAME, pline, pvar, pvar
					bad = 1
				}
				pending = 0
			}
			if (line ~ /^[ \t]*var[ \t]+[A-Za-z_][A-Za-z0-9_]*[ \t]*;[ \t]*(\/\/.*)?$/) {
				v = line; sub(/^[ \t]*var[ \t]+/, "", v); sub(/[ \t]*;.*$/, "", v)
				pvar = v; pline = NR; pending = 1
			} else if (line !~ /^[ \t]*(\/\/.*)?$/) {
				pending = 0
			}
		}
		END { exit bad }
	' "$1"
}

# --- negative control -------------------------------------------------
ctrl_dir=$(mktemp -d)
trap 'rm -rf "$ctrl_dir"' EXIT

printf 'void f() {\n    var x;\n    x["k"] = 1;\n}\n' > "$ctrl_dir/bad.inc"
if scan_file "$ctrl_dir/bad.inc" > /dev/null 2>&1; then
	echo "check-dialect-literals: NEGATIVE CONTROL FAILED — the scanner" >&2
	echo "accepted a bare 'var x; x[\"k\"]=' build; the gate is broken." >&2
	exit 2
fi

# initialized-var mutation, a computed key, and an integer index are all
# legitimate — the scanner must PASS this file.
printf 'void g() {\n    var a = { "k": 1 };\n    a["m"] = 2;\n    var b;\n    b[key.c_str()] = 1;\n    var c;\n    c[0] = 9;\n}\n' > "$ctrl_dir/good.inc"
if ! scan_file "$ctrl_dir/good.inc" > /dev/null 2>&1; then
	echo "check-dialect-literals: NEGATIVE CONTROL FAILED — the scanner" >&2
	echo "flagged legitimate mutation / computed-key / index construction." >&2
	exit 2
fi

# --- the real scan ----------------------------------------------------
rc=0
while IFS= read -r f; do
	[ -f "$f" ] || continue
	if ! scan_file "$f"; then
		rc=1
	fi
done < <(find tools -type f \( -name '*.inc' -o -name '*.mad' \) | sort)

if [ "$rc" -ne 0 ]; then
	echo "check-dialect-literals: FAIL — see .claude/rules/dialect-literals.md" >&2
	exit 1
fi
echo "check-dialect-literals: OK"
exit 0
