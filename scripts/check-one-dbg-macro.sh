#!/bin/bash
# DRIFT-PREVENTION GATE -- the debug-trace macro DBG(x) has ONE definition,
# include/datadef.h, beside the flag it tests:
#   #define DBG(x) do { if (madc_verbose_requested.load(...) && madc_verbose) { x; } } while (0)
# (the process-wide pre-test first, so a thread_local read -- a
# __tls_get_addr call in libmadc -- happens only once some thread asked for
# verbose output), and madc_verbose has ONE writer, madc_set_verbose, which
# keeps that pre-test true for every thread that turns verbose on.
#
# Thirty-seven source files each spelled their own copy of the macro reading
# the thread_local directly; a copy that survives keeps the old test.
#
# Rules over src/ and include/: no `#define DBG(` except in include/datadef.h,
# and no assignment to madc_verbose except in madc_set_verbose (the line that
# carries `// the one writer`). tests/unit is exempt: each unit-test binary
# defines its own madc_verbose and DBG before the project headers
# (.claude/rules/debug.md). Two-sided: the negative control proves both
# patterns still bite.
set -u
cd "$(dirname "$0")/.."

DEF='^[[:space:]]*#[[:space:]]*define[[:space:]]+DBG\('
SET='(^|[^_[:alnum:]])madc_verbose[[:space:]]*=[^=]'

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
#define DBG(x) do { if(madc_verbose){x;} } while(0)
	madc_verbose = true;
	if ( madc_verbose == true ) {}
	madc_verbose = v;	// the one writer
CTL
d=$(grep -cE "$DEF" "$ctl")
s=$(grep -E "$SET" "$ctl" | grep -vc 'the one writer')
if [ "$d" -ne 1 ] || [ "$s" -ne 1 ]; then
	echo "check-one-dbg-macro: NEGATIVE CONTROL FAILED"
	echo "  a DBG definition matched $d of 1, an assignment $s of 1"
	exit 1
fi

files=$(git ls-files --cached --others --exclude-standard 'src/*.cpp' 'src/*.h' \
	'src/**/*.cpp' 'src/**/*.h' 'src/**/*.c' 'include/*.h' 'include/**/*.h' \
	| grep -v '^include/datadef\.h$' | sort -u)
bad_def=$(grep -HnE "$DEF" $files)
bad_set=$(grep -HnE "$SET" $files include/datadef.h | grep -v 'the one writer')
n=$(printf '%s' "$bad_def" | grep -c . || true)
m=$(printf '%s' "$bad_set" | grep -c . || true)
echo "DBG definitions outside include/datadef.h: $n (target 0); madc_verbose writers outside madc_set_verbose: $m (target 0)"
if [ "$n" -ne 0 ] || [ "$m" -ne 0 ]; then
	[ -n "$bad_def" ] && printf '%s\n' "$bad_def"
	[ -n "$bad_set" ] && printf '%s\n' "$bad_set"
	echo "  -> include datadef.h for DBG; set the flag with madc_set_verbose(v)."
	exit 1
fi
echo "GREEN -- DBG has one definition and madc_verbose one writer."
