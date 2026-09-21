#!/usr/bin/env bash
# check-madcide-command-registry.sh — ONE action vocabulary (S1, 2026-09-08;
# re-anchored on the command ENUM, V0.5c 2026-09-09).
# The vocabulary is `enum ide_cmd` in tools/madcide/madcide_enums.inc; its
# names live in the ONE table cmd_table() beside it (`{ "name": "save",
# "code": cmdSAVE }` rows); profiles/default.menu is the command REGISTRY
# (every command a menu bar or a palette shows has exactly one row: its
# title). Five directions, each a drift a green suite cannot see:
#   1. every action the key profiles bind (unscoped lines AND @scope values,
#      profiles/*.keys) is a table name — a misspelt profile word would be
#      REFUSED at load, so the shipped profiles must all load;
#   2. every registered command is a table name (a title for a word the
#      dispatcher cannot spell is a menu item that does nothing);
#   3. every table row's enumerator exists in the enum, and every enumerator
#      has a table row — one vocabulary, two spellings, never drifting;
#   4. every enumerator is DISPATCHED somewhere in madcide_core.inc — a
#      `case cmdX:` label or a `== cmdX` / `!= cmdX` compare (a hidden
#      command nothing handles);
#   5. every `cmd…` spelled in madcide_core.inc is an enumerator (the
#      compiler enforces this too; the gate names the drift first).
set -u

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CORE="$ROOT/tools/madcide/madcide_core.inc"
ENUMS="$ROOT/tools/madcide/madcide_enums.inc"
MENU="$ROOT/tools/madcide/profiles/default.menu"
PROFILES="$ROOT/tools/madcide/profiles"

# The registry: column 2 of every data line that is not a separator.
registry_ids()
{
	grep -v '^[[:space:]]*#' "$1" | awk 'NF >= 2 && $2 != "-" { print $2 }' | sort -u
}

# The table rows of cmd_table(): "name enumerator" pairs.
table_pairs()
{
	grep -o -E '\{ "name": "[A-Za-z0-9_-]*", "code": cmd[A-Z][A-Z0-9_]* \}' "$1" |
	sed 's/{ "name": "//; s/", "code": / /; s/ }$//'
}

# The enum body's enumerators (between `enum ide_cmd` and its `};`).
enum_ids()
{
	awk '/^enum ide_cmd/ { on = 1 } on { print } on && /^};/ { exit }' "$1" |
	grep -o -E 'cmd[A-Z][A-Z0-9_]*' | sort -u
}

# Where a command is dispatched: case labels and compares against an
# enumerator — in the core AND in the enums file's converters (the motion
# commands are handled by motion_key_of's switch there).
dispatch_ids()
{
	cat "$@" |
	grep -o -E '(case[[:space:]]+cmd[A-Z][A-Z0-9_]*[[:space:]]*:|[!=]=[[:space:]]*cmd[A-Z][A-Z0-9_]*)' |
	grep -o -E 'cmd[A-Z][A-Z0-9_]*' | sort -u
}

# Every enumerator spelled anywhere in the core.
spelled_ids()
{
	grep -o -E 'cmd[A-Z][A-Z0-9_]*' "$1" | sort -u
}

# Profile actions: the LAST word of every data line, unscoped or @scoped
# (a scoped line's value is its last word too) — across the given files.
profile_ids()
{
	cat "$@" | grep -v '^[[:space:]]*#' | grep -v '^[[:space:]]*$' |
	awk '{ print $NF }' | sort -u
}

missing_from()
{
	comm -23 "$1" "$2"
}

