#!/usr/bin/env bash
# check-madcide-command-registry.sh — ONE action vocabulary (S1, 2026-09-08).
# profiles/default.menu is the command registry: every command a menu bar,
# a palette or a key profile can name has exactly one row there (its
# title). Three directions, each a drift a green suite cannot see:
#   1. every action the key profiles bind (unscoped lines, profiles/*.keys)
#      is a registered command, or a key spelling (the "action name IS the
#      key spelling" motion rule — tui_key_name's names);
#   2. every action apply_ide_event's action arm dispatches (`a == "x"`) is
#      registered — a dispatchable verb no menu can show is a hidden
#      command;
#   3. every registered command is dispatchable (or a key spelling) — a
#      title for nothing is a menu item that does nothing.
# Scoped (@scope) profile lines are modal-widget keys, not commands; the
# choose arm's row verbs (bld-*, opt-*, goto) are pane-row data, not
# commands — neither is in scope here.
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="$ROOT/tools/madcide/madcide_core.inc"
MENU="$ROOT/tools/madcide/profiles/default.menu"
KEYS_H="$ROOT/include/madcdis/keys.h"
PROFILES="$ROOT/tools/madcide/profiles"

# The registry: column 2 of every data line that is not a separator.
registry_ids()
{
	grep -v '^[[:space:]]*#' "$1" | awk 'NF >= 2 && $2 != "-" { print $2 }' | sort -u
}

# The dispatcher's action arm: from apply_ide_event's definition to its
# text arm; every `a == "name"` inside it.
dispatch_ids()
{
	awk '/^bool IdeSession::apply_ide_event/ { on = 1 }
	     on && /kind == ui::event_kind::text/ { exit }
	     on { print }' "$1" |
	grep -o 'a == "[A-Za-z0-9_-]*"' | sed 's/a == "//; s/"$//' | sort -u
}

# The key spellings (tui_key_name's names) — the motion vocabulary.
key_spellings()
{
	awk '/inline std::string tui_key_name/ { on = 1 }
	     on && /^}/ { exit }
	     on' "$1" | grep -o 'return "[a-z]*"' | sed 's/return "//; s/"$//' | sort -u
}

# Unscoped profile actions: the LAST word of every data line not starting
# with '@' — across the given profile files.
profile_ids()
{
	cat "$@" | grep -v '^[[:space:]]*#' | grep -v '^[[:space:]]*$' | grep -v '^@' |
	awk '{ print $NF }' | sort -u
}

# Names in $1 (one per line) that are in neither $2 nor $3.
missing_from()
{
	comm -23 "$1" <(sort -u "$2" "$3")
}

check()
{
	local core="$1" menu="$2" label="$3"
	local reg dis keys prof bad
	reg=$(mktemp); dis=$(mktemp); keys=$(mktemp); prof=$(mktemp)
	registry_ids "$menu" > "$reg"
	dispatch_ids "$core" > "$dis"
	key_spellings "$KEYS_H" > "$keys"
	profile_ids "$PROFILES"/*.keys > "$prof"
	local rc=0
	if [ ! -s "$reg" ] || [ ! -s "$dis" ] || [ ! -s "$keys" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — an extractor" \
		     "came back empty (registry $(wc -l < "$reg"), dispatch" \
		     "$(wc -l < "$dis"), keys $(wc -l < "$keys")); the anchors moved." >&2
		rc=1
	fi
	bad=$(missing_from "$prof" "$reg" "$keys")
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — a key profile" \
		     "binds an action with no row in profiles/default.menu:" \
		     $bad >&2
		rc=1
	fi
	bad=$(missing_from "$dis" "$reg" /dev/null)
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — the dispatcher" \
		     "handles an action with no row in profiles/default.menu" \
		     "(a command no menu or palette can show):" $bad >&2
		rc=1
	fi
	bad=$(missing_from "$reg" "$dis" "$keys")
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — profiles/default.menu" \
		     "titles a command the dispatcher does not handle:" $bad >&2
		rc=1
	fi
	rm -f "$reg" "$dis" "$keys" "$prof"
	return $rc
}

if ! check "$CORE" "$MENU" "live"; then
	exit 1
fi

# Negative controls: each direction must catch a synthetic drift.
tmpcore=$(mktemp); tmpmenu=$(mktemp)
awk '{ print } /^bool IdeSession::apply_ide_event/ { print "\tif ( a == \"zzbogus\" ) return true;" }' "$CORE" > "$tmpcore"
if check "$tmpcore" "$MENU" "control" 2>/dev/null; then
	rm -f "$tmpcore" "$tmpmenu"
	echo "check-madcide-command-registry: FAIL — negative control: an unregistered" \
	     "dispatcher action went undetected (the dispatch marker went blind)." >&2
	exit 1
fi
cat "$MENU" > "$tmpmenu"
echo "Help zzbogus Bogus" >> "$tmpmenu"
if check "$CORE" "$tmpmenu" "control" 2>/dev/null; then
	rm -f "$tmpcore" "$tmpmenu"
	echo "check-madcide-command-registry: FAIL — negative control: a registered" \
	     "command nothing dispatches went undetected (the registry marker went blind)." >&2
	exit 1
fi
rm -f "$tmpcore" "$tmpmenu"
n=$(registry_ids "$MENU" | wc -l)
echo "check-madcide-command-registry: OK ($n commands; profiles, dispatcher and menu agree; controls bite)"
