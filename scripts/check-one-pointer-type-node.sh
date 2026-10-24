#!/bin/bash
# DRIFT-PREVENTION GATE -- a `char *` or `void *` (or `void **`) TYPE node the
# CIR builder spells -- a cast target -- has ONE owner each
# (src/cir_builder.cpp): CirBuilder::char_ptr_type() and
# CirBuilder::void_ptr_type(levels).
#
# Both owners existed, yet 32 sites spelled their exact node by hand
# (`node2(N_TYPE, node1(N_LIST, simple(N_CHAR)), node2(N_DECL, ignore(),
# node1(N_LIST, pointer())))`) and five more built the vtable read's `void **`
# from a two-pointer() declarator list.
#
# One rule over src/cir_*.cpp: a simple(N_CHAR) / simple(N_VOID) spec with an
# N_TYPE and a pointer( level within two lines before or three after is a
# hand-spelled pointer type -- except where the spec line is marked
# `// allowed-exception: <why>` (the owners).
# Two-sided: the negative control proves the pattern still bites.
set -u
cd "$(dirname "$0")/.."

scan() {
	awk '{ line[NR] = $0 }
	     END {
		for (i = 1; i <= NR; i++) {
			if (line[i] !~ /simple\(N_(CHAR|VOID)\)/ || line[i] ~ /^[[:space:]]*\/\//)
				continue
			w = line[i-2] " " line[i-1] " " line[i] " " line[i+1] " " line[i+2] " " line[i+3]
			if (w ~ /N_TYPE/ && w ~ /pointer\(/)
				print FILENAME ":" i ": " line[i]
		}
	     }' "$1"
}

ctl=$(mktemp)
trap 'rm -f "$ctl"' EXIT
cat > "$ctl" <<'CTL'
	node_t charp = node2(N_CAST,
		node2(N_TYPE, node1(N_LIST, simple(N_CHAR)),
		      node2(N_DECL, ignore(), node1(N_LIST, pointer()))),
		self, origin);
	// simple(N_VOID) N_TYPE pointer() in a comment: skipped
	append(spec, simple(N_VOID));	// allowed-exception: control
	node_t decl_list = list();
	append(decl_list, pointer());
	return node2(N_TYPE, spec, node2(N_DECL, ignore(), decl_list));
CTL
c=$(scan "$ctl" | grep -v 'allowed-exception' | grep -c .)
m=$(scan "$ctl" | grep -c 'allowed-exception')
if [ "$c" -ne 1 ] || [ "$m" -ne 1 ]; then
	echo "check-one-pointer-type-node: NEGATIVE CONTROL FAILED"
	echo "  an unmarked hand-spelled type matched $c of 1, a marked one $m of 1"
	exit 1
fi

files=$(git ls-files --cached --others --exclude-standard 'src/cir_*.cpp')
bad=$(for f in $files; do scan "$f"; done | grep -v 'allowed-exception')
n=$(printf '%s' "$bad" | grep -c . || true)
marked=$(for f in $files; do scan "$f"; done | grep -c 'allowed-exception' || true)
echo "hand-spelled char/void pointer types: $n (target 0); marked sites: $marked"
if [ "$n" -ne 0 ]; then
	printf '%s\n' "$bad"
	echo "  -> use CirBuilder::char_ptr_type() or void_ptr_type(levels)."
	exit 1
fi
echo "GREEN -- a char/void pointer type node has one owner each."
