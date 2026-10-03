#!/bin/bash
# DRIFT-PREVENTION GATE -- "is this diagnostic an error" has ONE owner,
# Program::Diagnostic::is_error(). The questions built on it are Program's:
# first_error_diagnostic(), has_error_diagnostic() and error_diagnostic_count().
#
# Four scans of a diagnostics list each tested `severity == ...::error` by
# hand: parse_entry's refusal check, classify_entry's first error, the build
# lane's "a failed build says why" belt, and the validated refresh's error
# count. They agreed, but only by accident; the session's link refusal (plan
# §41.3) needed the same question a fifth time.
#
# One rule over src/ and include/: no code line compares a diagnostic's
# severity (`.severity` / `->severity`) with an enumerator by hand, unless it
# is marked `// allowed-exception: <why>` on the same line.
# Two-sided: the negative control proves the pattern still bites.
set -u
cd "$(dirname "$0")/.."

SCAN='(\.|->)severity *[!=]= *[A-Za-z_:]*(DiagnosticSeverity|diag_severity)::'

code_lines() { grep -HnE "$1" "${@:2}" | grep -vE '^[^:]*:[0-9]+:[[:space:]]*//'; }

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	if ( child.diagnostics[i].severity == ::Program::DiagnosticSeverity::error )
	if ( d->severity != DiagnosticSeverity::warning ) // allowed-exception: control
	// the older `d.severity == DiagnosticSeverity::error` test (a comment: skipped)
CTL
c=$(code_lines "$SCAN" "$ctl" | grep -v 'allowed-exception' | grep -c .)
if [ "$c" -ne 1 ]; then
	echo "check-one-error-diagnostic-scan: NEGATIVE CONTROL FAILED -- matched $c of 1"
	exit 1
fi

files=$(git ls-files 'src/*.cpp' 'src/*.c' 'src/*.h' 'include/*.h')
un=$(code_lines "$SCAN" $files | grep -v 'allowed-exception')
n=$(printf '%s' "$un" | grep -c . || true)
echo "hand-written severity tests on a diagnostic: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$un"
	echo "  -> ask Diagnostic::is_error(), or Program::first_error_diagnostic() /"
	echo "     has_error_diagnostic() / error_diagnostic_count() for a list."
	exit 1
fi
echo "GREEN -- \"is this diagnostic an error\" has one owner."
