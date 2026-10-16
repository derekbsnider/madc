#!/bin/bash
# forest_pack_roots.sh — a pack header list's roots, by population. Sourced
# (forest_pack.sh, forest_dm_oracle.sh):
#
#   forest_pack_roots <list> all              every root, in list order
#   forest_pack_roots <list> system           the C/C++ system headers alone
#   forest_pack_roots <list> through-dialect  the roots through the last
#                                             dialect fragment
#
# A dialect fragment is the compiler's embedded_dialect_fragment_p rule
# (src/lexer.cpp) in the shell's alphabet: a top-level ns_* or a bits/<name>
# entry, extensionless. A system header is every other root — the set g++
# can include too.
forest_pack_roots() {
    grep -vE '^[[:space:]]*(#|$)' "$1" | awk -v mode="$2" '
	function fragment(s) {
	    if ( s ~ /\./ || s ~ /^ns_[^\/]*\// || s ~ /^bits\/[^\/]*\// )
		return 0
	    return s ~ /^ns_/ || s ~ /^bits\//
	}
	{ line[NR] = $0; frag[NR] = fragment($0); if ( frag[NR] ) last = NR }
	END {
	    for ( i = 1; i <= NR; ++i )
		if ( mode == "all" || (mode == "system" && !frag[i]) \
		     || (mode == "through-dialect" && i <= last) )
		    print line[i]
	}'
}