check()
{
	local core="$1" enums="$2" menu="$3" profiles="$4" label="$5"
	local reg pairs tnames tenums enumids dis spelled prof bad
	reg=$(mktemp); pairs=$(mktemp); tnames=$(mktemp); tenums=$(mktemp)
	enumids=$(mktemp); dis=$(mktemp); spelled=$(mktemp); prof=$(mktemp)
	registry_ids "$menu" > "$reg"
	table_pairs "$enums" > "$pairs"
	awk '{ print $1 }' "$pairs" | sort -u > "$tnames"
	awk '{ print $2 }' "$pairs" | sort -u > "$tenums"
	enum_ids "$enums" | grep -v '^cmdNONE$' > "$enumids"
	dispatch_ids "$core" "$enums" > "$dis"
	spelled_ids "$core" > "$spelled"
	profile_ids "$profiles"/*.keys > "$prof"
	local rc=0
	if [ ! -s "$reg" ] || [ ! -s "$pairs" ] || [ ! -s "$enumids" ] || [ ! -s "$dis" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — an extractor" \
		     "came back empty (registry $(wc -l < "$reg"), table" \
		     "$(wc -l < "$pairs"), enum $(wc -l < "$enumids"), dispatch" \
		     "$(wc -l < "$dis")); the anchors moved." >&2
		rc=1
	fi
	bad=$(missing_from "$prof" "$tnames")
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — a key profile" \
		     "binds a word the command table does not name (the profile" \
		     "would be REFUSED at load):" $bad >&2
		rc=1
	fi
	bad=$(missing_from "$reg" "$tnames")
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — profiles/default.menu" \
		     "titles a command the table does not name (the menu would be" \
		     "REFUSED at load):" $bad >&2
		rc=1
	fi
	bad=$(missing_from "$tenums" "$enumids")
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — the command table" \
		     "names an enumerator the enum lacks:" $bad >&2
		rc=1
	fi
	# cmdBUILDROW has no table row by design: its name is "build-<n>".
	bad=$(missing_from "$enumids" "$tenums" | grep -v '^cmdBUILDROW$')
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — an enumerator has" \
		     "no command-table row (no name can reach it):" $bad >&2
		rc=1
	fi
	bad=$(missing_from "$enumids" "$dis")
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — an enumerator is" \
		     "dispatched nowhere in madcide_core.inc (a hidden command):" $bad >&2
		rc=1
	fi
	bad=$(missing_from "$spelled" <(sort -u "$enumids" <(echo cmdNONE)))
	if [ -n "$bad" ]; then
		echo "check-madcide-command-registry: FAIL ($label) — madcide_core.inc" \
		     "spells a cmd… that is not an enumerator:" $bad >&2
		rc=1
	fi
	rm -f "$reg" "$pairs" "$tnames" "$tenums" "$enumids" "$dis" "$spelled" "$prof"
	return $rc
}

if ! check "$CORE" "$ENUMS" "$MENU" "$PROFILES" "live"; then
	exit 1
fi

# Negative controls: each direction must catch a synthetic drift.
tmpcore=$(mktemp); tmpenums=$(mktemp); tmpmenu=$(mktemp); tmpprof=$(mktemp -d)
# (a) a table row whose enumerator nothing dispatches
sed 's/^    t = rows;$/    rows[] = { "name": "zzbogus", "code": cmdZZBOGUS };\n    t = rows;/; s/^    cmdBUILDROW\(.*\)$/    cmdZZBOGUS, cmdBUILDROW\1/' "$ENUMS" > "$tmpenums"
if check "$CORE" "$tmpenums" "$MENU" "$PROFILES" "control" 2>/dev/null; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof"
	echo "check-madcide-command-registry: FAIL — negative control: an enumerator" \
	     "nothing dispatches went undetected (the dispatch marker went blind)." >&2
	exit 1
fi
# (b) a registered command the table does not name
cat "$MENU" > "$tmpmenu"
echo "Help zzbogus Bogus" >> "$tmpmenu"
if check "$CORE" "$ENUMS" "$tmpmenu" "$PROFILES" "control" 2>/dev/null; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof"
	echo "check-madcide-command-registry: FAIL — negative control: a registered" \
	     "command the table lacks went undetected (the registry marker went blind)." >&2
	exit 1
fi
# (c) a profile binding a word the table does not name
cp "$PROFILES"/*.keys "$tmpprof"/
echo "^k 9 zzbogus" >> "$tmpprof/joe.keys"
if check "$CORE" "$ENUMS" "$MENU" "$tmpprof" "control" 2>/dev/null; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof"
	echo "check-madcide-command-registry: FAIL — negative control: a profile" \
	     "word outside the table went undetected (the profile marker went blind)." >&2
	exit 1
fi
# (d) the core spelling an enumerator that does not exist
awk '{ print } /^bool IdeSession::apply_ide_event/ { print "\tif ( code == cmdZZBOGUS ) return true;" }' "$CORE" > "$tmpcore"
if check "$tmpcore" "$ENUMS" "$MENU" "$PROFILES" "control" 2>/dev/null; then
	rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof"
	echo "check-madcide-command-registry: FAIL — negative control: a cmd… the" \
	     "enum lacks went undetected (the spelling marker went blind)." >&2
	exit 1
fi
rm -rf "$tmpcore" "$tmpenums" "$tmpmenu" "$tmpprof"
n=$(registry_ids "$MENU" | wc -l)
m=$(enum_ids "$ENUMS" | grep -vc '^cmdNONE$')
echo "check-madcide-command-registry: OK ($n registered commands of $m enumerators; profiles, registry, table, enum and dispatcher agree; controls bite)"
