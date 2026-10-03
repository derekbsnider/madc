#!/bin/bash
# DRIFT-PREVENTION GATE -- a reference-returning call's result has one owner.
# madc lowers T& as T*, so a call to a reference-returning function yields the
# referent's ADDRESS, and the call expression is `*call` (the referent lvalue,
# read or written). CirBuilder::reference_call_result is that rule.
#
# It was inlined at 18 sites -- every call, method, operator, subscript and
# postfix arm, the dump walker and the manipulator bind -- and the host-call
# shim (synth_call_shim_var) was the copy that lacked it: it handed the
# referent's address to the integer / real / text setters. c2mir warned on an
# `int &` return, refused the whole program on a `double &` return, and a
# host program::call returned the address as the value (B134;
# tests/testrefreturnshim, tests/unit test_libmadc_program).
#
# Rule, over the tracked src/*.cpp: a line testing `returns_reference()` with
# `N_DEREF` on it or on one of the next two lines is an inline copy. The owner
# is marked `allowed-exception: the owner`; a comment line is skipped.
# Two-sided: the negative control proves the rule still bites.
set -u
cd "$(dirname "$0")/.."

scan() {
	awk '
	FNR == 1 { m = 0 }
	/^[[:space:]]*\/\// { next }
	/returns_reference\(\)/ { m = ($0 ~ /allowed-exception: the owner/) ? 0 : FNR }
	/N_DEREF/ && m && FNR - m <= 2 { print FILENAME ":" FNR; m = 0 }
	' "$@"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	if (callee && callee->returns_reference())
		return node1(N_DEREF, call, origin);
	return fd->returns_reference() ? node1(N_DEREF, call, tb) : call;
	if (callee && callee->returns_reference())	// allowed-exception: the owner
		return node1(N_DEREF, call, origin);
	// if (fd->returns_reference()) return node1(N_DEREF, call, tb);
	return reference_call_result(callee, call, origin);
CTL
c=$(scan "$ctl" | grep -c .)
if [ "$c" -ne 2 ]; then
	echo "check-one-reference-call-result: NEGATIVE CONTROL FAILED -- matched $c of 2"
	scan "$ctl"
	exit 1
fi

files=$(git ls-files 'src/*.cpp')
hits=$(scan $files)
n=$(printf '%s' "$hits" | grep -c . || true)
echo "reference-return call results read by hand: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$hits" | sed 's/^/  /'
	echo "  -> read a call's result through CirBuilder::reference_call_result"
	echo "     (callee, call, origin)."
	exit 1
fi
echo "GREEN -- a reference-returning call's result has one owner."
