#!/usr/bin/env bash
# check-madcide-one-accept-loop.sh — madcide's serving faces stay consolidated
# (V6c-4, docs/plans/2026-09-15-v6c4-session-discovery-plan.md).
#
# TWO invariants, both of which regrew once already in this arc:
#
# 1. ONE ACCEPT LOOP. `serve_accept_task` is the only place a connection is
#    accepted and dispatched — the headless server calls it, the language
#    server spawns it, the editor spawns it. A second hand-rolled
#    wait_readable/accept/detach loop is how the shutdown poke, the
#    advertisement refresh and the connection-task join quietly diverge (the
#    run_serve copy did exactly that until V6c-4 folded it in). Marker: the
#    number of `go serve_conn_task(` spawns across tools/madcide == 1.
#
# 2. EVERY FACE THAT BINDS, ADVERTISES. A session that listens but does not
#    publish is invisible to discovery — and invisible is indistinguishable
#    from absent, so a client silently opens a SECOND session on the same file:
#    two carets, two undo histories, last save wins. Marker: the number of
#    `listen://` bind sites == the number of `session_advertise(` call sites.
#    A new face adds both lines or fails here.
set -u

DIR="$(dirname "$0")/../tools/madcide"

# Code sites only — a prose mention of listen:// in a header comment is not a
# bind, so the marker anchors on the format() that builds the URI.
count_spawns()   { grep -h -c 'go serve_conn_task(' "$DIR"/*.inc | paste -sd+ | bc; }
count_binds()    { grep -h -o 'format("listen://' "$DIR"/*.inc | wc -l; }
count_adverts()  { grep -h -o '^[^/]*session_advertise(S, ' "$DIR"/*.inc | wc -l; }

n=$(count_spawns)
if [ "$n" -ne 1 ]; then
	echo "check-madcide-one-accept-loop: FAIL — $n serve_conn_task spawn" \
	     "site(s) in tools/madcide (expected 1: serve_accept_task). A" \
	     "second accept loop diverges on the shutdown poke, the" \
	     "advertisement refresh and the connection-task join — call" \
	     "serve_accept_task instead of restating it." >&2
	exit 1
fi

binds=$(count_binds)
adverts=$(count_adverts)
if [ "$binds" -ne "$adverts" ]; then
	echo "check-madcide-one-accept-loop: FAIL — $binds listen:// bind" \
	     "site(s) but $adverts session_advertise() call(s). A face that" \
	     "listens without advertising is invisible to discovery, and a" \
	     "client that cannot find it opens a SECOND session on the same" \
	     "file. Advertise beside every bind." >&2
	exit 1
fi

# Negative controls: a synthetic violation of each marker must be caught.
tmp=$(mktemp -d)
cp "$DIR"/*.inc "$tmp/"
printf '\tgo serve_conn_task(S, doc, conn);\t// synthetic\n' >> "$tmp/madcide_serve.inc"
if [ "$(grep -h -c 'go serve_conn_task(' "$tmp"/*.inc | paste -sd+ | bc)" -ne 2 ]; then
	rm -rf "$tmp"
	echo "check-madcide-one-accept-loop: FAIL — negative control did not" \
	     "detect a synthetic accept-loop spawn (the marker went blind)." >&2
	exit 1
fi
rm -rf "$tmp"
tmp=$(mktemp -d)
cp "$DIR"/*.inc "$tmp/"
printf '\tvar u = format("listen://{}", addr);\t// synthetic\n' >> "$tmp/madcide_serve.inc"
if [ "$(grep -h -o 'format("listen://' "$tmp"/*.inc | wc -l)" -le "$binds" ]; then
	rm -rf "$tmp"
	echo "check-madcide-one-accept-loop: FAIL — negative control did not" \
	     "detect a synthetic bind site (the marker went blind)." >&2
	exit 1
fi
rm -rf "$tmp"

echo "check-madcide-one-accept-loop: OK (one accept loop; $binds bind site(s)," \
     "$adverts advertised)"
exit 0
