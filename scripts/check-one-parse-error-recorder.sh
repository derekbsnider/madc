#!/bin/bash
# check-one-parse-error-recorder.sh — ONE front-end error recording rule
# (error-tolerant parse dupaudit, arc doc 3.5 slice A).
#
# The rules:
#   1. record_frontend_error is THE record-and-render rule for front-end
#      catch arms (set_error + print_last_diagnostic). Nine inlined copies
#      existed across the parser's two clusters and the lexer's two
#      (tokenize / tokenize_buffer); a tenth must not appear — adopt the
#      helper (record_parse_error is the parser-phase convenience).
#   2. record_throw_diagnostic is THE Throw-origin recording rule (message
#      from Throw's buffer, e.what() fallback). Five copies existed; its
#      distinctive spelling anywhere else is a new copy.
#
# Markers (the concept's distinctive spellings):
#   1. print_last_diagnostic within 2 lines after a set_error( call — the
#      record-and-render pair inlined
#   2. the Throw-buffer message fallback: Throw.str().empty() ? e.what()
# One permitted site each — the owner's body in src/parser.cpp. Any other
# match is a new hand-rolled copy — adopt the helper; never add an
# exemption here.
set -u
cd "$(dirname "$0")/.."

fail=0

# Marker 1: an inlined set_error+print_last_diagnostic pair. Owner =
# record_frontend_error's body (the one permitted adjacency).
pairs=$(grep -rn -A2 'set_error(' src include \
	--include='*.cpp' --include='*.h' 2>/dev/null \
	| grep 'print_last_diagnostic' || true)
pair_count=0
[ -n "$pairs" ] && pair_count=$(echo "$pairs" | wc -l)
if [ "$pair_count" -gt 1 ]; then
	echo "check-one-parse-error-recorder: inlined set_error+print_last_diagnostic pair outside record_frontend_error:"
	echo "$pairs"
	fail=1
fi

# Marker 2: the Throw-buffer fallback spelling outside its owner.
throws=$(grep -rnE 'Throw\.str\(\)\.empty\(\) \? e\.what\(\)' src include \
	--include='*.cpp' --include='*.h' 2>/dev/null || true)
throw_count=0
[ -n "$throws" ] && throw_count=$(echo "$throws" | wc -l)
if [ "$throw_count" -gt 1 ]; then
	echo "check-one-parse-error-recorder: Throw-origin recording spelling outside record_throw_diagnostic:"
	echo "$throws"
	fail=1
fi

# Marker 3: the scope-depth restore recipe (pop compounds to an entry
# depth) outside its owner, restore_parse_scope_depths — the third family
# this slice consolidated (two sites: derive-body-catch + the top-level
# containment).
restores=$(grep -rnE 'compounds\.size\(\) > saved' src include \
	--include='*.cpp' --include='*.h' 2>/dev/null || true)
restore_count=0
[ -n "$restores" ] && restore_count=$(echo "$restores" | wc -l)
if [ "$restore_count" -gt 1 ]; then
	echo "check-one-parse-error-recorder: scope-depth restore recipe outside restore_parse_scope_depths:"
	echo "$restores"
	fail=1
fi

# Negative controls: each marker must catch a synthetic violation, or the
# gate is dead and the verdict means nothing.
ctrl=$(mktemp)
cat > "$ctrl" <<'EOF'
set_error(DiagnosticPhase::parser, msg, f, l, c);
print_last_diagnostic(error());
x = Throw.str().empty() ? e.what() : Throw.str();
while ( compounds.size() > saved_compounds )
EOF
if ! grep -A2 'set_error(' "$ctrl" | grep -q 'print_last_diagnostic'; then
	echo "check-one-parse-error-recorder: NEGATIVE CONTROL FAILED (pair marker)"
	fail=1
fi
if ! grep -qE 'Throw\.str\(\)\.empty\(\) \? e\.what\(\)' "$ctrl"; then
	echo "check-one-parse-error-recorder: NEGATIVE CONTROL FAILED (throw marker)"
	fail=1
fi
if ! grep -qE 'compounds\.size\(\) > saved' "$ctrl"; then
	echo "check-one-parse-error-recorder: NEGATIVE CONTROL FAILED (restore marker)"
	fail=1
fi
rm -f "$ctrl"

if [ "$fail" -ne 0 ]; then
	exit 1
fi
echo "check-one-parse-error-recorder: OK (one owner each, controls live)"
