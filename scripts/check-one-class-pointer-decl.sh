#!/bin/bash
# DRIFT-PREVENTION GATE -- a class POINTER the CIR builder spells has ONE
# owner per shape (src/cir_builder.cpp):
#   CirBuilder::class_ptr_type   -- the `struct Cls *` type node (a cast target)
#   CirBuilder::class_ptr_bind   -- `struct Cls *<nm> = <init>;` (a bound temp)
#   CirBuilder::class_ptr_param  -- `struct Cls *<nm>` (`__this`, a vbase param)
#
# About thirty sites spelled one of those by hand -- class_tag_ref() plus a
# pointer() level, the N_SHARE / N_DECL / three ignore() slots -- the upcast and
# vbase-view temporaries, the copy and constructor receiver binds, new / new[],
# the range-for capture, the method-shim slots, every vbase and base cast, the
# synthesized destructors' `__this` parameters.
#
# One rule over src/cir_*.cpp: a class_tag_ref( call with a pointer() level on
# its line or the next three is a hand-spelled class pointer -- except where
# the class_tag_ref line is marked `// allowed-exception: <why>` (the owners).
# Two-sided: the negative control proves the pattern still bites.
set -u
cd "$(dirname "$0")/.."

scan() {
	awk '{ line[NR] = $0 }
	     END {
		for (i = 1; i <= NR; i++) {
			if (line[i] !~ /class_tag_ref\(/ || line[i] ~ /^[[:space:]]*\/\//)
				continue
			w = line[i] " " line[i+1] " " line[i+2] " " line[i+3]
			if (w ~ /pointer\(\)/)
				print FILENAME ":" i ": " line[i]
		}
	     }' "$1"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	node_t vt = node2(N_TYPE, node1(N_LIST, class_tag_ref(vb)),
		node2(N_DECL, ignore(), node1(N_LIST, pointer())));
	// class_tag_ref(x) pointer() in a comment: skipped
	append(spec, class_tag_ref(cdd));
	append(spec, class_tag_ref(cdd, origin));	// allowed-exception: control
	node_t decl_list = list();
	append(decl_list, pointer());
CTL
c=$(scan "$ctl" | grep -v 'allowed-exception' | grep -c .)
m=$(scan "$ctl" | grep -c 'allowed-exception')
if [ "$c" -ne 2 ] || [ "$m" -ne 1 ]; then
	echo "check-one-class-pointer-decl: NEGATIVE CONTROL FAILED"
	echo "  an unmarked hand-spelled pointer matched $c of 2, a marked one $m of 1"
	exit 1
fi

files=$(git ls-files --cached --others --exclude-standard 'src/cir_*.cpp')
bad=$(for f in $files; do scan "$f"; done | grep -v 'allowed-exception')
n=$(printf '%s' "$bad" | grep -c . || true)
marked=$(for f in $files; do scan "$f"; done | grep -c 'allowed-exception' || true)
echo "hand-spelled class pointers: $n (target 0); marked sites: $marked"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bad"
	echo "  -> spell a class pointer with CirBuilder::class_ptr_type (a type),"
	echo "     class_ptr_bind (a bound temporary) or class_ptr_param (a parameter)."
	exit 1
fi
echo "GREEN -- a class pointer the CIR builder spells has one owner per shape."
