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
# 2. ONE BIND, AND IT ADVERTISES. `serve_listen` is the only place a serve
#    face binds `listen://` and the only place it publishes the result: the
#    headless server, the language server and the editor all call it. Before
#    the V6 seam the bind was restated three times, and the copies had already
#    diverged — two reported a failed bind, the editor's swallowed it, so an
#    editor whose ephemeral port would not bind ran unadvertised and said
#    nothing. A session that listens but does not publish is invisible to
#    discovery — and invisible is indistinguishable from absent, so a client
#    silently opens a SECOND session on the same file: two carets, two undo
#    histories, last save wins. Marker: exactly one `listen://` bind site and
#    exactly one `session_advertise(` call site in tools/madcide. A new face
#    calls serve_listen or fails here.
#
# 3. A TEST THAT SPAWNS A BINDING FACE KEEPS ITS ADVERTISEMENT IN tmp/. Every
#    face that binds advertises (2), so a test whose exec:// child runs with
#    `--serve` publishes a record — into the developer's REAL state directory
#    ($XDG_STATE_HOME / ~/.local/state) unless the test's .env fixture points
#    MADCIDE_SESSION_DIR under tmp/. Found at the V6 seam: testmadcide_lsp_serve
#    moved the mtime of ~/.local/state/madcide/sessions on every run, and while
#    it ran a `--attach` with no address anywhere in this repo could have joined
#    the TEST's child. (A default-TUI spawn would bind too; no test spawns one —
#    the editor needs a pty.) Marker: every tests/*.mad with an exec:// spawn of
#    madcide.mad carrying --serve has a sibling .env naming
#    MADCIDE_SESSION_DIR=tmp/…
set -u

DIR="$(dirname "$0")/../tools/madcide"
TESTS="$(dirname "$0")/../tests"

# Code sites only — a prose mention of listen:// in a header comment is not a
# bind, so the marker anchors on the format() that builds the URI.
count_spawns()   { grep -h -c 'go serve_conn_task(' "$DIR"/*.inc | paste -sd+ | bc; }
count_binds()    { grep -h -o 'format("listen://' "$DIR"/*.inc | wc -l; }
count_adverts()  { grep -h -o '^[^/]*session_advertise(S, ' "$DIR"/*.inc | wc -l; }
# Tests whose spawned madcide child binds a serve face, minus the ones whose
# .env keeps the advertisement under tmp/.
unhermetic()
{
	local dir="$1" f base
	for f in $(grep -l -E 'exec://.*madcide\.mad.*--serve' "$dir"/*.mad 2>/dev/null); do
		base="${f%.mad}"
		grep -q 'MADCIDE_SESSION_DIR=tmp/' "$base.env" 2>/dev/null || echo "$f"
	done
}

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
if [ "$binds" -ne 1 ] || [ "$adverts" -ne 1 ]; then
	echo "check-madcide-one-accept-loop: FAIL — $binds listen:// bind" \
	     "site(s) and $adverts session_advertise() call(s) in tools/madcide" \
	     "(expected 1 and 1: serve_listen). A restated bind is how the" \
	     "editor's copy came to swallow a failed bind while its siblings" \
	     "reported it; a face that listens without advertising is invisible" \
	     "to discovery, and a client that cannot find it opens a SECOND" \
	     "session on the same file. Call serve_listen." >&2
	exit 1
fi

bad=$(unhermetic "$TESTS")
if [ -n "$bad" ]; then
	echo "check-madcide-one-accept-loop: FAIL — a test spawns a madcide" \
	     "child with --serve but no .env fixture keeps its advertisement" \
	     "under tmp/ (MADCIDE_SESSION_DIR=tmp/…); it publishes into the" \
	     "developer's real state directory while it runs:" >&2
	echo "$bad" >&2
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
# A synthetic test that spawns a --serve child with no .env must be reported.
tmp=$(mktemp -d)
printf 'var u = format("exec://{} tools/madcide/madcide.mad {} --lsp --serve 127.0.0.1:0", a, b);\n' > "$tmp/synthetic.mad"
if [ "$(unhermetic "$tmp")" != "$tmp/synthetic.mad" ]; then
	rm -rf "$tmp"
	echo "check-madcide-one-accept-loop: FAIL — negative control did not" \
	     "detect a synthetic unhermetic --serve test (the marker went blind)." >&2
	exit 1
fi
# ...and the same test WITH the fixture must pass, or the check is a tautology.
printf 'MADCIDE_SESSION_DIR=tmp/synthetic_sessions\n' > "$tmp/synthetic.env"
if [ -n "$(unhermetic "$tmp")" ]; then
	rm -rf "$tmp"
	echo "check-madcide-one-accept-loop: FAIL — positive control failed: a" \
	     "--serve test WITH a tmp/ MADCIDE_SESSION_DIR fixture was reported." >&2
	exit 1
fi
rm -rf "$tmp"

echo "check-madcide-one-accept-loop: OK (one accept loop; one bind site," \
     "advertised; --serve tests hermetic)"
exit 0
