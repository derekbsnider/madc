#!/usr/bin/env bash
# check-madcide-id-width.sh — a graph id travels as int64_t in dialect code,
# never as `long` (V6 seam, the wine lane's finding).
#
# The code-graph ids are 64-bit by contract: a 21-bit parse-GENERATION tag
# sits at bit 40 (madc_program.cpp GRAPH_GEN_SHIFT), so a revision's node and
# the live node share every low bit and differ only up there. The dialect's
# `long` is the TARGET's long — 32 bits on LLP64 (win64) — so a `long` that
# holds an id silently drops the tag: `ids-differ: 0`, graph.children on an
# id that "belongs to another generation", graph.route to nowhere. Every
# graph test failed under wine while every Linux lane was green. `int64_t`
# is always known to include-free dialect code (<ns_madc> declares its own
# publics with it). Marker: in tools/madcide, no `long <…id…> = …["id"]`
# read and no `long id)` parameter.
set -u

DIR="$(dirname "$0")/../tools/madcide"

# The READ marker covers every tool file (an id read off a node's "id" /
# "to" / "from" … field is a graph id wherever it happens). The PARAMETER
# marker covers the code-graph seat files, where a `long id` parameter is a
# graph id by construction; a View row's id (the core) or a connection's
# share id (self_id / cid, small engine-minted counters) are not graph ids.
GRAPH_FILES="$DIR/madcide_mcp.inc $DIR/madcide_past.inc $DIR/madcide_nexus.inc $DIR/madcide_propose.inc $DIR/madcide_lsp.inc $DIR/madcide_layers.inc $DIR/madcide_tests.inc"
count_long_id_reads()  { grep -h -cE 'long [a-z0-9_]* = [^;]*\["(id|to|from|in|at|func|node)"\]' "$@" | paste -sd+ | bc; }
count_long_id_params() { cat "$@" | grep -E '\blong [a-z0-9_]*id\)' | grep -vcE '\blong (self_id|share_id|cid|conn_id|client_id)\)'; }

n=$(count_long_id_reads "$DIR"/*.inc)
m=$(count_long_id_params $GRAPH_FILES)
if [ "$n" -ne 0 ] || [ "$m" -ne 0 ]; then
	echo "check-madcide-id-width: FAIL — $n long-typed id read(s) and $m" \
	     "long-typed id parameter(s) in tools/madcide. A graph id is 64-bit" \
	     "(a generation tag at bit 40); the dialect's long is 32-bit on" \
	     "LLP64. Spell it int64_t:" >&2
	grep -nE 'long [a-z0-9_]* = [^;]*\["(id|to|from|in|at|func|node)"\]' "$DIR"/*.inc >&2
	grep -nE '\blong [a-z0-9_]*id\)' $GRAPH_FILES | grep -vE '\blong (self_id|share_id|cid|conn_id|client_id)\)' >&2
	exit 1
fi

# Negative controls: a synthetic read and a synthetic parameter must be caught.
tmp=$(mktemp -d)
printf '    long leaf = node["id"].as_integer();\nvoid f(var &out, long id)\n' > "$tmp/synthetic.inc"
if [ "$(count_long_id_reads "$tmp"/*.inc)" -ne 1 ] || [ "$(count_long_id_params "$tmp"/*.inc)" -ne 1 ]; then
	rm -rf "$tmp"
	echo "check-madcide-id-width: FAIL — negative control did not detect a" \
	     "synthetic long-typed id (the marker went blind)." >&2
	exit 1
fi
# ...and the int64_t spelling must pass, or the check is a tautology.
printf '    int64_t nid = node["id"].as_integer();\nvoid f(var &out, int64_t id)\n' > "$tmp/synthetic.inc"
if [ "$(count_long_id_reads "$tmp"/*.inc)" -ne 0 ] || [ "$(count_long_id_params "$tmp"/*.inc)" -ne 0 ]; then
	rm -rf "$tmp"
	echo "check-madcide-id-width: FAIL — positive control failed: the int64_t" \
	     "spelling was reported." >&2
	exit 1
fi
rm -rf "$tmp"

echo "check-madcide-id-width: OK (graph ids travel as int64_t in tools/madcide; controls bite)"
exit 0
