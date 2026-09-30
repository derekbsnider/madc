#!/bin/bash
# DRIFT-PREVENTION GATE -- a constructor's member initialization has ONE owner,
# CirBuilder::class_member_init_stmts (src/cir_builder.cpp): the members in
# declaration order ([class.base.init]/13), each from its mem-initializer, else
# its default member initializer, else its default-initialization.
#
# The user-ctor prologue ran three passes over the members -- every
# mem-initializer, then the scalar default member initializers, then the
# class-type members -- the implicit default ctor ran the class-type members
# before the scalars, and a tsubst hit put its substituted mem-initializers at
# the body head, after all of them. Members initialized out of order, so an
# initializer that read another member, or had a side effect, saw the wrong
# state (`struct S { Tr a; int b = ++n; Tr c{0}; int d; S() : d(++n) {} };`
# gave `3 2 104 1`, g++ `1 2 103 4`).
#
# Rule over src/*.cpp: each per-member step is called only from its owner --
#   member_initializer_stmts        -> class_member_init_stmts
#   member_default_construct_stmts  -> class_member_init_stmts
#   class_member_default_construct  -> member_default_construct_stmts
# except where the line is marked `// allowed-exception: <why>`.
# Two-sided: the negative control proves a stray call still bites.
set -u
cd "$(dirname "$0")/.."

# file:line: <step> called from <enclosing>, for every call of an owned step
# outside its owner. The enclosing function is the last column-0 definition.
stray_calls() {
	awk '
	BEGIN {
		owner["member_initializer_stmts"] = "class_member_init_stmts"
		owner["member_default_construct_stmts"] = "class_member_init_stmts"
		owner["class_member_default_construct"] = "member_default_construct_stmts"
	}
	FNR == 1 { fn = "" }
	/^[A-Za-z_][^;]*\(/ && !/^(\/|#)/ {
		head = substr($0, 1, index($0, "(") - 1)
		n = split(head, w, /[^A-Za-z_0-9]+/)
		while (n > 0 && w[n] == "") n--
		if (n > 0) fn = w[n]
		next
	}
	/^[[:space:]]*\/\// { next }
	/allowed-exception/ { next }
	{
		for (s in owner) {
			if (match($0, "(^|[^A-Za-z_0-9:])" s "\\(") && fn != owner[s])
				printf "%s:%d: %s called from %s (owner: %s)\n",
					FILENAME, FNR, s, fn, owner[s]
		}
	}' "$@"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
bool CirBuilder::class_member_init_stmts(int)
{
	member_initializer_stmts(a);
}
bool CirBuilder::stray_member_walk(int)
{
	member_default_construct_stmts(a);
	// member_default_construct_stmts(a); (a comment: skipped)
	member_initializer_stmts(b); // allowed-exception: control
}
CTL
c=$(stray_calls "$ctl" | grep -c .)
if [ "$c" -ne 1 ]; then
	echo "check-one-member-init-walk: NEGATIVE CONTROL FAILED"
	echo "  a stray step call matched $c of 1"
	exit 1
fi

files=$(git ls-files 'src/*.cpp')
bad=$(stray_calls $files)
n=$(printf '%s' "$bad" | grep -c . || true)
echo "member-init steps called outside the walk: $n (target 0)"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bad"
	echo "  -> initialize members through CirBuilder::class_member_init_stmts,"
	echo "     the one declaration-order walk both constructor paths share."
	exit 1
fi
echo "GREEN -- a constructor's member initialization has one owner."
