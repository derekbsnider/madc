#!/usr/bin/env bash
# check-madcide-seam.sh — the gateway seam's layer boundary (design doc
# 2026-08-31): madcide_core.inc is the SESSION layer and never touches a
# tui handle. Every tui call lives in the TUI client
# (madcide_client.inc); what a terminal knows reaches the session as
# client-pushed facts, and terminal actions leave it as parked requests.
#
# The ONE allowed tui-named call in the session is ui::tui_validate_keys —
# handle-free by contract (the session validates keybinding-profile data;
# only the client binds).
set -u

CORE="$(dirname "$0")/../tools/madcide/madcide_core.inc"
CLIENT="$(dirname "$0")/../tools/madcide/madcide_client.inc"

# tui CALLS in the session layer (name followed by an open paren — prose
# mentions don't count), minus the handle-free validator.
count_tui_calls()
{
	grep -o 'ui::tui_[a-z_]*(' "$1" | grep -vc '^ui::tui_validate_keys($'
}

# The old bag key that parked the tui handle on the session's state.
count_tui_key()
{
	grep -c '"tui"' "$1"
}

n=$(count_tui_calls "$CORE")
if [ "$n" -ne 0 ]; then
	echo "check-madcide-seam: FAIL — $n ui::tui_* call(s) in the session" \
	     "layer (tools/madcide/madcide_core.inc). The session never" \
	     "touches a tui handle: push the fact from the client, or park" \
	     "a terminal request (term_request) the client services." >&2
	exit 1
fi

n=$(count_tui_key "$CORE")
if [ "$n" -ne 0 ]; then
	echo "check-madcide-seam: FAIL — $n \"tui\" bag key(s) in the session" \
	     "layer. The tui handle is the CLIENT's local; it never rides" \
	     "the session's bags." >&2
	exit 1
fi

if ! grep -q 'run_ide' "$CLIENT"; then
	echo "check-madcide-seam: FAIL — the TUI client" \
	     "(tools/madcide/madcide_client.inc) no longer holds run_ide;" \
	     "the loop belongs to the client layer." >&2
	exit 1
fi

# Negative controls: a synthetic violation of each marker must be caught.
tmp=$(mktemp)
cat "$CORE" > "$tmp"
echo '    ui::tui_refresh(t);	// synthetic' >> "$tmp"
if [ "$(count_tui_calls "$tmp")" -ne 1 ]; then
	rm -f "$tmp"
	echo "check-madcide-seam: FAIL — negative control did not detect a" \
	     "synthetic tui call (the marker went blind)." >&2
	exit 1
fi
cat "$CORE" > "$tmp"
echo '    long t = es_int(w, es, "tui", 0);	// synthetic' >> "$tmp"
if [ "$(count_tui_key "$tmp")" -ne 1 ]; then
	rm -f "$tmp"
	echo "check-madcide-seam: FAIL — negative control did not detect a" \
	     "synthetic \"tui\" bag key (the marker went blind)." >&2
	exit 1
fi
rm -f "$tmp"

echo "check-madcide-seam: OK (session layer tui-free; the client holds" \
     "render/input/refresh/suspend)"
exit 0
